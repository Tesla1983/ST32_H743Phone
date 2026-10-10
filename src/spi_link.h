#ifndef SPI_LINK_H
#define SPI_LINK_H

/* ===========================================================================
 * SPI1 主 —— STM32H743 ↔ ESP32（VSPI 从）**业务链路**
 *
 * 2026-10-10 由「自检通道」升级为**真正替换 UART4 的上行链路**：
 * ESP32 的时间/天气帧（$DT/$WD/$WF/$WX）与命令响应（$!RS/$!BD）改走 SPI，
 * STM32 的命令（$?PING/$?WEA/$?WGET）也改走 SPI。UART4 的代码保留作回退，
 * 但物理线已拆、默认不启用。
 *
 * 接线（与 ESP32 侧 main/spi_link.h 逐脚对应）
 * ---------------------------------------------------------------------------
 *   STM32H743（主 / SPI1）      ESP32 经典款（从 / VSPI=SPI3_HOST）
 *   PA5  SPI1_SCK   AF5   <-->  GPIO18  SCK
 *   PA7  SPI1_MOSI  AF5   <-->  GPIO23  MOSI
 *   PB4  SPI1_MISO  AF5   <-->  GPIO19  MISO
 *   PC5  CS（软件 GPIO）  <-->  GPIO4   CS
 *   PC0  ready 输入      <-->  GPIO21  ready 输出（从机"有数据待读"）
 *   GND                  <-->  GND
 *
 * ⚠ 关于 PB4（以及 PA15/PB3）—— H7 与 F1/F4 不一样：H7 的 HAL **没有**
 *   HAL_DBGMCU_DisableJTAG()。只要把 AFR 配成 AF5（SPI1）就从调试模块接管回来，
 *   SWD 仍在 PA13/PA14，probe-rs 不受影响。
 *
 * 业务事务协议（64 字节全双工，SPI 模式 0，master 每拍主动发起）
 * ---------------------------------------------------------------------------
 *   Master TX（STM32 → ESP32）：
 *     [0]    op: 0x00 = 无命令 / 0x01 = 有命令字节
 *     [1..2] cmd_len（LE uint16）：有效命令字节数，位于 [3 .. 3+len-1]
 *     [3 .. 3+len-1] 命令字节（一条 $?... 命令行的片段，可能跨多个事务）
 *     [rest] 0x00
 *   Slave  TX（ESP32 → STM32）：
 *     [0]    status: bit0 = 有数据（出站队列非空）
 *     [1..2] data_len（LE uint16）：有效数据字节数，位于 [3 .. 3+len-1]
 *     [3 .. 3+len-1] 数据字节（$DT/$WD/... 字节流的片段）
 *     [rest] 0xFF（填充；data_len 权威，忽略填充）
 *
 *   ⚠ 流水线延迟（SPI 从机全双工、无"先收后答"机会）：从机在事务 T 里回的
 *     data_len/数据，反映的是 **T-1 结束时**出站队列的状态；主机的命令字节在
 *     事务 T 里被从机收到，T 结束后从机才处理。这天然收敛——主循环高频轮询即可，
 *     命令往返约 1~2 个事务（<100 ms），对 PING/WEA/WGET 完全够。
 *
 *   ⚠ 字节喂入解析层走 **uart_link_feed()**：从机 data 字节与 UART 收到的字节
 *     共用同一套行重组 / $!BD 二进制模式 / parse_frame，传输层已解耦（见 uart_link.h）。
 *
 * Ready 线（PC0 ← GPIO21）：从机出站队列非空时拉高，提示主机来读；主机也会
 *   周期性轮询，ready 仅为加速提示（不依赖它也能工作）。
 *
 * 用法（SWD）
 * ----------
 *   g_spi_presc   分频（2/4/8/16/32/64/128/256），默认 64 ≈ 1.56 MHz（杜邦线先求稳）
 *   spi_link_poll(now_ms)  主循环每拍调用，按 ~5 ms 节拍跑一次事务
 * =========================================================================== */

#include <stdint.h>

/* 初始化（配 GPIO + SPI1）。失败只记 g_spi_init_rc，不影响其他模块。 */
int  spi_link_init(uint32_t baud_presc);

/* 主循环里周期调用：跑一次业务事务（发命令 + 收数据）。 */
void spi_link_poll(uint32_t now_ms);

/* SPI 是否接管了业务链路（init 成功即视为接管）。uart_link_cmd 据此决定命令走 SPI 还是 UART。 */
int  spi_link_active(void);

/* 把一条命令（已组好帧、含 \r\n）的字节入队，等 spi_link_poll 在事务里发出。
 * 返回实际入队的字节数（环满则截断）。 */
int  spi_link_cmd_enqueue(const char* text);

/* ---- 诊断量（全部 volatile，供 probe-rs 与 tools 下的 py 脚本直接读）---- */
extern volatile int      g_spi_init_rc;    /* 0=OK */
extern volatile int      g_spi_presc;      /* 当前分频（可写，改完下次事务自动重配） */
extern volatile uint32_t g_spi_hz;         /* 换算出的实际 SCK Hz */
extern volatile uint32_t g_spi_tx_n;       /* 已跑事务数 */
extern volatile uint32_t g_spi_rx_bytes;   /* 从机下行收到并喂给解析层的字节数 */
extern volatile uint32_t g_spi_tx_bytes;   /* 命令环里已发出的字节数（去往从机） */
extern volatile uint32_t g_spi_irq_level;  /* ready(PC0) 最近电平 */
extern volatile int      g_spi_last_code;  /* 最近一次事务结果：0=有数据收到 >0=空  <0=硬件错 */
/* 追"从机收到 trans_len=0 空事务"用（2026-10-10）：主机 HAL 传输失败的笔数与首个失败码。
 * 判据：本计数 > 0 ⇒ 空事务来自主机这次失败（HAL 没出时钟但 CS 已经动过）。
 * 若本计数恒 0 而从机仍报空事务 ⇒ 是 CS 侧电气/时序问题，不在 HAL 这一层。 */
extern volatile uint32_t g_spi_err;
extern volatile int      g_spi_err_code;
/* 一拍最多连跑几笔事务（2026-10-10 P4 加，为文件传输提速）。
 * 0 或 1 ⇒ 退回"每 5 ms 一笔"的老节奏（12 KB/s，够跑命令/状态帧）；
 * 默认 8 ⇒ 有活时（ready 高 / 命令环非空 / 上一笔有数据）连跑，≈2.7 ms/拍。
 * 运行期可写：SWD 写个 1 就能在不重烧固件的前提下回退。 */
extern volatile uint32_t g_spi_burst;
extern volatile uint32_t g_spi_cs_gap;   /* 连续事务之间 CS 高电平的空转次数（≈ n/133 µs）。 */

#endif /* SPI_LINK_H */
