# 核心板排针与 MCU 引脚对照表

> **数据来源（厂商一手资料，非推断）**
> - 引脚功能/连接：`1. 用户手册/STM32H743VIT6 Core Board V1.2_用户手册.pdf` 表 1.1（第 14-16 页）
> - 排针顺序：`2. 原理图/STM32H743VIT6 V1.2_SCH.pdf`，P1/P2 = `Header 23X2`
> - 串口复用：`5. 芯片资料/DS12110_STM32H743xI单片机数据手册.pdf` Table 9

## 关键结论

1. **排针引出 80 个 IO**（手册 §11：除晶振 PH0/PH1 外全部引出），走线顺序 = **芯片引脚号升序**，
   唯一例外是 PC15 补在最末。校验：80 个 IO 全升序，仅 `PE1(98) → PC15(9)` 一处回退。
2. **USART6 在 LQFP100 上只有 PC6/PC7 一组**；另一组 PG14/PG9 在该封装不存在（芯片 77~100 脚无 PG 口）。
   换脚就必须换串口外设，或者改用软件位打。
3. **PC6 与 PC7 在排针上相邻**（IO 序列第 48 / 49 脚）。PC7 已接好且下行已通，
   **PC6 就在它旁边那一个位置**，不需要去 CAMERA 座找。

## IO 序列（按排针排列顺序）

| 序列 | 引脚 | 芯片脚 | 官方功能 | 连接 | 串口复用 | 可用性 |
|---:|:---|---:|:---|:---|:---|:---|
| 1 | **PE2** | 1 | QSPI_IO2 | W25Q128 WP | - | 项目占用 |
| 2 | **PE3** | 2 | FMC_A19 | LCD1 RS | - | 板载占用 |
| 3 | **PE4** | 3 | DCMI_D4 | 摄像头 D4 | - | 摄像头（未插即空闲） |
| 4 | **PE5** | 4 | DCMI_D6 | 摄像头 D6 | - | 摄像头（未插即空闲） |
| 5 | **PE6** | 5 | DCMI_D7 | 摄像头 D7 | - | 摄像头（未插即空闲） |
| 6 | **PC13** | 7 | LED_EN | 摄像头 XCLK | - | 摄像头（未插即空闲） |
| 7 | **PC14** | 8 | RTC 晶振 | 32.768 kHz | - | 板载占用 |
| 8 | **PC0** | 15 | 普通 IO | / | - | 空闲 |
| 9 | **PC1** | 16 | 普通 IO | / | - | 空闲 |
| 10 | **PC2** | 17 | 模拟输入 | / | - | ? |
| 11 | **PC3** | 18 | 模拟输入 | / | - | ? |
| 12 | **PA0** | 22 | WK_UP 按键 | 按键 WK_UP / 待机唤醒 | UART4_TX | 板载占用 |
| 13 | **PA1** | 23 | KEY0 按键 | 连接按键 KEY0 | UART4_RX | 板载占用 |
| 14 | **PA2** | 24 | LED0 | 板载 LED0（红） | USART2_TX | 板载占用 |
| 15 | **PA3** | 25 | LED1 | 板载 LED1（绿） | USART2_RX | 板载占用 |
| 16 | **PA4** | 28 | DCMI_HSYNC | 摄像头 HREF | - | 摄像头（未插即空闲） |
| 17 | **PA5** | 29 | 普通 IO | / | - | 空闲 |
| 18 | **PA6** | 30 | DCMI_PCLK | 摄像头 PCLK | - | 摄像头（未插即空闲） |
| 19 | **PA7** | 31 | 普通 IO | / | - | 空闲 |
| 20 | **PC4** | 32 | CTP-INT_RTP-PEN | 触摸 | - | 板载占用 |
| 21 | **PC5** | 33 | 普通 IO | / | - | 空闲 |
| 22 | **PB0** | 34 | LCD_DC | LCD2 17 脚 | - | 板载占用 |
| 23 | **PB1** | 35 | LCD_BL | 背光控制 | - | 板载占用 |
| 24 | **PB2** | 36 | QSPI_CLK | W25Q128 CLK | - | 项目占用 |
| 25 | **PE7** | 37 | FMC_D4 | LCD1 D4 | UART7_RX | 板载占用 |
| 26 | **PE8** | 38 | FMC_D5 | LCD1 D5 | UART7_TX | 板载占用 |
| 27 | **PE9** | 39 | FMC_D6 | LCD1 D6 | - | 板载占用 |
| 28 | **PE10** | 40 | FMC_D7 | LCD1 D7 | - | 板载占用 |
| 29 | **PE11** | 41 | FMC_D8 | LCD1 D8 | - | 板载占用 |
| 30 | **PE12** | 42 | FMC_D9 | LCD1 D9 | - | 板载占用 |
| 31 | **PE13** | 43 | FMC_D10 | LCD1 D10 | - | 板载占用 |
| 32 | **PE14** | 44 | FMC_D11 | LCD1 D11 | - | 板载占用 |
| 33 | **PE15** | 45 | FMC_D12 | LCD1 D12 | - | 板载占用 |
| 34 | **PB10** | 46 | QSPI_CSN | W25Q128 CS | USART3_TX | 项目占用 |
| 35 | **PB11** | 47 | CTP-SCL_RTP-SCK | 触摸 | USART3_RX / UART5_TX | 板载占用 |
| 36 | **PB12** | 51 | CTP-SDA_RTP-MOSI | 触摸 | - | 板载占用 |
| 37 | **PB13** | 52 | CTP-RST_RTP-CS | 触摸 | - | 板载占用 |
| 38 | **PB14** | 53 | RTP_MISO | 触摸 | USART1_TX | 板载占用 |
| 39 | **PB15** | 54 | 普通 IO | / | USART1_RX | 空闲 |
| 40 | **PD8** | 55 | FMC_D13 | LCD1 D13 | USART3_TX | 板载占用 |
| 41 | **PD9** | 56 | FMC_D14 | LCD1 D14 | USART3_RX | 板载占用 |
| 42 | **PD10** | 57 | FMC_D15 | LCD1 D15 | - | 板载占用 |
| 43 | **PD11** | 58 | QSPI_IO0 | W25Q128 DI | - | 项目占用 |
| 44 | **PD12** | 59 | QSPI_IO1 | W25Q128 DO | - | 项目占用 |
| 45 | **PD13** | 60 | QSPI_IO3 | W25Q128 HOLD | - | 项目占用 |
| 46 | **PD14** | 61 | FMC_D0 | LCD1 D0 | - | 板载占用 |
| 47 | **PD15** | 62 | FMC_D1 | LCD1 D1 | UART8_TX | 板载占用 |
| 48 | **PC6** | 63 | DCMI_D0 | 摄像头 D0 | USART6_TX | 项目占用 |
| 49 | **PC7** | 64 | DCMI_D1 | 摄像头 D1 | USART6_RX | 项目占用 |
| 50 | **PC8** | 65 | SDIO_D0 | TF 卡 D0 | - | 项目占用 |
| 51 | **PC9** | 66 | SDIO_D1 | TF 卡 D1 | - | 项目占用 |
| 52 | **PA8** | 67 | LCD_RST | LCD1/LCD2 复位 | UART7_RX | 板载占用 |
| 53 | **PA9** | 68 | USART1_TX | CH340X RXD | USART1_TX | 板载占用 |
| 54 | **PA10** | 69 | USART1_RX | CH340X TXD | USART1_RX | 板载占用 |
| 55 | **PA11** | 70 | USB_D- | USB OTG | UART4_RX | 板载占用 |
| 56 | **PA12** | 71 | USB_D+ | USB OTG | UART7_TX / UART4_TX | 板载占用 |
| 57 | **PA13** | 72 | SWDIO | SWD 仿真 | - | 板载占用 |
| 58 | **PA14** | 76 | SWDCLK | SWD 仿真 | - | 板载占用 |
| 59 | **PA15** | 77 | SPI1_CS | LCD2 LT_SCS | UART7_TX | 板载占用 |
| 60 | **PC10** | 78 | SDIO_D2 | TF 卡 D2 | USART3_TX / UART4_TX | 项目占用 |
| 61 | **PC11** | 79 | SDIO_D3 | TF 卡 D3 | USART3_RX / UART4_RX | 项目占用 |
| 62 | **PC12** | 80 | SDIO_CLK | TF 卡 CLK | UART5_TX | 项目占用 |
| 63 | **PD0** | 81 | FMC_D2 | LCD1 D2 | UART4_RX | 板载占用 |
| 64 | **PD1** | 82 | FMC_D3 | LCD1 D3 | UART4_TX | 板载占用 |
| 65 | **PD2** | 83 | SDIO_CMD | TF 卡 CMD | UART5_RX | 项目占用 |
| 66 | **PD3** | 84 | DCMI_PWDN | 摄像头 PWDN | - | 摄像头（未插即空闲） |
| 67 | **PD4** | 85 | FMC_NOE | LCD1 RD | - | 板载占用 |
| 68 | **PD5** | 86 | FMC_NWE | LCD1 WR | USART2_TX | 板载占用 |
| 69 | **PD6** | 87 | DCMI_RST | 摄像头 RESET | USART2_RX | 摄像头（未插即空闲） |
| 70 | **PD7** | 88 | FMC_NE1 | LCD1 CS | - | 板载占用 |
| 71 | **PB3** | 89 | SPI1_SCK | LCD2 LT_SCK | - | 板载占用 |
| 72 | **PB4** | 90 | SPI1_MISO | LCD2 LT_MISO | - | 板载占用 |
| 73 | **PB5** | 91 | SPI1_MOSI | LCD2 LT_MOSI | UART5_RX | 板载占用 |
| 74 | **PB6** | 92 | DCMI_D5 | 摄像头 D5 | USART1_TX / UART5_TX | 摄像头（未插即空闲） |
| 75 | **PB7** | 93 | DCMI_VSYNC | 摄像头 VSYNC | USART1_RX | 摄像头（未插即空闲） |
| 76 | **PB8** | 95 | DCMI_SCL | 摄像头 SCCB SCL | UART4_RX | 摄像头（未插即空闲） |
| 77 | **PB9** | 96 | DCMI_SDA | 摄像头 SCCB SDA | UART4_TX | 摄像头（未插即空闲） |
| 78 | **PE0** | 97 | DCMI_D2 | 摄像头 D2 | UART8_RX | 摄像头（未插即空闲） |
| 79 | **PE1** | 98 | DCMI_D3 | 摄像头 D3 | UART8_TX | 摄像头（未插即空闲） |
| 80 | **PC15** | 9 | RTC 晶振 | 32.768 kHz | - | 板载占用 |

## 命令通道 TX 该怎么换脚

命令通道是查询-响应，要 TX + RX 两根。按推荐度排序：

| 方案 | TX | RX | 序列 | 外设 / IRQ | 官方功能 | 改动量 |
|:---|:---|:---|:---|:---|:---|:---|
| **A 首推** | PC6 | PC7 | 48 / 49 | USART6 / IRQ71 | 摄像头 D0 / D1（未插即空闲） | **零改动** |
| B | PB9 | PB8 | 78 / 77 | UART4 / IRQ52 | 摄像头 SCCB SDA / SCL（未插即空闲） | 换外设 + 向量表开槽 |
| C | PE1 | PE0 | 79 / 78 | UART8 / IRQ83 | 摄像头 D3 / D2（未插即空闲） | 换外设 + 向量表开槽 |
| D | PB6 | PB7 | 75 / 76 | USART1 / IRQ37 | 摄像头 D5 / VSYNC | 换外设，且 USART1 与 CH340 共用 |

**已被排除的组合**（别踩）：

- `PA2/PA3` = **板载 LED0/LED1**，不是空闲 IO（手册明确），不能当串口。
- `PD5/PD6` = FMC_NWE / DCMI_RST，FMC 在跑 LCD，占用。
- `PB10/PB11` = QSPI_CSN / 触摸，占用。
- `PC10/PC11`、`PD0/PD1`、`PD8/PD9`、`PE7`~`PE15` = SDIO / FMC，占用。
- `PB14` = RTP_MISO 触摸，占用（虽然带 USART1_TX 复用）。

## 兜底：只换 TX，用软件位打

命令通道的 **RX（PC7）已经通了**（`$DT` / `$WD` 正常收），所以其实**只需要换 TX 一根**：
保留 USART6 的 RX 完全不动，把发送改成任意空闲普通 IO 位打（19200~38400 波特）。
好处：不动 USART6、不动中断向量表、不受引脚复用限制。

可选空闲普通 IO：**PC0、PC1、PC5、PA5、PA7、PB15**（官方标“普通 IO 口”，板上无用途）。
代价：要写约 60 行 bit-bang 发送，并把命令通道波特率从 921600 降到 19200~38400
（命令帧只有几十字节，降速后约 10~20 ms 发完，对开机拉天气无感知影响）。
