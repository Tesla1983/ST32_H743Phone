/* ===========================================================================
 * 音频输出层实现 —— 设计说明见 audio_out.h
 *
 * 【出音用哪条路：TIM6 周期中断喂 FIFO（不是 DMA）】
 *   第一版走的是 DMA1_Stream2 循环双缓冲，板上实测**送不出数据**：
 *     SPI_CR1=0x201（SPE+CSTART 都在）、I2SCFGR 分频正确、内核时钟 200 MHz 正常、
 *     DMA 的 CR/CIRC/MINC/PSIZE 都对，但 SR 恒为 0、NDTR 一秒只动几十项
 *     ⇒ I2S 的 TXP（FIFO 有空位）一次都没出现，DMA 请求也就无从谈起。
 *   追了两轮没拿到更细的证据（主机侧 probe-rs 读不到外设寄存器，只能靠 RAM 快照），
 *   ⇒ 改成**已被验证过的那条路**：audio_test.c 里出过声的做法是"轮询 TXP → 写 TXDR"
 *     （at_stream→at_tx_wait，case 4 实测出声），这里把它搬进 **TIM6 的周期中断**
 *     （20 µs），ISR 里把 FIFO 填到满——与 at_stream 的内层循环是同一套逻辑，只是
 *     从"主循环阻塞轮询"变成"定时器中断滴灌"，从而**不冻结 UI**。
 *     主循环只负责把 TF 卡/正弦源的数据补进环形缓冲（RING），不在 ISR 里碰文件系统。
 *
 *   ⚠ 为什么必须靠中断滴灌：I2S TX FIFO 只有 8 个半字，48 828 Hz × 2 声道下只够撑
 *     ~82 µs；主循环一帧 ~16 ms，远慢于 82 µs，纯主循环补数会让 FIFO 长期枯竭
 *     ⇒ 连续出声**只能**靠"高频中断"或"DMA"，DMA 在本板送不出 ⇒ 只能走 TIM6。
 *
 *   ⚠ 为什么 20 µs：取 FIFO 预算（82 µs）的 ~1/4 做余量。ISR 每次最多填 8 个半字，
 *     开销可忽略；万一 TXP 因"时钟没起"而恒为 1，ISR 内的 8 次上限也保证它不会被
 *     拖成死循环（否则会饿死主循环 —— 2026-10-12 实测过这个后果）。
 *
 * 【接线 / H7 两条铁律】见 audio_out.h 顶部与 docs/DEVELOPMENT_LOG.md 的
 * 「2026-10-12 · ES8311 音频模块接入」一节。
 * =========================================================================== */
#include "audio_out.h"
#include "es8311.h"

#include "stm32h7xx_hal.h"

#include <string.h>
#include <math.h>

/* ---- 诊断量 ---- */
volatile int32_t  g_ao_rc       = 99;
volatile uint32_t g_ao_codec    = 0;
volatile uint32_t g_ao_playing  = 0;
volatile uint32_t g_ao_udr      = 0;
volatile uint32_t g_ao_frames   = 0;     /* 累计送出帧数（÷48828 ≈ 秒数）*/
volatile uint32_t g_ao_underrun = 0;     /* 主循环补数没跟上、被迫填静音的次数 */
volatile uint32_t g_ao_push     = 0;     /* ISR 实际写进 FIFO 的半字数 */
volatile uint32_t g_ao_pump     = 0;     /* 主循环补过多少次环形缓冲 */
volatile uint32_t g_ao_txp_seen = 0;     /* tick 里看到 TXP=1 的次数（时钟在不在跑）*/
volatile uint32_t g_ao_isr      = 0;     /* TIM6 中断次数 */
volatile uint32_t g_ao_isr_dry  = 0;     /* ISR 进来却发现环形缓冲没数据的次数 */
/* 起播阶段进度（排障用，**不是**错误码）：
 *   1 = codec 序列写完   2 = I2S 初始化 OK   3 = 环形缓冲已铺满
 *   4 = SPE/CSTART 已置  5 = TIM6 心跳已起   6 = 全部就绪（此时 g_ao_rc=0）
 * ⚠ 为什么单独要一个阶段量：上一版出现过"起播函数没走完、系统就停摆"的情况，
 *   而 g_ao_rc 只在失败路径上写 ⇒ 靠它无法区分"卡在第几步"。 */
volatile uint32_t g_ao_stage    = 0;

/* ---- 环形缓冲 → SRAM1 的 .bss_audio（MPU region0：非缓存）----
 * 8192 个半字 = 16 KB = 4096 帧 ≈ 84 ms 的音频；一次补半个环（2048 帧 ≈ 42 ms）。 */
#define AUDIO_BUF_ATTR   __attribute__((section(".bss_audio"), aligned(32)))
#define RING_HALFWORDS   8192u
#define RING_MASK        (RING_HALFWORDS - 1u)
#define RING_HALF_FRAMES (RING_HALFWORDS / 4u)      /* 2048 帧 = 4096 半字 */
static uint16_t AUDIO_BUF_ATTR s_ring[RING_HALFWORDS];
/* feeder 的落脚点（连续缓冲，补完再拷进 ring）—— 2048 帧 × 4 B = 8 KB */
static int16_t  AUDIO_BUF_ATTR s_tmp[RING_HALF_FRAMES * 2u];

static volatile uint32_t s_wr;          /* 写指针（半字），主循环推进 */
static volatile uint32_t s_rd;          /* 读指针（半字），ISR 推进 */
static volatile uint8_t  s_tim_pending = 0u;   /* 起播延后到下一拍再起 TIM6（见下）*/

static I2S_HandleTypeDef s_hi2s;        /* I2S2 = SPI2 的 I2S 模式 */
static audio_out_feeder  s_feeder = NULL;

/* 内置正弦自检（不依赖 TF 卡），见 audio_out.h 的 audio_out_demo_start。
 * 用 200 样本 LUT + 每帧步进 4 ⇒ 频率 = 4 × fs / 200 = 4 × 48828 / 200 ≈ 977 Hz，
 * 与 audio_test.c case 4 同一口径（已实测出声）。 */
#define DEMO_HZ      977u
#define DEMO_SEC     3u
#define DEMO_LUT_N   200u
#define DEMO_AMP     28000           /* 峰值 ≈85% 满幅（先排除"太轻听不见"）*/
static int16_t  s_demo_lut[DEMO_LUT_N];
static uint32_t s_demo_left;
static uint32_t s_demo_acc;           /* LUT 相位累加（样本数）*/
static uint8_t  s_demo_done;
volatile uint32_t g_ao_demo = 0;

/* UI 音量的权威值在 main.c（es8311 起播时会读它写 REG32）。 */
extern volatile uint32_t g_audio_volume;

/* ---- 寄存器现场快照（2026-10-12 排障用）----
 * ⚠⚠ 为什么必须有它：主机侧 probe-rs **读不到外设寄存器**（只对 RAM 地址有效，
 *   读 0x4000_xxxx 返回空）—— 要拿外设现场只能把寄存器**拷进 RAM** 再让 SWD 读。
 * 索引：0 SPI_CR1 / 1 SPI_SR / 2 SPI_CFG1 / 3 SPI_I2SCFGR / 4 TIM6_ARR
 *      5 ring 已用半字 / 6 g_ao_push / 7 ISR 空转次数 / 8 g_ao_isr
 *      9 NVIC ISER0 / 10 ring 起始地址低 16 位 / 11 txp_seen
 *     12 RCC->APB1LENR / 13 RCC->D2CCIP1R */
volatile uint32_t g_ao_dbg[16];

static void dbg_snapshot(void)
{
    g_ao_dbg[0]  = s_hi2s.Instance->CR1;
    g_ao_dbg[1]  = s_hi2s.Instance->SR;
    g_ao_dbg[2]  = s_hi2s.Instance->CFG1;
    g_ao_dbg[3]  = s_hi2s.Instance->I2SCFGR;
    g_ao_dbg[4]  = TIM6->ARR;
    g_ao_dbg[5]  = (s_wr - s_rd) & RING_MASK;
    g_ao_dbg[6]  = g_ao_push;
    g_ao_dbg[7]  = g_ao_isr_dry;
    g_ao_dbg[8]  = g_ao_isr;
    g_ao_dbg[9]  = NVIC->ISER[0u];
    g_ao_dbg[10] = (uint32_t)((uintptr_t)&s_ring[0] & 0xFFFFu);
    g_ao_dbg[11] = g_ao_txp_seen;
    g_ao_dbg[12] = RCC->APB1LENR;
    g_ao_dbg[13] = RCC->D2CCIP1R;
}

/* ====================== I2S2（SPI2）====================== */

static void i2s_gpio_init(void)
{
    GPIO_InitTypeDef g;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;      /* 全部 AF5，DS12110 Table 9 逐脚核对过 */

    g.Pin = GPIO_PIN_6;               /* PC6 = I2S2_MCK */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_3;               /* PD3 = I2S2_CK */
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_9;               /* PB9 = I2S2_WS ⚠ 会顶掉 UART4_TX，stop 时还原 */
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_15;              /* PB15 = I2S2_SDO → 模块的 DIN */
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_1;               /* PC1 = I2S2_SDO（兼容另一种接线，用户只接一根即可）*/
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;               /* PC2 = I2S2_SDI（录音用，本版不用）*/
    HAL_GPIO_Init(GPIOC, &g);
}

/* 把 PB9 还给 UART4_TX（uart_link.c 开机时的配置）：借出去的脚用完必须还回去。 */
static void i2s_gpio_release(void)
{
    GPIO_InitTypeDef g;

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF8_UART4;
    g.Pin       = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &g);
}

static int i2s_init(void)
{
    s_hi2s.Instance              = SPI2;
    s_hi2s.Init.Mode             = I2S_MODE_MASTER_TX;
    s_hi2s.Init.Standard         = I2S_STANDARD_PHILIPS;
    /* ⚠ 必须是 EXTENDED（16 位数据装 32 位时隙 ⇒ SCK = 64fs），
     *   ES8311 侧 esp-adf 系数表也是 bclk_div=4（64fs）。改成 16B 会错位。 */
    s_hi2s.Init.DataFormat       = I2S_DATAFORMAT_16B_EXTENDED;
    s_hi2s.Init.MCLKOutput       = I2S_MCLKOUTPUT_ENABLE;
    s_hi2s.Init.AudioFreq        = I2S_AUDIOFREQ_48K;
    s_hi2s.Init.CPOL             = I2S_CPOL_LOW;
    s_hi2s.Init.FirstBit         = I2S_FIRSTBIT_MSB;
    s_hi2s.Init.WSInversion      = I2S_WS_INVERSION_DISABLE;
    s_hi2s.Init.Data24BitAlignment = I2S_DATA_24BIT_ALIGNMENT_RIGHT;
    s_hi2s.Init.MasterKeepIOState  = I2S_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_I2S_Init(&s_hi2s) != HAL_OK)
    {
        return -1;
    }

    /* 显式写一遍 I2SDIV/ODD：不依赖 HAL 的时钟查询，从 PLL1DIVR 现场推内核时钟。
     * 200 MHz / 16 = 12.5 MHz MCLK ⇒ fs = 12.5M/256 = 48 828 Hz。
     * ⚠ I2SDIV 在 I2SCFGR bits 23:16、ODD 在 bit 24（曾按 bits 7:0 解码读出假值）。 */
    {
        uint32_t sysclk = SystemCoreClock;
        uint32_t p1     = ((RCC->PLL1DIVR >> 9) & 0x7Fu) + 1u;
        uint32_t q1     = ((RCC->PLL1DIVR >> 16) & 0x7Fu) + 1u;
        uint32_t kclk   = (sysclk * p1) / q1;
        uint32_t target = 48000u * 256u;
        uint32_t div    = (kclk + target / 2u) / target;
        uint32_t odd    = div & 1u;
        uint32_t i2sdiv = (div - odd) / 2u;

        if (i2sdiv < 1u)   { i2sdiv = 1u;  odd = 0u; }
        if (i2sdiv > 255u) { i2sdiv = 255u; odd = 1u; }
        MODIFY_REG(s_hi2s.Instance->I2SCFGR,
                   (SPI_I2SCFGR_I2SDIV | SPI_I2SCFGR_ODD),
                   ((i2sdiv << SPI_I2SCFGR_I2SDIV_Pos) | (odd << SPI_I2SCFGR_ODD_Pos)));
    }

    /* ⚠ 显式把 CFG1.DSIZE 设成 **16 位**（复位值是 7 = 8 位，HAL 在 I2S 模式下
     *   根本不碰这个字段）。ISR 里按 16 位往 TXDR 写，两边必须一致。 */
    MODIFY_REG(s_hi2s.Instance->CFG1, SPI_CFG1_DSIZE, (15u << SPI_CFG1_DSIZE_Pos));

    /* 清全部挂起标志（与 HAL_I2S_Init 的 WRITE_REG(IFCR, 0x0FF8) 同口径），
     * 免得上一次残留的 UDR/SUSP 把 TXP 压在 0。 */
    s_hi2s.Instance->IFCR = 0x0FF8u;
    return 0;
}

static void i2s_stop(void)
{
    /* 先挂起再关：CSUSP 让当前帧自然结束、硬件自清 CSTART，不在时钟中途硬切。 */
    if (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
    {
        uint32_t t0 = HAL_GetTick();

        SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSUSP);
        while (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
        {
            if (HAL_GetTick() - t0 > 100u)
            {
                break;                 /* 100 ms 兜底，不等死 */
            }
        }
    }
    __HAL_I2S_DISABLE(&s_hi2s);
    HAL_I2S_DeInit(&s_hi2s);
}

/* ====================== TIM6：喂 FIFO 的心跳 ======================= */

static void tim6_start(void)
{
    /* ⚠ 周期取 20 µs：FIFO 只有 8 个半字、48.8 kHz 立体声下够撑约 82 µs，
     *   取 1/4 当余量。APB1 定时器时钟 = 200 MHz（APB1=100 MHz × 2）⇒
     *   PSC=0、ARR=3999 ⇒ 4000 个周期 = 20 µs。
     *   即使实际只有 100 MHz（40 µs）也仍在 82 µs 预算内 —— 这就是留 4 倍余量的意义。 */
    __HAL_RCC_TIM6_CLK_ENABLE();

    TIM6->CR1  = 0u;
    TIM6->PSC  = 0u;
    TIM6->ARR  = 3999u;
    TIM6->CNT  = 0u;
    TIM6->SR   = 0u;                    /* 清更新中断标志 */
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->CR1  = TIM_CR1_CEN;

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

static void tim6_stop(void)
{
    HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
    TIM6->DIER = 0u;
    TIM6->SR   = 0u;
    TIM6->CR1  = 0u;
}

/* ⚠ 这个 handler 定义在**本文件**而不是 stm32h7xx_it.c：让模块自带 handler，
 * 保证"编进来就一定接得上"。startup 的向量表该项是 weak，本定义直接覆盖。
 * ⚠⚠ 同时也必须在 linker 里 EXTERN 出来 —— 否则 --gc-sections 会把只有 weak
 *   引用的函数整段删掉（2026-10-12 实测：handler 消失了但编译链接全绿）。 */
void TIM6_DAC_IRQHandler(void)
{
    /* 先清更新标志（防重入/累计）；写 1 清 0，TIM6 只有 UIF 这一位。 */
    TIM6->SR = (uint32_t)~TIM_SR_UIF;
    g_ao_isr++;

    /* 防御：心跳理论上只在播放期间开，但万一 stop 与 ISR 竞态，多一道闸。 */
    if (g_ao_playing == 0u)
    {
        return;
    }

    if (((s_wr - s_rd) & RING_MASK) == 0u)
    {
        g_ao_isr_dry++;                 /* 缓冲空了：这次什么都不做（保持上一次的值/静音）*/
        return;
    }

    /* 把 TX FIFO 填到满 —— 与 audio_test.c 出声时同一手法。
     * ⚠ 显式上限 8 次：FIFO 就 8 个半字，写超了没有意义；加计数上限是为了
     *   万一 TXP 因为"时钟没起而恒为 1"时，ISR 不会被拖成死循环（那样会
     *   把整个主循环饿死 —— 2026-10-12 实测过这个后果）。 */
    for (uint32_t k = 0u; k < 8u; ++k)
    {
        if (((s_wr - s_rd) & RING_MASK) == 0u)
        {
            break;
        }
        if ((READ_BIT(SPI2->SR, SPI_SR_TXP) & SPI_SR_TXP) == 0UL)
        {
            break;
        }
        *((volatile uint16_t *)&SPI2->TXDR) = s_ring[s_rd & RING_MASK];
        s_rd = (s_rd + 1u) & RING_MASK;
        g_ao_push++;
        if ((g_ao_push & 1u) == 0u)      /* 2 个半字 = 1 帧 */
        {
            g_ao_frames++;
        }
    }
}

/* ====================== 环形缓冲补数 ======================= */

static void ring_put(const int16_t *src, uint32_t halfwords)
{
    for (uint32_t i = 0; i < halfwords; ++i)
    {
        s_ring[s_wr] = (uint16_t)src[i];
        s_wr = (s_wr + 1u) & RING_MASK;
    }
}

/* 补半个环（2048 帧 = 4096 半字）。feeder 给不满就补静音，绝不留下上一首的残影。 */
static void pump_half(void)
{
    uint32_t got = 0;
    int      n   = -1;

    if (s_feeder != NULL)
    {
        n = s_feeder(s_tmp, RING_HALF_FRAMES);
    }
    if (n > 0)
    {
        got = (uint32_t)n;
    }
    if (got > RING_HALF_FRAMES)
    {
        got = RING_HALF_FRAMES;
    }
    if (got < RING_HALF_FRAMES)
    {
        memset(&s_tmp[got * 2u], 0, (RING_HALF_FRAMES - got) * 4u);
        if ((s_feeder != NULL) && (s_demo_left == 0u))
        {
            g_ao_underrun++;             /* 数据供不上（卡慢/播到最后一段）*/
        }
    }
    ring_put(s_tmp, RING_HALF_FRAMES * 2u);
    g_ao_pump++;
}

/* ====================== 内置正弦自检（不碰 TF 卡）=======================
 * 【为什么需要它】"codec + I2S + 心跳" 能不能响，与"卡上有没有 WAV、WAV 解析对不对"
 *   是两件事。先有一个不碰卡的声源，就能把两类问题分开定位：
 *     demo 响、播 WAV 不响 ⇒ 播放器/文件的问题；demo 也不响 ⇒ 音频硬件/接线的问题。 */

static void demo_lut_fill(void)
{
    int32_t peak = DEMO_AMP;

    if (peak > 32000) peak = 32000;     /* 留 1 LSB 余量，防包络乘爆 */
    if (peak < 0)     peak = 0;
    for (uint32_t i = 0u; i < DEMO_LUT_N; i++)
    {
        double s = sin(6.283185307179586 * (double)i / (double)DEMO_LUT_N);
        s_demo_lut[i] = (int16_t)(s * (double)peak);
    }
}

/* 每帧步进 LUT_STEP 个样本 ⇒ 频率 = LUT_STEP × fs / LUT_N。
 * LUT_STEP=4 ⇒ 4 × 48828 / 200 = 977 Hz（与 audio_test case 4 同口径）。 */
#define DEMO_LUT_STEP  4u
static int demo_feeder(int16_t *dst, uint32_t frames)
{
    uint32_t n = (frames < s_demo_left) ? frames : s_demo_left;

    for (uint32_t i = 0; i < n; ++i)
    {
        if (s_demo_acc >= DEMO_LUT_N) s_demo_acc -= DEMO_LUT_N;
        int16_t v = s_demo_lut[s_demo_acc];
        dst[2u * i]      = v;
        dst[2u * i + 1u] = v;
        s_demo_acc += DEMO_LUT_STEP;
    }
    s_demo_left -= n;
    if (s_demo_left == 0u)
    {
        s_demo_done = 1u;
    }
    return (int)n;
}

int audio_out_demo_start(void)
{
    demo_lut_fill();
    s_demo_left = 48828u * (uint32_t)DEMO_SEC;
    s_demo_acc  = 0u;
    s_demo_done = 0u;
    audio_out_set_feeder(demo_feeder);
    audio_out_set_volume((uint8_t)g_audio_volume);
    return audio_out_start();
}

/* ====================== 对外接口 ======================= */

void audio_out_set_feeder(audio_out_feeder cb)
{
    s_feeder = cb;
}

void audio_out_set_volume(uint8_t vol0_100)
{
    /* codec 没起来（没接模块 / 未播放）就只记录 —— g_audio_volume 是权威值，
     * es8311_start_dac() 起播时会读它写一次 REG32。 */
    (void)es8311_set_volume_ui((uint32_t)vol0_100);
}

int audio_out_start(void)
{
    int rc;

    if (g_ao_playing != 0u)
    {
        return 0;                       /* 已在播：幂等 */
    }

    /* 1) 控制通道：I2C1 + ES8311 */
    rc = es8311_bus_init();
    if (rc != 0)
    {
        g_ao_rc = -1;
        return -1;
    }
    /* CE 脚焊法未知 ⇒ 两个地址都探一次，探到哪个用哪个。 */
    if (es8311_probe(ES8311_I2C_ADDR_CE0) == 0)
    {
        es8311_set_addr(ES8311_I2C_ADDR_CE0);
    }
    else if (es8311_probe(ES8311_I2C_ADDR_CE1) == 0)
    {
        es8311_set_addr(ES8311_I2C_ADDR_CE1);
    }
    else
    {
        g_ao_rc = -2;                   /* 总线上没有 ES8311：没接模块或线错了 */
        g_ao_codec = 0u;
        return -2;
    }

    rc  = es8311_init();
    rc |= es8311_config_i2s();
    rc |= es8311_start_dac();           /* 内部会按当前 UI 音量写 REG32 */
    if (rc != 0)
    {
        g_ao_rc = -3;
        g_ao_codec = 0u;
        return -3;
    }
    g_ao_codec = 1u;
    g_ao_stage = 1u;

    /* 2) 数据通道：I2S2 + TIM6 心跳 */
    i2s_gpio_init();
    if (i2s_init() != 0)
    {
        g_ao_rc = -4;
        return -4;
    }
    g_ao_stage = 2u;

    s_wr = 0u;
    s_rd = 0u;
    pump_half();                        /* 先铺满整个环，避免第一拍就断 */
    pump_half();
    g_ao_stage = 3u;

    /* ⚠⚠ H7 铁律：SPE 只使能外设，必须再置 CSTART 才开始传输（2026-10-12 实测）。 */
    __HAL_I2S_ENABLE(&s_hi2s);
    SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART);
    g_ao_stage = 4u;

    /* ⚠⚠ 心跳**不在这里起**：tim6_start 末尾的 NVIC_EnableIRQ 会让中断在
     *   audio_out_start() 还没返回时就进来一次（即便没有 EGR_UG，也使能即可能
     *   响应挂起标志），而那时 g_ao_playing 还是 0 —— 一旦 ISR 里再出点意料之外
     *   的事，整个起播函数就再也回不去了。改成由 tick 在下一拍启动（见 audio_out_tick）。 */
    s_tim_pending = 1u;

    g_ao_playing = 1u;
    g_ao_rc      = 0;
    g_ao_stage   = 5u;

    /* ⚠ 诊断量按"本次播放"清零：这些量在 ISR/主循环里只增不减，若不在这里清，
     *   多次播放会累加（上一次 3 s 的 146 k 帧会带进下一次），验收脚本算不出
     *   单次播放的精确量。仅 resetting 播放相关的计数，不动 g_ao_stage/g_ao_codec 等状态位。 */
    g_ao_frames   = 0u;
    g_ao_push     = 0u;
    g_ao_isr      = 0u;
    g_ao_isr_dry  = 0u;
    g_ao_udr      = 0u;
    g_ao_underrun = 0u;
    g_ao_pump     = 0u;
    return 0;
}

void audio_out_stop(void)
{
    if (g_ao_playing == 0u)
    {
        return;
    }
    tim6_stop();
    i2s_stop();
    i2s_gpio_release();                 /* PB9 还给 UART4 */
    (void)es8311_stop_dac();
    g_ao_playing = 0u;
    g_ao_codec   = 0u;
    s_wr = 0u;
    s_rd = 0u;
    s_tim_pending = 0u;
}

int audio_out_playing(void)
{
    return (g_ao_playing != 0u) ? 1 : 0;
}

void audio_out_tick(void)
{
    /* dbg 无条件刷新：起播卡住时 playing 往往是 0，那时才最需要看现场
     * （上一版正是"dbg 因 playing=0 而全 0"，白白多烧了一遍固件）。 */
    if (g_ao_playing != 0u)
    {
        dbg_snapshot();
    }

    /* 心跳延后到这一拍再起（理由见 audio_out_start 里的注释）。 */
    if (s_tim_pending != 0u)
    {
        s_tim_pending = 0u;
        tim6_start();
        g_ao_stage = 6u;
    }

    if (g_ao_playing == 0u)
    {
        return;
    }

    /* 欠载自恢复（H7 第二个坑）：UDR 置位后引擎停吃 FIFO，必须写 UDRC 清掉才继续。 */
    if ((READ_BIT(s_hi2s.Instance->SR, SPI_SR_UDR) & SPI_SR_UDR) != 0UL)
    {
        SET_BIT(s_hi2s.Instance->IFCR, SPI_IFCR_UDRC);
        g_ao_udr++;
    }
    if ((READ_BIT(s_hi2s.Instance->SR, SPI_SR_TXP) & SPI_SR_TXP) != 0UL)
    {
        g_ao_txp_seen++;
    }

    dbg_snapshot();

    /* demo 播完：feeder 已把剩余帧数走完，这里负责收尾（停心跳 + 挂 codec）。 */
    if (s_demo_done != 0u)
    {
        s_demo_done = 0u;
        g_ao_demo   = 0u;
        audio_out_stop();
        return;
    }

    /* 补数：环形缓冲里剩的不超过半个环就补半个环（≈42 ms 的量，主循环一拍 16 ms 足够）。 */
    {
        uint32_t used = (s_wr - s_rd) & RING_MASK;

        if (used <= (RING_HALFWORDS / 2u))
        {
            pump_half();
        }
    }
}
