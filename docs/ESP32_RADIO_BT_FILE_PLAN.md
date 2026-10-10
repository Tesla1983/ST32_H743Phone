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

## 0.5 实施进度（截至 2026-10-10 第二轮）

| 阶段 | 状态 | 关键结论 |
|---|---|---|
| **P0 使能 BT** | ✅ 完成 | ⚠ **偏离原计划**：原计划「双模 BTDM + SPP」，实测 `iram0_0_seg overflowed by 3912 bytes`（Bluedroid+WiFi 抢 IRAM）。改为 **经典蓝牙单模** `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y` + `CONFIG_BT_BLE_ENABLED=n`（sdkconfig 与 defaults 两处同改）。构建通过，bin `0x13d780`，app 分区余 **38%** |
| **P1 `bt_radio.c`** | ✅ 完成 | `bt_radio_enable()` 用官方宏 `BTDM_CONTROLLER_MODE_EFF`（单模时 = `ESP_BT_MODE_CLASSIC_BT`），**不再硬编码 `ESP_BT_MODE_BTDM`**（与单模构建冲突）；`bt_radio_disable()` 仍只 disable+deinit、**绝不 `esp_bt_mem_release()`**，内存保留以支持运行时反复开关 |
| **P2 SPI 无线电控制** | ✅ 代码完成 + 两工程构建通过 | 命令 `$?RADIO,<目标>,<ON/OFF>`（STM32→ESP32）+ 状态帧 `$RD,<wifi>,<bt>`（ESP32→STM32，开机与每次 `$?RADIO`/`$?RD` 后回报）；ESP32 `wifi_sta_enable/disable` + `bt_radio_enable/disable` 已接命令机；STM32 `uart_link.c` 解析 `$RD` 并暴露 `BoardNet_SetWifi/SetBt/RadioWifiOn/RadioBtOn/QueryRadio`；设置页新增**蓝牙卡** + WiFi 开关改真命令 + 开关由 `$RD` 回流刷新（app_tick 比对 `g_net_rd_pkts`） |
| P3 SPP 收文件 | ⬜ 未开始 | 见 §5 |
| P4 SPI 批量文件协议 + TF 写 | ⬜ 未开始 | |
| P5 STM32 UI 接收流程 | ⬜ 未开始 | |
| P6 收尾提交 | ⬜ 未开始 | |

> **部署状态（2026-10-10 第二轮）**：ESP32 已构建并烧录到 COM10（bin `0x13d780`，app 分区余 38%）；
> STM32 已构建并烧录（FLASH 1 637 452 B / 78.08%）。两侧均已在真机跑通，实测见下。

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
2. 修 SPI 对齐（**未做，治本**）：让主机每事务固定出 64 字节（当前 `real trans_len` 不是
   4 字节整数倍），彻底消掉那条 `spi_slave` 告警。⚠ 之前记的"已修掉"是误判，告警仍在。
3. ✅ 解析层兜底（已做）：`handle_line` 加 `解析结果: type='...' args='...'` 日志，
   并把 hex 打印从 24 字节放宽到 48 字节 —— 否则截断帧**静默丢弃**根本看不出来。

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

---

## 6. 决策点（2026-10-10 已确认）

1. **文件目的地**：✅ STM32 TF 卡（复用相册管线，ESP32 不落盘）。
2. **蓝牙 profile**：✅ 经典 SPP（手机/PC BT 串口即可，最简单）。
3. **「接收文件」用途**：✅ 进相册/图库（复用 `img_store` 图片管线）。

→ 设计闭环，进入 P0 实现。
