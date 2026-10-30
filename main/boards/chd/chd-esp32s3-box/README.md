# chd-esp32s3-box

## 简介
chd-esp32s3-box AIoT 开发套件，搭载 ESP32-S3-WROOM-1 模组，配备 2.8 英寸 320x240 ST7789 显示屏，双麦克风阵列，支持离线语音唤醒与设备端回声消除（AEC）功能。

## 硬件特性

- **主控**: ESP32-S3-WROOM-1 (16MB Flash, 8MB PSRAM)
- **显示屏**: 2.8 英寸 LCD (320x240, ST7789)
- **音频**: ES8311 音频 Codec + ES7210 双麦 ADC
- **音频功能**: 支持设备端 AEC (回声消除)
- **按键**: Boot 按键 (单击/双击功能)
- **其他**: USB-C 供电与通信，锂电池供电、支持ml307-4G

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

### 基本配置
- `Xiaozhi Assistant` → `Board Type` → 选择 `虫洞 esp32s3-BOX`

### 音频功能配置

#### 设备端回声消除 (AEC)
- `Xiaozhi Assistant` → `Enable Device-Side AEC` → 启用

ESP-BOX-3 硬件支持设备端 AEC 功能，可有效消除扬声器播放声音对麦克风的干扰，提升语音识别准确率。

**运行时切换**: 双击 Boot 按键可在运行时开启/关闭 AEC 功能。

> **说明**: 设备端 AEC 需要干净的扬声器输出参考路径和良好的麦克风与扬声器物理隔离才能正常工作。ESP-BOX-3 硬件已做优化设计。

### 唤醒词配置

ESP-BOX-3 支持多种唤醒词实现方式：

- `Xiaozhi Assistant` → `Wake Word Implementation Type` → 选择唤醒词类型

推荐选择：
- **Wakenet model with AFE** (`USE_AFE_WAKE_WORD`) - 支持 AEC 的唤醒词检测

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 ESP-BOX-3 连接至电脑，并运行：

```bash
idf.py flash
```

## 触摸交互（task-29：触摸接管，BOOT 键不再参与对话）

### 为什么去掉 BOOT 键的对话用途
厂商确认 **IO0 = BOOT 键 = SD_CMD**。`boot_button_` 用的是 `iot_button` 的 GPIO 中断，挂在 IO0 上：
SD 卡高速传输时 CMD 线翻转会被当成按键 → 随机 `ToggleChatState()`（现在没暴露只是因为卡只在启动时读一下）。
**已彻底移除**：`boot_button_` 成员、`InitializeButtons()`、三个 `HandlePrimaryButton*`、`SetClickHandler` 注册全部删除，
板级再无 `iot_button` / `BOOT_BUTTON_GPIO` 的代码引用（只剩解释性注释）。
**仍然保留**：
- 芯片级下载模式（BOOT 拉低 + 复位）——纯硬件行为，软件不去驱动 IO0；
- 配网兜底（`wifi_board.cc:112` 无 SSID 自动进配网、`:164` 连接超时 60 s 自动进配网），不依赖按键。

### 手势表（触摸）
| 手势 | 行为 | 备注 |
|---|---|---|
| **单击**（空白处） | `ToggleChatState()`：开/关一轮对话 | 打 `CHD_UI: tap -> ToggleChatState` |
| **单击脸的任意位置** | 一次表情反应（HAPPY→WINK→SURPRISED→LOVE 轮换，900 ms 后回 neutral） | 打 `CHD_UI: face tap -> reaction N`；**不会**开对话 |
| **上滑** | `ToggleChatState()`（保留原行为） | 打 `CHD_UI: Swipe Up detected` |
| **左右滑** | 留空（无动作） | |
| **下滑** | 保留原有「设置栏」（本次未改动） | |

### 怎么保证「滑动不误触发单击」
1. 手势回调里记录 `lv_tick_get()` 到 `last_gesture_ms_`；
2. 单击回调若距上次手势 < **400 ms** → 直接忽略并打 `CHD_UI: tap ignored: N ms after a swipe`。
3. 单击回调还要求 `target == current_target`（事件不是从子对象冒泡上来的）；
4. 脸的子树递归去掉 `LV_OBJ_FLAG_CLICKABLE`，点击只会落在脸面板；
5. 字幕条与其 label 也去掉 `LV_OBJ_FLAG_CLICKABLE`（否则会吃掉落点，单击失效）。

### UI v2 Phase B1/B2：A/B/C 信息屏（text-only）

- 9哥 2026-10-28 拍板 text-only：A/B/C 用信息屏替代 kawaii 脸；A 默认，点左上 `● Hermes 在线` 循环
  **A → B → C → A**，不落 NVS，开机回 A；保留 3 s boot guard + 400 ms 手势 guard；日志区不可点。
- 设计语言：纸白 `#F2EEE1`、墨字 `#202B2D`、橙色 `#E2703A`（仅状态点/ECG）；顶栏 44 px 一行
  `● Hermes 在线/离线` 在左，右侧 `静音/未静音` + `HH:MM`；静音/解除点右下 Hermes 头像（30% 透明）。
  底部字幕 480×44 黑底白字；空闲显示 ECG，说话时隐藏 ECG 显示字幕。
- **A**：7 行日志 y44..275（33 px，宽 328，避开头像）；头像 64×72 @(408,204)；静音仍由头像触发。
- **B**：6 卡 3×2（状态/静音/音量/RSSI/SRAM/音频帧），卡 152×62、gap 4，y44..172；下方 3 行日志
  y180..273（31 px，宽 328）；头像/ECG/字幕同 A；切到 B 只刷新卡值（250 ms），不重建整屏。
- **C**：8 行日志 y44..276（29 px，宽 400，x8..408）；右下保留 **56×64 头像** @(416,212)（等比缩放，
  保留点头像静音/解除与静音 30% 透明）；底部字幕/ECG 同 A；调试值在顶栏/日志前缀，不另建浮层。
- 日志环 `UiLog`：10 行 ×112 B、mutex、250 ms 快照、覆盖最旧、同 tag 合并 + 5 行/s、不落 NVS；
  只接 stt/tts/state/llm:emotion/wake/vad/mute/net 精选事件，不转发全部 `ESP_LOG`；不伪造 thinking/confirm/barge。
- **Hermes 边界**：左上 `●` 是品牌名，点色/“在线”只表示**设备与 309 服务器的 WS 链路**
  （`Application::IsServerLinkUp()`，`OnAudioChannelOpened` 置真，`OnNetworkError`/`OnAudioChannelClosed`/断网置假）；
  设备看不到 Hermes 后端真实健康，不得说成“Hermes 已连接”。
- 字体只用 common_20；主题刷新时 `ApplyPhaseBFonts()` 会重指所有 B1/B2 的 20px label（含 6 卡标题/值、
  8 行日志、state/mute/time/Hermes 状态），避免旧 font 悬空导致 `lv_font_get_glyph_width` 崩溃。
- mute 200 ms 语义保持：点击先“切换中…”+`lv_refr_now()`，服务端确认后才落“已静音/解除”，失败“同步失败”。
- 隐藏的 kawaii 脸动画仍在后台运行（只 HIDDEN 未暂停）；留 B2/后续评估暂停以省 CPU。

### 触摸过滤修正（D1 幽灵死区 / D2 长按假 click）

- **D1（实测后收敛）**：2026-10-28 冷启动 60 s `CHD_TOUCH_RAW` 抓包里 `n>=1` 出现 **0 次**，
  无 `(442,202)` 幽灵点（该锚点属历史面板/状态）。因此**移除幽灵锚点与 60×60 死区**，
  首次触摸 `(442,202)±30 px`、冷启动右侧按住都不再被吞；保留 3 s 开机 tap guard、
  phantom 过滤与坐标映射。
- **若换到仍带幽灵的面板**：先冷启动 60 s 抓 `CHD_TOUCH_RAW_TRACE`，确认 `n>=1` 频次与锚点坐标，
  再决定是否恢复框过滤；**不要直接复用历史 60×60 死区或永久失效方案**（那会吞真实触摸/造成 stuck）。
- **D2（保留已验证逻辑）**：点静止 ≥ `kPointStaleMs`（2 s）时只打
  `reason=stale ... HOLD (LVGL stays pressed, no release)`，**不合成 RELEASED、不写 release 时间戳**；
  LVGL 保持 pressed，直到面板真的 `raw_points==0`（真实抬手）才释放，且只产生一次 CLICKED。
  原 `kForceReleaseMs` 合成抬手已删除，不再出现“按住 2 s 后延迟 toggle”。
- RAW 诊断固件已归档；本版保留紧凑的 indev/过滤心跳（`CHD_TOUCH_LOG_INDEV=1`，状态变化一行，
  静默时 5 s/10 s 一行）；需要完全静默时置 0。
- **已知风险**：若面板“长时报点但不释放”，D2 会让 LVGL 保持 pressed（不再合成 release）；
  已知恢复方式 = 重启。现场看心跳 `kept_last=1` / `state=PRESSED` 判断；下次触摸改动可再加 20 s watchdog
  （本次不加，避免改变已验证行为）。

### 卡持续读写时「不会误触发对话」怎么验
1. 串口/日志里所有对话切换都带来源：`CHD_UI: tap -> ...` / `CHD_UI: face tap -> ...` / `CHD_UI: Swipe Up detected`。
   按键路径已删除，所以**没有任何来源的对话切换 = 不可能发生**。
2. 内置压测工具（MCP）：`self.debug.sd_storm(seconds=30)` → 后台任务连续读 `/sdcard/_sdtest_storm.bin`，
   每秒打 `CHD_SD: storm: N s, ... KiB/s, total ... KiB`，结束时打 `storm: STOP`。
   **判读**：`storm: START` 到 `STOP` 之间若没有 `CHD_UI:` 行 → SD 流量没有误触发对话。

### 恢复路径（刷坏怎么办）
1. **USB-C 串口刷机始终可用**：本板 console 在 **USB-Serial-JTAG**（`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`），
   设备名 `/dev/cu.usbmodem*`；即使触摸/LVGL 全坏，bootloader 与串口仍在，可直接
   `esptool ... write-flash "@flash_args"`（`build/flash_args`）。
2. **配网不需要触摸/按键**：无 SSID 自动进配网；已有 SSID 但 60 s 连不上也自动进配网（`wifi_board.cc:112/164`）。
3. **下载模式靠 BOOT 引脚**：按住 BOOT（IO0）再点 RST → 进 ROM 下载模式，与固件状态无关。
4. 若触摸驱动刷坏导致开机异常：**不用救 UI**，直接按第 1 条重刷上一版镜像即可（本卡改动只在板级两个文件，
   回退只需重刷备份 bin）。

## TF 卡冒烟测试（task-30，仅验证能否挂载，不接业务）

引脚来自厂商引脚表（只有 CMD/CLK/DAT0 三根）：
`SD_CMD=IO0`（同时是 BOOT strap）、`SD_CLK=IO43`（同时 UART0 TXD）、`SD_DAT0=IO44`（同时 UART0 RXD）
→ **只能 1-bit SDMMC**，没有卡检测脚 → 当常驻卡处理，不做热插拔。

### 行为（rev2：分阶段诊断）
- 板级构造函数末尾 `xTaskCreate(chd_sd)`（栈 4096、优先级 2）：**探测在独立任务里跑，卡慢/没卡都不影响启动**。
- **阶段 0 引脚体检**：三根线设成输入 + 上拉，打印电平 —— 上拉还读到 0 说明被卡/BOOT 按键电路拉住。
- **阶段 1–3 逐级打码**：`sdmmc_host_init()` → `sdmmc_host_init_slot()` → `sdmmc_card_init()`，
  每步打 `esp_err_to_name` + 十六进制码。`card_init` 成功 = 接线/时序没问题，随即打印 `sdmmc_card_print_info` 与容量。
- **两级频率 × 3 轮**：先 `SDMMC_FREQ_PROBING`(400 kHz)、再 `SDMMC_FREQ_DEFAULT`(20 MHz)，每次之间 `vTaskDelay(200 ms)`。
  400 kHz 过、20 MHz 不过 ⇒ 信号完整性；两级都不过 ⇒ 接线/供电/无卡。
- **分区表探测**：`sdmmc_read_sectors(card, buf, 0, 1)` 读扇区 0，判 0x55AA 魔数，打 `partition table (sector 0): PRESENT/ABSENT`
  —— 用来区分「卡正常但没文件系统」与「卡根本没应答」。
- 然后 `esp_vfs_fat_sdmmc_mount`（`format_if_mount_failed=false`，**绝不格式化**），失败单独打错误码 + `errno`。
- 挂载点 `/sdcard`；失败后继续正常当语音助手用（只 `ESP_LOGE`/`ESP_LOGW`）。

### 错误码怎么读（已核 IDF 6.1 源码）
- `esp_vfs_fat_sdmmc_mount` 返回 **`ESP_FAIL (0xffffffff)` ⇔ 卡初始化成功、但 `f_mount` 不认这个文件系统**
  （`components/fatfs/vfs/vfs_fat_sdmmc.c:109`：`res != FR_OK` 才置 ESP_FAIL）。
  卡初始化失败会**原样透传自己的错误码**（同文件 `:253` `sdmmc_card_init` + `sdmmc_card_init failed` 日志），
  所以「看到 ESP_FAIL 但没看到 sdmmc_card_init failed」= 接线 OK。
- **本固件不支持 exFAT**：`components/fatfs/src/ffconf.h:294` 是 `#define FF_FS_EXFAT 0`（IDF 6.1 里没有 Kconfig 开关）。
  ≥64 GB 卡出厂多为 exFAT → 必然 `ESP_FAIL`。**解决：在电脑上把卡格式化成 FAT32**（或用 ≤32 GB 卡），
  不要在设备上格式化（我们保持 `format_if_mount_failed=false`）。

### 实测记录
- 2026-10-28：插卡刷 rev1 → `CHD_SD: mount failed: ESP_FAIL (0xffffffff)`，无 `sdmmc_card_init failed`
  → 按上面口径，接线/时序 OK，问题在文件系统。
- **IO0 boot strap 风险实测未发生**：启动日志 `rst:0x15 (USB_UART_CHIP_RESET), boot:0x28 (SPI_FAST_FLASH_BOOT)`
  → 卡在复位瞬间没有把 CMD(IO0) 拉低，正常进 app、无 panic。
- 成功：打印 `sdmmc_card_print_info`（卡类型/容量）→ `esp_vfs_fat_info`（总量/剩余）→ 根目录前 10 项（名字+大小）
  → 写 `/sdcard/_sdtest.txt`（"ok"）→ 读回比对 → 删除。全部用 tag **`CHD_SD`**，一次打印不循环。

### 一键关掉（紧急开关）
文件顶部有：
```c
#ifndef CHD_SD_ENABLE
#define CHD_SD_ENABLE 1     // 改成 0 或 -DCHD_SD_ENABLE=0 即完全不碰 IO0/IO43/IO44
#endif
```
已在本地验证：`CHD_SD_ENABLE 0` 时构建 EXIT=0，`xiaozhi.bin` 回到 **2,805,088 B**（与加 SD 前逐字节同尺寸，
说明 FAT/SDMMC 全部没链接进去）。**若刷了 SD 版开不了机，用这个 0 版本重刷即可。**

### 已知风险（必读）
1. **IO0 是 boot strap**：复位瞬间若卡把 CMD 拉低，芯片会进下载模式 → 现象是"刷完不启动、串口只出
   `waiting for download`"或完全没有启动日志（不是 panic、没有 backtrace）。此时**不能用普通复位救**，
   需要按住 BOOT + 按 RST 进下载模式重刷（`CHD_SD_ENABLE 0` 版或备份镜像）。
2. **IO43/IO44 与 UART0 控制台冲突**：SD 驱动占用这两根后，**UART0 串口日志会乱码/中断**。
   本板 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`，所以日志仍能从 **USB-Serial-JTAG** 看，
   刷机/看日志请走 USB 口（`/dev/cu.usbmodem*`）。
3. 卡与摄像头**引脚并不冲突**：TF 用 IO0/IO43/IO44，摄像头用 IO9-14/21/38-42，两组不相交。
   厂商 readme 说的「复用」指两者都与 PZ 扩展排针复用。见 `docs/board-hardware-notes.md`。

## 字幕层（LVGL 变体：`chd-esp32s3-box-lcd35`）

9哥 要「带字幕的角色脸」→ 采用「角色层 + 独立文字层」（没有任何同类项目把字幕画进脸里），
字幕只在**自绘 kawaii 那一版**（`emote::EmoteDisplay` 那版根本没有字幕实现，它的
`SetChatMessage` 只打日志）。

### 像素布局（480×320）
```
y   0..228   脸 panel 480×228（kawaii face_sz = min(480,228) = 228，水平居中）
y 228..312   字幕条 464×84（单行约 44，`LV_SIZE_CONTENT` 向上长）
             圆角 12、黑底 60%、padding 6；文本区 452 宽、30px/4bpp、WRAP 居中
y 312..320   底部留白 8
```
- **脸不缩放，只位移**：`lvgl_kawaii_face` 在 `face_animation_init()` 里把
  `face_sz = min(parent_w,parent_h)` 写死，运行时改父容器不会重排，缩放要销毁重建（闪 + 重分配
  PSRAM）。所以面板尺寸一次定死 480×228，状态切换只动 y：**空闲 +46、说话 0**，
  `lv_anim` 160 ms ease-out；字幕条 `bg_opa` 0↔60% 同步。
- **字幕条与脸是兄弟容器**（都挂在 `screen` 下），脸的 30 ms 重绘不会连带字幕区刷新。
- 字幕条锚定 `BOTTOM_MID, y=-8` 且高度 `LV_SIZE_CONTENT` → 单行时向上收，脸不动（不跳）。
- 打字机：协议只在 `sentence_start` 给整句、无字级时间戳，所以按 **90 ms/字（≈11 字/秒）**
  逐字显示，读完停留约 **2 s** 后淡出（200 ms），脸回落到 +46。不做逐字卡拉 OK 变色（无时间戳对不齐）。
- 空文本不显示；`self.screen` 的 `SetHideSubtitle()` 开关保留（隐藏时暂停打字并让脸回落）。
  `system` 类提示（连接/OTA）走**上游原有的 `bottom_bar_`**（本板不再隐藏它），只有
  `user`/`assistant` 对白进字幕条 —— 这样开机早期的系统提示不会碰到字幕层的对象。

### 字形（M7-S1B rev2：改用运行期 assets 字体）
字幕 label 用**显示层当前的文本字体**：`Assets::MountAssets()` 会从 assets 分区加载
全量 cbin 字体（默认 `font_noto_sans_common_20_4`，6845 字）并通过 `Display::SetTextFont()`
装进主题；板级 override 了 `SetTextFont`，字幕 label 跟着换字体（`ActiveTextFont()`）。
- 优点：字体对象由框架持有（不是手写的 `lv_font_t` 副本），覆盖全（常用汉字不缺字），
  app flash **不增加**（`xiaozhi.bin` 相对加字幕前只 +2.2 KB）。
- 代价：字号是 **20 px**（assets 里现有字体），不是 9哥 想要的 30 px。
- 想要 30 px：需要把 `78__xiaozhi-fonts/cbin/font_noto_sans_common_30_4.bin`（2.6 MB）加进
  assets 并单独加载给字幕（改 `scripts/spiffs_assets/`）+ 板级用 `LvglCBinFont` 包一层；
  assets 分区 8 MB（现用 1.78 MB）放得下。**这需要扩写范围，另开卡做。**
- **不要**再用编译进固件的 `font_noto_sans_basic_30_4`：实测常用字（是/小/智…）**渲染成豆腐块**，
  而且它要求手搓 `lv_font_t`（副本 + fallback 指针），是 task-19 首次刷机 panic 的头号嫌疑。

### 真机验证
说话时：脸抬到 y=0、字幕条从 0 淡入到 60%，文字逐字出现，两行封顶；说完停 2 s 后字幕淡出、
脸回落。单行字幕时字幕条变矮、脸不动。`face_check.py` 拍到的截图里，字幕条应在 y≈228–312。

## 两种脸：官方动画表情（EAF）vs 自绘 kawaii

本板有两套可切换的脸，各自一个构建变体（**同一块 480×320 屏**）：

| | `chd-esp32s3-box-lcd35`（默认） | `chd-esp32s3-box-lcd35-emote` |
|---|---|---|
| 实现 | LVGL UI + 自绘 `lvgl_kawaii_face`（`CustomLcdDisplay`） | 官方 `espressif2022/esp_emote_expression`（`emote::EmoteDisplay`） |
| 表情 | 自绘 17 种 `face_emotion_t`，几何由代码画 | 官方 20 个 emote 名 → 10 个 EAF 动画（20 fps） |
| UI | 保留 LVGL 顶栏/状态栏/字幕/设置面板/手势 | **放弃 LVGL UI**：emote 接管 panel + flush_cb，自己画状态/消息区 |
| 触摸 | 注册给 LVGL（手势/滑块/设置页） | 不注册（emote UI 无触摸目标） |
| app 大小 | 2,806,704 B | 2,590,432 B（−216 KB，少了 LVGL UI 代码） |
| assets 分区 | 默认 assets（约 1.7 MB） | `expression_assets.bin` → `configured_assets.bin` ≈ **2.74 MB**（8 MB 分区的 33%） |
| 构建 | `python3 scripts/build.py chd/chd-esp32s3-box --name chd-esp32s3-box-lcd35 …` | `… --name chd-esp32s3-box-lcd35-emote …` |

### 构建开关（emote 变体）
`config.json` 里该变体追加：`CONFIG_USE_EMOTE_MESSAGE_STYLE=y`、`CONFIG_FLASH_EXPRESSION_ASSETS=y`
（并把 `USE_DEFAULT_MESSAGE_STYLE` / `USE_WECHAT_MESSAGE_STYLE` / `FLASH_DEFAULT_ASSETS` 显式置 n）。
另外 `main/Kconfig.projbuild` 的 `USE_EMOTE_MESSAGE_STYLE` 白名单已加入 `BOARD_TYPE_CHD_ESP32S3_BOX`
（原来只允许 ESP32-S3-BOX 系列等板子，不加这行 sdkconfig 里的 =y 会被 Kconfig 丢弃）。

### 表情名映射（重要，已按实机 pack 核对）
- 资产包由 `managed_components/espressif2022__esp_emote_assets/320_240/emote.json` 生成，
  **索引名是 20 个逻辑名**（不是 EAF 文件名），`emote_load.c` 用大小写敏感的 `strcmp` 查表。
- **`SetEmotion("happy")` 直接命中官方动画**（存的是逻辑名 `happy` → 内部用 `Happy.eaf`）；
  EAF 文件名的 `Happy/Sad` 大写与查找无关。
- 20 个官方名：`happy, laughing, funny, loving, embarrassed, confident, delicious, sad,
  crying, sleepy, silly, angry, surprised, shocked, thinking, winking, relaxed, confused,
  neutral, idle`。
- 其中若干是**借用**（没有独立动画）：`thinking→confused.eaf`、`surprised→Happy.eaf`、
  `winking→neutral.eaf`、`neutral→winking.eaf`、`idle→neutral.eaf`、`relaxed/loving/…→Happy.eaf`。
  所以「neutral」在官方包里其实是一个眨眼循环，而「idle」才是常规中性脸。
- 本固件额外会发、但官方包没有的名字，走板级别名（`ChdEmoteDisplay::EmotionToPackName`）：
  `listening/listen→idle`、`speaking/robot_2→neutral`、`working_hard→thinking`、
  `cool→confident`、`kissy→loving`、`warning→shocked`，其余未知名回落 `neutral`。
  （这是 **emote 变体**的 EAF 路由，本卡未改动；默认 lcd35 走下面的 kawaii 表。）

### 六状态 → kawaii 表情路由（C1a，默认 lcd35 变体）

状态→表情的**单一定义在服务端 persona**；固件只把收到的 emotion 名路由到现有 kawaii 表情，
不构成第二套状态机。实现：`CustomLcdDisplay::FaceEmotionFromName`。

| 状态 / emotion | kawaii `face_emotion_t` | 说明 |
|---|---|---|
| idle / neutral / 未知 | `FACE_NEUTRAL` | 未知默认与 idle 同帧 |
| listening / listen / happy | `FACE_HAPPY` | |
| thinking / confused | `FACE_CONFUSED` | 思考专用困惑脸 |
| speaking | `FACE_EXCITED` | 与 listening 的 HAPPY 区分 |
| working / working_hard / tool-running / **angry** | `FACE_WORKING_HARD` | 服务端 working 当前发 `angry`；“冒汗使劲”最贴切 |
| confirm-pending / barge-in / surprised / shocked | `FACE_SURPRISED` | confirm 与 barge-in 可同帧 |
| 其他旧名 | 原合理映射 | `winking→FACE_WINK`、`cool→FACE_COOL`、`sad→FACE_SAD` 等 |
| 未知 | `FACE_NEUTRAL` | 表末默认 |

- **alias 先于 native 概念不存在**：kawaii 只有这一张表；`angry` 必须映射到 `FACE_WORKING_HARD`（不是 `FACE_ANGRY`），
  这样 working 状态才有辨识度。
- `speaking=FACE_EXCITED`，而 listening 的 `happy` 是 `FACE_HAPPY`，两者不同脸。
- emote 变体仍用 `ChdEmoteDisplay::EmotionToPackName` 的 EAF 路由，本卡未改动、未切变体。

### 布局风险（已知，未改素材）
官方布局只有 `320_240` / `360_360` / `1024_600` 三套，**没有 480×320**。CHD 变体沿用默认
`EMOTE_RESOLUTION=320_240`（同为横屏、最接近），布局是**写死像素坐标**（`320_240/layout.json`：
`eye_anim` 在 left-mid +(10,30)），不做缩放/拉伸 → 480 宽的屏上右侧会留约 160 px 空白、
表情偏左上。这是素材布局决定的，不改素材、不在代码里硬调。

## 按键说明

### Boot 按键功能

#### 单击
- **配网状态**: 进入 WiFi 配置模式
- **空闲状态**: 开始对话
- **对话中**: 打断或停止当前对话

#### 双击 (需启用设备端 AEC)
- **空闲状态**: 切换 AEC 开启/关闭

## 常见问题

### 1. 为什么需要设备端 AEC？
设备端 AEC 可以在本地实时消除扬声器播放声音对麦克风的干扰，在播放音乐或 TTS 回复时仍能准确识别语音指令。

### 3. 如何恢复出厂设置？
按 Boot 按键 5次，设备会清除所有配置并重启。

## M7-S1 换脸 spike（persona face）

本板默认 UI 的脸被替换为 `lvgl_kawaii_face` 的程序化圆脸（会眨眼、可平滑换表情）。
只改显示层：音频链路（唤醒/ASR/LLM/TTS/打断）、分区表、协议栈均未改动。

### 接入点

- `chd_esp32s3_box_board.cc`
  - `CustomLcdDisplay::SetupUI()` → `SetupFace()`：父类 UI 建好后隐藏原生 emoji 层
    (`emoji_box_`)，在屏幕正中建 320x320 透明 panel 交给 `face_animation_init()`。
  - `CustomLcdDisplay::SetEmotion()` override：emotion 字符串 → `face_emotion_t`，
    未知名字回落 `FACE_NEUTRAL`。
  - `InitializeTools()` 增加 `self.screen.set_emotion`（**debug/spike only**：只为 M7 验收
    能按需切表情，Step 2 再决定是否长期保留）。
- 组件：`components/lvgl_kawaii_face/`（本地 vendored，MIT，改动见 `LOCAL_PATCHES.md`）。
- `main/CMakeLists.txt`：`PRIV_REQUIRES` 增加 `lvgl_kawaii_face`（工程开了 MINIMAL_BUILD，
  不 require 会被裁掉）。

### 内存

panel 短边 S=320（480x320 屏的短边，即能放下的最大脸）→ 眼 `0.45S=144 px`、嘴 `144x121`。
canvas 是 `LV_COLOR_FORMAT_RGB565`（stride = `w*2`、data_size = `w*h*2`）：
`w*h*2` → 眼 **41,472 B ×2** + 嘴 **34,848 B** = **117,792 B（115.0 KiB）**，全部在 PSRAM。
（上游原来按 `sizeof(lv_color_t)`＝3 B/px 申请，多 50%；rev4 已改成与 RGB565 自洽的
2 B/px，见 `components/lvgl_kawaii_face/LOCAL_PATCHES.md` #2b/#4。）

本地 fork 让 canvas 优先从 PSRAM 申请，内部 RAM 留给 Wi-Fi/AFE/WakeNet（正常路径 0 B）。
启动日志应为（下面是**预期格式**，字节数是按公式算出的，尚未上真机核对）：

```
I (...) face_anim: canvas buffer 41472 B -> <ptr> (PSRAM)
I (...) face_anim: canvas buffer 41472 B -> <ptr> (PSRAM)
I (...) face_anim: canvas buffer 34848 B -> <ptr> (PSRAM)
I (...) face_anim: canvas RGB565: eye 144x144=41472 B x2, mouth 144x121=34848 B (stride 288/288)
I (...) face_anim: Parent: 320x320, face_sz: 320, eye: 144px, mouth: 144x121px
I (...) Chd-Esp32s3-Box-Board: kawaii face ready: panel=320x320
```

带宽：30 ms tick 下 canvas 每秒重绘 117,792 B × 33.3 ≈ **3.9 MB/s**；算上 LVGL 合成
（读 canvas + 写 draw buffer + 送屏 DMA，各 ~115 KiB/帧）约 **15.6 MB/s**。
80 MHz octal PSRAM 理论带宽 160 MB/s，实测上限一般 40~80 MB/s → 纯画布 3.9 MB/s 约占 **5%**，
含合成的典型 15.6 MB/s 约占 **20%**，最坏（按 40 MB/s 下限）约 **39%**。这只是纯带宽估算，实际帧率/掉帧要 verifier
上真机用 log 校准。

### 布局与 z-order（320 下的取舍）

面部 panel 被 `lv_obj_move_to_index(…, 1)` 压到背景容器之上、其余 UI 之下，所以
**状态栏/图标/相机预览/字幕都在脸之上，不会被脸盖住**。反面代价是：组件把嘴钉在
panel 底边（`0.62S..S`），S=320 时嘴落在 y=198..319，而底部字幕条（约 30 px，
**无字幕时 hidden**）落在 y≈290..320 —— 说话中出字幕时会压住下巴。
取舍理由：320 是短边能放下的最大脸（480 宽的屏幕上左右各留 80 px 背景），缩到 300
只会让脸更小、且嘴仍贴底（依旧压 10 px），并没有换来干净布局；
而字幕只在有字幕文本时出现（无文本时 `bottom_bar_` 是 hidden），且字幕本身可由服务端下发
assets 配置里的 `hide_subtitle` 关掉（`main/assets.cc:428-434` → `LcdDisplay::SetHideSubtitle`），
截图时本来就不会有字幕条。若后续要「脸全屏 + 字幕常显」两者兼得，应改组件把嘴位置上移
（vendor 改动，Step 2 再议）。

### 默认 emoji 层必须一直被压住

父类 `LcdDisplay::SetPreviewImage(nullptr)`（`main/display/lcd_display.cc:1091`）会
`lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN)`，而这条路径由 5 s 预览定时器
（`lcd_display.cc:90`）在每次 `self.screen.preview_image` 之后触发。`emoji_box_` 的层级高于
face panel，所以只要不处理，一次拍照预览就会把原生 emoji 盖回脸中央直到重启。
本板因此 override 了 `SetPreviewImage()`：先持有递归显示锁委托父类，再重新 `HIDDEN`
（一个显示帧都不会露出 emoji）。

另：S=320 时组件算出的眼 canvas 相对 panel 左移 2 px（left_x=-2、right_x=178，各溢出 2 px
被父对象裁掉）。眼实际绘制宽度是 canvas 的 0.75（两侧各留 18 px 边距），所以 2 px 裁切
不丢任何眼睛像素。

### 表情映射

完整路由以 `CustomLcdDisplay::FaceEmotionFromName` 为准，见上文「六状态 → kawaii 表情路由（C1a）」表；
本文件不再维护第二张 kawaii 映射表，避免与 C1a 语义冲突。

### 验证方法

设备起来后调用 MCP 工具 `self.screen.set_emotion`（参数 `emotion=<名字>`）即可直接切表情，
无需等 LLM 输出；调用方式（端点/鉴权）沿用现有工具，见 esp32-voice-entry 仓库的
`docs/device-capabilities.md`，与本板其他 `self.*` 工具一致。

截图（`self.screen.snapshot`）的颜色 bug 已在 rev2 修掉：`LvglDisplay::SnapshotToJpeg`
不再对原生 RGB565 快照做多余的字节交换（原因见 `components/lvgl_kawaii_face/LOCAL_PATCHES.md` #2c）。
面板实时显示不受影响 —— 面板字节序仍只在 flush 时交换一次（`esp_lvgl_port_disp.c:739-742`）。

串口日志里应有的关键行：`canvas buffer ... (PSRAM)`、`Parent: 320x320`、
`kawaii face ready: panel=320x320`、`face emotion: <name> -> <id>`。

