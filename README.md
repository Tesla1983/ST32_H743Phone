# ymgui-h743 —— YMGUI 在 STM32H743VIT6 上的手机原型

将国产 GUI 库 [YMGUI](https://github.com/Yao-Mi/YMGUI) 移植到一块 STM32H743VIT6 小系统板，运行其自带的中文手机 Shell（`project_Demo/phone_shell`）：320×480 并口屏 + 电容触摸 + 16 MB QSPI Flash + 外挂 **ES8311 音频编解码器** + 外挂 **ESP32 网络/蓝牙协处理器**，裸机（无 RTOS），CMake + GCC 构建，全部资源上板实测。

项目详情见 [`docs/`](docs/)（排针对照 `docs/HEADER_PINOUT.md`、硬件图 `docs/hw_ref/`）；开发过程叙事见 [`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md)。

---

## 硬件目标板

| 部件 | 型号 / 参数 | 接法 |
| --- | --- | --- |
| MCU | **STM32H743VIT6**，Cortex-M7 @ 400 MHz，2 MB Flash / 1 MB SRAM | — |
| 显示屏 | **320×480 ST7796**，RGB565 | 8080 并口经 FMC（NE1，A19 作 RS） |
| 触摸屏 | **GT9xxx** 电容触摸 | I²C（**PB11=SCL / PB14=SDA / PB12=RST / PB13=INT**，按实测，非厂商文档） |
| 外部 Flash | **W25Q128**，16 MB | QSPI，memory-mapped XIP @ `0x9000_0000` |
| TF 卡槽 | Micro SD（无卡检测脚） | SDMMC1 4-bit：D0–D3 = PC8–PC11、CLK = PC12、CMD = PD2 |
| VBAT | BAT54C 切换 VCC3.3 / VBAT_IN（无后备电池 ⇒ 断电 RTC 归零） | — |

> 硬件权威为厂商资料包 `E:\BaiduNetdiskDownload\慧勤智远 STM32H743VIT6 V1.2小系统板\`（引脚/器件以 `2. 原理图\STM32H743VIT6 V1.2_SCH.pdf` 为准）。

---

## 外挂模块接线

**ES8311 音频编解码器**（I²S 从机，STM32 出时钟；控制走 I²C，7 位地址 **0x18**）

| STM32 | 复用 | 接模块 | 说明 |
| --- | --- | --- | --- |
| PB6 / PB7 | I2C1 (AF4, 开漏) | SCL / SDA | I²C 控制（板上无外部上拉，靠 MCU 内部 ~40 kΩ） |
| PC6 | I2S2_MCK | MCLK | 主时钟 |
| PD3 / PB9 | I2S2_CK / I2S2_WS | BCLK / LRCK | ⚠ **PB9 与 UART4_TX 复用**：音频激活时为 LRCK，空闲时还给 UART4 |
| PB15 / PC2 | I2S2_SDO / I2S2_SDI | DIN / DOUT | 放音 PB15→DIN；录音 DOUT→PC2 |
| 5V / GND | — | VCC / GND | 模块外接 5V，必插喇叭 |

**ESP32 网络 / 蓝牙协处理器**（STM32 本身无无线，WiFi/蓝牙都在 ESP32 上）

- **UART4 控制/上行**（921600 8N1，无流控）：`PB9=TX→GPIO22` / `PB8=RX←GPIO1`
- **SPI1 批量数据**（STM32 主 / ESP32 从）：`PA5=SCK` / `PA7=MOSI` / `PB4=MISO` / `PC5=CS(软件)` / `PC0=READY←GPIO21`

完整排针对照见 [`docs/HEADER_PINOUT.md`](docs/HEADER_PINOUT.md)。

---

## 软件构成

裸机主循环（无 RTOS）：自建 board 层（启动 / 双档堆 / 故障快照）+ STM32H7 HAL/LL + FatFs + YMGUI 库 + `phone_shell`（桌面 / 启动器 / 状态栏 / 16 应用 / 中文输入法）。只读大资源（GB2312 字模 982 KB、词组词典 1.3 MB 等）常驻 QSPI，memory-mapped 就地访问（XIP），零 RAM 占用。

---

## 构建与烧录

依赖：`arm-none-eabi-gcc 14.3.rel1`、`Ninja`、`CMake ≥ 3.20`、`probe-rs`、`pyserial`。

```bash
cd /e/ymgui-h743
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build --target ymgui-bin          # 生成 bin / hex 并打印尺寸

P="--probe 0416:5021:0123456789AB --chip STM32H743VITx"
probe-rs download $P build/ymgui-h743.elf
probe-rs reset    $P                            # ⚠ download 后必须 reset，否则内核 halt 不跑
```

> 构建目录必须叫 `build`（链接脚本 map 路径写死）；烧录走 SWD（FireDAP），不经 COM3。

---

## 固件能力

- **桌面 Shell**：三页桌面（滑动切换）、应用抽屉、最近任务、快捷下拉面板、状态栏、设置页（含滚动容器 + 滚动条，本工程新增）。
- **16 个应用**：任务 · 音乐 · 相册 · 设置 · 时钟 · 天气 · 笔记 · 计算器 · 电话 · 短信 · 联系人 · 录音 · 文件 · 陀螺仪 · 相机 · 浏览器。
- **中文输入法**：拼音 → 候选 → 上屏（单字 7291 + 词组 47280；词组词典常驻外部 Flash，零 RAM 索引）。
- **时间 / 天气**：经 ESP32（UART4）取 NTP 时间 + 天气，状态栏与天气 App 显示。
- **蓝牙收文件**：手机 SPP → ESP32 → SPI1 → 本板 TF 卡 → 图库（文件管理器「蓝牙接收」分类带进度条）。
- **音乐 / 录音**：走 ES8311（I2S2 TX DMA 放音 / I2S2 主收录音）。
- **图片库**：TF 卡 BMP/JPEG → RGB565 → 灌 W25Q128 → XIP 零 RAM 显示。

---

## 目录结构

```
src/          固件源码（裸机）；linker/ 链接脚本（DTCM/SRAM1/SRAM2/AXI 分区）
third_party/  第三方库（YMGUI / FatFs / 字体）
tools/        主机侧 Python 诊断/验收脚本（SWD 读写，无需重编译或复位）
docs/         设计 / 排针对照 / 验收文档（HEADER_PINOUT.md、hw_ref/ 等）
ci/           固件参考哈希 + 逐字节比对脚本
build/        构建产物（须叫 build，gitignore）
```

---

## 已知限制

- 相册 JPEG 仅支持 ≤320×320（硬件 JPEG 无缩放 ⇒ 超尺寸拒收，大图需先卡上预缩）。
- 蓝牙收文件 GET 偶发需重发（杜邦线 SPI 链路固有抖动，非软件缺陷）。
