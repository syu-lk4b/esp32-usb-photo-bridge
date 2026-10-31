# ESP32 USB Photo Bridge

**Turn an old "USB stick only" digital photo frame into a self-updating Wi-Fi frame.**

English · [简体中文](README.md)

---

The frame thinks it is plugged into an ordinary USB flash drive. In reality that
"flash drive" is an ESP32-S3 on your Wi-Fi: it owns the SD card, receives photos
over HTTP, and **physically disconnects and reconnects the USB bus** after every
batch so a 2008 photo frame rescans and plays the new pictures.

```
┌──────────────┐   Wi-Fi / HTTP    ┌──────────────┐   USB MSC    ┌──────────────┐
│  Mac         │ ────────────────▶ │  ESP32-S3    │ ───────────▶ │  Photo frame │
│  Photos.app  │   /begin /upload  │  8 GB TF card│  read-only   │  8FF3CDW     │
│  faces + cron│   /end            │  FAT32       │  USB drive   │  800×600     │
└──────────────┘                   └──────────────┘              └──────────────┘
```

No cloud, no app, no subscription. Photos only travel inside your own LAN.

---

## Contents

- [The problem](#the-problem)
- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Manual install](#manual-install)
- [HTTP API](#http-api)
- [Implementation notes](#implementation-notes)
- [Privacy and security](#privacy-and-security)
- [Troubleshooting](#troubleshooting)
- [Compatibility](#compatibility)
- [License](#license)

---

## The problem

Old digital frames (e.g. the Philips 8FF3CDW) come with a set of painful limits:

- SD card / USB stick only — **copy files by hand**;
- while a USB stick is inserted it sits in "transfer mode" and **only starts
  playing after you unplug it**;
- **FAT32 + MBR only**, no exFAT or GPT;
- **Baseline JPEG only** — progressive JPEG simply does not display;
- native **800×600**; anything else is stretched or cropped;
- **ignores EXIF orientation**, so portrait shots show up sideways or upside down;
- no long filenames — 8.3 names such as `IMG0001.JPG` are safest;
- a writable volume invites the host OS to drop `.Trashes`, `.Spotlight-V100`
  and AppleDouble files onto the card, which eventually corrupts FAT.

Meanwhile every family photo lives on an iPhone, and nobody wants to copy them
by hand every week.

This project automates the whole chain: **pick photos by face from Apple Photos
→ convert them to what the frame understands → upload over Wi-Fi → the frame
refreshes itself.**

---

## How it works

One sync run:

1. **Mac side** (`tools/photo_frame_sync.py`)
   - opens `Photos.sqlite` read-only and selects recent photos per person
     (`ZPERSON`);
   - drops videos, hidden/trashed assets and **portrait** shots;
   - converts with `sips` to 800×600 Baseline JPEG and **bakes EXIF orientation
     into the pixels**;
   - renames to 8.3 names like `IMG0001.JPG`;
   - calls the ESP32 HTTP API over Wi-Fi.

2. **ESP32 side** (`src/main.cpp`)
   - `POST /begin` — detach USB (the frame sees the drive disappear);
   - `POST /upload` × N — write JPEGs to the TF card;
   - `POST /end` — flush dirty FAT → remount → reattach USB (the frame sees the
     drive appear again);
   - the frame rescans and starts playing.

3. **Why a real detach is required**
   Old frames do not rescan a volume just because files changed. Calling
   `mediaPresent(false)` is not enough: `tud_disconnect()` must drop the USB D+
   pull-up so the host sees an electrical unplug.

---

## Hardware

| Part | Requirement |
|---|---|
| MCU | **ESP32-S3** with native USB-OTG (PSRAM recommended) |
| Flash | 16 MB (default partition table); 8 MB needs a custom `partitions_ota.csv` |
| Storage | microSD / TF card, **FAT32 + MBR**, 8 GB tested |
| Wiring | 4-bit SDMMC (default CLK=38, CMD=40, D0=39, D1=41, D2=48, D3=47) |
| Power | the frame's USB-A port usually supplies 500 mA; the ESP32-S3 peaks around 350 mA |
| Cable | the ESP32's **native USB** port (GPIO19/20) to the frame's USB-A port, not the UART port |

> Many ESP32-S3 boards expose two USB-C ports: one is a UART bridge, the other
> is the native USB. **Use the native one**, otherwise `USBMSC` never appears.

---

## Quick start

```bash
git clone <your-repo-url> esp32-usb-photo-bridge
cd esp32-usb-photo-bridge
./setup.sh
```

`setup.sh` walks through:

1. dependency checks (PlatformIO, Python 3, Xcode CLT, curl/sips);
2. Wi-Fi / static IP / web password, writing `include/secrets.h` (mode 600);
3. build and flash the firmware;
4. wait for the device and read `/status`;
5. build a signed launcher so macOS can grant Full Disk Access;
6. configure the Photos library and the people to sync;
7. install the weekly launchd job;
8. dry-run, then the first real sync.

Host side only (device already flashed):

```bash
./setup.sh --skip-flash
```

Without the scheduled job:

```bash
./setup.sh --skip-launchd
```

---

## Manual install

### 1. Firmware

```bash
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h        # Wi-Fi, optional web password / static IP
pio run                          # build
pio run -t upload                # flash (board connected to the Mac)
```

### 2. First check

```bash
curl http://photo-frame.local/status
# or
curl http://<device-ip>/status
```

Example response:

```json
{
  "fw": "1.0.0",
  "ip": "192.168.1.50",
  "host": "photo-frame.local",
  "sd": { "ready": true, "usb_mounted": true, "readonly": true,
          "total_mb": 7674, "used_mb": 4, "photo_count": 39 }
}
```

Open `http://photo-frame.local/` for the manual upload / delete / clean UI.

### 3. Mac configuration

```bash
cp tools/config.example.json tools/config.json
chmod 600 tools/config.json
$EDITOR tools/config.json
```

Minimal config:

```json
{
  "device_url": "http://photo-frame.local",
  "photos_library": "~/Pictures/Photos Library.photoslibrary",
  "people": { "Alice": 92, "Bob": 481 }
}
```

The numbers are `ZPERSON.Z_PK` values from the Photos database:

```bash
python3 tools/photo_frame_sync.py --list-people
```

Leave `people` empty to sync the newest photos from the whole library.

### 4. Try it

```bash
python3 tools/photo_frame_sync.py --dry-run   # list what would be uploaded
python3 tools/photo_frame_sync.py             # upload for real
```

> Running Python directly requires **Terminal** to have Full Disk Access.
> For the scheduled job use the signed launcher built by `setup.sh`
> (see [Privacy and security](#privacy-and-security)).

### 5. Schedule

`setup.sh` installs `~/Library/LaunchAgents/com.esp32photobridge.sync.plist`,
running **every Sunday at 10:00** and once at login.

Change the schedule:

```bash
SYNC_WEEKDAY=3 SYNC_HOUR=3 SYNC_MINUTE=30 ./setup.sh --skip-flash
# 3 = Wednesday, 0 = Sunday
```

---

## HTTP API

All endpoints live under `http://<device>/`. When a web password is configured,
use HTTP Basic Auth.

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | browser UI |
| `GET` | `/status` | device status (IP, card, photo count) |
| `GET` | `/files` | file list (JSON) |
| `GET` | `/view?name=IMG0001.JPG` | preview one photo |
| `POST` | `/begin` | enter batch mode: detach USB |
| `POST` | `/upload` | upload one file (`multipart/form-data`, field `file`) |
| `POST` | `/end` | flush, remount, reattach USB |
| `POST` | `/delete?name=...` | delete a file (debounced remount after 2.5 s) |
| `POST` | `/remount` | force a remount (debug) |
| `POST` | `/clean` | remove `.Trashes` and friends (chunked, call repeatedly) |
| `POST` | `/mount` | retry mounting the card |
| `POST` | `/usb-rw?on=1` | **temporarily** allow host writes (for formatting) |
| `POST` | `/format` | always fails: on-device format is disabled |
| `POST` | `/ota` | upload `firmware.bin` (OTA) |

OTA example:

```bash
pio run
curl -u admin:****** -F "file=@.pio/build/esp32s3/firmware.bin" \
     http://photo-frame.local/ota
```

---

## Implementation notes

### 1. Making the ESP32 look like a USB drive

Arduino-ESP32's `USBMSC` + TinyUSB:

```cpp
msc.vendorID("ESP32S3");
msc.productID("PhotoFrameDisk");
msc.onRead(onUsbRead);
msc.onWrite(onUsbWrite);
USB.begin();          // must happen BEFORE mounting SD
msc.begin(sectorCount, sectorSize);
```

`onUsbRead` bypasses FatFs and calls `sdmmc_read_sectors()` on the raw card, so
the host sees the real block device rather than a filesystem emulation.

Crucially, **USB starts before the SD card is mounted**. If the card is corrupt
and `SD_MMC.begin()` blocks, a USB-first order still leaves you with a working
Wi-Fi management interface (`/mount`, `/status`, `/ota`) to recover the device.
The opposite order can brick it: no USB, no Wi-Fi, no way in.

### 2. Read-only mode: why the host may not write

In read-only mode `onUsbWrite` **pretends the write succeeded** and drops it:

```cpp
if (usbReadOnly) return (int32_t)bufsize;
```

The reason is practical. As soon as macOS mounts a writable volume it writes
`.Trashes/`, `.Spotlight-V100/` and `._*` AppleDouble files. Those writes go
through the host's filesystem cache while FatFs caches the FAT on the ESP32.
The two caches diverge, and the card ends up with a FAT that the frame cannot
read. Normal operation therefore keeps the card **exclusively writable by the
firmware**.

To format or repair the card, temporarily enable host writes:

```bash
curl -X POST "http://photo-frame.local/usb-rw?on=1"
# format as FAT32 / MBR with Disk Utility
curl -X POST "http://photo-frame.local/usb-rw?on=0"
```

### 3. Batch mode: one replug per batch

Toggling USB once per photo has two bad consequences:

1. the frame flashes "USB connected / disconnected" continuously;
2. macOS sees too many rapid re-enumerations and **disables the whole port**
   (the `USB 2.0 BILLBOARD` device disappears) until you move the cable or
   replug the Mac side.

So uploads are batched:

```
/begin  → detach once
/upload → write N files (USB stays detached)
/end    → flush + mount + attach once
```

Deletes are debounced too: `scheduleRemount(2500)` collapses several deletes
into one remount, with a hard 3-second minimum between bus toggles.

### 4. Flushing dirty FAT and remounting

After writes, FatFs may still hold FAT and directory entries in RAM. `/end`
does:

```cpp
SD_MMC.end();      // unmount, forcing every dirty sector to the card
delay(60);
mountSd();         // remount, obtaining a fresh sdmmc_card_t
usbAttach();       // re-enumerate
```

The order matters: **flush, then mount, then attach**. Attaching first would let
the frame read a half-written FAT.

### 5. The photo pipeline

**Landscape only.** Filtered in SQL:

```sql
AND a.ZWIDTH > a.ZHEIGHT
```

Portrait shots look bad on these frames, so they are skipped. The converter
also re-checks the rendered dimensions afterwards, because HEIC EXIF rotation
can turn a "landscape" asset into a portrait one.

**EXIF orientation must be baked into pixels.** This is the deepest trap:

```
source orientation = 3      (180°)
sips output        = 3      (tag copied, pixels untouched)
frame ignores EXIF → shows raw pixels → upside down
```

The fix:

1. read the JPEG's EXIF Orientation;
2. **set the tag to 1 first**, otherwise `sips` rotates twice;
3. rotate the pixels with `sips -r 180/90/270`;
4. force orientation back to 1.

**Exactly 800×600.** `sips --resampleHeightWidthMax 800` scales the long edge,
then `sips -p 600 800 --padColor 000000` letterboxes with black bars. Nothing is
cropped or stretched; a 16:9 photo gets bars top and bottom but keeps its aspect.

**8.3 filenames.** The firmware sanitises every upload to `IMG0001.JPG`:
7 characters plus `.JPG`, with an automatic suffix on collisions.

### 6. De-duplication, pruning and state

The Mac keeps uploaded Photos asset UUIDs in
`~/.local/state/esp32-photo-bridge/state.json`:

```json
{
  "uploaded": {
    "8A4538B4-...": { "name": "IMG0033.JPG", "person": "Alice", "date": 760000000 }
  },
  "next_index": 34
}
```

- one photo is uploaded once, even if it contains two tracked people;
- when the card holds more than `keep_on_card` (default 90) files, the oldest
  names are deleted;
- `--force` ignores the de-dupe state and re-uploads (creating duplicates).

### 7. Failure recovery

- **Watchdog friendly**: `cleanTree()` deletes at most 100 entries per HTTP
  request and returns `done:false`, so the UI can call it repeatedly. A single
  large delete used to trip the task watchdog.
- **On-device formatting is disabled**: `f_mkfs()` on an 8 GB card blocks for
  minutes, trips the watchdog, and can leave the FAT half-written; the next
  `SD_MMC.begin()` then deadlocks. Format from a computer instead.
- **`/end` always runs**: the Python script uses `try/finally`, so the USB bus is
  reattached even if an upload raises mid-batch.

---

## Privacy and security

There is **no cloud**:

- no third-party uploads;
- no telemetry, no licence checks;
- photos go from your Mac to your ESP32 over your LAN, then to the frame over USB.

The repository contains **no personal data**:

- `include/secrets.h` (Wi-Fi and web passwords) is git-ignored;
- `tools/config.json` (library path, person IDs) is git-ignored;
- runtime state and logs are git-ignored;
- no photos, names, Apple IDs or serial numbers are committed.

Files written by `setup.sh` are mode `600`.

**Recommendations:**

1. set `WEB_PASS` in `secrets.h` to put Basic Auth in front of the UI and API;
2. **never port-forward port 80** — this service is LAN-only by design;
3. turn `/usb-rw?on=1` back off after formatting;
4. put IoT devices on a separate VLAN if you share the network.

**About macOS Full Disk Access:**

`Photos.sqlite` is TCC-protected, and TCC identifies the **binary**, not
`python3`. Adding `/usr/bin/python3` to Full Disk Access usually does not stick.

The workaround is the small C launcher in `tools/photosync_launcher.c`: it
`execv`s into python3, and TCC records the grant against the signed launcher.
`setup.sh` builds and ad-hoc signs it, then asks you to approve it in System
Settings.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Mac never sees a USB drive | UART port used, or TinyUSB mode off | use the native USB port; check `ARDUINO_USB_MODE=0` |
| `/status` unreachable | mDNS not resolving | use the IP: `curl http://<ip>/status` |
| Frame shows no new photos | no real re-enumeration | check `/end` returned ok; `POST /remount` |
| Photos upside down | EXIF not baked into pixels | use this repo's `convert()` |
| Photos stretched | not 800×600 | check `sips`; look for `skip portrait` in the log |
| Upload hangs | batch never closed, USB left detached | `curl -X POST .../end` |
| Card fails to mount | not FAT32/MBR, or FAT corrupted | `/usb-rw?on=1`, reformat on a computer |
| macOS disables the USB port | too many rapid enumerations | move to another port; this repo debounces already |
| Scheduled job cannot read Photos | launcher lacks FDA | System Settings → Privacy & Security → Full Disk Access |
| OTA fails | partition table has no OTA slots | use the bundled `partitions_ota.csv` |

Logs:

```
~/.local/state/esp32-photo-bridge/sync.log
~/.local/state/esp32-photo-bridge/launchd.out
~/.local/state/esp32-photo-bridge/launchd.err
```

---

## Compatibility

| Item | Status |
|---|---|
| Philips 8FF3CDW / 93 | ✅ tested |
| FAT32 + MBR, 8 GB TF card | ✅ tested |
| Other "mount and read" USB frames | ⚠️ should work if they accept Baseline JPEG at 800×600 |
| exFAT / GPT | ❌ unsupported |
| Progressive JPEG | ❌ unsupported (the script emits Baseline) |
| Portrait photos | ❌ skipped on purpose |

> The frame's USB port must be a **host** (able to read a USB stick), not a
> charge-only port.

---

## License

Apache License 2.0. See [LICENSE](LICENSE).
