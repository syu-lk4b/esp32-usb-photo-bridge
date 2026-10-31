# ESP32 USB Photo Bridge

**把只会插 U 盘的老板数码相框，变成会自动更新的无线相框。**

[English](README.en.md) · 简体中文

---

相框以为它插着一个普通的 U 盘。实际上，那块"U 盘"是一块连着 Wi-Fi 的 ESP32-S3：
它自己管着一张 TF 卡，通过 HTTP 接收照片，然后在每一批照片写完后**物理断开再重连一次 USB**，
让 2008 年的相框乖乖重新扫描、播放新照片。

```
┌──────────────┐   Wi-Fi / HTTP    ┌──────────────┐   USB MSC    ┌──────────────┐
│  Mac         │ ────────────────▶ │  ESP32-S3    │ ───────────▶ │  相框         │
│  Photos.app  │   /begin /upload  │  TF 卡 8 GB  │  只读 U 盘    │  8FF3CDW     │
│  人脸 + 定时  │   /end            │  FAT32       │              │  800×600     │
└──────────────┘                   └──────────────┘              └──────────────┘
```

没有云端、没有 App、没有订阅。照片只在你自己的局域网里走。

---

## 目录

- [它解决什么问题](#它解决什么问题)
- [工作原理](#工作原理)
- [硬件要求](#硬件要求)
- [快速开始](#快速开始)
- [手动安装](#手动安装)
- [HTTP API](#http-api)
- [实现细节](#实现细节)
- [隐私与安全](#隐私与安全)
- [故障排查](#故障排查)
- [兼容性](#兼容性)
- [License](#license)

---

## 它解决什么问题

老式数码相框（比如飞利浦 8FF3CDW）有一堆今天看来很离谱的限制：

- 只能插 SD 卡或 U 盘，**手动拷照片**；
- 插着 U 盘时停在"传输模式"，**必须拔掉才会刷新播放**；
- 只认 **FAT32 + MBR**，不认 exFAT / GPT；
- 只认 **Baseline JPEG**，渐进式 JPEG 直接不显示；
- 原生 **800×600**，别的尺寸会被拉伸或裁切；
- **不认 EXIF 方向**，竖拍照片经常横着或倒着显示；
- 不认长文件名，最好用 `IMG0001.JPG` 这种 8.3 名字；
- 宿主系统（macOS）往卡里写 `.Trashes`、`.Spotlight-V100`，会把 FAT 弄坏。

结果就是：家人照片都在 iPhone 里，但没人愿意每周手动拷一次。

这个项目把整条链路自动化了：**每周从 Apple Photos 按人脸挑照片 → 转成相框能吃的格式 → Wi-Fi 上传 → 相框自动刷新**。

---

## 工作原理

一次同步的完整流程：

1. **Mac 端**（`tools/photo_frame_sync.py`）
   - 只读打开 `Photos.sqlite`，按人物（`ZPERSON`）取最近的照片；
   - 过滤掉视频、隐藏、已删除、**竖构图**；
   - 用 `sips` 转成 800×600 Baseline JPEG，**把 EXIF 方向真正烘进像素**；
   - 命名成 `IMG0001.JPG` 这种 8.3 文件名；
   - 通过 Wi-Fi 调 ESP32 的 HTTP 接口。

2. **ESP32 端**（`src/main.cpp`）
   - `POST /begin`：断开 USB（相框看到"U 盘被拔了"）；
   - `POST /upload` × N：把 JPEG 写进 TF 卡；
   - `POST /end`：刷脏 FAT → 重新挂载 → 重新连接 USB（相框看到"U 盘插上了"）；
   - 相框重新扫描，开始播新照片。

3. **为什么必须真的断开 USB**
   老相框不会因为"文件变了"就重扫卷。只调用 `mediaPresent(false)` 不够，
   必须用 `tud_disconnect()` 拉低 USB D+，让主机在电气层面看到一次真正的拔出。

---

## 硬件要求

| 部件 | 要求 |
|---|---|
| 主控 | **ESP32-S3**（需要原生 USB-OTG；带 PSRAM 更稳） |
| Flash | 16 MB（默认分区表）；8 MB 需自行调整 `partitions_ota.csv` |
| 存储 | TF / microSD 卡，**FAT32 + MBR**，8 GB 实测可用 |
| 接线 | SD 卡走 4-bit SDMMC（默认 CLK=38, CMD=40, D0=39, D1=41, D2=48, D3=47） |
| 供电 | 相框的 USB-A 口通常能提供 500 mA；ESP32-S3 峰值约 350 mA |
| 连接 | ESP32 的 **原生 USB 口**（GPIO19/20）接相框的 USB-A 口，不是 UART 调试口 |

> 很多"ESP32-S3 开发板"有两个 USB-C：一个是 UART 桥，一个是原生 USB。
> **必须用原生 USB 那个口**，否则 `USBMSC` 不会出现。

---

## 快速开始

```bash
git clone <your-repo-url> esp32-usb-photo-bridge
cd esp32-usb-photo-bridge
./setup.sh
```

`setup.sh` 会一步步完成：

1. 检查依赖（PlatformIO、Python 3、Xcode CLT、curl/sips）；
2. 询问 Wi-Fi、静态 IP、Web 密码，生成 `include/secrets.h`（权限 600）；
3. 编译并烧录固件；
4. 等设备上线并读取 `/status`；
5. 编译一个已签名的启动器（为了让 macOS 能授予"完全磁盘访问"）；
6. 配置 Photos 库和要同步的人物；
7. 安装每周一次的 launchd 定时任务；
8. 先 dry-run，再跑第一次真实同步。

只配置 Mac 端（设备已刷好）：

```bash
./setup.sh --skip-flash
```

不想装定时任务：

```bash
./setup.sh --skip-launchd
```

---

## 手动安装

### 1. 固件

```bash
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h        # 填 Wi-Fi、可选 Web 密码/静态 IP
pio run                          # 编译
pio run -t upload                # 烧录（板子用 USB 接 Mac）
```

### 2. 首次验证

```bash
curl http://photo-frame.local/status
# 或
curl http://<device-ip>/status
```

返回示例：

```json
{
  "fw": "1.0.0",
  "ip": "192.168.1.50",
  "host": "photo-frame.local",
  "sd": { "ready": true, "usb_mounted": true, "readonly": true,
          "total_mb": 7674, "used_mb": 4, "photo_count": 39 }
}
```

浏览器打开 `http://photo-frame.local/` 可以手动上传、删除、清理垃圾文件。

### 3. Mac 端配置

```bash
cp tools/config.example.json tools/config.json
chmod 600 tools/config.json
$EDITOR tools/config.json
```

最小配置：

```json
{
  "device_url": "http://photo-frame.local",
  "photos_library": "~/Pictures/Photos Library.photoslibrary",
  "people": { "Alice": 92, "Bob": 481 }
}
```

`people` 里的数字是 Photos 数据库的 `ZPERSON.Z_PK`。用下面的命令查：

```bash
python3 tools/photo_frame_sync.py --list-people
```

留空 `people` 就会同步整个照片库里最新的照片。

### 4. 试跑

```bash
python3 tools/photo_frame_sync.py --dry-run   # 只列出会上传什么
python3 tools/photo_frame_sync.py             # 真正上传
```

> 直接跑 Python 需要**终端**拥有"完全磁盘访问"权限。
> 定时任务请用 `setup.sh` 生成的签名启动器（见[隐私与安全](#隐私与安全)）。

### 5. 定时任务

`setup.sh` 会安装 `~/Library/LaunchAgents/com.esp32photobridge.sync.plist`，
默认**每周日 10:00** 运行，并在登录时补跑一次。

改时间：

```bash
SYNC_WEEKDAY=3 SYNC_HOUR=3 SYNC_MINUTE=30 ./setup.sh --skip-flash
# 3 = Wednesday, 0 = Sunday
```

---

## HTTP API

所有接口都在 `http://<device>/` 下。启用了 Web 密码时使用 HTTP Basic Auth。

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/` | 浏览器管理页 |
| `GET` | `/status` | 设备状态（IP、TF 卡、照片数） |
| `GET` | `/files` | 卡上文件列表（JSON） |
| `GET` | `/view?name=IMG0001.JPG` | 预览单张照片 |
| `POST` | `/begin` | 进入批次模式：断开 USB |
| `POST` | `/upload` | 上传一张（`multipart/form-data`，字段名 `file`） |
| `POST` | `/end` | 结束批次：刷盘、重新挂载、重连 USB |
| `POST` | `/delete?name=...` | 删除文件（2.5 秒后合并触发一次重挂载） |
| `POST` | `/remount` | 立即重挂载（调试用） |
| `POST` | `/clean` | 清理 `.Trashes` 等垃圾（分片执行，可重复调用） |
| `POST` | `/mount` | TF 卡未挂载时重试挂载 |
| `POST` | `/usb-rw?on=1` | **临时**允许电脑写卡（用于格式化） |
| `POST` | `/format` | 永远返回失败：设备端格式化已禁用 |
| `POST` | `/ota` | 上传 `firmware.bin` 做 OTA |

OTA 示例：

```bash
pio run
curl -u admin:****** -F "file=@.pio/build/esp32s3/firmware.bin" \
     http://photo-frame.local/ota
```

---

## 实现细节

### 1. 让 ESP32 变成一个 U 盘

用 Arduino-ESP32 自带的 `USBMSC` + TinyUSB：

```cpp
msc.vendorID("ESP32S3");
msc.productID("PhotoFrameDisk");
msc.onRead(onUsbRead);
msc.onWrite(onUsbWrite);
USB.begin();          // 必须在挂载 SD 之前
msc.begin(sectorCount, sectorSize);
```

`onUsbRead` 不经过 FatFs，而是直接对 SD 卡做 `sdmmc_read_sectors()`。
这样主机看到的是**块设备级别**的真实卡内容，而不是一层文件系统模拟。

关键点：**先启动 USB，再挂载 SD 卡。**
如果先挂 SD，卡坏了或 `SD_MMC.begin()` 卡死，USB 就永远不会出现，
设备会变成"网也连不上、U 盘也没有"的砖。现在的顺序保证即使卡坏了，
你仍然能通过 Wi-Fi 访问 `/mount`、`/status`、`/ota` 来救它。

### 2. 只读模式：为什么不让电脑写卡

`onUsbWrite` 在只读模式下**假装写成功**，然后直接丢弃：

```cpp
if (usbReadOnly) return (int32_t)bufsize;
```

原因很实际：macOS 一挂载可写卷，就会写：

- `.Trashes/`（回收站）
- `.Spotlight-V100/`（索引）
- `._*`（AppleDouble 资源叉）

这些写入走的是**主机的文件系统缓存**，而 ESP32 这边 FatFs 也在缓存 FAT。
两边各写各的，很快就会不一致。轻则新照片不显示，重则 FAT 损坏、相框直接不认卡。

所以正常运行时：**卡由固件独占写入，USB 侧只读。**

需要格式化卡时，临时打开写权限：

```bash
curl -X POST "http://photo-frame.local/usb-rw?on=1"
# 在 Mac 上用磁盘工具格式化为 FAT32 / MBR
curl -X POST "http://photo-frame.local/usb-rw?on=0"
```

### 3. 批次模式：一整批只拔插一次

如果每上传一张就断开重连一次 USB，会发生两件坏事：

1. 相框会不停闪烁"USB 已连接/已断开"；
2. macOS 在短时间内看到太多次重新枚举，会**直接停用整个 USB 端口**（`USB 2.0 BILLBOARD` 消失），
   只能换口或拔插 Mac 侧才能恢复。

所以上传走批次：

```
/begin  → 断开一次
/upload → 写 N 张（USB 保持断开）
/end    → 刷盘 + 挂载 + 连接一次
```

删除也做了防抖：`scheduleRemount(2500)` 把 2.5 秒内的多次删除合并成一次重挂载，
并且两次挂载之间强制间隔 3 秒。

### 4. 刷脏 FAT + 重新挂载

写完文件后，FatFs 的 FAT 表和目录项可能还在内存里。
`/end` 会执行：

```cpp
SD_MMC.end();      // 卸载，强制所有脏扇区落盘
delay(60);
mountSd();         // 重新挂载，拿到新的 sdmmc_card_t
usbAttach();       // 重新枚举
```

顺序很重要：**先落盘、再挂载、最后接 USB**。
如果先接 USB，相框可能在 FAT 还没写完时就来读，看到的是半个文件。

### 5. 照片流水线

**只取横构图。** SQL 层直接过滤：

```sql
AND a.ZWIDTH > a.ZHEIGHT
```

竖图在相框上会被压扁或旋转，观感很差，所以宁可不放。
转换完成后还会再检查一次实际宽高，防止 HEIC 的 EXIF 旋转把横图转成竖图。

**EXIF 方向必须烘进像素。** 这是最深的坑：

```
源文件 orientation = 3      (180°)
sips 输出 orientation = 3   (只复制标记，没转像素)
相框不认 EXIF → 直接显示原始像素 → 上下颠倒
```

修复顺序：

1. 读出 JPEG 的 EXIF Orientation；
2. **先把标记改成 1**（否则 sips 会二次旋转）；
3. 再 `sips -r 180/90/270` 物理旋转像素；
4. 最后强制写回 orientation = 1。

**统一成 800×600。** 用 `sips --resampleHeightWidthMax 800` 缩放长边，
再用 `sips -p 600 800 --padColor 000000` 补黑边，**不裁剪、不拉伸**。
16:9 的照片会得到上下黑边，但比例是对的。

**8.3 文件名。** 固件会把上传的文件名清洗成 `IMG0001.JPG`：
只保留 7 个字符的主名 + `.JPG`，重名自动加序号。

### 6. 去重、淘汰与状态

Mac 端在 `~/.local/state/esp32-photo-bridge/state.json` 里记录已上传的
Photos asset UUID：

```json
{
  "uploaded": {
    "8A4538B4-...": { "name": "IMG0033.JPG", "person": "Alice", "date": 760000000 }
  },
  "next_index": 34
}
```

- 同一张照片（哪怕出现在两个人的合影里）只传一次；
- 卡上超过 `keep_on_card`（默认 90）张时，按文件名删掉最旧的；
- `--force` 会忽略去重记录，重新上传（会产生副本）。

### 7. 失败恢复

- **看门狗友好**：清理目录用 `cleanTree()` 分片执行，每次 HTTP 请求最多删 100 个条目，
  然后返回 `done:false` 让前端继续调用。不会因为一次删太多而触发 WDT。
- **设备端格式化被禁用**：`f_mkfs()` 在 8 GB 卡上会阻塞几分钟，触发看门狗，
  把卡写一半，下次 `SD_MMC.begin()` 直接死锁。格式化请用电脑。
- **`/end` 一定会执行**：Python 脚本用 `try/finally` 保证即使上传中途异常，
  也会调 `/end` 把 USB 重新接上，避免相框一直看到"U 盘已拔出"。

---

## 隐私与安全

这个项目**没有云端**：

- 不上传任何数据到第三方；
- 不做遥测、不联网校验；
- 照片只从你的 Mac 走到你局域网里的 ESP32，再通过 USB 给相框。

仓库里**不包含**任何个人信息：

- `include/secrets.h`（Wi-Fi 密码、Web 密码）被 `.gitignore` 忽略；
- `tools/config.json`（照片库路径、人物 ID）被忽略；
- 运行状态（`state.json`、日志）被忽略；
- 没有照片、没有人名、没有 Apple ID、没有设备序列号。

`setup.sh` 写入的敏感文件权限是 `600`。

**建议：**

1. 在 `secrets.h` 里设置 `WEB_PASS`，给 Web UI 和 API 加上 Basic Auth；
2. **不要把 80 端口映射到公网**，这个服务本来只为局域网设计；
3. 不用 `/usb-rw?on=1` 时记得关回去；
4. 如果多人共用局域网，给 IoT 设备单独划一个 VLAN。

**关于 macOS 的"完全磁盘访问"：**

`Photos.sqlite` 受 TCC 保护。TCC 认的是**二进制**，不认 `python3`，
所以直接把 `/usr/bin/python3` 加进 FDA 通常不生效。

解决办法是一个几十行的 C 启动器（`tools/photosync_launcher.c`）：
它 `execv` 到 python3，TCC 把权限记在这个已签名的启动器上。
`setup.sh` 会自动编译并 ad-hoc 签名它，然后请你在系统设置里授权。

---

## 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| Mac 完全看不到 U 盘 | 用了 UART 口；或没开 TinyUSB 模式 | 换原生 USB 口；确认 `ARDUINO_USB_MODE=0` |
| `/status` 打不开 | mDNS 不解析 | 改用 IP：`curl http://<ip>/status` |
| 相框看不到新照片 | 没有真正重新枚举 | 确认 `/end` 返回 ok；必要时 `POST /remount` |
| 照片上下颠倒 | 源文件 EXIF 没烘进像素 | 确认用的是本仓库的 `convert()` |
| 照片被拉伸 | 尺寸不是 800×600 | 检查 `sips` 是否可用；看日志里的 `skip portrait` |
| 上传到一半卡住 | 批次没结束，USB 停在断开态 | `curl -X POST .../end` |
| TF 卡挂载失败 | 卡不是 FAT32/MBR，或 FAT 已损坏 | `/usb-rw?on=1` 后用电脑重新格式化 |
| macOS 禁用 USB 端口 | 短时间枚举次数过多 | 换 Mac 端口；本仓库已做防抖 |
| 定时任务读不到照片库 | 启动器没有 FDA | 系统设置 → 隐私与安全 → 完全磁盘访问 |
| OTA 失败 | 分区表不是双 OTA | 使用仓库里的 `partitions_ota.csv` |

日志位置：

```
~/.local/state/esp32-photo-bridge/sync.log
~/.local/state/esp32-photo-bridge/launchd.out
~/.local/state/esp32-photo-bridge/launchd.err
```

---

## 兼容性

| 项目 | 状态 |
|---|---|
| Philips 8FF3CDW / 93 | ✅ 实测 |
| FAT32 + MBR，8 GB TF 卡 | ✅ 实测 |
| 其他"挂载即读"的 USB 相框 | ⚠️ 理论可行，取决于是否接受 Baseline JPEG / 800×600 |
| exFAT / GPT | ❌ 不支持 |
| 渐进式 JPEG | ❌ 不支持（脚本会输出 Baseline） |
| 竖构图照片 | ❌ 主动跳过 |

> 相框的 USB 口必须是 **Host**（能读 U 盘），不能是只供电的充电口。

---

## License

Apache License 2.0，见 [LICENSE](LICENSE)。
