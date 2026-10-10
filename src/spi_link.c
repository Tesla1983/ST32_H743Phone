/* ===========================================================================
 * SPI1 主 —— STM32H743 ↔ ESP32（VSPI 从）业务链路
 * 协议 / 接线 / 判据见 spi_link.h 顶部长注释。
 *
 * 每拍由主循环按 ~5 ms 节拍发起一次 64 字节全双工事务：
 *   主发 [op][cmd_len][命令字节...]、从回 [status][data_len][数据字节...]；
 *   从机下行数据字节喂给 uart_link_feed()（与 UART 共用解析层），
 *   主机待发命令从命令环取出填入事务。不走 DMA / 中断（轮询），求确定性。
 * =========================================================================== */

#include "spi_link.h"
#include "uart_link.h"          /* uart_link_feed() */
#include "stm32h7xx_hal.h"
#include <string.h>

/* ---- CS / ready 的落点（与 spi_link.h 顶部的接线表一一对应）---- */
#define SPI_CS_PORT     GPIOC
#define SPI_CS_PIN      GPIO_PIN_5      /* PC5 → 对端 GPIO4(CS)   */
#define SPI_RDY_PORT    GPIOC
#define SPI_RDY_PIN     GPIO_PIN_0      /* PC0 ← 对端 GPIO21(ready) */

/* 业务事务长度（4 字节对齐、DMA 友好；> 命令/数据片段都够用） */
#define XFER            64u
#define CMD_RING_CAP    1024u           /* 命令环（2 的幂），容纳多条命令排队 */
#define CMD_RING_MASK   (CMD_RING_CAP - 1u)

static SPI_HandleTypeDef s_hspi;
static int               s_cur_presc = 0;

/* ---- 命令环：uart_link_cmd → 这里 → spi_link_poll 在事务里发出 ---- */
static uint8_t  s_cmd_ring[CMD_RING_CAP];
static uint32_t s_cmd_head;             /* 写（uart_link_cmd） */
static uint32_t s_cmd_tail;             /* 读（spi_link_poll） */

/* ---- 诊断量 ---- */
volatile int      g_spi_init_rc    = -1;
volatile int      g_spi_presc      = 64;      /* 默认 /64 ≈ 1.56 MHz：先求稳 */
volatile uint32_t g_spi_hz         = 0;
volatile uint32_t g_spi_tx_n       = 0;
volatile uint32_t g_spi_rx_bytes   = 0;
volatile uint32_t g_spi_tx_bytes   = 0;
volatile uint32_t g_spi_irq_level  = 0;
volatile int      g_spi_last_code  = -2;      /* -2=还没跑过 */

static uint32_t presc_to_hal(int p)
{
    switch (p)
    {
        case 2:   return SPI_BAUDRATEPRESCALER_2;
        case 4:   return SPI_BAUDRATEPRESCALER_4;
        case 8:   return SPI_BAUDRATEPRESCALER_8;
        case 16:  return SPI_BAUDRATEPRESCALER_16;
        case 32:  return SPI_BAUDRATEPRESCALER_32;
        case 64:  return SPI_BAUDRATEPRESCALER_64;
        case 128: return SPI_BAUDRATEPRESCALER_128;
        default:  return SPI_BAUDRATEPRESCALER_256;
    }
}

static void tiny_delay(void)
{
    for (volatile int i = 0; i < 40; i++) { }
}

static void spi_gpio_init(void)
{
    GPIO_InitTypeDef g;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PA5(SCK) / PA7(MOSI) / PB4(MISO) —— 全部 AF5 = SPI1
     * ⚠ PB4 复位后是 NJTRST（调试脚）：H7 上把 AFR 配成 AF5 就接管回来了，
     *   不需要"关 JTAG"。 */
    memset(&g, 0, sizeof(g));
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI1;

    g.Pin = GPIO_PIN_5 | GPIO_PIN_7;          /* PA5 SCK, PA7 MOSI */
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = GPIO_PIN_4;                       /* PB4 MISO */
    HAL_GPIO_Init(GPIOB, &g);

    /* CS：软件控制，空闲为高 */
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);
    memset(&g, 0, sizeof(g));
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Pin   = SPI_CS_PIN;
    HAL_GPIO_Init(SPI_CS_PORT, &g);

    /* ready：输入 + 下拉（对端还没跑起来时不悬空乱跳） */
    memset(&g, 0, sizeof(g));
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    g.Pin  = SPI_RDY_PIN;
    HAL_GPIO_Init(SPI_RDY_PORT, &g);
}

static int spi_apply(int presc)
{
    s_hspi.Init.BaudRatePrescaler = presc_to_hal(presc);
    if (HAL_SPI_Init(&s_hspi) != HAL_OK)
    {
        return -1;
    }
    s_cur_presc = presc;
    g_spi_hz = HAL_RCC_GetPCLK2Freq() / (uint32_t)presc;
    return 0;
}

int spi_link_init(uint32_t baud_presc)
{
    if (baud_presc == 0u)
    {
        baud_presc = 64u;
    }
    g_spi_presc = (int)baud_presc;

    spi_gpio_init();
    __HAL_RCC_SPI1_CLK_ENABLE();

    memset(&s_hspi, 0, sizeof(s_hspi));
    s_hspi.Instance                     = SPI1;
    s_hspi.Init.Mode                    = SPI_MODE_MASTER;
    s_hspi.Init.Direction               = SPI_DIRECTION_2LINES;   /* 全双工 */
    s_hspi.Init.DataSize                = SPI_DATASIZE_8BIT;
    s_hspi.Init.CLKPolarity             = SPI_POLARITY_LOW;       /* 模式 0 */
    s_hspi.Init.CLKPhase                = SPI_PHASE_1EDGE;
    s_hspi.Init.NSS                     = SPI_NSS_SOFT;           /* CS 自己管 */
    s_hspi.Init.BaudRatePrescaler       = presc_to_hal((int)baud_presc);
    s_hspi.Init.FirstBit                = SPI_FIRSTBIT_MSB;
    s_hspi.Init.TIMode                  = SPI_TIMODE_DISABLE;
    s_hspi.Init.CRCCalculation          = SPI_CRCCALCULATION_DISABLE;
    s_hspi.Init.CRCPolynomial           = 7u;
    s_hspi.Init.NSSPMode                = SPI_NSS_PULSE_DISABLE;
    s_hspi.Init.FifoThreshold           = SPI_FIFO_THRESHOLD_01DATA;
    s_hspi.Init.MasterKeepIOState       = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_SPI_Init(&s_hspi) != HAL_OK)
    {
        g_spi_init_rc = 1;
        return -1;
    }

    g_spi_init_rc = 0;
    s_cur_presc   = (int)baud_presc;
    g_spi_hz      = HAL_RCC_GetPCLK2Freq() / baud_presc;
    return 0;
}

int spi_link_active(void)
{
    return (g_spi_init_rc == 0) ? 1 : 0;
}

int spi_link_cmd_enqueue(const char* text)
{
    int n = 0;
    if (text == NULL)
        return 0;
    for (const char* p = text; *p != 0; ++p)
    {
        uint32_t next = (s_cmd_head + 1u) & CMD_RING_MASK;
        if (next == s_cmd_tail)
            break;                  /* 环满：丢弃（命令都很短，极少发生） */
        s_cmd_ring[s_cmd_head] = (uint8_t)(*p);
        s_cmd_head = next;
        n++;
    }
    g_spi_tx_bytes += (uint32_t)n;
    return n;
}

void spi_link_poll(uint32_t now_ms)
{
    static uint32_t s_last  = 0u;
    static uint8_t  tx[XFER];
    static uint8_t  rx[XFER];
    uint32_t        avail, n, i, dlen;
    int             rc;

    if (g_spi_init_rc != 0)
        return;
    if ((now_ms - s_last) < 5u)     /* ~5 ms 节拍：足够追上下行，又不占满总线 */
        return;
    s_last = now_ms;

    if ((int)g_spi_presc != s_cur_presc)
        (void)spi_apply((int)g_spi_presc);

    /* ---- 组主发缓冲：把命令环前段搬进 [3..]，其余填 0 ---- */
    memset(tx, 0, XFER);
    avail = (s_cmd_head - s_cmd_tail) & CMD_RING_MASK;
    if (avail > 0u)
    {
        n = (avail > (XFER - 3u)) ? (XFER - 3u) : avail;
        tx[0] = 0x01u;
        tx[1] = (uint8_t)(n & 0xFFu);
        tx[2] = (uint8_t)((n >> 8) & 0xFFu);
        for (i = 0u; i < n; i++)
        {
            tx[3u + i] = s_cmd_ring[s_cmd_tail];
            s_cmd_tail = (s_cmd_tail + 1u) & CMD_RING_MASK;
        }
    }

    /* ---- 一次全双工事务 ---- */
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_RESET);
    tiny_delay();
    rc = (HAL_SPI_TransmitReceive(&s_hspi, tx, rx, XFER, 200u) == HAL_OK) ? 0 : -1;
    tiny_delay();
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);

    g_spi_tx_n++;
    if (rc != 0)
    {
        g_spi_last_code = -1;
        return;
    }

    g_spi_irq_level = (HAL_GPIO_ReadPin(SPI_RDY_PORT, SPI_RDY_PIN) == GPIO_PIN_SET) ? 1u : 0u;

    /* ---- 解从机下行：data_len 权威，忽略填充 ---- */
    dlen = (uint32_t)(rx[1] | (rx[2] << 8));
    if (dlen > 0u && dlen <= (XFER - 3u))
    {
        uart_link_feed(rx + 3u, dlen);
        g_spi_rx_bytes += dlen;
        g_spi_last_code = 0;
    }
    else
    {
        g_spi_last_code = 1;        /* 空事务（从机出站队列正好空） */
    }
}
