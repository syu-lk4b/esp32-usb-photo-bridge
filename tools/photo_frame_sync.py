#!/usr/bin/env python3
"""
Sync an Apple Photos library to an ESP32 USB Photo Bridge.

Reads Photos.sqlite (read-only) and copies the newest photos of the chosen
people to the bridge over Wi-Fi.  Images are converted to the 800x600
Baseline JPEG that old USB photo frames understand.

Usage:
    python3 photo_frame_sync.py                     # sync using tools/config.json
    python3 photo_frame_sync.py --dry-run           # show what would be uploaded
    python3 photo_frame_sync.py --list-people       # print Photos person IDs
    python3 photo_frame_sync.py --force             # ignore the de-dupe state

macOS only: uses sips for image conversion and reads the Photos library
directly, so the calling process needs Full Disk Access.
"""

import argparse
import json
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from urllib.parse import quote

DEFAULTS = {
    "device_url": "http://photo-frame.local",
    "web_user": "admin",
    "web_password": "",
    "photos_library": "~/Pictures/Photos Library.photoslibrary",
    "people": {},
    "max_per_person": 12,
    "max_days": 730,
    "keep_on_card": 90,
    "jpeg_quality": "70",
    "width": 800,
    "height": 600,
    "state_file": "~/.local/state/esp32-photo-bridge/state.json",
    "log_file": "~/.local/state/esp32-photo-bridge/sync.log",
}

CFG = dict(DEFAULTS)
ESP = DEFAULTS["device_url"]
LIB = ""
DB = ""
STATE = ""
LOG = ""


def log(msg):
    line = "[%s] %s" % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line, flush=True)
    try:
        if LOG and os.path.getsize(LOG) > 5 * 1024 * 1024:
            shutil.move(LOG, LOG + ".1")
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def load_config(path):
    global CFG, ESP, LIB, DB, STATE, LOG
    cfg = dict(DEFAULTS)
    if path and os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            user = json.load(f)
        cfg.update(user)
    CFG = cfg

    ESP = str(cfg["device_url"]).rstrip("/")
    LIB = os.path.expanduser(str(cfg["photos_library"]))
    DB = os.path.join(LIB, "database", "Photos.sqlite")
    STATE = os.path.expanduser(str(cfg["state_file"]))
    LOG = os.path.expanduser(str(cfg["log_file"]))

    for p in (STATE, LOG):
        d = os.path.dirname(p)
        if d:
            os.makedirs(d, exist_ok=True)


def curl_json(path, method="GET", timeout=60, files=None):
    cmd = ["/usr/bin/curl", "-s", "-m", str(timeout), "-X", method]
    pwd = str(CFG.get("web_password") or "")
    if pwd:
        cmd += ["-u", "%s:%s" % (CFG.get("web_user", "admin"), pwd)]
    cmd += ["-w", "\n%{http_code}", ESP + path]
    if files:
        for f in files:
            cmd += ["-F", "file=@" + f]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout.rsplit("\n", 1)
    body = out[0] if len(out) > 1 else ""
    code = out[-1].strip()
    try:
        data = json.loads(body) if body else {}
    except Exception:
        data = {"raw": body}
    return code, data


def photos_db():
    # mode=ro honours the WAL (brand-new photos show up immediately);
    # immutable=1 ignores the WAL but never blocks.
    try:
        con = sqlite3.connect("file:%s?mode=ro" % DB, uri=True, timeout=10)
        con.execute("SELECT 1 FROM ZASSET LIMIT 1").fetchone()
        return con
    except Exception as e:
        log("photos db mode=ro failed (%s), falling back to immutable" % e)
        return sqlite3.connect("file:%s?immutable=1" % DB, uri=True, timeout=10)


def list_people():
    con = photos_db()
    rows = con.execute(
        "SELECT Z_PK, ZFULLNAME, ZDISPLAYNAME, ZFACECOUNT FROM ZPERSON "
        "WHERE ZFACECOUNT > 20 ORDER BY ZFACECOUNT DESC LIMIT 40").fetchall()
    con.close()
    print("Z_PK     faces  name")
    for pk, full, disp, cnt in rows:
        name = full or disp or ""
        print("%7d  %5d  %s" % (pk, cnt, name))


def fetch_assets(person_pk, limit, max_days):
    """Newest still, landscape photos (optionally of one person)."""
    con = photos_db()
    params = []
    where_time = ""
    if max_days and max_days > 0:
        where_time = "AND a.ZDATECREATED > ?"
        params.append(time.time() - 978307200 - max_days * 86400)
    params.append(limit * 8)

    if person_pk is None:
        query = """
            SELECT a.Z_PK, a.ZUUID, a.ZDATECREATED, a.ZDIRECTORY, a.ZFILENAME,
                   a.ZWIDTH, a.ZHEIGHT
            FROM ZASSET a
            WHERE a.ZTRASHEDSTATE = 0
              AND a.ZHIDDEN = 0
              AND a.ZKIND = 0
              AND a.ZWIDTH IS NOT NULL AND a.ZHEIGHT IS NOT NULL
              AND a.ZWIDTH > a.ZHEIGHT
              {where_time}
            ORDER BY a.ZDATECREATED DESC
            LIMIT ?
        """.format(where_time=where_time)
    else:
        params = [person_pk] + params
        query = """
            SELECT a.Z_PK, a.ZUUID, a.ZDATECREATED, a.ZDIRECTORY, a.ZFILENAME,
                   a.ZWIDTH, a.ZHEIGHT
            FROM ZASSET a
            JOIN ZDETECTEDFACE f ON f.ZASSETFORFACE = a.Z_PK
            WHERE f.ZPERSONFORFACE = ?
              AND f.ZHIDDEN = 0
              AND a.ZTRASHEDSTATE = 0
              AND a.ZHIDDEN = 0
              AND a.ZKIND = 0
              AND a.ZWIDTH IS NOT NULL AND a.ZHEIGHT IS NOT NULL
              AND a.ZWIDTH > a.ZHEIGHT
              {where_time}
            GROUP BY a.Z_PK
            ORDER BY a.ZDATECREATED DESC
            LIMIT ?
        """.format(where_time=where_time)

    rows = con.execute(query, params).fetchall()
    con.close()

    seen, out = set(), []
    for pk, uuid, created, directory, filename, w, h in rows:
        if uuid in seen:
            continue
        seen.add(uuid)
        src = os.path.join(LIB, "originals", directory or "", filename or "")
        if not os.path.isfile(src):
            continue
        out.append({
            "uuid": uuid,
            "src": src,
            "date": created,
            "w": w, "h": h,
            "filename": filename,
        })
        if len(out) >= limit:
            break
    return out


def read_exif_orientation(path):
    """Return EXIF orientation (1/3/6/8) of a JPEG, or 1 when absent."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except Exception:
        return 1
    if data[:2] != b"\xff\xd8":
        return 1
    i = 2
    n = len(data)
    while i + 4 <= n:
        if data[i] != 0xFF:
            return 1
        m = data[i + 1]
        if m in (0xD8, 0x01):
            i += 2
            continue
        if m == 0xDA:
            return 1
        ln = int.from_bytes(data[i + 2:i + 4], "big")
        if m == 0xE1 and data[i + 4:i + 10] == b"Exif\x00\x00":
            t = data[i + 10:i + 2 + ln]
            if t[:2] == b"II":
                e = "<"
            elif t[:2] == b"MM":
                e = ">"
            else:
                return 1
            off = struct.unpack(e + "I", t[4:8])[0]
            cnt = struct.unpack(e + "H", t[off:off + 2])[0]
            for k in range(cnt):
                p = off + 2 + k * 12
                if p + 12 > len(t):
                    break
                if struct.unpack(e + "H", t[p:p + 2])[0] == 0x0112:
                    return struct.unpack(e + "H", t[p + 8:p + 10])[0]
            return 1
        i += 2 + ln
    return 1


def set_exif_orientation(path, value=1):
    """Rewrite the EXIF Orientation tag in place. Returns True on success."""
    try:
        with open(path, "rb") as fh:
            data = bytearray(fh.read())
    except Exception:
        return False
    if data[:2] != b"\xff\xd8":
        return False
    i = 2
    n = len(data)
    while i + 4 <= n:
        if data[i] != 0xFF:
            return False
        m = data[i + 1]
        if m in (0xD8, 0x01):
            i += 2
            continue
        if m == 0xDA:
            return False
        ln = int.from_bytes(data[i + 2:i + 4], "big")
        if m == 0xE1 and data[i + 4:i + 10] == b"Exif\x00\x00":
            t_off = i + 10
            t = data[t_off:i + 2 + ln]
            if t[:2] == b"II":
                e = "<"
            elif t[:2] == b"MM":
                e = ">"
            else:
                return False
            off = struct.unpack(e + "I", t[4:8])[0]
            cnt = struct.unpack(e + "H", t[off:off + 2])[0]
            for k in range(cnt):
                p = off + 2 + k * 12
                if p + 12 > len(t):
                    break
                if struct.unpack(e + "H", t[p:p + 2])[0] == 0x0112:
                    abs_p = t_off + p + 8
                    data[abs_p:abs_p + 2] = struct.pack(e + "H", value)
                    with open(path, "wb") as fh:
                        fh.write(data)
                    return True
            return False
        i += 2 + ln
    return False


def image_size(path):
    r = subprocess.run(["/usr/bin/sips", "-g", "pixelWidth", "-g", "pixelHeight", path],
                       capture_output=True, text=True)
    w = h = 0
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("pixelWidth:"):
            w = int(line.split(":")[1])
        elif line.startswith("pixelHeight:"):
            h = int(line.split(":")[1])
    return w, h


def convert(src, dst):
    """HEIC/JPEG/PNG -> 800x600 baseline JPEG with orientation baked in.

    Old frames ignore EXIF, so the Orientation tag must be applied to the
    actual pixels.  sips copies the tag without rotating, so we neutralise the
    tag first, rotate physically, then write orientation=1 back.
    Returns (ok, width, height) measured before padding, so portrait results
    can still be rejected by the caller.
    """
    W = int(CFG["width"])
    H = int(CFG["height"])

    cmd = ["/usr/bin/sips", "-s", "format", "jpeg",
           "-s", "formatOptions", str(CFG["jpeg_quality"]),
           "--resampleHeightWidthMax", str(max(W, H)), src, "--out", dst]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(dst):
        r = subprocess.run(["/usr/bin/sips", "-s", "format", "jpeg", src, "--out", dst],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.isfile(dst):
            return False, 0, 0

    o = read_exif_orientation(dst)
    if o in (3, 6, 8):
        set_exif_orientation(dst, 1)
        deg = {3: 180, 6: 90, 8: 270}[o]
        subprocess.run(["/usr/bin/sips", "-r", str(deg), dst], capture_output=True)
    set_exif_orientation(dst, 1)

    w, h = image_size(dst)
    if w <= 0 or h <= 0:
        return False, 0, 0
    if w <= h:
        return True, w, h

    # Letterbox to the frame's native size instead of distorting.
    subprocess.run(["/usr/bin/sips", "-p", str(H), str(W),
                    "--padColor", "000000", dst], capture_output=True)
    set_exif_orientation(dst, 1)
    return True, w, h


def ensure_esp():
    code, st = curl_json("/status", timeout=10)
    if code != "200":
        log("ESP32 unreachable at %s (code=%s)" % (ESP, code))
        return None
    if not st.get("sd", {}).get("ready"):
        log("TF card not mounted on the bridge, trying /mount ...")
        code, m = curl_json("/mount", method="POST", timeout=30)
        log("  /mount -> %s" % m)
        code, st = curl_json("/status", timeout=10)
    return st


def load_state():
    if os.path.isfile(STATE):
        try:
            with open(STATE, encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            pass
    return {"uploaded": {}, "next_index": 1}


def save_state(st):
    tmp = STATE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, ensure_ascii=False, indent=1)
    os.replace(tmp, STATE)


def card_files():
    code, d = curl_json("/files", timeout=30)
    if code != "200":
        return []
    return d.get("files", [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=os.environ.get("PHOTO_BRIDGE_CONFIG", ""))
    ap.add_argument("--device", default="", help="override device_url")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--list-people", action="store_true")
    ap.add_argument("--force", action="store_true", help="ignore the de-dupe state")
    args = ap.parse_args()

    cfg_path = args.config
    if not cfg_path:
        here = os.path.dirname(os.path.abspath(__file__))
        candidate = os.path.join(here, "config.json")
        cfg_path = candidate if os.path.isfile(candidate) else ""
    load_config(cfg_path)

    if args.device:
        global ESP
        ESP = args.device.rstrip("/")

    if not os.path.isdir(LIB):
        log("Photos library not found: %s" % LIB)
        return 2
    if not os.path.isfile(DB):
        log("Photos database not found: %s" % DB)
        return 2

    if args.list_people:
        list_people()
        return 0

    st = ensure_esp()
    if st is None:
        return 1
    log("bridge ok: sd_ready=%s total=%sMB photos=%s"
        % (st["sd"]["ready"], st["sd"]["total_mb"], st["sd"]["photo_count"]))

    state = load_state()
    uploaded = state.setdefault("uploaded", {})
    existing = set(f["name"] for f in card_files())

    people = CFG.get("people") or {}
    groups = list(people.items()) if people else [("all", None)]

    jobs = []
    seen_assets = set()
    for name, pk in groups:
        assets = fetch_assets(pk, int(CFG["max_per_person"]), int(CFG["max_days"]))
        if len(assets) < 3:
            # People with no recent shots: fall back to their newest ever.
            more = fetch_assets(pk, int(CFG["max_per_person"]), 0)
            have = set(a["uuid"] for a in assets)
            assets += [a for a in more if a["uuid"] not in have]
        new = []
        for a in assets:
            if a["uuid"] in seen_assets:
                continue
            if not args.force and a["uuid"] in uploaded:
                continue
            seen_assets.add(a["uuid"])
            new.append(a)
        log("%s: %d candidates, %d new" % (name, len(assets), len(new)))
        for a in new:
            jobs.append((name, a))

    if not jobs:
        log("nothing new to upload")
        return 0

    jobs.sort(key=lambda x: x[1]["date"] or 0)  # oldest first

    if args.dry_run:
        for name, a in jobs:
            d = datetime.fromtimestamp((a["date"] or 0) + 978307200)
            print("  %-8s %s  %dx%d  %s" % (name, d.strftime("%Y-%m-%d"),
                                            a["w"], a["h"], a["filename"]))
        return 0

    tmp = tempfile.mkdtemp(prefix="photosync-")
    began = False
    try:
        idx = int(state.get("next_index", 1))
        prepared = []
        for name, a in jobs:
            dst = os.path.join(tmp, "%07d.jpg" % idx)
            ok, w, h = convert(a["src"], dst)
            if not ok:
                log("  convert failed: %s" % a["filename"])
                continue
            if w <= h:
                log("  skip portrait result %dx%d: %s" % (w, h, a["filename"]))
                continue
            out_name = "IMG%04d.JPG" % idx
            prepared.append((name, a, dst, out_name))
            idx += 1

        if not prepared:
            log("no convertible photos")
            return 0

        log("uploading %d photos (batch mode)..." % len(prepared))
        curl_json("/begin", method="POST", timeout=30)
        began = True

        ok = 0
        for name, a, dst, out_name in prepared:
            if out_name in existing:
                out_name = "IMG%04d.JPG" % idx
                idx += 1
            staged = os.path.join(tmp, out_name)
            shutil.copyfile(dst, staged)
            code, resp = curl_json("/upload", method="POST", timeout=120, files=[staged])
            if code == "200":
                uploaded[a["uuid"]] = {"name": out_name, "person": name,
                                       "date": a["date"], "file": a["filename"]}
                ok += 1
                log("  ok %s <- %s %s" % (out_name, name, a["filename"]))
            else:
                log("  FAIL %s (%s) code=%s %s" % (out_name, name, code, resp))

        state["next_index"] = idx
        save_state(state)
        log("uploaded %d/%d" % (ok, len(prepared)))

    finally:
        if began:
            code, _ = curl_json("/end", method="POST", timeout=300)
            log("batch closed (/end -> %s)" % code)
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- prune oldest beyond keep_on_card ----
    keep = int(CFG["keep_on_card"])
    if keep > 0:
        files = card_files()
        names = sorted([f["name"] for f in files if f["name"].upper().endswith(".JPG")])
        if len(names) > keep:
            drop = names[: len(names) - keep]
            log("pruning %d oldest photos (keep %d)" % (len(drop), keep))
            for n in drop:
                curl_json("/delete?name=" + quote(n), method="POST", timeout=60)
            time.sleep(4)  # let the debounced remount fire once

    return 0


if __name__ == "__main__":
    sys.exit(main())
