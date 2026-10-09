# ymgui-h743 —— YMGUI 在 STM32H743VIT6 上的手机上原型

把国产 GUI 库 [YMGUI](https://github.com/Yao-Mi/YMGUI) 移植到一块 STM32H743VIT6 小系统板上，
跑它自带的中文手机 Shell（`project_Demo/phone_shell`）：320×480 并口屏 + 电容触摸 +
16 MB QSPI Flash，裸机（无 RTOS），CMake + GCC 构建，全部资源近距离地上板实测。

> **本 README 只说明"这个项目是什么、怎么构建、怎么验收"。**
> 阶段过程、每一轮优化的实测与结论、踩坑修复、决策依据，全部在
> **[`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md)**。

---

## 1. 硬件目标板

| 部件 | 型号 / 参数 | 接法 |
|---|---|---|
| MCU | **STM32H743VIT6**，Cortex-M7 @ **400 MHz**，2 MB Flash / 1 MB SRAM | — |
| 显示屏 | **320×480 ST7796**，RGB565 | **8080 并口经 FMC**（NE1，`LCD_RAM @ 0x6010_0000`，A19 作 RS） |
| 触摸屏 | **GT1158**（GT9xxx 系）电容触摸 | I²C，产品 ID 读出 `"1158"` |
| 外部 Flash | **W25Q128**，16 MB | **QSPI**，memory-mapped（XIP）@ `0x9000_0000` |
| FMC 内核时钟 | **220 MHz**（PLL2_R） | 现写时序 `ADDSET=2 / DATAST=4` |
| LCD 背光 | 定时器 PWM 调光 | 调暗不再占用 CPU 画像素 |
| 低速晶振 | **32.768 kHz（LSE，可用）** | `Y1` + 6 pF×2，接 PC14/PC15 ⇒ **已接 RTC**（`src/rtc_clock.c`） |
| VBAT | BAT54C 在 `VCC3.3` 与 `VBAT_IN` 间自动切换 | **板子未接后备电池** ⇒ **真断电**后备份域（RTC + BKP）归零；但按复位键 / SWD reset / 看门狗复位时 VDD 没断 ⇒ RTC 时间与 BKP 标记**都保持** |
| TF 卡槽 | Micro SD（**无卡检测脚**，只能靠 CMD0/CMD8 通信判断有没有卡） | **SDMMC1 4-bit**：D0–D3 = PC8–PC11、CLK = PC12、CMD = PD2，均 AF12 |

> TF 卡引脚来自厂商「实验26 SD卡实验」的 `Drivers/BSP/SDMMC/sdmmc_sdcard.h`，
> 与 V1.2 原理图（`SDIO_D0..D3 / SDIO_CLK / SDIO_CMD` 网络）一致，不是本工程推测。

> **硬件的权威来源是厂商资料包**，不是任何 md 记录：
> `E:\BaiduNetdiskDownload\慧勤智远 STM32H743VIT6 V1.2小系统板\`
> —— 引脚、有无器件、时钟源一律以 `2. 原理图\STM32H743VIT6 V1.2_SCH.pdf` 为准，
> 用法参考看 `3. 程序源码\2，标准例程-HAL库版本\…\实验N`。
> 本 README 与 `docs/` 只是二手整理，会漏会过时，与资料冲突时**回查资料**。

## 2. 软件构成

```
裸机主循环（无 RTOS）
 ├─ 自建 board 层：启动文件 / 双档堆 / 故障快照 / 各外设 port
 ├─ STM32H7xx HAL + LL（含 LL_FMC / SD / SDMMC）+ 厂商 BSP·SYSTEM（已转 UTF-8）
 ├─ FatFs（third_party/FatFs）：卡上文件系统的唯一入口，diskio 接在 SDMMC 轮询读写上
 ├─ YMGUI 库（third_party/YMGUI）：显示、触控注入、控件、绘制图元、字体
 └─ project_Demo/phone_shell：桌面 · 启动器 · 状态栏 · 快捷面板 · 16 个应用 · 中文输入法
```

关键数据布局：**只读大资源**上 QSPI 用 memory-mapped 方式就地访问（XIP），内部 Flash 只留代码与少量索引。

| 资源 | 位置 | 大小 |
|---|---|---|
| GB2312 字模（7672 字 × 128 B，16×16 4bpp） | QSPI 偏移 `0x000000` → `0x9000_0000` | 982 016 B |
| 记账扇区（字模 CRC，避免每次上电重灌） | QSPI 偏移 `0x000FF000` | — |
| 全量词组词典 `phrases_xip.bin` | QSPI 偏移 `0x300000` → `0x9030_0000` | 1 303 456 B |
| 拼音单字词典 `pinyin_gb2312.bin` | 内部 Flash `.rodata.ime_chars` | 116 350 B |

## 3. 目录结构

```text
E:\ymgui-h743\
├─ CMakeLists.txt                # 唯一的顶层构建脚本（库 + 厂商代码 + 板级代码一起编）
├─ cmake\arm-none-eabi.cmake     # 交叉编译工具链文件
├─ linker\stm32h743vit6.ld       # 链接脚本：显式划分 DTCM / AXI / SRAM1-4 / sbrk 区
├─ src\
│   ├─ startup_gcc.s             # GCC 版启动 + 向量表（CMSIS 只带 Keil 版，故自写）
│   ├─ board_startup.c           # .data 搬运 / .bss 清零 / _init·_fini 桩 / 自写 _sbrk
│   ├─ board_alloc.c/.h          # 双档堆 heap0(DTCM) / heap1(AXI)，对应 GY_malloc0/1
│   ├─ board_fault.c/.h          # 故障现场快照（CFSR/HFSR/MMFAR/BFAR），SWD 事后读
│   ├─ ymgui_port.c/.h           # GYdisp 初始化 + flush_cb + 全帧镜像缓冲 + 显示自检
│   ├─ touch_port.c/.h           # GT9xxx → YMGUI_Inject_Pointer / Inject_Tick
│   ├─ qspi_port.c/.h            # W25Q128：indirect 读写擦 + memory-mapped XIP + 验收
│   ├─ qspi_provision.c/.h       # 产线用：主机分块灌写 QSPI（默认不编入）
│   ├─ font_provision.c/.h       # GB2312 字库首次上电自灌 + 记账扇区校验
│   ├─ dma_bench.c/.h            # DMA/MDMA 搬运微基准 + DMA↔D-Cache 一致性台架
│   ├─ sd_card.c/.h              # TF 卡：SDMMC1 4-bit 轮询驱动 + 写读比对自检（L2-1/2）
│   ├─ fatfs_port.c/.h           # FatFs 的 diskio 适配 + 文件系统自检（L2-3）
│   ├─ img_store.c/.h            # 图库：TF 卡 BMP → RGB565 → 灌 W25Q128 + 索引 + XIP 直读接口
│   ├─ uart_link.c/.h            # ESP32 上行链路：USART6 收 $DT/$WD/$WF，自写 ISR（不走 HAL IT）
│   ├─ rtc_clock.c/.h            # 板载 RTC（LSE 32.768 kHz）：跨复位保持时间，$DT 到达时校准
│   └─ main.c                    # 主循环：各模块 tick + 产线动作调度 + 渲染
├─ tools\                        # 宿主机侧 Python 工具（读板上变量、抓屏、回归、灌库）
├─ ci\                           # 构建输入检查 + 固件可复现性闸门
├─ docs\                         # 专项报告与开发过程记录
└─ third_party\
    ├─ YMGUI\                    # YMGUI 仓库根（库本体在再下一级 YMGUI\）
    ├─ CMSIS\                    # 内核头（已从官方 CubeH7 补齐 cmsis_gcc.h）
    ├─ STM32H7xx_HAL_Driver\
    ├─ BSP\{LCD,LED,MPU}\        # 厂商外设驱动
    ├─ FatFs\source\             # FatFs（厂商「实验27」那份，只改了 ffconf.h）
    └─ SYSTEM\{sys,delay,usart}\ # 厂商系统层
```

## 4. 构建与烧录

**依赖**：`arm-none-eabi-gcc 14.3.rel1`、`Ninja`、`CMake ≥ 3.20`、`probe-rs 0.29.1`、Python 3 + `pyserial`。

```bash
cd /e/ymgui-h743
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
cmake --build build --target ymgui-bin          # 生成 bin / hex 并打印尺寸

P="--probe 0416:5021:0123456789AB --chip STM32H743VITx"
probe-rs download $P build/ymgui-h743.elf
probe-rs reset    $P
```

> ⚠ **`probe-rs download` 跑完内核仍然 halt**，不会自己跑起来。只 download 不 reset，
> RAM 里还是上一份固件的残余，用新固件的符号地址去读会得到一串莫名其妙的数据——
> 看起来像"新功能没生效"，其实是**压根没跑**。每次烧完都要跟一条 `probe-rs reset`。

> ⚠ **构建目录必须叫 `build`**：CMakeLists 里 linker map 的输出路径写死成 `${ROOT}/build/ymgui-h743.map`。

### 构建选项

| CMake option | 默认 | 作用 |
|---|---|---|
| `YMGUI_IME_PHRASES_XIP` | **ON** | 词组词典走外部 Flash XIP（需先灌库，见 `$6` 输入法一节） |
| `YMGUI_QSPI_PROVISION` | OFF | 编入 QSPI 分块灌库支持（一次性产线动作） |
| `YMGUI_XIP_BENCH` | OFF | XIP / 内部 Flash 读延迟台架（`micro.py` case 20~26） |
| `YMGUI_DIAG` | OFF | 渲染诊断计数/计时（明显拖慢帧率，仅供测量） |
| `YMGUI_PORT_FRAME_MIRROR` | OFF | flush 时同步 300 KB 全帧镜像（抓屏与 A2 需要） |
| `YMGUI_ANIM` | ON | UI 动画模块 |

出货形态 = 全部取默认值。

## 5. 固件能力

**桌面 Shell**：三页桌面（可滑动切换）、应用抽屉、最近任务、快捷下拉面板、状态栏、设置页。

**内置 16 个应用**：任务 · 音乐 · 相册 · 设置 · 时钟 · 天气 · 笔记 · 计算器 · 电话 ·
短信 · 联系人 · 录音 · 文件 · 陀螺仪 · 相机 · 浏览器。

**中文输入法**：拼音 → 候选 → 上屏，走真实软键盘路径。
单字 **7291** 条 + 词组 **47 280** 条；词组词典常驻外部 Flash，**零 RAM 索引**（按桶游标解码）。

```bash
python tools/ime_probe.py zhongguo xianzai zhongguoren
# [OK] 'zhongguo'     候选(4)=中国 中过 中果 中郭       上屏 '中国'
# [OK] 'xianzai'      候选(4)=现在 想在 先在 现在爱     上屏 '现在'
# [OK] 'zhongguoren'  候选(3)=中国人 中国仍 中国任      上屏 '中国人'
```

**文本光标**：下划线样式 + 闪烁，改在库里，两个文本控件共用；宽度取光标处那个字符（中文 16 px / 英文 9 px）。

**亮度**：定时器 PWM 硬件调背光，不占用 CPU 逐像素叠加。

**多语言**：中 / 英两套词条，运行时可切，选择落 QSPI 持久保存并带 CRC 校验。

**TF 卡 + 文件系统**（2026-10-08 打通，路线图 L2-1/2/3）：

- SDMMC1 4-bit @ **25 MHz**（`sdmmc_ker_ck` = PLL1_Q = 200 MHz，`ClockDiv=4`），
  **轮询模式**（CPU 搬 FIFO，不走 IDMA ⇒ 无 D-Cache 一致性问题），**硬件流控已开**。
- 卡识别：**SDHC/SDXC，61 069 312 块 × 512 B = 29.12 GB**，FAT32。
- 扇区级：512 B 与 8×512 B 写→读**逐字节失配 0**，连跑 15 轮全过；吞吐写 ≈1.7 MB/s、读 ≈4.1 MB/s。
- 文件级：FatFs 挂载 → 建「/YMGUI」→ 写 1024 B → 关 → 重开读回**失配 0** → 删除，整轮 194 ms。
- ⚠ 自检**不破坏卡上数据**：只挑卡尾部扇区，且"先备份 → 写图案 → 比对 → **写回原内容** → 再验证"。
- **非缓存 DMA 区已生效**（L3）：MPU 把 SRAM1 配成非缓存（TEX=001/C=0/B=0），
  判据是 `tools/dma_check.py` 用例② 失配由 **64 → 0** —— DMA 写完 CPU 不做任何 invalidate
  也能读到新值，一致性由 **MPU 保证**而不是靠调用方记得做。
- **IDMA 通路已打通**（L2-4，2026-10-08）：`python tools/sd_dma_check.py [--hs]`。
  判据三项全 0（IDMA 写→读失配 / 哨兵残留超基线 / 原内容恢复），连跑 5 轮 PASS。
  吞吐（4096 B）：读 轮询 0.90~1.15 ms → **IDMA@50 MHz 0.75~0.96 ms（≈5.5 MB/s）**；
  **写无收益**（卡内编程才是瓶颈）⇒ FatFs 日常仍走轮询：简单、无中断依赖。
- ⚠ **两条硬约束**（改 SD/DMA 代码前必读，都是上板实测踩出来的）：
  1. **SDMMC1 的 IDMA 访问不到 SRAM1/2/3**（ST AN5200）—— 它在 D1 域，只能访问 **AXI SRAM**；
     实测放 SRAM1 时一个字节都搬不动（TX_UNDERRUN / RX_OVERRUN，DCOUNT 只走 28/4096）。
     ⇒ IDMA 缓冲钉在 **AXI 尾部 8 KB = 0x2407E000**（链接脚本 `.bss_sd_dma`）。
  2. **MPU 是「编号越大优先级越高」**（ARMv7-M ARM DDI0403），不是编号小优先。
     ⇒ 想压住覆盖它的大 region 必须取更大编号：AXI 尾部 8 KB 的非缓存区用的是 **region7**。

**图片库：TF 卡 BMP → RGB565 → 灌 W25Q128 → XIP 直读显示**（2026-10-08 打通，相册已接真图）：

- 固件**自己**把卡上的 BMP 解码成 RGB565 裸数据，写进 W25Q128 的 `0x100000` 起图片区，
  相册再从 **XIP 直读**显示。PC 侧零参与（对比 `tools/img2rgb565.py` 那条需要接线灌库的路）。
- **流式分块**：`f_read` 一行 → 转 565 → 攒满一个 4 KB 扇区 → `qspi_write` → 丢弃。
  峰值 RAM 仅 **6 KB**（`s_blk` 4 KB + `s_line` 2 KB，在 SRAM1 的 `.bss_img` 段），
  **与图片尺寸无关** ⇒ heap1 那 115 KB 余量不再是约束，全尺寸 320×480 也放得下。
- **零 RAM 显示**：`YMGUI_DrawImg.h` 里 `GYimg.data` 是 `const GYpx*` ⇒ 像素可以指向
  `0x9000_0000 + offset`，整幅图留在 flash 里不进 RAM（面板 284×230 全尺寸 RGB565 =
  130 640 B，本来是放不进 heap1 余量的）。
- 支持 24/32 bpp、**bottom-up 与 top-down 都认**；输出一律 top-down。
  bottom-up 时输出块**从后往前写**，让源行区间在文件里单调递增 ⇒ **全程顺序读，零回退 lseek**。
- 图库索引单扇区：16 B/槽 × 256 槽，magic `0x4947`。槽位分配把整块索引 `memcpy` 到 RAM 再扫
  ⇒ 256 次 XIP 读降成 1 次。
- ⚠⚠ **最深的坑**：`qspi_write` **只退映射不恢复**，而在 indirect 模式下读 XIP 窗口会触发
  **精确 BusFault 升级 HardFault**（实测 `g_fault.kind=1`、`CFSR=0x8200`、`BFAR=0x9001_0000`），
  主循环直接卡死。**单次读不挂是假阴性**（连读上百次才现）。
  ⇒ 两条硬规矩：① 每次 indirect 写之后、下一次 XIP 读之前必须 `qspi_enter_mmap()`；
  ② 导入期间 `g_img_busy = 1`，主循环跳过渲染与触摸。

**ESP32 上行链路：NTP 时间 + 天气 → 界面**（2026-10-08 打通）：

- **接线**（2026-10-10 从 USART6 换到 UART4）：`PB9 (UART4_TX) → ESP32 GPIO22 (命令口 RX)`、
  `PB8 (UART4_RX) ← ESP32 GPIO1 (帧口 TX)`、共地。**921600** 8N1，无流控。
  **PB8/PB9 的 UART4 复用是 AF8**（PC6/PC7 的 USART6 才是 AF7，别搞混）。
  为什么换：PC6/PC7 在这块 LQFP100 上只有 USART6 一个串口复用，而它们在核心板是
  摄像头座 DCMI_D0/D1 专用脚，按"PC6 排针"找位置极易接错；PB9/PB8 在 80 脚排针的
  第 78/77 脚（相邻）且摄像头没插时空闲。**完整的 80 脚对照表见
  [`docs/HEADER_PINOUT.md`](docs/HEADER_PINOUT.md)**。
  ⚠ 换脚时踩过的坑：命令方向一度不通，最后查出是**那根杜邦线内部断了**（三条独立证据：
  环回 PB9↔PB8 收到 6 帧、对端心跳 0 字节、TIM4 输入捕获正对照 PB8 有边沿而 PB9 零边沿；
  ⚠ TIM4_CH3/CH4 在 PB8/PB9 上是 **AF2**，AF 号写错会得到假阴性）。
- **协议**（对端 `E:\workbuddy\esp32-com8`，NMEA 风格，`\r\n` 结尾）：
  `$<TYPE>,<字段...>*<HH>`，`HH` 是对 `"<TYPE>,<字段...>"`（不含 `$` 与 `*`）逐字节 **XOR**。
  - `$DT,<unix_ts>,<YYYY-MM-DD>,<HH:MM:SS>,<wday>` —— 时间帧，**10 s 一帧**。
    日期与时间**已按 CST-8 换算过**，板上直接显示，不再加时区。
  - `$WD,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text>` —— 天气帧（**默认城市**），**30 min 一帧**。
    `temp_x10` 是温度×10（200 = 20.0 ℃）；`code` 是天气数字码，**图标按码选，不比对中文**。
  - `$WX,<citykey>,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text>` —— **逐城**天气，
    是 `$?WEA,<城市码>` 的应答（天气 app 的多城列表走这条）。字段与 `$WD` 相同、
    只多一个**原样回传的城市码**，靠它对号入座（不靠"当前在问哪一城"）。
  - `$WF,<up>,<ip>,<rssi>` —— WiFi 状态，变化时发一次。
- **界面落点**：状态栏 `HH:MM`；桌面大时间 `HH:MM` + **冒号每秒闪一次**（秒位已去掉，
  拆成 小时/冒号/分钟 三个绝对定位 label，冒号闪成空串时不挤动两侧）；
  日期 `10月8日 星期四`；天气 `晴 / 20℃`；天气图标按 code 换（晴=纯太阳、雨=云+雨滴、
  雷=云+闪电、雾=霾=横条…）。控制中心顶部的时间/日期也走同一条链路。
- **状态栏 WiFi 图标**（2026-10-10）：按 `$WF` 的 `up`/`rssi` 画 WiFi 扇面 ——
  **≥−55 dBm 三格 / ≥−70 两格 / ≥−85 一格 / 更低零格**；未收到过 `$WF` 时只留底座圆点、
  已知断开时是圆点 + 最外一条弧，这两种"不可用"态一律画淡色，与已连的亮白一眼可分
  （否则"开机头几十秒还没收到首帧"会被误读成断网）。
  ⚠⚠ **语义**：**WiFi 射频在对端 ESP32 上，H743 本身没有无线** —— 本板是把 ESP32 当
  网络协处理器/网关用的，图标表达的是"经 ESP32 网关的联网状态"，不是本芯片的无线状态。
  ⚠ 状态栏右侧那半边**电池图标仍是装饰性的**（板上没有电池也没有电量计，永远满格）；
  原来那 4 格信号柱同样是**硬画的假信号**，本版已删掉换成这个接真值的 WiFi 图标。
  验收：`python tools/wifi_shot.py`（六态覆盖，`bright` 严格递增 `[26,44,56,64]`）。
- **天气 app 是真数据**（2026-10-10 改造）：原来城市/温度/天气三张表全是**写死的字符串**、
  副标题写着"离线示例天气"。现在整条链路补通了 ——
  本板 `$?WEA,<城市码>` → 对端 HTTP 取 sojson → `$WX,<城市码>,...` → 城市表 → UI。
  三个城市（杭州 `101210101` / 上海 `101020100` / 成都 `101270101`，**三个码都实测过 HTTP 200
  且 cityInfo.city 与名字一致**）**串行**问：对端的"待取城市"只是一个槽位的邮箱，
  连发会被回 `$!RS,WEA,0`，所以本板是"发一城 → 等 `$WX` 或 8 s 超时 → 再下一城"。
  开机序列（PING→WEA→WDATA）跑完才开始扫描，此后每 30 min 重扫一轮（与 `$WD` 的周期对齐）。
  - 界面：大区显示所选城市（城市名/温度/天气），另起一行 `湿度 69%  PM2.5 31  AQI 42`；
    下面三行是三个城市的真实天气；「切换城市」按钮在 3 城间循环。
  - 副标题按真实状态走：`等待网络数据` / `更新中 2/3` / `实时 HH:MM 更新`，
    **不再有"离线示例天气"**。没数据时一律显示占位串（`--` / `等待数据`），
    绝不拿 `0℃` 冒充"没取到"。
  - 验收：`python tools/weather_shot.py`（数据层直读 `s_city_wx` 判 `3/3`；
    视觉层连按 4 次「切换城市」抓 4 张，判据 = **前三张城市区域互异、第 4 张 == 第 1 张**
    （按 3 循环）。⚠ 比的是**城市区域**的哈希不是整图 —— 状态栏和副标题里有走时的时间，
    拿整图比会跨分钟误报）。
    ⚠ 图标坐标一律**现算**（读桌面 `order[]` + `PhoneLauncher_Place()` 公式），
    别写死：`order[]` 可被用户拖动重排，写死的坐标会"成功打开一个 app"、只是不是天气。
- **秒是板上自己递推的**：时间帧 10 s 才来一次，`current_clock()` 以最近一帧的 `HH:MM:SS`
  为基准、用板载毫秒时基走秒，否则屏幕上的秒会每 10 秒才跳一次。
  ⚠ 时基**不能**直接用 `DWT->CYCCNT`：32 位 @400 MHz 约 10.7 s 就回绕，`main.c` 另累了一个
  `uptime_ms`。
- ⚠⚠ **三个绕开厂商代码的决定**（改 USART 相关代码前必读，详见 `src/uart_link.h` 顶部）：
  1. **不能用 `HAL_UART_Receive_IT()`**。厂商 `third_party/SYSTEM/usart/usart.c` 强定义了
     `HAL_UART_RxCpltCallback()` 与 `HAL_UART_MspInit()` —— 这俩是**全局唯一**符号（不是
     per-instance）。MspInit 里 `if (huart->Instance == USART_UX)`（= USART1）⇒ 给 USART6 调
     `HAL_UART_Init` 时**时钟和 GPIO 一个都不会配**；RxCpltCallback 同样只认 USART1 ⇒ 开 IT
     接收的话收完一字节没人重启下一轮，链路一帧之后死。
     ⇒ 本工程自己配 GPIO/时钟/波特率（不走 MspInit），**自己写 ISR 直接读 RDR**。
  2. USART6 在 **APB2**（PCLK2 = 100 MHz），HAL 的 `UART_GETCLOCKSOURCE` 认 USART6 ⇒ 波特率算得对（921600 时整数分频取 BRR=109，实际 917 431，−0.45%，实测 FE/NE 增量为 0）。
  3. **中断向量要手工开槽**：启动文件原来是 `.rept 150` 全落 `Default_Handler`（死循环）。
     `USART6_IRQn = 71`，把 150 拆成 `49 + 1 + 21 + 1 + 78`（第一次拆是给 SDMMC1/IRQ49）。
- **对端会把日志和帧混在同一行发**（实测原文：
  `I (2146494) uplink: → STM32  时间 2026-10-08 23:22:58 | $DT,1791472978,...,*2E`）。
  解析器取行内**最后一个 `$`** 再取它之后的第一个 `*`，所以照样能正确提取 —— 别改成"取第一个 `$`"。

**设置页滚动容器 + 滚动条**（本工程新增，**上游 YMGUI 没有**）：

- **上游只有"滚动语义"，没有滚动条**。引擎提供对象级 `scroll_x/scroll_y`
  偏移（`YMGUI/OPOBJ/YMGUI_Obj.h:71`，子对象绝对坐标在 `YMGUI_Obj.c:290` 减去它）
  与 `ClipChildren` 子裁剪，`tests/test_scroll.c` 是官方单测；但整个 `YMGUI/` 目录里
  搜 `滚动条`/`scrollbar` **零命中** —— 会滚的 `List`/`EditView`/`TextView`/`Table`/`Grid`
  五个控件**都不画条**。上游要"条"时是拿 `Slider` 冒充（`project_Demo/video_stidio` 的时间轴）。
- **本工程做法**：`project_Demo/phone_shell/apps/settings.c` 自建滚动容器
  （开 `ClipChildren` + 用引擎的 `scroll_y`）+ **自绘 3 px 圆角灰色滑块**，
  滑块高度/位置由视口高与内容总高算出（见下方"几何"）。为什么不用现成的 `YMGUI_List`：
  它只能加"文字条目"（内部 `malloc` 一个 `GYitem_data` 挂上自己的 `itemDrawCb`），
  塞不进 `PhoneUI_panel` + 一堆子控件的卡片。
- **几何**：视口 `320×356`（app 视图区高 412 − 头部 56），内容总高 **491** ⇒
  `clamp` 上限 **135**；滑块高 = `356²/491 = 258 px`，可移动量 `98 px`。
- **事件转发**：库的 `sendEvent` **不向父冒泡**、`event_cb == NULL` **静默丢弃**
  ⇒ 必须**递归给子树每个对象**装 wrapper（卡片 `PhoneUI_panel` 没有 `event_cb`，
  否则按在卡片空白处怎么拖都不动）。wrapper 还要用 `SLOP + moved` 吞掉滚动误触的 `Clicked`
  （否则按住开关上拖会在松手时把开关翻掉）。
- **软键盘**：弹起时**收缩视口高度**，而不是让 IME 去挪控件
  （`PhoneIME_Show()` 的避让段混用绝对/局部坐标，在滚动容器里必然错位）。
- **验收**：数据 `tools/ribbon_check.py` ③（拖动 → `scroll_y` 变 → 滚到底恰好停在 clamp 上限）；
  视觉 `tools/scroll_shot.py`（在两态抓屏里量滑块几何，实测 `y=92..350` → `y=190..448`、
  高 258、下移 98 ⇒ PASS）。
- **完整参考**（几何算式、六个函数、六个坑、在新页面复用的步骤）：
  **[`docs/SCROLL_VIEW.md`](docs/SCROLL_VIEW.md)**。

## 6. 内存布局与硬性约束

```text
DTCM   0x2000_0000  128 KB   栈 + heap0 + 部分关键数据（最快，不放 DMA）
AXI    0x2400_0000  512 KB   _frame_buf 307200 B(0x24000000~0x2404AFFF) + heap1
                             + **尾部 8 KB(0x2407E000) = SDMMC1 IDMA 缓冲，非缓存**（MPU region7）
SRAM1  0x3000_0000  128 KB   **非缓存 DMA 区**（MPU region0，TEX=001/C=0/B=0）
                             + DMA 台架 + TF 卡备份缓冲 + **`.bss_img` 6 KB（图库流式缓冲）**
SRAM2  0x3002_0000  128 KB   常规可缓存数据
SRAM3  0x3004_0000   32 KB
SRAM4  0x3800_0000   64 KB
QSPI   0x9000_0000   16 MB   memory-mapped XIP（GB2312 字模 / 词组词典）
LCD    0x6010_0000           FMC NE1 + A19 作 RS
```

必须知道的几条：

- **抓屏 / A2 依赖全帧镜像**，而它在出货构建里默认关闭。`tools/*` 的抓屏脚本会自己
  写 `g_frame_mirror = 1`，不用手工开。
- **画面静止时 `YMGUI_Refresh` 因脏区为空直接返回**，镜像里留的是上一帧。
  抓屏前必须先写 `g_force_redraw = 1` 再等约 0.3 s（≥ 2 拍），否则抓到老帧。
- **`GY_MixPx` 在存储域混合**（先算加权再量化一次），避免系统性偏暗；`opa=255` 时
  结果与 `GY_ColorToPx` 逐位相同。
- sbrk 有自己的 16 KB 保留区，**不与 heap0 重叠**（重叠会造成极难排查的内存踩踏）。
- 被 `--gc-sections` 吃掉的变量：本工程开了 `-fdata-sections` + `--gc-sections`，
  只读不写的诊断全局会被删掉（`__attribute__((retain))` 在本机 ld 上无效），
  要保留就得在代码里真的读写一次。

## 7. 板上工具与验收

工具都在 `tools/`，一般用 SWD 读写全局变量，**不需要重新编译或复位**。

| 脚本 | 用途 |
|---|---|
| `tools/read_vars.py <符号…>` | 读一组全局变量（自动从 ELF 取地址、合并相邻读） |
| `tools/ime_probe.py <拼音…>` | 输入法端到端：拼音 → 候选 → 上屏 |
| `tools/flash_and_verify.py` | **完整回归**：烧录 + 语言三层验收 + A2 + IME + 光标 |
| `tools/heap_peak_check.py` | 双档堆峰值 |
| `tools/micro.py <case>` | 圆/半透明填充/位图搬运等图元微基准 |
| `tools/bench.py <帧数>` | 满负载整帧基准（强制每帧整屏标脏） |
| `tools/grab_ymgui.py` `tools/frame_grab.py` | 读 `_frame_buf` 还原 PNG |
| `tools/caret_check.py` | 光标形状与闪烁周期（差分两帧 + 设备端计时） |
| `tools/lang_check.py` | 中/英切换：交互生效 · 落盘 CRC · 掉电重启保持 |
| `tools/verify_board_image.py` | 把板内 flash 全量读回，与本地 `.bin` 逐字节比对 |
| `tools/check_ribbon_equiv.py` | 两种壁纸算法逐像素等价性（主机侧） |
| `tools/dma_check.py` | DMA ↔ D-Cache 一致性三用例台架 |
| `tools/sd_check.py [--repeat N]` | TF 卡扇区级自检：卡识别 + 写→读逐字节比对（**默认要连跑几轮看稳定性**） |
| `tools/fs_check.py` | FatFs 自检：挂载 + 建目录 + 写文件 + 重开读回比对 + 删除 |
| `tools/sd_dma_check.py [--hs]` | SDMMC1 **IDMA** 通路验收：轮询 vs IDMA 吞吐对比 + 中断链路计数 + 非缓存区证据 |
| `tools/sd_restore.py` | 自检失败后**把卡上被改写的扇区还原**（用 `build/sd_bss_sd.bin` 里的备份） |
| `tools/img2rgb565.py` | 图片 → **RGB565 裸数据**（供 W25Q128 XIP 直读，`GYimg.data` 是 `const GYpx*` ⇒ 零 RAM 开销）。`--selftest` 跑纯色探针自检 |
| `tools/img_check.py` | **图库全链路验收**：XIP 冲突探针 + 造测试 BMP 写卡 + 触发固件导入 + **XIP 读回与主机侧独立参考逐字节比对** |
| `tools/gallery_shot.py` | 相册页**抓屏**验收：打开相册 → 抓屏 → 点「下一张」→ 再抓屏（附 caption 裁剪，看中文有没有被截断） |
| `tools/scroll_shot.py` | **设置页滚动条**视觉验收：顶部态/底部态各抓一帧，在 `x=315` 列量滑块几何并与算式对账（数据侧在 `ribbon_check.py` ③） |
| `tools/uart_check.py` | **ESP32 上行链路验收**：① 解析自检（喂三行实测样本，与线无关）② 线上等 N 秒看 `g_uart_rx_bytes` 涨不涨、`FE` 是否为 0 ③ 打印最后一帧原文 |
| `tools/cmd_link_check.py` | **命令通道在板验收**（本板 → 对端 ESP32 方向）：读 `g_cmd_tx/g_cmd_rx` 判双向通；`--ping` / `--weather` / `--wget` 可手动触发，`--wget` 还会把收到的 2 KB 负载与 **PC 抓的同一 URL** 逐字节比对 |
| `tools/clock_shot.py` | **NTP 时间/天气上屏抓屏**：回首页 → 读链路状态 → 抓屏并裁出状态栏/大时间/日期/天气/图标五块放大。`--blink` 验冒号闪烁，`--icons` 逐个写 `g_net_code` 验图标分派 |
| `tools/files_check.py` | **文件管理器接真实目录验收**：建 `/YMGUI/PIC`+`/YMGUI/NOTE` → 写一张测试 BMP → 扫描条目 0→1 → 从中导入 → 笔记含中文往返逐字节比对 |
| `tools/render_scale.py` | **渲染规模量化**：测各场景的峰值脏区面积与 `visit/draw` 次数 —— 判断置脏范围、裁剪/遮挡剔除类优化**值不值得做**的依据（读数用 `_max` 版，普通版只留最近一帧会被盖掉） |

### 验收判据与当前基线

| 编号 | 判据 | 实测（出货形态；各行取**最近一次**实测，日期见行内） |
|---|---|---|
| **A1** | 满负载整帧刷新 | **14.69 ms = 68.1 FPS**（2026-10-09 第九版，复位后干净态；60 帧整屏标脏、band=15 行、桌面场景，四次复现 14.64~14.69）。⚠ 早期记录的 **10.92 ms 已不可复现**：把第七版（无任何 JPEG 改动）重新编译烧录对照，实测 **5 856 496 cycles** vs 第九版 **5 874 638 cycles**，只差 **0.3%** ⇒ 那条基线是别的测量条件（band 行数/场景）下的值，**不是回归**。跨版本比帧时间前务必先确认 band 行数与页面 |
| **A2** | 显示通路逐像素自检 | `g_ramp_mismatch = 0`、`g_gram_mismatch = 0` |
| **A3** | 触摸识别 | `g_tp_pid = 0x31313538`（`"1158"`） |
| **A4** | XIP 与 indirect 读一致 | 比对覆盖全部 16 MB，失配 0 |
| **A5** | UI 交互 + 中文输入法可用 | 合成滑动/单击通过；IME 3/3 |
| **A6** | TF 卡扇区级写→读一致 | 512 B 失配 0、8×512 B 失配 0、原内容恢复失配 0（连跑 15 轮） |
| **A7** | 文件系统可用 | `f_mount` OK（FAT32）+ 写 1024 B 重开读回失配 0 + 删除 OK |
| **A8** | DMA ↔ CPU 一致性（非缓存 DMA 区生效） | `tools/dma_check.py` 用例②（DMA 写 → CPU 读**不做维护**）失配 **0 / 64**（改动前是 64/64） |
| **A9** | SDMMC1 **IDMA** 通路 | `tools/sd_dma_check.py`：IDMA 写→读 4096/4096 失配 0 + 哨兵残留 15−基线 15=0 + 原内容恢复 0，连跑 5 轮 PASS |
| **A10** | 图库全链路（BMP → W25Q128 → XIP 显示） | `tools/img_check.py`：导入 `rc=0`、96×64 → 12 288 B、耗时 262 ms，**XIP 读回 vs 主机侧独立参考逐字节失配 0**，索引条目一致；`tools/gallery_shot.py` 抓屏确认测试图正确显示且 caption 由 "1 / 2" 切到 "2 / 2 张卡上图片" |
| **A11** | ESP32 上行链路（USART6 收 NTP 时间 / 天气），**921600**（2026-10-09 提速） | `tools/uart_check.py --wait 20`：自检 3/3（$DT/$WD/$WF 各 +1、XOR 失败 0）；线上 `baud=921600`、`rx_bytes +204`、`frames +3`、`$DT +2`、**FE=0 NE=0**；70 s 稳定期 FE/NE/ORE/PE 增量**全 0**、漏字节 0；`tools/clock_shot.py` 抓屏确认状态栏 **18:41**、大时间 **18:41**、日期 **10月9日 星期五**、天气 **霾 / 25.9℃** |
| **A12** | 设置页滚动容器 + 滚动条（**本工程新增，上游无此控件**） | 数据 `tools/ribbon_check.py` ③：拖动 `scroll_y` 0 → 135 且滚到底**恰好停在 clamp 上限**、滚到底后关机按钮可点。视觉 `tools/scroll_shot.py`：滑块 `y=92..350`（顶部态）→ `y=190..448`（底部态），高 **258** px（= 356²/491）、下移 **98** px（= 356−258），与几何算式一致 ⇒ PASS |
| **A13** | 文件管理器接真实目录（照片 / 笔记） | `tools/files_check.py`：往 `/YMGUI/PIC` 写一个 .bmp 后扫描条目 **0 → 1**（证明列的是真实目录）、从中导入 `g_img_rc = 0` 输出 12 288 B、笔记含中文往返**失配 0**（30 B）；扫描一次耗时约 **1.0 ms** ⇒ PASS |
| **A15** | 相册 **JPEG 硬件解码**（分块 → RGB565 → W25Q128 → XIP） | `tools/jpeg_check.py`：240×180 → `rc=0`、86 400 B、平均误差 **3.51**、超差 **1.06%**；284×230 → `rc=0`、130 640 B、平均 **3.61**、超差 **1.44%**；640×480 → `rc=7 RC_TOO_WIDE` 正确拒绝。**平坦区**平均误差 2.72 / 2.73 且**零超差**（证明解码与色彩转换本身是对的，剩余差异只落在颜色突变处——最近邻 vs libjpeg fancy 上采样的固有差别）。**峰值 RAM ≈ 12 KB**（输入 4 KB + YUV 块 7.7 KB），而不是整帧 130 640 B |
| **A16** | **命令通道**（本板 → 对端 ESP32，把对端当「外挂 WiFi」用） | 对端三用例（`esp32-com8/tools/cmd_check.py`，**用 PC 冒充 STM32**，命令口临时切到 UART0：PC 经 CH340 驱动 GPIO3，单一驱动源、零 GPIO 风险）：`$?PING` → `$!RS,PING,1,4786*20`；`$?WEA` → `$!RS,WEA,1*42` 且随后 `$WD,259,43,76,105,17,霾*D2`；`$?WGET` → `$!BD,1,2048,DF73*32` + 2048 字节负载，CRC-16 一致且**与 PC 抓的同一 URL 逐字节相同**（总长 4106，按设计截断）。**免接线兜底**：对端每 60 s 重推缓存天气 ⇒ 实测本板**复位后 39 s** 拿到 $WD（此前最坏 1800 s）。⚠ 命令方向的线（PC6）尚未接，故 `g_cmd_tx=3 / g_cmd_rx=0`，见 §10 |
| **A14** | 渲染规模（判断优化值不值得做的依据） | `tools/render_scale.py`：桌面时间刷新峰值脏区 **1 452 px（0.9% 屏）**；拖动亮度滑块 **6 912 px（4.5% 屏）**；滚动设置页 **113 920 px（74%）**；每帧 `visit 74~1682 / draw 5~173` ⇒ 裁剪已挡掉 69~1509 次访问 |
| — | 故障计数器 | `g_fault.magic = 0`（从未发生内核故障） |
| — | 文本光标 | 16×2 下划线，周期 300 ms |

跑一遍 A2（不用重编译）：写 1 到 `g_gram_recheck`，等一帧后读那两个变量。

> ⚠ **帧率依赖场景**：同一份固件在不同页面会差很多（整表都按"重置后的桌面页"量）。
> 跨场景比数字没有意义，比之前先确认场景。
>
> ⚠ **堆峰值是开机以来的累计最大值**：先跑过别的操作（如 `lang_check.py` 的英文模式 +
> flash 落盘）再读会读到偏大的值。要干净基线就先 `probe-rs reset` 再单独测。
>
> 上电闪一下的彩色波纹（约 86 ms）是 **A2 的 ramp 自检图案**，不是花屏：它把整屏 GRAM
> 写成「像素 i = i & 0xFFFF」再读回比对，证明 8080 读通路可信，随后自动恢复画面。
> 该自检默认不随上电运行（`src/ymgui_port.h` 的 `YMGUI_PORT_BOOT_SELFTEST = 0`）。

## 8. 资源占用（出货形态）

```text
FLASH   1 630 292 B  / 2 MB   77.74 %   （含 FatFs + 图库 + ESP32 上行链路 + 命令通道 + RTC + 目录扫描；CP936 转码表已移除，见下）
DTCM       36 928 B  / 128 KB  28.17 %  （含 uart_link：512 B 环形缓冲 + 命令通道的 2 KB 负载缓冲 —— 无 DMA，放 CPU 私有区最快；笔记的 2 KB 正文缓冲）
SRAM1      31 456 B  / 128 KB  24.00 %  （TF 卡 4 KB 缓冲 + 图库 .bss_img：块/行 6 KB + JPEG 的 YUV 快照 + 扫描表 1.2 KB + DMA 台架 512 B）
SRAM2      62 000 B  / 128 KB  47.30 %  （IME 词典索引）
SRAM3       20 KB    /  32 KB  62.50 %
text 1 626 100 / data 4 184 / bss 670 976（bss 含 AXI 的 .bss_sd_dma 512 KB 段）
heap0 峰值  32 832 B /  52.0 KB  66 %
heap1 峰值  71 432 B / 204.0 KB  34 %   （复位后干净态；跑过导入/文件系统后会涨，那是累计值）
```

> **2026-10-09 已把 `FF_CODE_PAGE` 从 936 改成 437**，FLASH 1 802 908 → 1 628 780 B
> （85.97% → 77.67%，**省 174 128 B / 170 KB**）。消失的只有 `ffunicode.c` 里那张
> GBK 双字节转码表（1894~7350 行），`.data`/`.bss`/堆峰值均未变。
>
> ⚠ 代价不是"不能用中文名"，而是"**卡与 PC 的中文名不互通**"（板上实测，见 §10）：
> 437 下**新建 GBK 中文名能成功**，STM32 自己写→自己读**往返字节一致**，
> `f_open` / `f_unlink` 都能用；但 LFN 里存的是 CP437 映射后的码位
> （0xB7 → U+2556 制表符，而不是"风" U+98CE）⇒ 卡拔到 PC 上显示乱码，
> PC 建的中文名 STM32 也读不回原字节。本工程所有落盘路径本就是 ASCII，不受影响。
>
> 图库模块本身只占 **+4.5 KB FLASH**：像素数据全在 W25Q128 上，片上不存图。

## 9. 持续集成

`.github/workflows/firmware-build.yml`，**push 到 `main` 或 PR 到 `main` 触发**（也可手动 `workflow_dispatch`），
跑在 `ubuntu-latest`，工具链钉死 **arm-gnu 14.3.rel1**。

四个闸门：

1. **构建输入完整性** `ci/check_build_inputs.py` —— 两个 objcopy 进固件的 `.bin` 必须在库里且字节数精确相等；
2. **构建** `cmake -B build` + `ymgui-bin`；
3. **可复现性** `ci/check_firmware_hash.py` —— CI 产物与 `ci/firmware_reference.sha256`
   **逐字节比对**。那个参考哈希的来源不是随手一算：本机出货形态构建 → 烧录复位 →
   跑完 `flash_and_verify.py` 全套回归后，把 elf / bin 的 sha256 记下来，即是板上在跑的那份字节；
4. **产物留存**（`if: always()`，红灯也能下载排查），保留 30 天。

> 参考哈希只在三种情况下允许更新，都要在 commit message 里写清原因：
> ① 有意为之的功能改动；② 工具链版本升级；③ 构建参数调整。
> **没人改代码却变红 = 环境漂移，去查环境，不要改哈希。**
> 另外 `check_firmware_hash.py` 把**所有未注释行**都当参考项，换基准时记得把旧行注释掉。

## 10. 已知问题与未做项

| 项 | 说明 |
|---|---|
| `shijie` 打不出"世界" | 引擎打分问题：单字"是"频率 1 180 957，`composeQuality()` 对 2 段复合除 16 后"是接"仍 74 920 分 > 世界 30 808。改打分公式会连带影响其他输入，需单独一轮做回归比对 |
| GB2312/CJK 字模生成参数未覆盖验证 | CJK 用 `cell_w=16 / PT=16`（Noto Sans CJK），本机无该字体，只验证了 ASCII 那部分 |
| `InvalidateOldNew` API 未做 | **实测下来没有收益，故不做**（2026-10-09）：拖亮度滑块的峰值脏区只有 **6 912 px（4.5% 屏）** —— 滑块自己 `YMGUI_Obj_Invalidate` 脏的整块控件矩形已经覆盖了拖柄的旧位置与新位置；整屏重绘只在 `g_dim_mode==1`（软件调暗）时才发生，出厂是 PWM 模式。**唯一脏区大的场景是滚动**（113 920 px / 74%），而滚动要重画的是整块可见区，OldNew 解决不了（那是位块搬移/局部滚动的事）。判据与工具见 `tools/render_scale.py` |
| MDMA + 第二块 band buffer | **已做**（2026-10-04，本条是过时描述）：`buf1`(AXI) + `buf2`(SRAM3) **双 band 缓冲**，`flush_cb` 起 DMA（MEM2MEM 软件触发，FMC 无请求线）后立即返回，CPU 渲染下一条 band 与 DMA 推上一条重叠。A/B 实测 **14.71 ms vs CPU 直推 18.14 ms，快 3.43 ms（−18.9%）**，GRAM 回读一致，出厂默认开（`g_lcd_dma_ok=1`）。用的是 DMA 而不是 MDMA —— 对 FMC 这个场景 MDMA 没有额外收益 |
| 遮挡剔除 / 子树剔除 | **子树（裁剪）剔除已做**（`drawObjRecInner` 的 band 相交测试 + `ClipChildren` 收窄后完全在外即 return）。实测每帧 `visit 74~1682 / draw 5~173` ⇒ **已被挡掉 69~1509 次访问**。**遮挡剔除未做**，且实测余量很小：真正进 `draw_cb` 的只有 5~173 个对象，且大多不重叠，做它要引入 z 序与不透明标记，风险与收益不成比例 |
| IDMA 未接到 FatFs | L2-4 的**通路已通**（A9 验收过），但 FatFs 仍走轮询：实测 IDMA 对 4 KB 的写**没有收益**（卡内编程主导），读也只有 1.2~1.5×，不值得引入中断依赖。大文件场景（相册/音乐）真要做时再接 |
| 16 个 APP 仍是演示态 | 文件系统已通、**相册已接真图**（BMP → W25Q128 → XIP 显示）；笔记/短信/文件管理器仍要改成读写卡上的真实文件 |
| 相册 JPEG 只支持 ≤320×320 | **硬件 JPEG 没有缩放**（`CONFR0~7` 无 SCALE 位、DMA2D 也不能缩放）⇒ 超尺寸只能拒（`RC_TOO_WIDE`）。要支持大图得先在卡上预缩，或自己写降采样（CPU 做，代价另算）。解码本身已完成，见 A15 |
| ~~FLASH 85.32% 偏高~~ | **已解决**（2026-10-09）：`FF_CODE_PAGE` 936 → 437，FLASH 1 802 908 → **1 628 780 B（77.67%）**，省 174 128 B。代价见 §8 的说明——是"卡与 PC 的中文名不互通"，不是"不能用中文名" |
| 时间**真断电**后要重等首帧 `$DT` | 时间源：板载 RTC（LSE）+ ESP32 的 NTP 校准。**复位**（按键 / SWD / 看门狗）时 VDD 不断、备份域保持 ⇒ 立刻有正确时间；**拔电**则备份域丢失（板上无后备电池）⇒ 要等首帧 `$DT` 才可信，之前显示占位 `--:--`。天气帧 **30 min** 一帧，但**对端每 60 s 会把缓存的那一帧重推一次**（`CONFIG_UPLINK_WEATHER_REPUSH_S`，为命令通道接线前的兜底）⇒ 实测复位后 **39 s** 就有真实天气，不再干等半小时 |
| `$WD` 的 pm25 / aqi 收下了但没上屏 | 解析与诊断量都有（`g_net_pm25`/`g_net_aqi`），界面暂时只用了温度+天气文本+码。要展示空气质量得先设计放哪 |
| **命令方向（PC6）还没接线** | 命令通道的软件两侧都已实现并各自验收通过（对端三用例全 PASS），但实测板上 `g_cmd_tx=3 / g_cmd_rx=0` ⇒ **本板 → 对端这根线没接**。判定方法：把 PC6 临时切成带下拉的输入**被动读电平**（不驱动任何引脚），得 `PC7=1`（被对端 TX 空闲高电平驱动，说明方法有效）、`PC6=0`（没有东西在驱动它）。⇒ 补一根线 **PC6(USART6_TX) → 对端 GPIO16（板上标 RX2）** 即可，代码一行不用改（命令口默认已是 UART2）。**不要接 GPIO3** —— 那是板载 CH340 的 TX，两个推挽驱动会抢同一条线。接好前「开机立刻有天气」由对端的 60 s 重推兜底兑现（实测复位后 39 s 拿到） |
| ~~对端日志与帧混在同一行~~ | **已解决**（2026-10-09，改的是对端 `E:\workbuddy\esp32-com8`）：实测比"混行"更严重 —— PC7 上跑的从头到尾都是 ESP32 的 **console 日志流**（板上 `g_uart_line` 出现过完全不含 `$` 的纯日志行），时间是从日志行里抠出来的。对端已重写串口层：日志不再打印整帧原文 + `esp_log_set_vprintf()` 硬性接管日志到另一口，帧口只吐帧。现 STM32 收到的是 39 字节纯帧，稳定期错误增量全 0 |

## 11. 文档索引

| 文档 | 内容 |
|---|---|
| **[`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md)** | **开发过程全记录**：阶段进度、每轮优化实测、踩坑与修复、决策依据 |
| `docs/CI_REPRODUCIBILITY.md` | CI 与构建可复现性的建立过程与排查方法 |
| `docs/IME_PHRASE_DICT_FEASIBILITY.md` | 词组词典上外部 Flash 的可行性论证、取舍与落地结果 |
| `docs/FONT_GLYPH_CLIPPING.md` | 字母 W 渲染残缺的根因、证据与上板结果 |
| `docs/EXTERNAL_FLASH_OFFLOAD_FEASIBILITY.md` | 只读大资源外挂到 QSPI 的可行性论证与布局 |
| `docs/MEMORY_ARCHIVE.md` | 内存布局、历史结论与已归档论证 |
| **[`docs/SCROLL_VIEW.md`](docs/SCROLL_VIEW.md)** | **滚动容器与滚动条实现参考**：上游 YMGUI 只有滚动语义、**没有滚动条控件**；本工程设置页的自建容器 + 自绘条（几何算式、六个坑、验收、复用步骤） |
| **[`docs/TF_CARD_CACHE_ROADMAP.md`](docs/TF_CARD_CACHE_ROADMAP.md)** | TF 卡接入与缓存一致性的路线图（L1–L4）；**L1–L3 与 L2-1~L2-4 全部已完成** |
| `docs/BOOT_HANG_VOSRDY_LESSONS.md` | 上电卡死的定位过程与教训 |
| `docs/PREVIEW_BRIGHTNESS_ACCEPTANCE.md` | 亮度/预览的验收标准与目视结论 |
| `docs/SETTINGS_RIBBON_ACCEPTANCE.md` | 设置页 ribbon 外观的验收记录 |
| `ci/README.md` | 参考固件哈希的来源、更新规则与复现步骤 |
| `third_party/YMGUI/project_Demo/phone_shell/APP_GUIDE.md` | phone_shell 应用开发指南 |
