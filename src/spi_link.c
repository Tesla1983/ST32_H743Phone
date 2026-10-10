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

/* 诊断（2026-10-10 追事务截断用）：HAL 传输失败的笔数与首个失败码。
 * 从机实测偶尔收到 trans_len = 0 的"空事务"（CS 动了、一个时钟都没有），
 * 而主机只要 HAL 返回 HAL_OK 就一定会出满 512 个时钟 ⇒ 空事务只能是
 * **HAL 传输失败**这一路来的。这个计数就是判据。 */
volatile uint32_t g_spi_err      = 0;
volatile int      g_spi_err_code = 0;

/* burst：一拍最多连跑几笔事务（0/1 ⇒ 退回"每 5 ms 一笔"的老节奏）。
 * 见 spi_link_poll 里的长度账：2 KB 的一块要 35 笔，只按 5 ms 一拍跑是 12 KB/s，
 * 顶上不去；连跑 8 笔 ≈ 2.7 ms/拍，帧期从 ~11 ms 涨到 ~14 ms，用户看不出来。
 * 做成运行期可写是为了能**不改固件**地把它调小回退（SWD 写一下即可）。 */
volatile uint32_t g_spi_burst    = 8;

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

/* ⚠ 原来这里有个 tiny_delay()（40 次空转 ≈ 0.3 µs），CS 抬升后就靠它。
 *   2026-10-10 第四轮发现它**比 CS 高电平所需的最小宽度还短**，已换成 cs_gap()，
 *   函数本身随之删除（留着会被 -Wunused-function 抓到）。下面两处注释里的
 *   "tiny_delay" 是历史记录，指的是这个已删除的函数。 */

/* CS 建立/保持延时。
 *
 * ⚠ 为什么不能只用 tiny_delay（2026-10-10）：tiny_delay 是 40 次空循环，
 *   在 400 MHz 下约 0.3 µs —— **比一个 SPI 位周期还短**（/64 时 640 ns/位）。
 *   CS 建立/保持时间小于一个位周期时，从机可能出现"CS 拉低后第一拍没跟上"
 *   或"最后一拍还没采完 CS 就抬了"⇒ 事务被截断（从机 IDF 报 trans_len 不是
 *   4 字节对齐、尾部字节丢失）。这里是纯软件延时、只占主循环几百 µs 里的几 µs，
 *   代价可忽略，但把时序余量从"小于 1 位"提到"数倍位周期"，安全得多。 */
static void cs_delay(void)
{
    for (volatile int i = 0; i < 800; i++) { }   /* ≈ 6~8 µs @400 MHz，> 10 个位周期 */
}

/* CS **抬升**之后的间隔（连续事务之间）。
 *
 * ⚠ 为什么必须单独有这个（2026-10-10 第四轮）：原来 CS 抬升后只跟一个
 *   tiny_delay（40 次空转 ≈ 0.3 µs），而 burst 模式下下一笔事务紧接着就把 CS
 *   又拉低 —— 留给从机的"CS 高电平"只有 0.3 µs ≈ 72 个 240 MHz 周期，
 *   再经 GPIO 矩阵，余量太小。ESP32 从机要认出 CS 上升沿才结束本笔、装配下一笔；
 *   识别不到就会：① 报 trans_len = 0 的"空事务"；② 把相邻两笔并成一笔
 *   （trans_len = 1024 而非 512，第 2 笔的命令被挤到缓冲区之外丢掉）。
 *   实测佐证：主机侧 **g_spi_err 恒 0（HAL 从未失败）**、从机 command 区截断 0，
 *   但"收下命令"比主机发的少 ~10% ⇒ 只能出在 CS 时序上。
 *   ⇒ 抬升后等 ≈3 µs（≈ 1.5625 MHz 下 5 个位周期），代价 8 笔/burst × 3 µs
 *     = 24 µs/拍，相对 ~11 ms 的帧期可忽略。 */
/* 默认 1200 ≈ 9 µs：够从机认出 CS 上升沿，又只占 burst 8 笔的 72 µs/拍。
 * 扫档实测 3 / 9 / 22 / 60 µs 四档里 22 µs 那一档 GET 连续失联最轻，
 * 但单轮噪声大、不足以区分 9 与 22（差 104 µs/拍）⇒ 取中间偏保守的 9 µs。 */
volatile uint32_t g_spi_cs_gap = 1200u;  /* 空转次数；≈ n/133 µs @400 MHz */

static void cs_gap(void)
{
    uint32_t n = g_spi_cs_gap;
    for (volatile uint32_t i = 0; i < n; i++) { }
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
    uint32_t        budget;
    int             rc;

    if (g_spi_init_rc != 0)
        return;

    /* ---- 本拍要不要干活？两种情形 ----
     *   ① 从机说"有数据"（ready 高），或命令环里还有东西要发 ⇒ 有事，走 burst；
     *   ② 都没有 ⇒ 只是空闲心跳，维持 ~5 ms 一拍（用于把 $?RD 这类命令发出去、
     *      以及从机 ready 还没拉起来时也能把状态帧取回来）。
     * ⚠ 空闲心跳不能删：ready 是"从机出站队列非空"，而队列里刚被取空的那一刻
     *   到下一次 ready 拉高之间没有信号，全靠主机周期轮询兜住。 */
    if ((g_spi_burst == 0u) || (g_spi_burst == 1u))
    {
        /* burst 被关掉（g_spi_burst=0）或 =1 ⇒ 退回"每 5 ms 一笔"的老节奏 */
        if ((now_ms - s_last) < 5u)
            return;
        s_last  = now_ms;
        budget  = 1u;
    }
    else
    {
        int busy = (HAL_GPIO_ReadPin(SPI_RDY_PORT, SPI_RDY_PIN) == GPIO_PIN_SET) ? 1 : 0;
        if (((s_cmd_head - s_cmd_tail) & CMD_RING_MASK) != 0u)
            busy = 1;
        if (!busy)
        {
            if ((now_ms - s_last) < 5u)
                return;
            s_last = now_ms;
            budget = 1u;
        }
        else
        {
            s_last = now_ms;        /* 有活的这一拍把心跳也刷新掉 */
            budget = g_spi_burst;
        }
    }

    if ((int)g_spi_presc != s_cur_presc)
        (void)spi_apply((int)g_spi_presc);

    /* ---- 跑 budget 笔事务（有活时连跑，把 61 字节/笔的搬运速率顶上去）----
     * 为什么需要 burst：一笔事务只搬 61 字节，一次文件块 2 KB 要 35 笔。
     * 只按 5 ms 一拍跑 ⇒ 12 KB/s，100 KB 要 8 秒以上，而且期间从机出站队列
     * 一直压着几千字节（ready 恒高）却只能慢慢漏。连跑几笔后整条链路的
     * "搬运带宽"才跟得上写卡。
     * ⚠ 上限必须存在：本函数跑在渲染前的主循环里，burst 太长会把帧时间拉长。
     *   8 笔 × 约 0.34 ms(/64) ≈ 2.7 ms，帧期 ~11→14 ms（约 70 FPS），可以接受。 */
    for (uint32_t k = 0; k < budget; k++)
    {
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
        cs_delay();
        rc = (HAL_SPI_TransmitReceive(&s_hspi, tx, rx, XFER, 200u) == HAL_OK) ? 0 : -1;
        cs_delay();
        HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);
        cs_gap();               /* ⚠ 不能退回 tiny_delay：从机要认出 CS 上升沿，见 cs_gap 注释 */

        g_spi_tx_n++;
        if (rc != 0)
        {
            g_spi_last_code = -1;
            /* 记录失败（追"空事务"来源）：HAL 状态放在 hspi.ErrorCode / State 里 */
            if (g_spi_err == 0u)
                g_spi_err_code = (int)s_hspi.ErrorCode;
            g_spi_err++;
            return;                 /* ⚠ 出错立刻收手：连跑会把一个超时放大成 N 个 */
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
            g_spi_last_code = 1;    /* 空事务（从机出站队列正好空） */
        }

        /* ---- 还有活就继续连跑，没活就收手（省掉无谓的空事务）---- */
        {
            int more = (HAL_GPIO_ReadPin(SPI_RDY_PORT, SPI_RDY_PIN) == GPIO_PIN_SET) ? 1 : 0;
            if (((s_cmd_head - s_cmd_tail) & CMD_RING_MASK) != 0u)
                more = 1;
            if (dlen > 0u)
                more = 1;           /* 刚收到数据 ⇒ 队列里多半还有，继续追 */
            if (!more)
                break;
        }
    }
}
