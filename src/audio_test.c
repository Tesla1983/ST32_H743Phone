/* ===========================================================================
 * 音频链路自检实现 —— 接线/触发/判据见 audio_test.h；最终接线表与踩坑见
 * docs/DEVELOPMENT_LOG.md 的「2026-10-12 · ES8311 音频模块接入」一节
 * （原 docs/ES8311_PIN_FEASIBILITY.md 已并入那里）。
 * 这里只记"上板前必须知道的几件事"：
 *
 * 1. I2S2 内核时钟没人动过：SPI123SEL 是复位默认 pll1_q（=200 MHz），
 *    与 SPI1（ESP32 链路）**共用同一个时钟源域**。本模块只读它、不写它 ——
 *    改 SPI123SEL 会顺带改 SPI1 波特率，那是另一件事（RM0433 RCC_D2CCIP1R）。
 *    HAL_I2S_Init 会自己按 200 MHz 算分频器，我们只把结果回读出来当期望值。
 *
 * 2. 分频语义（HAL 源码 stm32h7xx_hal_i2s.c:385 核对过）：
 *    MCKOE=1 时线性分频器输出 = MCLK = i2sclk/(2×I2SDIV+ODD)；
 *    16B_EXTENDED（16 位数据装 32 位时隙）时 SCK = MCLK/4 = 64fs，
 *    LRCK = SCK/64 = MCLK/256 = fs。
 *    ES8311 侧系数（pre_div=1/multi=1/dac_div=1/osr=0x10/bclk_div=4/lrck=0xFF）
 *    同样是 fs=MCLK/256、SCK=MCLK/4 —— **两边由同一颗 MCLK 推导，天然同步**，
 *    不挑 MCLK 绝对精度（本板 12.5 MHz ⇒ fs=48.83 kHz，比标称快 1.7%，无听感差异）。
 *
 * 3. 数据格式：ES8311 16 位 / Philips / 64fs SCK（esp-adf 系数表 bclk_div=4）
 *    ⇒ STM32 侧必须是 I2S_DATAFORMAT_16B_EXTENDED（CHLEN=32）。
 *    用 16B（CHLEN=16，SCK=32fs）会让 codec 收错位 —— 别改。
 *
 * 4. 不走 HAL_I2S_Transmit：它按"整块缓冲一次性轮询"设计，两次调用之间有
 *    函数进出间隙，48.8 kHz 立体声（一个字 10.2 µs）下可能欠载。
 *    改为自写流写循环：每次写左/右声道前都轮询 TXP（发送 FIFO 有空位），
 *    从结构上不可能欠载；等待带 100 ms 卡死保护（时钟没起来时不会挂主板）。
 * =========================================================================== */
#include "audio_test.h"
#include "es8311.h"

#include "stm32h7xx_hal.h"
#include <math.h>

/* ---- 触发量与结果量 ---- */
volatile uint32_t g_audio_test = 0;

volatile int      g_at_rc       = 99;
volatile uint32_t g_at_stage    = 0;
volatile uint32_t g_at_i2c_addr = 0;
volatile uint32_t g_at_id0      = 0;
volatile uint32_t g_at_id1      = 0;
volatile uint32_t g_at_id2      = 0;
volatile int      g_at_i2s_rc   = 99;
volatile uint32_t g_at_frames   = 0;
volatile uint32_t g_at_mclk_hz  = 0;
volatile uint32_t g_at_fs_hz    = 0;
volatile uint32_t g_at_sck_hz   = 0;
volatile uint32_t g_at_hang     = 0;
volatile uint32_t g_at_udr      = 0;    /* 流式期间欠载次数（清掉就能继续，属正常容错）*/
volatile uint32_t g_at_amp      = 28000;    /* 峰值 ≈85% 满幅（2026-10-12：24% 时用户没听到，*/
                                           /* 先排除"太轻"这一变量；0x32=0xBF=0dB 数字增益不变）*/
volatile uint32_t g_at_probe_us = 0;
volatile uint32_t g_at_scan_n   = 0;
volatile uint32_t g_at_scan_first = 0;
volatile uint32_t g_at_bb_norm  = 0;
volatile uint32_t g_at_bb_cross = 0;
volatile uint32_t g_at_bb_first = 0;
volatile uint32_t g_at_kclk     = 0;

/* ---- 配置 ---- */
#define AT_LUT_N          200u                        /* 正弦表样本数（= 周期）*/
#define AT_SDO_DUAL_PIN   1                           /* DOUT 兼容 PB15/PC1 两颗，见下 */
#define AT_TX_TIMEOUT_CYC (200000000u / 10u)          /* 50 ms 卡死保护（400 MHz 主频）*/
#define AT_FADE_MS        40u                         /* 淡入/淡出时长 */

static I2S_HandleTypeDef  s_hi2s;
static int16_t            s_lut[AT_LUT_N];

/* ====================== I2S2 总线 ====================== */

static int at_i2s_init(void)
{
    GPIO_InitTypeDef g;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();      /* I2S2 = SPI2 的 I2S 模式 */

    g.Mode  = GPIO_MODE_AF_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;      /* 全部 AF5，DS12110 Table 9 逐脚核对过 */

    g.Pin = GPIO_PIN_6;               /* PC6 = I2S2_MCK —— MCLK 唯一可用源之一 */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_3;               /* PD3 = I2S2_CK（摄像头 PWDN 脚，空闲）*/
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_9;               /* PB9 = I2S2_WS ⚠ 占用即作废 UART4 回退 TX
                                          （其 RX 线第十八版已拆，本就只剩半边）*/
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_15;              /* PB15 = I2S2_SDO（方案 A 主选 DOUT）*/
    HAL_GPIO_Init(GPIOB, &g);
#if AT_SDO_DUAL_PIN
    /* PC1 = I2S2_SDO：方案 A 的 DOUT 允许 PB15 或 PC1，用户具体接了哪颗无法从
     * 固件侧知道 ⇒ 两颗都配成 SDO，接哪颗都能响，没接的那颗只是空转。
     * 确定接线后把另一颗从这里删掉（并清 AT_SDO_DUAL_PIN）。 */
    g.Pin = GPIO_PIN_1;
    HAL_GPIO_Init(GPIOC, &g);
#endif
    g.Pin = GPIO_PIN_2;               /* PC2 = I2S2_SDI（DIN，本测试只播不录，
                                         先配上备将来录音；AF_PP 对输入脚即 Hi-Z）*/
    HAL_GPIO_Init(GPIOC, &g);

    s_hi2s.Instance              = SPI2;
    s_hi2s.Init.Mode             = I2S_MODE_MASTER_TX;
    s_hi2s.Init.Standard         = I2S_STANDARD_PHILIPS;
    s_hi2s.Init.DataFormat       = I2S_DATAFORMAT_16B_EXTENDED;  /* 16 位装 32 位时隙 ⇒ SCK=64fs */
    s_hi2s.Init.MCLKOutput       = I2S_MCLKOUTPUT_ENABLE;
    s_hi2s.Init.AudioFreq        = I2S_AUDIOFREQ_48K;            /* HAL 自行算 I2SDIV/ODD */
    s_hi2s.Init.CPOL             = I2S_CPOL_LOW;
    s_hi2s.Init.FirstBit         = I2S_FIRSTBIT_MSB;
    s_hi2s.Init.WSInversion      = I2S_WS_INVERSION_DISABLE;
    s_hi2s.Init.Data24BitAlignment = I2S_DATA_24BIT_ALIGNMENT_RIGHT;
    s_hi2s.Init.MasterKeepIOState = I2S_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_I2S_Init(&s_hi2s) != HAL_OK)
    {
        g_at_i2s_rc = -1;
        return -1;
    }
    g_at_i2s_rc = 0;

    /* ---- 显式写死 I2SDIV/ODD（不依赖 HAL 的时钟查询函数）----
     * HAL_I2S_Init 自己也会算（用 HAL_RCCEx_GetPeriphCLKFreq(SPI123)），本板实测
     * 它算对了：I2SCFGR=0x02080405 ⇒ I2SDIV=8/ODD=0 ⇒ 分频=16。
     * 这里按**真实内核时钟**再写一遍同样的值，让"频率从哪来"在代码里自证：
     * 内核时钟从 SystemCoreClock + PLL1DIVR 现场算（P/Q 字段都是"值-1"存储），
     * 不信任何 HAL 查询函数；若将来有人改 SPI123SEL 或 PLL 配置，这里自动跟着变。
     * （2026-10-12 教训：曾据错误位域回读误判"HAL 分频算错"——I2SDIV 在 bits 23:16，
     *   不在 bits 7:0；回读解码见下方，与这里写入口径一致。） */
    {
        uint32_t sysclk = SystemCoreClock;                      /* 400 MHz */
        uint32_t p1 = ((RCC->PLL1DIVR >> 9) & 0x7Fu) + 1u;      /* P=2 */
        uint32_t q1 = ((RCC->PLL1DIVR >> 16) & 0x7Fu) + 1u;     /* Q=4 */
        uint32_t vco  = sysclk * p1;                            /* 800 MHz */
        uint32_t kclk = vco / q1;                               /* 200 MHz */
        uint32_t target = 48000u * 256u;                        /* 12.288 MHz */
        uint32_t div, odd, i2sdiv;

        g_at_kclk = kclk;
        div = (kclk + target / 2u) / target;                    /* 四舍五入 ⇒ 16 */
        odd = div & 1u;
        i2sdiv = (div - odd) / 2u;
        if (i2sdiv < 1u)   { i2sdiv = 1u; odd = 0u; }
        if (i2sdiv > 255u) { i2sdiv = 255u; odd = 1u; }

        MODIFY_REG(s_hi2s.Instance->I2SCFGR,
                   (SPI_I2SCFGR_I2SDIV | SPI_I2SCFGR_ODD),
                   ((i2sdiv << SPI_I2SCFGR_I2SDIV_Pos) | (odd << SPI_I2SCFGR_ODD_Pos)));
    }

    /* ---- 回读分频器，把期望频率留给示波器/逻辑分析仪对照 ----
     * I2SDIV 在 I2SCFGR bits 23:16，ODD 在 bit 24（曾按 bits 7:0 解码，读出假 20 MHz）。 */
    {
        uint32_t cfgr = s_hi2s.Instance->I2SCFGR;
        uint32_t div  = 2u * ((cfgr >> 16) & 0xFFu) + ((cfgr >> 24) & 1u);
        uint32_t kclk = g_at_kclk;

        if (div == 0u) div = 1u;
        if (kclk == 0u) kclk = 200000000u;    /* 与 sys.c 的 PLL1_Q 一致 */
        g_at_mclk_hz = kclk / div;
        g_at_fs_hz   = g_at_mclk_hz / 256u;
        g_at_sck_hz  = g_at_mclk_hz / 4u;
    }

    /* 清全部状态标志再起引擎（与 HAL_I2S_Init 的 WRITE_REG(IFCR, 0x0FF8) 一致），
     * 防止上一轮遗留的 UDR/SUSP 把 TXP 压在 0。 */
    s_hi2s.Instance->IFCR = 0x0FF8u;

    /* ⚠⚠ H7 特有坑（2026-10-12 实测）：I2S 置 SPE 只是"使能外设"，**不会开始传输**；
     * 必须再置 CR1 的 CSTART（bit 9）——HAL_I2S_Transmit/Receive 内部都是这么干的
     * （见 stm32h7xx_hal_i2s.c 各收发函数的 SET_BIT(CR1, SPI_CR1_CSTART)）。
     * 漏了它：TXP 永远不亮，往 TXDR 写满 FIFO（8 包）后必超时挂死。 */
    __HAL_I2S_ENABLE(&s_hi2s);
    SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART);
    return 0;
}

static void at_i2s_teardown(void)
{
    GPIO_InitTypeDef g;

    /* 先挂起再关（照 HAL_I2S_Suspend 的模式）：CSUSP 等当前帧自然结束、
     * 硬件清 CSTART，再关 SPE —— 不在时钟中途硬切，codec 侧不会看到半个帧。 */
    if (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
    {
        uint32_t t0 = HAL_GetTick();

        SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSUSP);
        while (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
        {
            if (HAL_GetTick() - t0 > 100u) break;    /* 100 ms 兜底，不等死 */
        }
    }
    __HAL_I2S_DISABLE(&s_hi2s);

    /* PB9 还给 UART4_TX（uart_link.c 开机时的配置：AF8 / 上拉 / 高速）——
     * 测试做完把回退路径还回去，别让一次自检悄悄改掉启动状态。 */
    g.Pin       = GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOB, &g);

    /* 其余 I2S 脚（PC6/PD3/PB15/PC1/PC2）留在 AF 上，但外设已关 ⇒ 输出驱动器
     * 关闭、等效悬空输入，无害，不额外恢复。 */
}

/* ====================== 流写 ====================== */

/* H7 I2S 实测规则（2026-10-12）：**欠载（UDR=1）会把 TXP 一直压在 0，且引擎停吃
 * FIFO** —— 必须清 IFCR 的 UDRC 才能恢复。HAL_I2S_Transmit 每写一笔就检查并清一次
 * UDR（见 stm32h7xx_hal_i2s.c 895-903 行），这里照做。
 * 触发条件：FIFO 深度只有 8 包（≈82 µs @48.8 kHz），CPU 任何一次 >82 µs 的停顿
 * （中断/缓存未命中）都会欠载一次；不清 UDR 的话流就永久卡死。 */
static int at_tx_wait(void)
{
    uint32_t t0 = DWT->CYCCNT;

    for (;;)
    {
        uint32_t sr = SPI2->SR;

        if ((sr & SPI_SR_TXP) != 0u) return 0;
        if ((sr & SPI_SR_UDR) != 0u)
        {
            SPI2->IFCR = SPI_IFCR_UDRC;      /* 清欠载标志，TXP 才会放开 */
            g_at_udr++;
            continue;
        }
        if ((uint32_t)(DWT->CYCCNT - t0) > AT_TX_TIMEOUT_CYC)
        {
            return -1;                        /* 时钟没起来时的卡死保护 */
        }
    }
}

static void at_lut_fill(void)
{
    uint32_t i;
    int32_t  peak = (int32_t)g_at_amp;
    double   twopi = 6.283185307179586;

    if (peak > 32000) peak = 32000;           /* 留 1 LSB 余量，防包络乘爆 */
    if (peak < 0)     peak = 0;
    for (i = 0; i < AT_LUT_N; i++)
    {
        double s = sin(twopi * (double)i / (double)AT_LUT_N);
        s_lut[i] = (int16_t)(s * (double)peak);
    }
}

/* 播一个音：period_inc ∈ {1,2,4} ⇒ 周期 {200,100,50} 样本 ⇒
 * 频率 {fs/200, fs/100, fs/50} ≈ {244, 488, 977} Hz。首尾 AT_FADE_MS 线性淡入淡出。 */
static int at_stream(uint32_t period_inc, uint32_t ms)
{
    uint32_t cyc_per_ms = SystemCoreClock / 1000u;
    uint32_t total = ms * cyc_per_ms;
    uint32_t fade  = AT_FADE_MS * cyc_per_ms;
    uint32_t t0 = DWT->CYCCNT;
    uint32_t acc = 0;
    uint32_t frames = 0;

    for (;;)
    {
        uint32_t el = (uint32_t)(DWT->CYCCNT - t0);
        int32_t  amp, s;

        if (el >= total) break;

        if (el < fade)              amp = (int32_t)((uint64_t)el * 256u / fade);
        else if (el > total - fade) amp = (int32_t)((uint64_t)(total - el) * 256u / fade);
        else                        amp = 256;
        if (amp < 0)   amp = 0;
        if (amp > 256) amp = 256;

        if (acc >= AT_LUT_N) acc -= AT_LUT_N;
        s = ((int32_t)s_lut[acc] * amp) >> 8;

        if (at_tx_wait() != 0) { g_at_hang = 1; g_at_frames += frames; return -1; }
        *(volatile uint16_t *)&SPI2->TXDR = (uint16_t)(int16_t)s;   /* 左 */

        if (at_tx_wait() != 0) { g_at_hang = 1; g_at_frames += frames; return -1; }
        *(volatile uint16_t *)&SPI2->TXDR = (uint16_t)(int16_t)s;   /* 右（同值=单声道）*/

        acc += period_inc;        /* LUT 每**帧**进一步 ⇒ 频率 = period_inc×fs/200 */
        frames++;
    }

    g_at_frames += frames;
    return 0;
}

/* ====================== case ====================== */

/* case 1：I2C 全地址段普查 + ES8311 探测 + 读芯片 ID（不出声）。
 * 2026-10-12 晚加了两级诊断（模块不 ACK 时的定位）：
 *   ① 全地址段 0x08~0x77 普查（2 ms 超时、只读探测）——万一器件不在 0x18/0x19；
 *   ② 探测耗时（DWT 计时）——快速 NACK 说明外设在正常收发，等满 6×20ms 超时
 *      说明内核时钟没跑/总线卡死；
 *   ③ 探测失败后照样试一次**真实 2 字节写**（走 Master_Transmit 而非 0 字节探测）
 *      ——区分"HAL 的 Size=0 探测路径有问题"和"真 NACK"。 */
static void at_case_scan(void)
{
    uint8_t id[3] = {0u, 0u, 0u};
    uint32_t t0;
    uint8_t a;

    g_at_stage = 1;
    if (es8311_bus_init() != 0) { g_at_rc = -1; g_at_stage = 100; return; }

    /* ① 全地址段普查（0x08~0x77，2 ms 超时，只读探测不写寄存器）*/
    g_at_stage = 2;
    g_at_scan_n     = 0;
    g_at_scan_first = 0;
    for (a = 0x08u; a <= 0x77u; a++)
    {
        if (es8311_probe_to(a, 2u) == 0)
        {
            if (g_at_scan_n == 0u) g_at_scan_first = a;
            g_at_scan_n++;
        }
    }

    /* ② ES8311 两个候选地址 + DWT 耗时测量 */
    t0 = DWT->CYCCNT;
    g_at_i2c_addr = 0;
    if (es8311_probe(ES8311_I2C_ADDR_CE0) == 0)      g_at_i2c_addr = ES8311_I2C_ADDR_CE0;
    else if (es8311_probe(ES8311_I2C_ADDR_CE1) == 0) g_at_i2c_addr = ES8311_I2C_ADDR_CE1;
    g_at_probe_us = (uint32_t)((DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u));

    /* ③ 探测失败 ≠ 器件不在：试一次真实 2 字节写（0x00=0x80 是软复位，无害）*/
    if (g_at_i2c_addr == 0u)
    {
        es8311_set_addr(ES8311_I2C_ADDR_CE0);
        if (es8311_write_reg(0x00, 0x80) == 0)      g_at_i2c_addr = ES8311_I2C_ADDR_CE0;
        else
        {
            es8311_set_addr(ES8311_I2C_ADDR_CE1);
            if (es8311_write_reg(0x00, 0x80) == 0)  g_at_i2c_addr = ES8311_I2C_ADDR_CE1;
        }
    }
    else
    {
        es8311_set_addr(g_at_i2c_addr);
    }

    if (g_at_i2c_addr == 0u) { g_at_rc = -2; g_at_stage = 100; return; }

    g_at_stage = 3;
    if (es8311_read_id(id) != 0) { g_at_rc = -3; g_at_stage = 100; return; }
    g_at_id0 = id[0];
    g_at_id1 = id[1];
    g_at_id2 = id[2];
    g_at_rc    = 0;
    g_at_stage = 100;
}

/* 全链路放音（case 2 = 五音上行×2 轮；case 4 = 单音 3 秒长音）——阻塞期间 UI 冻结。
 *
 * ⚠ 2026-10-12 晚改顺序：**先起 I2S 时钟再探测 I2C**。原顺序是 bus→probe→codec→I2S，
 *   探测失败就退出 ⇒ I2S 时钟从没起来过。有些 codec（含 ES 系）要先有时钟才应答 I2C，
 *   这个假设此前从没被测过。时钟起来后还带 1 s 的反复重试（给 codec 内部电路稳定时间）。
 * long_tone=1 ⇒ 连续 3 秒 ≈977 Hz 长音（用户要求"不要滴滴滴，直接放三秒长音"）；
 *                 =0 ⇒ 五音上行滴两轮。 */
static void at_case_play(uint32_t long_tone)
{
    uint32_t try_n;
    uint8_t  id[3];

    g_at_frames = 0;
    g_at_hang   = 0;

    g_at_stage = 1;
    if (es8311_bus_init() != 0) { g_at_rc = -1; g_at_stage = 100; return; }

    /* ---- 先把 I2S2 的 MCLK/SCK/LRCK 跑起来（时钟先行假设）---- */
    g_at_stage = 5;
    if (at_i2s_init() != 0)       { g_at_rc = -6; g_at_stage = 100; return; }
    HAL_Delay(100u);              /* 让时钟稳定，也喂饱 codec 的内部逻辑 */

    /* ---- 时钟下来后再探测 I2C，带 1 s 重试 ---- */
    g_at_stage = 2;
    g_at_i2c_addr = 0;
    for (try_n = 0; try_n < 10u; try_n++)
    {
        if (es8311_probe(ES8311_I2C_ADDR_CE0) == 0)      { g_at_i2c_addr = ES8311_I2C_ADDR_CE0; break; }
        if (es8311_probe(ES8311_I2C_ADDR_CE1) == 0)      { g_at_i2c_addr = ES8311_I2C_ADDR_CE1; break; }
        HAL_Delay(100u);
    }
    if (g_at_i2c_addr == 0u)
    {
        /* 探测仍失败：试一次真实 2 字节写（0x00=0x80 软复位，无害）*/
        es8311_set_addr(ES8311_I2C_ADDR_CE0);
        if (es8311_write_reg(0x00, 0x80) == 0)      g_at_i2c_addr = ES8311_I2C_ADDR_CE0;
        else
        {
            es8311_set_addr(ES8311_I2C_ADDR_CE1);
            if (es8311_write_reg(0x00, 0x80) == 0)  g_at_i2c_addr = ES8311_I2C_ADDR_CE1;
        }
    }
    else
    {
        es8311_set_addr(g_at_i2c_addr);
    }
    if (g_at_i2c_addr == 0u) { g_at_rc = -2; g_at_stage = 100; at_i2s_teardown(); return; }

    /* 顺便读一次芯片 ID —— 单独跑 case 2/4 时输出里也能看到 ID（原来只有 case 1 读）*/
    if (es8311_read_id(id) == 0)
    {
        g_at_id0 = id[0];
        g_at_id1 = id[1];
        g_at_id2 = id[2];
    }

    g_at_stage = 4;
    if (es8311_init() != 0)       { g_at_rc = -4; g_at_stage = 100; at_i2s_teardown(); return; }
    if (es8311_config_i2s() != 0) { g_at_rc = -5; g_at_stage = 100; at_i2s_teardown(); return; }

    g_at_stage = 6;
    if (es8311_start_dac() != 0)  { g_at_rc = -7; g_at_stage = 100; at_i2s_teardown(); return; }

    at_lut_fill();

    if (long_tone != 0u)
    {
        /* 单音长音：≈977 Hz 连续 3 秒（LUT 分辨率 = fs/200 = 244 Hz；period_inc=4）*/
        at_stream(4u, 3000u);
    }
    else
    {
        /* 五音上行（244/488/733/977/1221 Hz），各 400 ms，两轮 */
        uint32_t pass;

        for (pass = 0u; pass < 2u; pass++)
        {
            at_stream(1u, 400u);           /* ≈244 Hz */
            HAL_Delay(200u);               /* 间隔里 I2S 继续跑（欠载=静音），时钟不断 */
            at_stream(2u, 400u);           /* ≈488 Hz */
            HAL_Delay(200u);
            at_stream(3u, 400u);           /* ≈733 Hz */
            HAL_Delay(200u);
            at_stream(4u, 400u);           /* ≈977 Hz */
            HAL_Delay(200u);
            at_stream(5u, 400u);           /* ≈1221 Hz */
            if (pass == 0u) HAL_Delay(600u);
        }
    }

    es8311_stop_dac();
    at_i2s_teardown();

    g_at_rc    = 0;
    g_at_stage = 100;
}

/* ====================== case 3：交叉位拍 I2C 普查 ====================== */

/* 位拍 I2C 主机（开漏 + 内部上拉，半周期 5 µs ≈ 100 kHz）。
 * 用来测一个 HAL I2C 测不出的假设：**SDA/SCL 在某处被接反了**（模块丝印错/用户看错位）。
 * 正常朝向 SDA=PB7、SCL=PB6 作对照，交叉朝向 SDA=PB6、SCL=PB7 是待验假设 ——
 * 若只有交叉朝向扫得到器件，把模块那两根线对调即可，硬件没坏。 */
static uint16_t s_bb_sda = GPIO_PIN_7;
static uint16_t s_bb_scl = GPIO_PIN_6;

static void bb_delay(void)
{
    uint32_t t0 = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - t0) < 2000u) { }   /* 5 µs @400 MHz */
}

static void bb_line(int level, uint16_t pin)
{
    HAL_GPIO_WritePin(GPIOB, pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void bb_start(void)
{
    bb_line(1, s_bb_sda); bb_line(1, s_bb_scl); bb_delay();
    bb_line(0, s_bb_sda); bb_delay();
    bb_line(0, s_bb_scl);
}

static void bb_stop(void)
{
    bb_line(0, s_bb_sda); bb_delay();
    bb_line(1, s_bb_scl); bb_delay();
    bb_line(1, s_bb_sda); bb_delay();
}

/* 写一个字节（MSB 先行），返回 0=从机 ACK */
static int bb_write_byte(uint8_t b)
{
    uint8_t i;
    int ack;

    for (i = 0u; i < 8u; i++)
    {
        bb_line((b & 0x80u) ? 1 : 0, s_bb_sda);
        bb_delay();
        bb_line(1, s_bb_scl); bb_delay();
        bb_line(0, s_bb_scl);
        b = (uint8_t)(b << 1);
    }
    bb_line(1, s_bb_sda);                 /* 释放 SDA 等 ACK */
    bb_delay();
    bb_line(1, s_bb_scl); bb_delay();
    ack = HAL_GPIO_ReadPin(GPIOB, s_bb_sda);   /* 0=被从机拉低=ACK */
    bb_line(0, s_bb_scl);
    return ack;
}

/* 扫 0x08~0x77，返回命中数；first 输出第一个命中地址（0=没有）*/
static uint32_t bb_scan(volatile uint32_t *first)
{
    uint8_t a;
    uint32_t n = 0u;

    *first = 0u;
    for (a = 0x08u; a <= 0x77u; a++)
    {
        int ack;
        bb_start();
        ack = bb_write_byte((uint8_t)(a << 1));
        bb_stop();
        if (ack == 0)
        {
            if (n == 0u) *first = a;
            n++;
        }
    }
    return n;
}

static void at_case_crossed(void)
{
    uint32_t dummy;
    GPIO_InitTypeDef g;

    g_at_stage = 1;

    /* 先把 I2C1 关掉（之前跑过 case 1/2 的话它是使能的），GPIO 才归我们管 */
    I2C1->CR1 &= ~0x1u;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_OUTPUT_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);

    /* ① 正常朝向对照：SDA=PB7, SCL=PB6（应与 HAL I2C 结果一致：0 命中）*/
    s_bb_sda = GPIO_PIN_7;
    s_bb_scl = GPIO_PIN_6;
    g_at_bb_norm = bb_scan(&dummy);

    /* ② 交叉朝向：SDA=PB6, SCL=PB7（待验假设）*/
    s_bb_sda = GPIO_PIN_6;
    s_bb_scl = GPIO_PIN_7;
    g_at_bb_cross = bb_scan(&g_at_bb_first);

    g_at_rc    = (g_at_bb_cross > 0u || g_at_bb_norm > 0u) ? 0 : -2;
    g_at_stage = 100;
}

/* ====================== 轮询入口 ====================== */

void audio_test_poll(void)
{
    uint32_t t = g_audio_test;

    if (t == 0u) return;
    g_audio_test = 0u;             /* 先清：重入不会二次触发 */

    switch (t)
    {
    case 1u:  at_case_scan(); break;
    case 2u:  at_case_play(0u); break;      /* 五音上行 ×2 轮 */
    case 3u:  at_case_crossed(); break;
    case 4u:  at_case_play(1u); break;      /* 单音 3 秒长音 ≈977 Hz */
    default:  g_at_rc = -100; g_at_stage = 100; break;
    }
}
