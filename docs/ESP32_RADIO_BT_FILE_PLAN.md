# STM32 控制 ESP32 WiFi/BT 开关 + 蓝牙接收文件 — 可行性报告与实施计划

> 日期：2026-10-10
> 依据：ESP-IDF v6.0.3 官方 API（`esp_wifi.h` / `esp_bt.h` / `esp_spp.h`）+ 当前两侧工程实际状态
> 范围：跨仓库。ESP32 固件在 `E:\workbuddy\esp32-com8`；STM32 固件/UI 在 `E:\ymgui-h743`。

---

## 0. 背景与现状（事实核对）

| 项 | 现状 |
|---|---|
| 传输层 | SPI1 业务链路已升格（2026-10-10）：STM32(SPI 主) ↔ ESP32(VSPI 从)，64B 全双工事务，`/64`=1.5625 MHz，命令机 + 下行帧均已验证通过 |
| ESP32 工程 | 仅 WiFi STA 上行（时间/天气/NTP）。**`sdkconfig` 未启用任何蓝牙**（无 `CONFIG_BT_*`）。**ESP32 侧无任何文件系统**（无 SPIFFS/FATFS 分区） |
| STM32 工程 | 手机 UI；TF 卡 FATFS + 图片/字体管线 + 相册 + 设置页（已有滚动条/开关控件与 `PhoneUI_text_if_changed` 刷新纪律） |
| WiFi 开关 | 不需要新硬件；ESP-IDF 支持运行时开关 |
| 蓝牙开关 | **当前固件根本没编进 BT**，必须先使能 BT 组件才能开关 |

---

## 0.5 实施进度（**全部完成**，2026-10-10 第三轮收尾）

| 阶段 | 状态 | 关键结论 |
|---|---|---|
| **P0 使能 BT** | ✅ 完成 | ⚠ **偏离原计划**：原计划「双模 BTDM + SPP」，实测 `iram0_0_seg overflowed by 3912 bytes`（Bluedroid+WiFi 抢 IRAM）。改为 **经典蓝牙单模** `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y` + `CONFIG_BT_BLE_ENABLED=n`（sdkconfig 与 defaults 两处同改）。构建通过，bin `0x13d780`，app 分区余 **38%** |
| **P1 `bt_radio.c`** | ✅ 完成 | `bt_radio_enable()` 用官方宏 `BTDM_CONTROLLER_MODE_EFF`（单模时 = `ESP_BT_MODE_CLASSIC_BT`），**不再硬编码 `ESP_BT_MODE_BTDM`**（与单模构建冲突）；`bt_radio_disable()` 仍只 disable+deinit、**绝不 `esp_bt_mem_release()`**，内存保留以支持运行时反复开关 |
| **P2 SPI 无线电控制** | ✅ 代码完成 + 两工程构建通过 | 命令 `$?RADIO,<目标>,<ON/OFF>`（STM32→ESP32）+ 状态帧 `$RD,<wifi>,<bt>`（ESP32→STM32，开机与每次 `$?RADIO`/`$?RD` 后回报）；ESP32 `wifi_sta_enable/disable` + `bt_radio_enable/disable` 已接命令机；STM32 `uart_link.c` 解析 `$RD` 并暴露 `BoardNet_SetWifi/SetBt/RadioWifiOn/RadioBtOn/QueryRadio`；设置页新增**蓝牙卡** + WiFi 开关改真命令 + 开关由 `$RD` 回流刷新（app_tick 比对 `g_net_rd_pkts`） |
| **P3 SPP 收文件** | ✅ 完成 | ESP32 `main/bt_spp.c`（经典 SPP 服务端，收字节流）+ `main/bt_file.c/.h`（文件会话管道：`OPEN`/`GET,<seq>`/`ABORT`/`TEST`，2 KB 一块 + CRC16，留"最后发出去那块"的副本供重传）。协议真源 = `main/bt_file.h` 顶部。**P4/P5 全程用它做自测源**（`$?BTF,TEST,<w>,<h>` 让对端合成 BMP，走与手机完全相同的下游管道）⇒ 验收不依赖真手机 |
| **P4 SPI 批量 + TF 写** | ✅ 完成（板验收 B0–B6 全 PASS） | `src/bt_recv.c/.h`（新）+ `uart_link.c` 解析 `$BT`/`$!RS,BTF`/`$!BD`。**100 KB 从 2 分钟降到 9~20 s**（固件自记 15 136 ms / 20 416 ms ⇒ 6.6 / 4.9 KB/s）。详见 §7 的两条真根因 |
| **P5 STM32 UI** | ✅ 完成（U1/U2/U3 全 PASS） | 文件管理器第 4 分类由「下载」改成**「蓝牙接收」**（`apps/files.c`）：进度条（`YMGUI_Bar`，建在 y=291）+ 开始/中止按钮 + 状态文字；`phone_shell_board.h` 加 `BoardBt_*` 转发。抓屏实测：空闲 0 像素 / 接收中 114-272（42%，"136/399 KB（43%）"）/ 完成满格 + "已收 bt_test.bmp（399 KB）" |
| **P6 收尾** | ✅ 完成 | 新增 `tools/bt_file_check.py`（P4 数据验收，B0–B6）与 `tools/bt_ui_shot.py`（P5 抓屏验收）；`ci/firmware_reference.sha256` 更新到**第十九版**（bin `bec473fa…` / elf `271500a5…`，`verify_board_image.py` 全量读回 1 643 648 B 逐字节一致）；两仓已提交 |

> **部署状态（2026-10-10 第三轮，全部落地）**：ESP32 bin `0x13fb80`（app 分区余 38%）已烧 COM10；
> STM32 **FLASH 1 643 648 B（78.37%）** 已烧并 reset。两侧均已在真机跑通，实测见下。

### ★ 第三轮新增：P4/P5 的真根因与残余问题（摘要，细节见代码注释）

**根因①：ESP32 从机的"有效事务深度"不由 `queue_size` 决定，而由"每完成一笔、任务补排一笔"维持。**
IDF 的 `spi_intr()` 只在 `trans_queue` 非空时才给硬件装配下一笔；掏空即 `cur_trans=NULL`
—— 此后主机那一笔事务**连中断都不产生**，对从机**完全不可见**（`s_rx_n` 不涨、`空事务` 也不涨）。
而解析一条 `$?BTF,GET` 要读文件块 + 往 16 KB 出站环灌 2 KB，是**毫秒级**的。
原顺序（先解析、后补排）让硬件整段裸露 ⇒ 约 **1/3 的 GET 凭空消失**
（STM32 侧表现为"GET 无应答、白等 2 s 再重发"，正是 100 KB 要跑 22~44 s 的真因）。
⇒ 修法：把"memcpy 拷出 rx → `slave_fill_queue(k)` 补排"提到解析之前，解析改用栈上副本。
⚠ 配套坑：`out` 就是 `&s_trans[k]`，而补排里 `memset` 了它 ⇒ **`trans_len` 必须在补排之前读**，
否则每一笔都被当成 0 bit 的空事务（实测症状：`空事务 8192 / 收下命令 0`，整条链路哑掉）。

**根因②：GET 超时必须分两档，单档把两种情形混在一起了。**
· 一个下行字节都没有 ⇒ GET 丢了 → 300 ms 就重发；
· 有字节在下行 ⇒ 块正在搬，**绝不能重发**（对端会把同一块再灌一遍，界进按长度组装的
  负载中间 ⇒ 那一块必 CRC 坏）。
实测：单档 2000 ms ⇒ 22~44 s；单档 500 ms ⇒ 13~16 s 但每轮多 1 个坏块；**两档 ⇒ 9~20 s 且坏块 0**。

**⚠ 残留（未解决，如实记录）**：GET 仍有 6%~20% 需重发一次。已逐一排除 ——
等待窗口（1500/3000 ms 一样丢）、SCK 速率（3.125 MHz **更差**、0.781 MHz 更慢，
1.5625 MHz 最优 ⇒ 不是越快越好）、CS 抬升宽度（3/9/22/60 µs 无趋势）、
从机补排顺序（已修，把丢命令从 38% 降到 ~15%）。
⇒ 结论：属**杜邦线 SPI 链路的固有抖动**；代价已压到每笔 300 ms（原 2000 ms）。

### 真机实测结论（SWD 直读 + COM10 日志，2026-10-10 16:30–16:55）

| 项目 | 结果 | 证据 |
|---|---|---|
| `$RD` 状态帧回流 | ✅ | `g_net_rd_pkts` 从 0 涨到 31；开机相位 `CMD_PH_RADIO` + 30 s 周期重查都生效 |
| WiFi 关 | ✅ | `g_net_radio_wifi` 1→0，`$RD` 帧数 18→19 |
| WiFi 开（恢复） | ✅ | `g_net_radio_wifi` 0→1，19→20 |
| 蓝牙开 | ✅ 稳定 | 对端日志 `解析结果: type='?RADIO' args='BT,ON'` → `RADIO 命令: BT ON → rc=ESP_OK` → `$RD bt=1`（`mode=0x02` = CLASSIC_BT 单模正确） |
| 蓝牙关 | ⚠ **受传输层丢帧影响，不稳定** | 单独跑通过一次：`disable 1/4~4/4` 全 `ESP_OK`、栈余量 2504 B、`$RD bt=0`；但多数尝试命令根本没到对端 |

### ⚠⚠ 根因：SPI 从机 DMA 未按 4 字节对齐 ⇒ 命令帧被截断丢弃

对端持续告警（每次都能在日志里看到）：
```
W spi_slave: Use DMA but real trans_len is not 4 bytes aligned, slave may loss data
```
铁证（COM10 抓到的原始行，**命令尾部被零填充吞掉**）：
```
收到命令行(36 字节) hex=24 3F 52 41 44 49 4F 2C 42 54 2C 4F 4E 00 00 00 00 00 24 3F 57 45 41 ...
                        $  ?  R  A  D  I  O  ,  B  T  ,  O  N  ← 缺 "*79\r\n"，被 5 个 0x00 取代
```
环形缓冲里反复出现 `24 00 00 00 ...`（`$` 后全零）＝ 命令只剩起始字节。
⇒ 帧被截断后 XOR 校验尾丢失 ⇒ `handle_line` 直接丢弃（**静默，不打日志**，排查极难）。

**为什么 WiFi/周期命令看起来正常**：天气、`$?RD` 都是周期性反复发的，丢一次下一轮补上；
而开关是**一次性命令**，丢了就丢了。这也解释了「BT ON 能通、BT OFF 常常不通」的假象——
不是 ON/OFF 有别，而是**单次命令命中概率问题**（19 字节帧比 18 字节帧更容易被截断）。

**修法（按性价比排序）**
1. ✅ **命令重试（已实施）**：`BoardNet_SetWifi/SetBt` 登记"待确认的期望态"，`cmd_tick` 里
   用 `$RD` 回流的**真实状态**比对，没翻转就重发 —— `RADIO_RETRY_MAX=5` / `RADIO_RETRY_MS=500`
   （2.5 s 内发完 5 次）。判据是"状态有没有真的翻转"，不是"我发没发"。
   ⚠ 已达成期望态时不再重发（对端不会推新帧），验收脚本相应加了"本就是期望态即 PASS"。
2. ✅ **SPI 对齐/丢帧已治本（2026-10-10 第二轮）** —— 见下节「SPI 丢帧根治」。
3. ✅ 解析层兜底（已做）：`handle_line` 加 `解析结果: type='...' args='...'` 日志，
   并把 hex 打印从 24 字节放宽到 48 字节 —— 否则截断帧**静默丢弃**根本看不出来。

### ★ SPI 丢帧根治（2026-10-10 第二轮，用户要求"先修 SPI 对齐再往下走"）

**定位过程**（都是实测，不是推理）：
1. 在从机加诊断打印 `out->trans_len`：正常事务就是 **512 bit**（64 B×8），
   `%32==0` —— 说明"声明长度"本身没问题，异常是**偶发**的。
2. 在从机打印"带命令的事务"的 rx hex，抓到决定性证据：
   命令**完整无损**（`01 12 00 $?RADIO,BT,ON*79\r\n`，cmdlen=18）——
   说明正常路径完全正确，问题只在偶发事务上。
3. 把异常事务的 `trans_len` 打出来：**等于 0**（主机拉了 CS 却没出时钟），
   另有少量"非 0 但不是 32 整数倍"（尾部丢位）。两者都会让
   `rx` 尾部保留事务前的 `memset(rx,0)` 值 ⇒ **命令帧尾部变成 0x00**
   ⇒ XOR 校验尾丢失 ⇒ 对端静默丢弃（这就是"拨开关没反应"的真相）。

**三个根因 + 对应修法**（都在 ESP32 `main/spi_link.c`）：

| # | 根因 | 修法 |
|---|---|---|
| 1 | 从机"排一笔→等结果→处理→再排"中间有**空窗**，主机 5 ms 轮询不看从机状态照发 ⇒ 空窗内那一拍被截断 | **预排队 2 笔**（`SLAVE_PRIME`）：处理第 1 笔结果时第 2 笔已在硬件里等主机，处理完立刻补排，任何时刻都有货。队列深度 3→4 |
| 2 | **热循环里打 UART 日志**（每笔 `ESP_LOGW`）本身就是空窗来源 —— 加诊断后坏帧反而更多 | 只在**异常**时打点且限 8 条；周期性汇总从每 1024 笔放宽到 **8192 笔**（~40 s） |
| 3 | 判据太粗：原来要求整笔 512 bit 收满才喂命令，尾部丢几个填充位就整包丢弃 | 改成**按"命令区是否完整到达"判**：SPI 是移位寄存器，收到的 bit 一定从**头**开始连续，所以只要 `trans_len ≥ (3+cmdlen)×8` 就说明命令字节完整 ⇒ 就喂；命令区本身被截断才丢弃（等重试） |

主机侧（STM32 `src/spi_link.c`）配套：CS 建立/保持延时从 `tiny_delay`（~0.3 µs，
**比一个位周期 640 ns 还短**）换成 `cs_delay`（~6–8 µs，>10 个位周期），
新增 `g_spi_err` / `g_spi_err_code` 计数 HAL 传输失败笔数（用来判定空事务来源）。

**实测效果**（90 s 窗口，COM10 日志）：
- 截断告警 **16 条 → 1 条**；
- 命令区截断 **0**、空事务 **0**；
- `tools/radio_check.py` 连跑 **7/7 轮 ALL PASS(3/3)**（修前 7 轮只有 6 过）。

### 抽屉（控制中心）接线（2026-10-10 第二轮）

下拉抽屉里本来就有 WiFi / 蓝牙两个快捷瓦片，但都是**本地假开关**。已接成真：

- `phone_quick_builtin.c`：`builtin_active(id)` 统一取"当前是不是开的"——
  WIFI/BLUETOOTH 读 `BoardNet_RadioWifiOn/RadioBtOn`（**$RD 回流**，-1=未同步当关），
  其余项仍是本地联动状态；`toggle()` 对这两项改发 `BoardNet_SetWifi/SetBt`。
  `FEATURE` 宏的 `active` 也改走 `builtin_active(id)`（否则瓦片仍显示本地假状态）。
- `phone_shell.c`：`PhoneHost_SetWifi` 改成真命令、`PhoneHost_GetWifi` 读真实状态
  （原来在板级构建里是个**静默空操作**）。
- `phone_shade.c`：`PhoneShade_Inspect` 新增 `"wifi"` 条目（自检要按名字取）。
- 抽屉的刷新是天然的：`PhoneShade_Update` 每拍轮询 `feature->active()`，
  值一变就置脏重绘 ⇒ `$RD` 一到，瓦片颜色自动跟着变。

⚠ `phone_selftest.inc` 里原来那两条断言（"点一下 `PhoneHost_GetWifi()` 就变"、
"蓝牙可切"）**必须改**：它们要等对端 ESP32 回帧，而板级自检必须在 ESP32 不在线时也能过。
已改成确定性判据：① 点瓦片 ⇒ `g_cmd_tx` 增加（命令真的发出）；
② 直接写 `g_net_radio_wifi/bt` + `g_net_rd_pkts` ⇒ `PhoneShade_Inspect("wifi"/"bluetooth")`
跟随（瓦片显示跟随回流值）。

**验收**：新增 `tools/shade_radio_check.py`（SWD 注入拖拽/点击 + 抓屏比像素）：
```
S0 抽屉可开      全屏像素差 143260
S1 拨瓦片发命令  WiFi g_cmd_tx 0→4、蓝牙 4→5
S2 瓦片跟随 $RD  WiFi 关/开像素差 3976、蓝牙 3935
→ ALL PASS
```
⚠ 脚本两条踩坑：① 判"抽屉是否打开"要用**全屏**像素差（最初拿单个瓦片区域当判据，
得了 0 的假阴性）；② 必须先**收起**抽屉再测打开（本来就开着时"拖前/拖后"无差异）。
⚠ 探针读 `visible` 时取错了符号（0x20006798 不是 phone_shade 的 `visible`）——
**别靠猜符号，用像素差更可靠**。

### 重试后的复测（`tools/radio_check.py --wait 10`）
- **3 次重发**：3 轮里 2 轮 `ALL PASS(3/3)`、1 轮 BT OFF 未命中（≈2/3）。
- **5 次重发**（现行）：实测 **7 轮里 6 轮 `ALL PASS(3/3)`**（≈85%），
  含一次 `ALL PASS(5/5)`（含 WiFi 关/开）。**仍有偶发未命中** —— 重试只是把概率
  从"经常不灵"抬到"基本可用"，**不等于根治**。真要保证必须修下面第 2 条（SPI 对齐）。
- ⚠ 验收脚本的判据是"**$RD 帧数涨 + 目标字段翻转**"，只看 `g_cmd_tx` 涨不算
  （命令发出去 ≠ 对端执行了，这正是当初被静默丢弃时最容易误判的地方）。

---

## 1. 可行性结论

**可行。** 两个子功能相互独立，但共用「STM32 → ESP32 命令 + 状态回报」这一层：

1. **WiFi/BT 无线电开关**：ESP-IDF 提供完整的运行时开关 API，无需重新烧录即可随时开关。
2. **蓝牙接收文件**：经典蓝牙 **SPP** 是最简单的「任意字节流」通道——手机/PC 用标准 BT 串口即可发文件；接收后转发到 **STM32 的 TF 卡**（复用既有相册/图片管线），最贴合本架构。

---

## 2. 关键技术依据（ESP-IDF v6.0.3 官方 API）

### 2.1 WiFi 无线电开关
- 关：`esp_wifi_disconnect()` → `esp_wifi_stop()` → `esp_wifi_deinit()`
- 开：`esp_wifi_init(WIFI_INIT_CONFIG_DEFAULT())` → `esp_wifi_set_mode(WIFI_MODE_STA)` →
  `esp_wifi_start()` → `esp_wifi_connect()`
- 注意：WiFi 凭证存在 WiFi NVS，重连自动生效。`wifi_sta.c` 当前是一次性初始化，需重构为**可重入**的 `wifi_radio_off() / wifi_radio_on()`。

### 2.2 蓝牙无线电开关（关键坑）
官方**关闭顺序**（来自 `esp_bt.h` 文档示例）：
```
esp_bluedroid_disable();  esp_bluedroid_deinit();
esp_bt_controller_disable();  esp_bt_controller_deinit();
// esp_bt_mem_release(ESP_BT_MODE_BTDM);  ← 不可逆！运行时反复开关【严禁调用】
```
**开启顺序**（逆序）：
```
esp_bt_controller_init(&cfg);
esp_bt_controller_enable(ESP_BT_MODE_BTDM /* 或 ESP_BT_MODE_CLASSIC_BT */);
esp_bluedroid_init();  esp_bluedroid_enable();
```
⚠ **`esp_bt_mem_release()` 一旦调用就不可逆**——该模式内存永久释放，再也无法 `enable`。本功能是「运行时反复开关」，所以**铁律：只用 disable/deinit，禁止 mem_release**；下次 enable 重新 init 即可（多花几十 ms，但内存保留）。写进 `bt_radio.c` 头注释。
⚠ BT 与 WiFi 共存：ESP32 硬件支持 **BTDM（双模）**，WiFi+BT 同时开受官方支持，代价是多约 30–40 KB heap 与 RF 调度开销。

### 2.3 经典蓝牙 SPP（接收文件）
- 依赖：`CONFIG_BTDM_CTRL_MODE_BTDM` + `CONFIG_BLUEDROID_ENABLED` + `CONFIG_CLASSIC_BT_ENABLED` + `CONFIG_BT_SPP_ENABLED`。
- 流程：`esp_spp_init(ESP_SPP_MODE_CB)` → 注册回调 → 作为 **SPP Server** 等待连接 → 对端（手机/PC 的 BT 串口）连上后，数据以 **`ESP_SPP_DATA_IND_EVT`** 逐块送达（每块 ≤ ~990 字节），`esp_spp_write()` 回送。
- 适合「接收任意长度文件」：把 `DATA_IND` 的字节流直接管道化即可。

---

## 3. 架构方案

### 3.1 无线电开关（复用现有 SPI 命令层）
- 新增命令动词（STM32 经 `uart_link_feed`→命令机→`spi_link_cmd_enqueue` 已现成）：
  `?RADIO WIFI ON` / `?RADIO WIFI OFF` / `?RADIO BT ON` / `?RADIO BT OFF`
- ESP32 `uplink_cmd` 解析 → `wifi_radio_set(on)` / `bt_radio_set(on)`。
- ESP32 回报新状态帧 **`$RD,<wifi>,<bt>`**（0/1），STM32 设置页据此更新**真实**开关态（避免「发了但 HW 失败」不同步）。
- STM32 设置页加两个开关控件（WiFi / Bluetooth），改动走既有 `PhoneUI_text_if_changed` 纪律。

### 3.2 蓝牙接收文件（SPP → SPI → TF）
**建议目的地：STM32 TF 卡**（不是 ESP32 本地存）：
- 流程：STM32 设置页「接收文件」→ `?RADIO BT ON` → `?FILE OPEN <name> <size>` → ESP32 进入「文件接收会话」，SPP 收字节 → 经 SPI 批量通道流式转发给 STM32 → STM32 写 TF 扇区（复用 `img_store`/FATFS 经验）→ CRC 校验 → 相册可见。
- **SPI 吞吐瓶颈**：当前 `/64`=1.56 MHz、64B/事务、~5 ms 节拍，传文件偏慢。对策二选一或组合：
  - 批量传输期把 SPI 提到 `/16`(12.5 MHz) 或 `/8`(25 MHz)（H7+ESP32 都能吃下），控制帧仍可用低速；
  - 协议层加「文件块」命令（分片 + 序号 + CRC），与现有 64B 事务兼容（多事务拼一个大块）。
- ESP32 不落盘：字节到达即转发，峰值 RAM 仅环形缓冲几 KB。

---

## 4. 风险与对策

| 风险 | 对策 |
|---|---|
| BT 栈 RAM/Flash 占用（ESP32 当前 app 44%，有余量） | P0 先开 BT 编一遍，确认 Flash/heap 预算 |
| 运行时反复开关 BT 误调 `mem_release` 致不可逆 | 铁律：只用 disable/deinit，禁止 mem_release；写进 `bt_radio.c` 头注释 |
| `sdkconfig` 只改一处被覆盖（本项目历史坑） | **同时改 `sdkconfig` 与 `sdkconfig.defaults`** |
| WiFi+BT 共存 RF 干扰/稳定性 | 实测联调，必要时 BT 仅文件传输时开、用完即关 |
| SPI 文件传输慢 | 提速 + 分块协议；先验证 100 KB 级文件 |
| 重入 `esp_wifi_init/deinit` 偶发问题 | 先用临时 UART 命令单测 `wifi_radio_set`，再接 SPI |

---

## 5. 实施计划（分阶段，每阶段可独立验收）

- **P0 使能 BT（双模+SPP）**：改两个 `sdkconfig`；`idf.py build` 确认 Flash/heap；BT 默认开机仍 OFF。验收：编译过、app 分区余量充足。
- **P1 ESP32 `bt_radio.c`**：运行时 enable/disable（禁 mem_release）；先用临时 UART 命令单测开关。验收：`bt_radio_set(1/0)` 后 `esp_bt_controller_get_status()` 状态正确。
- **P2 SPI 无线电控制**：`?RADIO` 命令 + `$RD` 状态帧；STM32 设置页两开关。验收：脚本读到 `$RD` 翻转；屏幕开关与真实态一致。
- **P3 ESP32 `bt_spp.c`**：SPP Server 收字节流 → 文件接收会话管道。验收：手机 BT 串口发已知数据，ESP32 收全、CRC 对。
- **P4 SPI 批量文件协议 + STM32 TF 写**：`?FILE OPEN/SEND/DONE` + 分块 + 提速；STM32 写 TF。验收：发 100 KB 文件，TF 读回 CRC 一致、相册可见。
- **P5 STM32 UI**：「接收文件」流程 + 进度条 + 接相册。验收：抓屏脚本确认。
- **P6 收尾**：验证脚本 + 提交两仓（ESP32 改动不影响 STM32 CI 哈希；仅当 STM32 侧有代码改动时才更新 `ci/firmware_reference.sha256`）。

> **P3–P6 全部完成（2026-10-10 第三轮）**，验收命令与结果：
> · P4 数据：`tools/bt_file_check.py --snap 0 --wait 30 --burst 8` ⇒ B0–B6 全 PASS（连跑两轮）
> · P5 视觉：`tools/bt_ui_shot.py` ⇒ U1/U2/U3 全 PASS，产物 `build/bt_ui_{1,2,3}_*.png`
> · 出货形态核验：`tools/verify_board_image.py build/ymgui-h743.bin` ⇒ 1 643 648 B 逐字节一致
>
> ⚠ `bt_file_check.py` 有一条**测量纪律**（写在文件头，别省）：它的快照是"一次读整段
> DTCM（37 KB）"，实测**一次要 ~6 s**。期间主循环被拖慢一个数量级、还会把主机侧的命令
> 吃掉（SWD 停核几秒 = CS 一直低 ⇒ 从机把相邻两笔并成一笔）。
> ⇒ **判"通不通"一律先跑 `--snap 0`（触发后完全不碰 SWD，事后读一次）**；
> 耗时只看固件自记的 `g_bt_ms`，不看脚本墙钟。

---

## 6. 决策点（2026-10-10 已确认）

1. **文件目的地**：✅ STM32 TF 卡（复用相册管线，ESP32 不落盘）。
2. **蓝牙 profile**：✅ 经典 SPP（手机/PC BT 串口即可，最简单）。
3. **「接收文件」用途**：✅ 进相册/图库（复用 `img_store` 图片管线）。

→ 设计闭环，进入 P0 实现。
