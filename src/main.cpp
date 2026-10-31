/*
 * ESP32 USB Photo Bridge
 * ----------------------
 * Turns an old "USB stick only" digital photo frame into a Wi-Fi frame.
 *
 * The frame sees a normal USB mass-storage device.  Behind that facade the
 * ESP32-S3 owns the SD card, accepts photos over HTTP, and re-enumerates the
 * USB bus once per batch so the frame notices the new files.
 *
 * Hardware target: ESP32-S3 (16 MB flash, PSRAM), SD card wired in 4-bit SDMMC
 * mode.  Tested with a Philips 8FF3CDW / 93 frame and an 8 GB FAT32 card.
 *
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <time.h>
#include <SD_MMC.h>
#include "driver/sdmmc_types.h"
#include "sdmmc_cmd.h"
#include "USB.h"
#include "USBMSC.h"
#include "secrets.h"

// TinyUSB raw control (declared manually to avoid header name clashes)
extern "C" bool tud_disconnect(void);
extern "C" bool tud_connect(void);

#define FW_VERSION "1.0.0"

// ---------- SD pins (override with -DPIN_SD_* in platformio.ini) ----------
#ifndef PIN_SD_CLK
#define PIN_SD_CLK 38
#endif
#ifndef PIN_SD_CMD
#define PIN_SD_CMD 40
#endif
#ifndef PIN_SD_D0
#define PIN_SD_D0 39
#endif
#ifndef PIN_SD_D1
#define PIN_SD_D1 41
#endif
#ifndef PIN_SD_D2
#define PIN_SD_D2 48
#endif
#ifndef PIN_SD_D3
#define PIN_SD_D3 47
#endif

#ifndef MDNS_HOST
#define MDNS_HOST "photo-frame"
#endif

// ---------- Global state ----------
struct SDCardAccessor : public fs::SDMMCFS {
  static sdmmc_card_t* getCard(fs::SDMMCFS &fs) { return static_cast<SDCardAccessor&>(fs)._card; }
};

static USBMSC msc;
static sdmmc_card_t* sdCard = nullptr;
static bool sdReady = false;
static bool usbStarted = false;
static volatile bool usbMscMounted = false;
static volatile bool uploadingActive = false;
static volatile bool batchActive = false;
static volatile int uploadRejectCode = 0;   // 0 = accepted, 401 = auth, 503 = card
static volatile bool otaRejected = false;
static volatile uint32_t lastUploadActivityMs = 0;
static uint64_t sdTotalMB = 0;
static uint64_t sdUsedMB = 0;
static int sdPhotoCount = 0;
static SemaphoreHandle_t sdMutex = nullptr;
static volatile uint32_t remountAtMs = 0;
static volatile uint32_t lastUsbToggleMs = 0;
static volatile bool remountPending = false;

// Read-only USB flag: we never let the host OS write to the card.  macOS drops
// .Trashes and .Spotlight-V100 onto any writable volume, which desynchronises
// FatFs' cached FAT and eventually makes the frame stop seeing new files.
static volatile bool usbReadOnly = true;

// Debounced remount: deleting 30 files must not toggle the USB bus 30 times.
// macOS disables a port after too many rapid re-enumerations, and the frame
// would lose the drive entirely.  Collapse them into one detach/flush/attach.
static void scheduleRemount(uint32_t delayMs = 2500) {
  remountAtMs = millis() + delayMs;
  remountPending = true;
}

// ---------- SD helpers ----------
static bool mountSd() {
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
  if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 16)) return true;
  Serial.println("[SD] 4-bit mount failed, trying 1-bit...");
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 16)) return true;
  return false;
}

static bool isPhotoName(const String &n) {
  String l = n;
  l.toLowerCase();
  return l.endsWith(".jpg") || l.endsWith(".jpeg") || l.endsWith(".png") ||
         l.endsWith(".bmp") || l.endsWith(".gif");
}

static void closeQuietly(File &f) { if (f) f.close(); }

static void refreshSdStats() {
  if (!sdReady) return;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(4000)) != pdTRUE) return;
  sdTotalMB = SD_MMC.totalBytes() / (1024ULL * 1024ULL);
  sdUsedMB  = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
  int cnt = 0;
  File root = SD_MMC.open("/");
  if (root && root.isDirectory()) {
    while (true) {
      File f = root.openNextFile();
      if (!f) break;
      String fn = String(f.name());
      bool dir = f.isDirectory();
      closeQuietly(f);
      if (dir) continue;
      if (fn.startsWith(".")) continue;
      if (isPhotoName(fn)) cnt++;
    }
    closeQuietly(root);
  }
  sdPhotoCount = cnt;
  xSemaphoreGive(sdMutex);
}

// Flush every dirty FAT sector + directory entry to the card, then remount.
// This is what makes new files actually visible to the frame.
static bool flushAndRemountSd() {
  if (!sdReady) return false;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(8000)) != pdTRUE) return false;
  SD_MMC.end();
  delay(60);
  bool ok = mountSd();
  sdCard = ok ? SDCardAccessor::getCard(SD_MMC) : nullptr;
  if (!ok || !sdCard) sdReady = false;
  xSemaphoreGive(sdMutex);
  Serial.printf("[SD] flush+remount -> %s\n", (ok && sdCard) ? "OK" : "FAILED");
  return ok && sdCard;
}

// ---------- USB MSC ----------
static int32_t onUsbRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!sdCard || !usbMscMounted) return -1;
  uint32_t secSize = sdCard->csd.sector_size ? sdCard->csd.sector_size : 512;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) != pdTRUE) return -1;
  esp_err_t err = ESP_OK;
  if (offset == 0 && (bufsize % secSize == 0)) {
    err = sdmmc_read_sectors(sdCard, buffer, lba, bufsize / secSize);
  } else {
    uint8_t tmp[512];
    err = sdmmc_read_sectors(sdCard, tmp, lba, 1);
    if (err == ESP_OK) memcpy(buffer, tmp + offset, bufsize);
  }
  xSemaphoreGive(sdMutex);
  return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static int32_t onUsbWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!sdCard || !usbMscMounted) return -1;
  // Read-only mode: pretend the write succeeded, then drop it.  The host stays
  // happy, the card stays clean, and FatFs' cache stays authoritative.
  if (usbReadOnly) return (int32_t)bufsize;
  uint32_t secSize = sdCard->csd.sector_size ? sdCard->csd.sector_size : 512;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) != pdTRUE) return -1;
  esp_err_t err = ESP_OK;
  if (offset == 0 && (bufsize % secSize == 0)) {
    err = sdmmc_write_sectors(sdCard, buffer, lba, bufsize / secSize);
  } else {
    uint8_t tmp[512];
    err = sdmmc_read_sectors(sdCard, tmp, lba, 1);
    if (err == ESP_OK) {
      memcpy(tmp + offset, buffer, bufsize);
      err = sdmmc_write_sectors(sdCard, tmp, lba, 1);
    }
  }
  xSemaphoreGive(sdMutex);
  return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static bool onUsbStartStop(uint8_t power_condition, bool start, bool load_eject) { return true; }

static void setMediaPresent(bool present) {
  usbMscMounted = present;
  msc.mediaPresent(present);
}

// Real physical detach: drops the USB D+ pull-up so the host sees an unplug.
// Without this, old hosts (like the Philips 8FF3CDW) never rescan the volume.
static void usbDetach() {
  setMediaPresent(false);
  if (usbStarted) { tud_disconnect(); }
  Serial.println("[USB] detached");
}

static void usbAttach() {
  if (usbStarted) { tud_connect(); }
  delay(120);
  setMediaPresent(true);
  Serial.println("[USB] attached");
}

static void initSdAndUsbMsc() {
  sdMutex = xSemaphoreCreateMutex();

  // Start USB FIRST so a Mac can always see the device, even if the card is
  // corrupt and SD_MMC.begin() blocks.  Otherwise a bad card bricks recovery.
  msc.vendorID("ESP32S3");
  msc.productID("PhotoFrameDisk");
  msc.productRevision("1.0");
  msc.onRead(onUsbRead);
  msc.onWrite(onUsbWrite);
  msc.onStartStop(onUsbStartStop);
  msc.mediaPresent(false);
  USB.begin();
  usbStarted = true;
  delay(120);

  sdReady = mountSd();
  if (!sdReady) {
    Serial.println("[SD] mount failed - USB stays alive, waiting for /mount");
    return;
  }
  sdCard = SDCardAccessor::getCard(SD_MMC);
  if (!sdCard) { sdReady = false; return; }
  refreshSdStats();

  uint32_t secSize = sdCard->csd.sector_size ? sdCard->csd.sector_size : 512;
  uint32_t secCount = sdCard->csd.capacity;
  Serial.printf("[SD] mounted %llu MB, sectors=%lu secSize=%lu\n",
                (unsigned long long)SD_MMC.cardSize() / (1024ULL * 1024ULL),
                (unsigned long)secCount, (unsigned long)secSize);

  msc.begin(secCount, secSize);
  msc.mediaPresent(true);
  usbMscMounted = true;
}

// ---------- Filename helpers (8.3 uppercase .JPG for old frames) ----------
static String make83Name(const String &raw) {
  String base = raw;
  int slash = base.lastIndexOf('/');
  if (slash >= 0) base = base.substring(slash + 1);
  int bslash = base.lastIndexOf('\\');
  if (bslash >= 0) base = base.substring(bslash + 1);
  int dot = base.lastIndexOf('.');
  if (dot > 0) base = base.substring(0, dot);
  String out;
  for (size_t i = 0; i < base.length() && out.length() < 7; i++) {
    char c = base[i];
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c == '_' || c == '-') out += '_';
  }
  if (out.length() == 0) out = "PHOTO";
  return out + ".JPG";
}

static String uniquePath(const String &candidate) {
  String base = candidate.substring(0, candidate.length() - 4);
  String path = "/" + candidate;
  int n = 1;
  while (SD_MMC.exists(path) && n < 1000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", n);
    String trimmed = base.substring(0, min((int)base.length(), 7 - (int)strlen(buf)));
    path = "/" + trimmed + buf + ".JPG";
    n++;
  }
  return path;
}

// Reject path traversal: all files live in the card's root directory.
static bool isSafeRootName(const String &n) {
  if (n.length() == 0 || n.length() > 96) return false;
  if (n.indexOf('/') >= 0 || n.indexOf('\\') >= 0) return false;
  if (n.indexOf("..") >= 0) return false;
  return true;
}

static String jsonEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) {
      char b[7];
      snprintf(b, sizeof(b), "\\u%04x", (uint8_t)c);
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}

// ---------- Web server ----------
static WebServer server(80);
static File uploadFile;
static String uploadTargetPath;

// Optional HTTP Basic auth.  Leave WEB_PASS empty in secrets.h to disable.
static bool webAuthOk() {
  if (strlen(WEB_PASS) == 0) return true;
  return server.authenticate(WEB_USER, WEB_PASS);
}

static bool requireAuth() {
  if (webAuthOk()) return true;
  server.requestAuthentication();
  return false;
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Photo Frame Bridge</title>
<style>
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:16px}
  .card{background:#1e293b;border-radius:12px;padding:16px;margin-bottom:16px;box-shadow:0 4px 12px rgba(0,0,0,.3)}
  h2{margin:0 0 12px;font-size:18px;color:#38bdf8}
  .stat{font-size:14px;color:#94a3b8;margin-bottom:12px;line-height:1.6}
  .btn{background:#0284c7;color:#fff;border:none;border-radius:8px;padding:10px 16px;font-size:14px;cursor:pointer;margin-right:8px;margin-bottom:8px}
  .btn.green{background:#10b981}.btn.gray{background:#475569}.btn.red{background:#ef4444;padding:6px 10px;font-size:12px}
  input[type=file]{margin-bottom:10px;display:block;color:#cbd5e1}
  label.opt{display:flex;align-items:flex-start;gap:8px;font-size:13px;color:#cbd5e1;margin-bottom:10px;cursor:pointer;line-height:1.4}
  table{width:100%;border-collapse:collapse;font-size:14px}
  th,td{text-align:left;padding:8px 4px;border-bottom:1px solid #334155}
  .prog{height:10px;background:#334155;border-radius:5px;overflow:hidden;margin:10px 0;display:none}
  .prog-bar{height:100%;background:#10b981;width:0%;transition:width .1s}
  .log{font-size:12px;color:#94a3b8;white-space:pre-wrap;max-height:140px;overflow:auto;margin-top:8px}
</style>
</head>
<body>
<div class="card">
  <h2>🖼️ ESP32 Photo Frame Bridge</h2>
  <div class="stat" id="sdStat">Loading…</div>
  <input type="file" id="fileInput" multiple accept="image/*">
  <label class="opt">
    <input type="checkbox" id="opt800" checked>
    <span>Optimize to 800×600 JPEG before upload (recommended for old frames)</span>
  </label>
  <button class="btn green" onclick="uploadFiles()">Upload / 上传</button>
  <button class="btn gray" onclick="cleanJunk()">Clean junk / 清理垃圾</button>
  <button class="btn" onclick="remountUsb()">Remount / 重新挂载</button>
  <div class="prog" id="progBox"><div class="prog-bar" id="progBar"></div></div>
  <div id="msg" style="font-size:13px;color:#34d399;margin-top:6px"></div>
  <div class="log" id="logBox"></div>
</div>
<div class="card">
  <h2>📂 Files on the card</h2>
  <table><thead><tr><th>Name</th><th>Size</th><th></th></tr></thead><tbody id="fileList"></tbody></table>
</div>
<script>
const logBox = document.getElementById('logBox');
function log(m){ logBox.innerText += m + "\n"; logBox.scrollTop = logBox.scrollHeight; }

async function loadFiles(){
  const r = await fetch('/files');
  const d = await r.json();
  document.getElementById('sdStat').innerText =
    (d.sd_ready
      ? `TF card: ${d.used_mb} MB / ${d.total_mb} MB · ${d.photo_count} photos · USB ${d.usb_mounted ? 'mounted' : 'detached'}`
      : '❌ TF card not ready');
  const tb = document.getElementById('fileList');
  tb.innerHTML = '';
  for(const f of d.files){
    const tr = document.createElement('tr');
    tr.innerHTML = `<td><a href="/view?name=${encodeURIComponent(f.name)}" target="_blank" style="color:#38bdf8">${f.name}</a></td><td>${(f.size/1024).toFixed(1)} KB</td><td><button class="btn red" onclick="delFile('${f.name}')">Delete</button></td>`;
    tb.appendChild(tr);
  }
}

function compressImage(file, maxW, maxH, quality){
  return new Promise((resolve)=>{
    const img = new Image();
    const url = URL.createObjectURL(file);
    img.onload = ()=>{
      URL.revokeObjectURL(url);
      let w = img.width, h = img.height;
      const ratio = Math.min(maxW / w, maxH / h);
      if (ratio < 1) { w = Math.round(w * ratio); h = Math.round(h * ratio); }
      const c = document.createElement('canvas');
      c.width = w; c.height = h;
      const ctx = c.getContext('2d');
      ctx.fillStyle = '#000';
      ctx.fillRect(0, 0, w, h);
      ctx.drawImage(img, 0, 0, w, h);
      c.toBlob((blob)=>{
        let base = file.name.replace(/\.[^.]+$/, '');
        resolve(new File([blob], base + '.jpg', {type:'image/jpeg'}));
      }, 'image/jpeg', quality);
    };
    img.onerror = ()=>{ URL.revokeObjectURL(url); resolve(file); };
    img.src = url;
  });
}

function postFile(file, url, onProg){
  return new Promise((resolve, reject)=>{
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.timeout = 60000;
    xhr.upload.onprogress = e => { if (e.lengthComputable) onProg(e.loaded / e.total); };
    xhr.onload = () => (xhr.status === 200 ? resolve(xhr.responseText) : reject(new Error('HTTP ' + xhr.status)));
    xhr.onerror = () => reject(new Error('network'));
    xhr.ontimeout = () => reject(new Error('timeout'));
    const fd = new FormData();
    fd.append('file', file, file.name);
    xhr.send(fd);
  });
}

async function uploadFiles(){
  const inp = document.getElementById('fileInput');
  if(!inp.files.length){ alert('Choose at least one photo'); return; }
  const opt800 = document.getElementById('opt800').checked;
  const box = document.getElementById('progBox');
  const bar = document.getElementById('progBar');
  const msg = document.getElementById('msg');
  logBox.innerText = '';
  const total = inp.files.length;

  box.style.display = 'block';
  bar.style.width = '0%';
  msg.innerText = 'Detaching USB and preparing…';

  try { await fetch('/begin', {method:'POST'}); } catch(e) {}
  let okCount = 0, failCount = 0;

  for(let i=0;i<total;i++){
    let file = inp.files[i];
    try {
      if(opt800) file = await compressImage(file, 800, 600, 0.92);
    } catch(e) { log('resize failed, using original: ' + file.name); }

    let done = false;
    for (let attempt = 1; attempt <= 3 && !done; attempt++) {
      try {
        msg.innerText = `Uploading (${i+1}/${total}): ${file.name} (${(file.size/1024).toFixed(0)} KB) try ${attempt}`;
        await postFile(file, '/upload', ratio => {
          bar.style.width = Math.round(((i + ratio) / total) * 100) + '%';
        });
        done = true; okCount++;
        log(`✅ ${file.name} (${(file.size/1024).toFixed(0)} KB)`);
      } catch(e) {
        log(`⚠️ try ${attempt} failed ${file.name}: ${e.message}`);
        await new Promise(r => setTimeout(r, 400));
      }
    }
    if (!done) { failCount++; log(`❌ giving up on ${file.name}`); }
    bar.style.width = Math.round(((i + 1) / total) * 100) + '%';
  }

  msg.innerText = 'Flushing and re-attaching USB…';
  try { await fetch('/end', {method:'POST'}); } catch(e) {}
  await new Promise(r => setTimeout(r, 1500));
  box.style.display = 'none';
  msg.innerText = `Done: ${okCount} ok, ${failCount} failed`;
  inp.value = '';
  loadFiles();
}

async function delFile(name){
  if(!confirm('Delete ' + name + '?')) return;
  await fetch('/delete?name=' + encodeURIComponent(name), {method:'POST'});
  loadFiles();
}

async function cleanJunk(){
  if(!confirm('Remove .Trashes / .Spotlight-V100 and similar junk?')) return;
  const msg = document.getElementById('msg');
  let total = 0, guard = 0;
  while (guard++ < 400) {
    msg.innerText = `Cleaning… ${total} MB freed`;
    let d;
    try { d = await (await fetch('/clean', {method:'POST'})).json(); }
    catch(e) { msg.innerText = 'clean interrupted: ' + e.message; break; }
    total += (d.freed_mb || 0);
    if (d.done) break;
  }
  msg.innerText = `Clean finished, ${total} MB freed`;
  loadFiles();
}

async function remountUsb(){
  document.getElementById('msg').innerText = 'Remounting…';
  await fetch('/remount', {method:'POST'});
  setTimeout(()=>{ document.getElementById('msg').innerText = '✅ Remounted'; loadFiles(); }, 1600);
}

loadFiles();
</script>
</body>
</html>
)rawliteral";

static void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleFiles() {
  if (!requireAuth()) return;
  refreshSdStats();
  String json = "{\"sd_ready\":" + String(sdReady ? "true" : "false");
  json += ",\"usb_mounted\":" + String(usbMscMounted ? "true" : "false");
  json += ",\"readonly\":" + String(usbReadOnly ? "true" : "false");
  json += ",\"total_mb\":" + String((unsigned long)sdTotalMB);
  json += ",\"used_mb\":" + String((unsigned long)sdUsedMB);
  json += ",\"photo_count\":" + String(sdPhotoCount);
  json += ",\"files\":[";
  if (sdReady && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(4000)) == pdTRUE) {
    File root = SD_MMC.open("/");
    bool first = true;
    if (root && root.isDirectory()) {
      while (true) {
        File f = root.openNextFile();
        if (!f) break;
        String fn = String(f.name());
        bool dir = f.isDirectory();
        unsigned long sz = (unsigned long)f.size();
        closeQuietly(f);
        if (dir || fn.startsWith(".")) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + jsonEscape(fn) + "\",\"size\":" + String(sz) + "}";
      }
      closeQuietly(root);
    }
    xSemaphoreGive(sdMutex);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Batch begin: detach USB once so nothing races with the SD writes.
static void handleBegin() {
  if (!requireAuth()) return;
  batchActive = true;
  uploadingActive = true;
  lastUploadActivityMs = millis();
  usbDetach();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Batch end: flush FAT, remount the card, re-attach USB exactly once.
static void handleEnd() {
  if (!requireAuth()) return;
  uploadingActive = false;
  lastUploadActivityMs = millis();
  bool ok = flushAndRemountSd();
  refreshSdStats();
  usbAttach();
  batchActive = false;
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleUploadDone() {
  if (uploadRejectCode) {
    int code = uploadRejectCode;
    uploadRejectCode = 0;
    if (code == 401) {
      server.requestAuthentication();
    } else {
      server.send(code, "application/json", "{\"ok\":false}");
    }
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleUploadStream() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    if (!webAuthOk()) { uploadRejectCode = 401; return; }
    if (!sdReady) { uploadRejectCode = 503; return; }
    uploadingActive = true;
    lastUploadActivityMs = millis();
    up.filename = make83Name(String(up.filename));
    uploadTargetPath = "";
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      uploadTargetPath = uniquePath(String(up.filename));
      uploadFile = SD_MMC.open(uploadTargetPath, FILE_WRITE);
      xSemaphoreGive(sdMutex);
    }
    Serial.printf("[UPLOAD] start %s\n", uploadTargetPath.c_str());
    return;
  }

  if (uploadRejectCode) return;

  if (up.status == UPLOAD_FILE_WRITE) {
    lastUploadActivityMs = millis();
    if (uploadFile && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      uploadFile.write(up.buf, up.currentSize);
      xSemaphoreGive(sdMutex);
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      uploadFile.flush();
      uploadFile.close();
      xSemaphoreGive(sdMutex);
    }
    lastUploadActivityMs = millis();
    // A lone /upload (no /begin) must not leave the main loop in "busy" mode.
    if (!batchActive) uploadingActive = false;
    Serial.printf("[UPLOAD] end %s (%u bytes)\n", uploadTargetPath.c_str(), (unsigned int)up.totalSize);
  }
}

// Bounded-work recursive delete: at most `budget` entries per HTTP request,
// then return false so the caller can continue.  Never blocks long enough to
// trip the task watchdog (formatting/large deletes used to crash the device).
static bool cleanTree(const String &path, int &budget, uint64_t &freedBytes) {
  if (budget <= 0) return false;
  File dir = SD_MMC.open(path);
  if (!dir) return true;
  if (!dir.isDirectory()) { dir.close(); return true; }

  while (budget > 0) {
    File child = dir.openNextFile();
    if (!child) {
      dir.close();
      SD_MMC.rmdir(path);
      return true;
    }
    String cname = String(child.name());
    bool cdir = child.isDirectory();
    unsigned long csz = (unsigned long)child.size();
    child.close();
    String cpath = cname.startsWith("/") ? cname : (path + "/" + cname);
    budget--;
    if (cdir) {
      if (!cleanTree(cpath, budget, freedBytes)) { dir.close(); return false; }
    } else {
      if (SD_MMC.remove(cpath)) freedBytes += csz;
    }
    delay(1);  // yield -> feed the watchdog
  }
  dir.close();
  return false;
}

static void handleClean() {
  if (!requireAuth()) return;
  uint64_t freed = 0;
  int budget = 100;
  bool done = true;
  if (sdReady && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(4000)) == pdTRUE) {
    const char* junk[] = {"/.Trashes", "/.Spotlight-V100", "/.fseventsd",
                          "/.TemporaryItems", "/.apdisk", "/System Volume Information"};
    for (auto p : junk) {
      if (!SD_MMC.exists(p)) continue;
      if (!cleanTree(String(p), budget, freed)) { done = false; break; }
    }
    xSemaphoreGive(sdMutex);
  }
  String body = "{\"ok\":true,\"done\":" + String(done ? "true" : "false") +
                ",\"freed_mb\":" + String((unsigned long)(freed / (1024ULL * 1024ULL))) + "}";
  server.send(200, "application/json", body);
  if (done && freed > 0) { refreshSdStats(); scheduleRemount(1500); }
}

// On-device formatting is deliberately disabled: f_mkfs on an 8 GB card blocks
// for minutes, trips the watchdog, and can leave the FAT half-written.
// Use /usb-rw?on=1, format from a computer, then /usb-rw?on=0.
static void handleFormat() {
  if (!requireAuth()) return;
  server.send(200, "application/json",
              "{\"ok\":false,\"hint\":\"on-device format disabled; use /usb-rw?on=1 and format on a computer\"}");
}

// Temporary read-write toggle so a computer can format/repair the card once.
static void handleUsbRw() {
  if (!requireAuth()) return;
  String on = server.hasArg("on") ? server.arg("on") : "0";
  bool rw = (on == "1" || on == "true");
  usbDetach();
  delay(150);
  usbReadOnly = !rw;
  usbAttach();
  Serial.printf("[USB] readOnly=%d\n", usbReadOnly ? 1 : 0);
  String body = "{\"ok\":true,\"readonly\":" + String(usbReadOnly ? "true" : "false") + "}";
  server.send(200, "application/json", body);
}

static void handleDelete() {
  if (!requireAuth()) return;
  if (!sdReady || !server.hasArg("name")) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String fn = server.arg("name");
  if (!isSafeRootName(fn)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  if (!fn.startsWith("/")) fn = "/" + fn;
  bool ok = false;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    ok = SD_MMC.remove(fn);
    xSemaphoreGive(sdMutex);
  }
  // Do NOT toggle USB per delete (macOS kills the port after rapid re-enumeration).
  scheduleRemount(2500);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleView() {
  if (!requireAuth()) return;
  if (!sdReady || !server.hasArg("name")) { server.send(404, "text/plain", "Not found"); return; }
  String fn = server.arg("name");
  if (!isSafeRootName(fn)) { server.send(400, "text/plain", "Bad name"); return; }
  if (!fn.startsWith("/")) fn = "/" + fn;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    File f = SD_MMC.open(fn, FILE_READ);
    if (!f) { xSemaphoreGive(sdMutex); server.send(404, "text/plain", "File not found"); return; }
    String mime = "application/octet-stream";
    String lower = fn;
    lower.toLowerCase();
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) mime = "image/jpeg";
    else if (lower.endsWith(".png")) mime = "image/png";
    else if (lower.endsWith(".gif")) mime = "image/gif";
    else if (lower.endsWith(".bmp")) mime = "image/bmp";
    server.streamFile(f, mime);
    f.close();
    xSemaphoreGive(sdMutex);
  }
}

static void handleRemount() {
  if (!requireAuth()) return;
  usbDetach();
  flushAndRemountSd();
  refreshSdStats();
  usbAttach();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleMount() {
  if (!requireAuth()) return;
  bool ok = false;
  if (!sdReady) {
    ok = mountSd();
    if (ok) {
      sdCard = SDCardAccessor::getCard(SD_MMC);
      ok = (sdCard != nullptr);
      sdReady = ok;
    }
  } else {
    ok = true;
  }
  if (ok && sdCard) {
    uint32_t secSize = sdCard->csd.sector_size ? sdCard->csd.sector_size : 512;
    msc.begin(sdCard->csd.capacity, secSize);
    msc.mediaPresent(true);
    usbMscMounted = true;
    refreshSdStats();
  }
  String body = "{\"ok\":" + String(ok ? "true" : "false") +
                ",\"total_mb\":" + String((unsigned long)sdTotalMB) + "}";
  server.send(200, "application/json", body);
}

static void handleHttpStatus() {
  if (!requireAuth()) return;
  char json[512];
  snprintf(json, sizeof(json),
    "{\"fw\":\"%s\",\"ip\":\"%s\",\"host\":\"%s.local\",\"free_heap\":%u,\"uptime_s\":%lu,"
    "\"sd\":{\"ready\":%s,\"usb_mounted\":%s,\"readonly\":%s,\"total_mb\":%llu,\"used_mb\":%llu,\"photo_count\":%d}}",
    FW_VERSION, WiFi.localIP().toString().c_str(), MDNS_HOST,
    (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000),
    sdReady ? "true" : "false", usbMscMounted ? "true" : "false", usbReadOnly ? "true" : "false",
    (unsigned long long)sdTotalMB, (unsigned long long)sdUsedMB, sdPhotoCount);
  server.send(200, "application/json", json);
}

// Executed from loop() when a debounced remount is due.
static void runPendingRemount() {
  if (!remountPending) return;
  if ((int32_t)(millis() - remountAtMs) < 0) return;
  remountPending = false;
  if (millis() - lastUsbToggleMs < 3000) {   // rate-limit bus toggling
    scheduleRemount(1500);
    return;
  }
  lastUsbToggleMs = millis();
  usbDetach();
  flushAndRemountSd();
  refreshSdStats();
  usbAttach();
}

static void applyStaticIp() {
  IPAddress ip, gw, mask;
  if (!ip.fromString(STATIC_IP) || !gw.fromString(STATIC_GW) || !mask.fromString(STATIC_MASK)) return;
  if ((uint32_t)ip == 0) return;
  WiFi.config(ip, gw, mask);
  Serial.printf("[WiFi] static ip %s\n", STATIC_IP);
}

static void maintainWiFi() {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[BOOT] ESP32 USB Photo Bridge %s\n", FW_VERSION);

  initSdAndUsbMsc();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOST);
  applyStaticIp();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] not connected yet - will keep retrying");
  }

  configTime(0, 0, "pool.ntp.org", "time.google.com");

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local/\n", MDNS_HOST);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/begin", HTTP_POST, handleBegin);
  server.on("/end", HTTP_POST, handleEnd);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadStream);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/view", HTTP_GET, handleView);
  server.on("/remount", HTTP_POST, handleRemount);
  server.on("/clean", HTTP_POST, handleClean);
  server.on("/format", HTTP_POST, handleFormat);
  server.on("/usb-rw", HTTP_POST, handleUsbRw);
  server.on("/mount", HTTP_POST, handleMount);
  server.on("/status", HTTP_GET, handleHttpStatus);
  server.on("/ota", HTTP_POST, []() {
    if (otaRejected) { otaRejected = false; server.requestAuthentication(); return; }
    bool ok = !Update.hasError();
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    delay(300);
    if (ok) ESP.restart();
  }, []() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
      if (!webAuthOk()) { otaRejected = true; return; }
      uploadingActive = true;
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!otaRejected) Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
      if (!otaRejected) Update.end(true);
      uploadingActive = false;
    }
  });
  server.begin();
  Serial.println("[HTTP] server started on :80");
}

void loop() {
  server.handleClient();

  // A batch whose client vanished (Wi-Fi drop, crashed script) would leave the
  // USB bus detached forever. Close it after 10 minutes without any upload.
  if (batchActive && uploadingActive && millis() - lastUploadActivityMs > 600000UL) {
    Serial.println("[BATCH] stale, auto-closing");
    uploadingActive = false;
    flushAndRemountSd();
    refreshSdStats();
    usbAttach();
    batchActive = false;
  }

  if (uploadingActive) return;   // tight loop: maximum TCP throughput

  runPendingRemount();
  maintainWiFi();
  delay(2);
}
