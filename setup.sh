#!/usr/bin/env bash
#
# ESP32 USB Photo Bridge - interactive installer
#
#   ./setup.sh                 full install
#   ./setup.sh --skip-flash    configure host side only (device already flashed)
#   ./setup.sh --skip-launchd  do not install the weekly job
#
# Tested on macOS 13+ with PlatformIO and the Command Line Tools installed.
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

LABEL="com.esp32photobridge.sync"
LAUNCHER="$HOME/Applications/esp32-photo-bridge-sync"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
CONFIG="$REPO_DIR/tools/config.json"
SECRETS="$REPO_DIR/include/secrets.h"
STATE_DIR="$HOME/.local/state/esp32-photo-bridge"

SYNC_WEEKDAY="${SYNC_WEEKDAY:-0}"   # 0 = Sunday
SYNC_HOUR="${SYNC_HOUR:-10}"
SYNC_MINUTE="${SYNC_MINUTE:-0}"

SKIP_FLASH=0
SKIP_LAUNCHD=0
for arg in "$@"; do
  case "$arg" in
    --skip-flash)   SKIP_FLASH=1 ;;
    --skip-launchd) SKIP_LAUNCHD=1 ;;
    -h|--help)      sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ -t 1 ]; then
  B=$'\033[1m'; C=$'\033[36m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'
else
  B=; C=; G=; Y=; R=; N=
fi

TOTAL=8
STEP=0

step() {
  STEP=$((STEP + 1))
  printf "\n${B}${C}[%d/%d] %s${N}\n" "$STEP" "$TOTAL" "$1"
}
ok()   { printf "  ${G}✓${N} %s\n" "$1"; }
warn() { printf "  ${Y}!${N} %s\n" "$1"; }
die()  { printf "${R}✗ %s${N}\n" "$1" >&2; exit 1; }

ask() {
  local __var="$1" __prompt="$2" __def="${3-}" __ans=""
  if [ -n "$__def" ]; then
    read -r -p "  $__prompt [$__def]: " __ans || true
    __ans="${__ans:-$__def}"
  else
    read -r -p "  $__prompt: " __ans || true
  fi
  printf -v "$__var" '%s' "$__ans"
}

ask_secret() {
  local __var="$1" __prompt="$2" __ans=""
  read -r -s -p "  $__prompt: " __ans || true
  echo
  printf -v "$__var" '%s' "$__ans"
}

confirm() {
  local __prompt="$1" __def="${2:-Y}" __ans=""
  read -r -p "  $__prompt [$__def]: " __ans || true
  __ans="${__ans:-$__def}"
  case "$__ans" in [yY]*) return 0 ;; *) return 1 ;; esac
}

get_define() {
  # read a quoted #define from secrets.h
  sed -n "s/^#define[[:space:]][[:space:]]*$1[[:space:]][[:space:]]*\"\\(.*\\)\"/\\1/p" "$SECRETS" 2>/dev/null | head -1
}

c_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

printf "${B}ESP32 USB Photo Bridge - installer${N}\n"
printf "repo: %s\n" "$REPO_DIR"

# ---------------------------------------------------------------------------
step "Checking prerequisites"
# ---------------------------------------------------------------------------
[ "$(uname -s)" = "Darwin" ] || die "This installer targets macOS (the sync side needs Photos + sips)."
ok "macOS $(sw_vers -productVersion)"

command -v pio >/dev/null 2>&1 || die "PlatformIO not found. Install it with: brew install platformio"
ok "PlatformIO $(pio --version | head -1 | sed 's/PlatformIO Core, version //')"

command -v xcode-select >/dev/null 2>&1 && xcode-select -p >/dev/null 2>&1 \
  || die "Xcode Command Line Tools missing. Run: xcode-select --install"
ok "Command Line Tools"

[ -x /usr/bin/python3 ] || die "/usr/bin/python3 not found"
/usr/bin/python3 -c 'import sqlite3, json, struct' || die "python3 is missing required stdlib modules"
ok "python3 $(/usr/bin/python3 -V 2>&1 | awk '{print $2}')"

for bin in curl sips codesign plutil cc; do
  command -v "$bin" >/dev/null 2>&1 || die "$bin not found"
done
ok "curl / sips / codesign / plutil / cc"

mkdir -m 700 -p "$STATE_DIR"

# ---------------------------------------------------------------------------
step "Device configuration (include/secrets.h)"
# ---------------------------------------------------------------------------
if [ -f "$SECRETS" ]; then
  warn "include/secrets.h already exists."
  if confirm "Reuse it and skip Wi-Fi questions?" "Y"; then
    WIFI_SSID="$(get_define WIFI_SSID)"
    WIFI_PASS="$(get_define WIFI_PASS)"
    WEB_USER="$(get_define WEB_USER)"; WEB_USER="${WEB_USER:-admin}"
    WEB_PASS="$(get_define WEB_PASS)"
    STATIC_IP="$(get_define STATIC_IP)"
    MDNS_HOST="$(get_define MDNS_HOST)"; MDNS_HOST="${MDNS_HOST:-photo-frame}"
    ok "reusing existing secrets.h"
    SKIP_SECRETS=1
  else
    SKIP_SECRETS=0
  fi
else
  SKIP_SECRETS=0
fi

if [ "$SKIP_SECRETS" -eq 0 ]; then
  ask WIFI_SSID "Wi-Fi SSID (2.4 GHz only)" "${WIFI_SSID:-}"
  [ -n "$WIFI_SSID" ] || die "SSID cannot be empty"
  ask_secret WIFI_PASS "Wi-Fi password"
  [ -n "$WIFI_PASS" ] || die "Wi-Fi password cannot be empty"

  STATIC_IP=""; STATIC_GW=""; STATIC_MASK=""
  if ! confirm "Use DHCP for the bridge?" "Y"; then
    ask STATIC_IP "Static IP (e.g. 192.168.1.50)"
    ask STATIC_GW "Gateway (e.g. 192.168.1.1)"
    ask STATIC_MASK "Subnet mask" "255.255.255.0"
  fi

  WEB_USER="admin"; WEB_PASS=""
  if confirm "Protect the web UI / API with a password?" "Y"; then
    WEB_PASS="$(openssl rand -hex 6)"
    ok "generated web password: $WEB_PASS"
    warn "write it down - it is also stored in tools/config.json"
  fi

  ask MDNS_HOST "mDNS hostname" "photo-frame"

  BACKUP=""
  if [ -f "$SECRETS" ]; then
    BACKUP="$SECRETS.bak.$(date +%Y%m%d-%H%M%S)"
    (umask 077 && cp "$SECRETS" "$BACKUP")
    chmod 600 "$BACKUP"
    warn "old secrets backed up to $BACKUP"
  fi

  (
    umask 077
    cat > "$SECRETS" <<EOF
#pragma once

// Generated by setup.sh - DO NOT COMMIT THIS FILE.

#define WIFI_SSID "$(c_escape "$WIFI_SSID")"
#define WIFI_PASS "$(c_escape "$WIFI_PASS")"

#define WEB_USER "$(c_escape "$WEB_USER")"
#define WEB_PASS "$(c_escape "$WEB_PASS")"

#define STATIC_IP   "$(c_escape "$STATIC_IP")"
#define STATIC_GW   "$(c_escape "$STATIC_GW")"
#define STATIC_MASK "$(c_escape "$STATIC_MASK")"

#define MDNS_HOST "$(c_escape "$MDNS_HOST")"
EOF
  )
  chmod 600 "$SECRETS"
  ok "wrote include/secrets.h (mode 600)"
fi

# ---------------------------------------------------------------------------
step "Build and flash the firmware"
# ---------------------------------------------------------------------------
if [ "$SKIP_FLASH" -eq 1 ]; then
  warn "skipped (--skip-flash)"
else
  echo "  Connect the ESP32-S3 to this Mac with a USB data cable."
  echo "  (The same USB port is used later for the photo frame.)"
  if confirm "Build and flash now?" "Y"; then
    pio run
    ok "firmware built"

    FLASH_FAILED=0
    if [ -n "${UPLOAD_PORT:-}" ]; then
      pio run -t upload --upload-port "$UPLOAD_PORT" || FLASH_FAILED=1
    else
      pio run -t upload || FLASH_FAILED=1
    fi

    if [ "$FLASH_FAILED" -eq 0 ]; then
      ok "firmware flashed"
    else
      warn "flash failed. Common causes:"
      echo "    - board not in download mode (hold BOOT, tap RESET)"
      echo "    - wrong USB port (use the USB-OTG port, not UART)"
      echo "    - set UPLOAD_PORT=/dev/cu.usbmodemXXXX and rerun"
      if ! confirm "Continue with host-side setup anyway?" "Y"; then
        exit 1
      fi
    fi
  else
    warn "skipped"
  fi
fi

# ---------------------------------------------------------------------------
step "Find the bridge on the network"
# ---------------------------------------------------------------------------
DEFAULT_URL="http://$MDNS_HOST.local"
if [ -n "$STATIC_IP" ]; then
  DEFAULT_URL="http://$STATIC_IP"
fi
DEVICE_URL="${DEVICE_URL:-$DEFAULT_URL}"
ask DEVICE_URL "Device URL" "$DEVICE_URL"
DEVICE_URL="${DEVICE_URL%/}"

# bash 3.2 (macOS default) errors on "${empty[@]}" under set -u, so use a helper
device_curl() {
  local url="$1"
  if [ -n "$WEB_PASS" ]; then
    printf 'user = "%s:%s"\n' "$(c_escape "$WEB_USER")" "$(c_escape "$WEB_PASS")" | curl -fsS -m 3 -K - "$url"
  else
    curl -fsS -m 3 "$url"
  fi
}

STATUS_JSON=""
for _ in $(seq 1 30); do
  if STATUS_JSON="$(device_curl "$DEVICE_URL/status" 2>/dev/null)"; then
    break
  fi
  STATUS_JSON=""
  printf "."
  sleep 2
done
echo
if [ -n "$STATUS_JSON" ]; then
  ok "device online: $DEVICE_URL"
  /usr/bin/python3 - "$STATUS_JSON" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
sd = d.get("sd", {})
print("    firmware : %s" % d.get("fw", "?"))
print("    ip       : %s" % d.get("ip", "?"))
print("    tf card  : %s MB total, %s photos, %s" % (
    sd.get("total_mb", "?"), sd.get("photo_count", "?"),
    "mounted" if sd.get("usb_mounted") else "detached"))
print("    readonly : %s" % sd.get("readonly"))
PY
else
  warn "could not reach $DEVICE_URL"
  warn "if you set a password, the URL must be reachable from this Mac"
  warn "check: curl -u $WEB_USER:*** $DEVICE_URL/status"
  if ! confirm "Continue anyway?" "Y"; then
    exit 1
  fi
fi

# ---------------------------------------------------------------------------
step "Install the Full Disk Access launcher"
# ---------------------------------------------------------------------------
echo "  macOS attributes Photos-library access to a binary, not to python3."
echo "  This step builds a tiny signed launcher and asks you to grant it FDA."

if [ -x "$LAUNCHER" ]; then
  warn "launcher already exists: $LAUNCHER"
  if confirm "Rebuild it? (you will have to re-grant Full Disk Access)" "N"; then
    REBUILD=1
  else
    REBUILD=0
  fi
else
  REBUILD=1
fi

if [ "$REBUILD" -eq 1 ]; then
  mkdir -p "$HOME/Applications"
  cc -O2 -DSCRIPT_PATH="\"$REPO_DIR/tools/photo_frame_sync.py\"" \
     -o "$LAUNCHER" "$REPO_DIR/tools/photosync_launcher.c"
  codesign --force --sign - "$LAUNCHER" >/dev/null 2>&1
  ok "built $LAUNCHER"
fi

echo
echo "  Opening System Settings -> Privacy & Security -> Full Disk Access."
echo "  Add (or drag in): $LAUNCHER"
echo "  If it is listed but not working, remove it and add it again."
open "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles" 2>/dev/null || true
read -r -p "  Press Enter once Full Disk Access is granted..." _ || true

# ---------------------------------------------------------------------------
step "Configure the Photos library and people"
# ---------------------------------------------------------------------------
DEFAULT_LIB="$HOME/Pictures/Photos Library.photoslibrary"
PHOTOS_LIB="${PHOTOS_LIB:-$DEFAULT_LIB}"
ask PHOTOS_LIB "Photos library path" "$PHOTOS_LIB"
PHOTOS_LIB="${PHOTOS_LIB/#\~/$HOME}"

[ -f "$PHOTOS_LIB/database/Photos.sqlite" ] || warn "Photos.sqlite not found under $PHOTOS_LIB"

WEB_PASS_ENV="$WEB_PASS" /usr/bin/python3 - "$CONFIG" "$DEVICE_URL" "$WEB_USER" "$PHOTOS_LIB" <<'PY'
import json, os, sys
path, url, user, lib = sys.argv[1:5]
pwd = os.environ.get("WEB_PASS_ENV", "")
cfg = {
    "device_url": url,
    "web_user": user,
    "web_password": pwd,
    "photos_library": lib,
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
if os.path.isfile(path):
    with open(path) as f:
        old = json.load(f)
    old.update({k: v for k, v in cfg.items() if k not in ("people",) or not old.get("people")})
    cfg = old
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "w") as f:
    json.dump(cfg, f, ensure_ascii=False, indent=2)
os.chmod(path, 0o600)
print("  wrote %s (mode 600)" % path)
PY

echo
echo "  Reading the Photos database (this needs Full Disk Access) ..."
if "$LAUNCHER" --list-people 2>&1 | head -25; then
  :
else
  warn "could not read the Photos database - check Full Disk Access"
fi

echo
echo "  Enter the people to sync as  Name=Z_PK , separated by commas."
echo "  Example:  Alice=92,Bob=481"
echo "  Leave empty to sync the newest photos from the whole library."
ask PEOPLE_RAW "People" ""

/usr/bin/python3 - "$CONFIG" "$PEOPLE_RAW" <<'PY'
import json, sys
path, raw = sys.argv[1], sys.argv[2]
cfg = json.load(open(path))
people = {}
for part in raw.split(","):
    part = part.strip()
    if not part or "=" not in part:
        continue
    name, pk = part.split("=", 1)
    name, pk = name.strip(), pk.strip()
    if name and pk.isdigit():
        people[name] = int(pk)
if people:
    cfg["people"] = people
    json.dump(cfg, open(path, "w"), ensure_ascii=False, indent=2)
print("  people: %s" % json.dumps(cfg["people"], ensure_ascii=False))
PY

# ---------------------------------------------------------------------------
step "Install the weekly launchd job"
# ---------------------------------------------------------------------------
if [ "$SKIP_LAUNCHD" -eq 1 ]; then
  warn "skipped (--skip-launchd)"
else
  mkdir -p "$HOME/Library/LaunchAgents" "$STATE_DIR"
  cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$LAUNCHER</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$REPO_DIR</string>
    <key>StartCalendarInterval</key>
    <dict>
        <key>Weekday</key>
        <integer>$SYNC_WEEKDAY</integer>
        <key>Hour</key>
        <integer>$SYNC_HOUR</integer>
        <key>Minute</key>
        <integer>$SYNC_MINUTE</integer>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>ProcessType</key>
    <string>Background</string>
    <key>StandardOutPath</key>
    <string>$STATE_DIR/launchd.out</string>
    <key>StandardErrorPath</key>
    <string>$STATE_DIR/launchd.err</string>
</dict>
</plist>
EOF
  plutil -lint "$PLIST" >/dev/null || die "generated plist is invalid"
  ok "wrote $PLIST"

  launchctl unload "$PLIST" >/dev/null 2>&1 || true
  launchctl load "$PLIST"
  ok "loaded (weekly: weekday $SYNC_WEEKDAY $SYNC_HOUR:$SYNC_MINUTE, plus at login)"
fi

# ---------------------------------------------------------------------------
step "First run"
# ---------------------------------------------------------------------------
echo "  Dry run: what would be uploaded?"
if "$LAUNCHER" --dry-run; then
  ok "dry run finished"
else
  warn "dry run failed - check the log at $STATE_DIR/sync.log"
fi

if confirm "Upload a first batch now?" "Y"; then
  "$LAUNCHER" || warn "sync failed - see $STATE_DIR/sync.log"
fi

printf "\n${B}${G}Done.${N}\n"
printf "  device UI : %s/\n" "$DEVICE_URL"
printf "  config    : %s\n" "$CONFIG"
printf "  log       : %s/sync.log\n" "$STATE_DIR"
printf "  schedule  : %s\n" "$PLIST"
printf "\n  Manual run: %s\n" "$LAUNCHER"
printf "  Dry run   : %s --dry-run\n\n" "$LAUNCHER"
