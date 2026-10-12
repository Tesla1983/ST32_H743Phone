/* ===========================================================================
 * ES8311 驱动实现 —— 见 es8311.h 顶部的硬件背景与序列来源说明
 * =========================================================================== */
#include "es8311.h"

#include "stm32h7xx_hal.h"

/* ---- 诊断量 ---- */
volatile int      g_es_rc     = 0;
volatile uint32_t g_es_wr_ok  = 0;
volatile uint32_t g_es_wr_ng  = 0;
volatile uint32_t g_es_rd_ok  = 0;
volatile uint32_t g_es_rd_ng  = 0;
volatile uint32_t g_es_bus_rc = 99;

static I2C_HandleTypeDef s_hi2c;
static uint8_t           s_addr7 = ES8311_I2C_ADDR_CE0;   /* 当前选定的 7 位地址 */

/* UI 音量的真正来源在 main.c（g_audio_volume）—— 本模块不认识 app_config，
 * 与 phone_shell 侧的"持久化回调"同一套分层：这里只读它的当前值。
 * ⚠ 声明必须在本文件**靠前**的位置：es8311_start_dac() 在下面几百行处就要用它。
 * ⚠ 出货构建里本文件不编进来（YMGUI_AUDIO_TEST=OFF），所以 main.c 那边对
 *   es8311_* 的调用必须同样包在 #if 里。 */
extern volatile uint32_t g_audio_volume;
/* codec 是否已经成功初始化过（es8311_init 返回 0 才置位）。
 * 【为什么需要它】UI 的音量滑杆在**出货构建**里也存在，而"拖音量"可能发生在
 *   任何人跑过 es8311_init 之前（甚至是没接模块的板子）。若无条件发 I2C 写，
 *   每次拖动都会撞一次 NACK ⇒ g_es_wr_ng 一直涨，看起来像"I2C 坏了"。
 *   ⇒ 没初始化就干脆不动总线（返回 -1，调用方忽略返回值）。 */
static uint8_t           s_codec_ready = 0u;

/* ====================== I2C1 总线 ====================== */

int es8311_bus_init(void)
{
    GPIO_InitTypeDef g;

    /* 时钟：I2C1 挂在 APB1（pclk1 = 100 MHz，sys.c 里 D2PPRE1=/2 配死）。
     * I2C123SEL 是复位默认值 pclk1，本工程没人动过它（全仓 grep 确认）。 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    /* PB6/PB7 → I2C1_SCL/SDA（AF4）。开漏 + 内部上拉（板上无外部上拉，见 .h 说明）。 */
    g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_AF_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    s_hi2c.Instance              = I2C1;
    s_hi2c.Init.Timing           = es8311_i2c_timing(HAL_RCC_GetPCLK1Freq());
    s_hi2c.Init.OwnAddress1      = 0;
    s_hi2c.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    s_hi2c.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    s_hi2c.Init.OwnAddress2      = 0;
    s_hi2c.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    s_hi2c.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&s_hi2c) != HAL_OK)
    {
        g_es_bus_rc = -1;
        g_es_rc     = -1;
        return -1;
    }
    /* 噪声滤波用复位默认（模拟滤波开 ANFOFF=0、数字滤波 DNF=0）——
     * 不显式调 HAL_I2CEx_Config*Filter：那两个函数在 hal_i2c_ex.c 里，
     * 为它多编一个文件不值得，默认值正好是想要的。 */
    g_es_bus_rc = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 100 kHz 标准模式的 TIMINGR 现场计算（不照抄 CubeMX 数值，换时钟树也不哑）
 *
 * TIMINGR = PRESC[31:28] | SCLDEL[23:20] | SDADEL[19:16] | SCLH[15:8] | SCLL[7:0]
 *   tPRESC = (PRESC+1)/pclk        取 ≈100 ns（pclk=100M ⇒ PRESC=9）
 *   tSCLL  = (SCLL+1)×tPRESC       低电平时间
 *   tSCLH  = (SCLH+1)×tPRESC       高电平时间
 *   fSCL   = 1/(tSCLL+tSCLH)       目标 100 kHz ⇒ 两周合计 10 µs，按 55/45 分
 *   SDADEL = 数据建立（SDA 在 SCL 上升前多久稳定）  取 ≈250 ns
 *   SCLDEL = 数据保持（SDA 在 SCL 下降后保持多久）  取 ≈500 ns
 * 100 MHz 代入：SCLL=54/SCLH=44（5.5 µs/4.5 µs，满足标准模式 4.7/4.0 µs 下限）。
 *
 * ⚠⚠ 2026-10-12 踩过并修掉的溢出 bug（第一版写法，留下来当墓碑）：
 *   原写法 `tick_ns = ((presc+1) * 1000000000u) / pclk` —— (presc+1)×1e9 = 1e10
 *   **超过 uint32**（2^32≈4.29e9）⇒ pclk=100M 时 tick_ns 算成 14 而不是 100，
 *   tot=714、SCLH/SCLL 又被 8 位字段截断（321&0xFF=65、391&0xFF=135），
 *   最终 TIMINGR=0x90FF4187（I2S 时钟约 43 kHz、建立/保持全错），器件无应答。
 *   现写法全部**先约分再除**：tot = pclk/((presc+1)×1e5)、
 *   sdadel = pclk/((presc+1)×4e6)、scldel = pclk/((presc+1)×2e6)，
 *   中间量最大几千，全程不爆。教训：ns↔Hz 换算别在 32 位里乘 1e9。
 * ------------------------------------------------------------------------- */
uint32_t es8311_i2c_timing(uint32_t pclk)
{
    uint32_t presc, tot, scll_t, sclh_t, sdadel, scldel;

    if (pclk == 0u) pclk = 100000000u;

    presc = pclk / 10000000u;            /* 目标 tPRESC≈100 ns */
    if (presc > 0u) presc -= 1u;
    if (presc > 15u) presc = 15u;

    /* 100 kHz ⇒ 半周期合计 10 µs，折算成 tPRESC 个数：pclk/((presc+1)×1e5) */
    tot = pclk / ((presc + 1u) * 100000u);
    if (tot < 8u) tot = 8u;
    scll_t = (tot * 55u) / 100u;         /* 低 55% */
    if (scll_t < 4u) scll_t = 4u;
    sclh_t = tot - scll_t;
    if (sclh_t < 4u) { sclh_t = 4u; scll_t = tot - sclh_t; if (scll_t < 4u) scll_t = 4u; }

    /* 250 ns / 500 ns 折算成 tPRESC 个数（先约分防溢出，同上）*/
    sdadel = pclk / ((presc + 1u) * 4000000u);
    if (sdadel < 1u)  sdadel = 1u;
    if (sdadel > 15u) sdadel = 15u;
    scldel = pclk / ((presc + 1u) * 2000000u);
    if (scldel < 2u)  scldel = 2u;
    if (scldel > 15u) scldel = 15u;

    return (presc   << 28) | (scldel << 20) | (sdadel << 16) |
           ((sclh_t - 1u) << 8) | (scll_t - 1u);
}

/* ====================== 寄存器读写 ====================== */

int es8311_probe(uint8_t addr7)
{
    return es8311_probe_to(addr7, 20u);
}

int es8311_probe_to(uint8_t addr7, uint32_t timeout_ms)
{
    if (HAL_I2C_IsDeviceReady(&s_hi2c, (uint16_t)(addr7 << 1), 3u, timeout_ms) == HAL_OK)
    {
        return 0;
    }
    return -1;
}

void es8311_set_addr(uint8_t addr7)
{
    s_addr7 = addr7;
}

int es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = val;
    if (HAL_I2C_Master_Transmit(&s_hi2c, (uint16_t)(s_addr7 << 1), buf, 2u, 50u) == HAL_OK)
    {
        g_es_wr_ok++;
        return 0;
    }
    g_es_wr_ng++;
    return -1;
}

int es8311_read_reg(uint8_t reg, uint8_t *val)
{
    if (HAL_I2C_Master_Transmit(&s_hi2c, (uint16_t)(s_addr7 << 1), &reg, 1u, 50u) != HAL_OK)
    {
        g_es_rd_ng++;
        return -1;
    }
    if (HAL_I2C_Master_Receive(&s_hi2c, (uint16_t)(s_addr7 << 1), val, 1u, 50u) != HAL_OK)
    {
        g_es_rd_ng++;
        return -1;
    }
    g_es_rd_ok++;
    return 0;
}

int es8311_read_id(uint8_t id[3])
{
    uint8_t v;

    if (es8311_read_reg(ES8311_REG_ID1, &v) != 0) return -1;
    id[0] = v;
    if (es8311_read_reg(ES8311_REG_ID2, &v) != 0) return -1;
    id[1] = v;
    if (es8311_read_reg(ES8311_REG_VER, &v) != 0) return -1;
    id[2] = v;
    return 0;
}

/* ====================== 初始化序列（esp-adf 移植） ====================== */

int es8311_init(void)
{
    uint8_t v;
    int rc = 0;

    /* ---- 1a. 上电默认配置 ---- */
    rc |= es8311_write_reg(0x01, 0x30);   /* CLK_MANAGER_REG01 */
    rc |= es8311_write_reg(0x02, 0x00);   /* clk divider/multiplier */
    rc |= es8311_write_reg(0x03, 0x10);   /* adc fs mode + osr */
    rc |= es8311_write_reg(0x16, 0x24);   /* ADC_REG16：模拟 PGA 增益 */
    rc |= es8311_write_reg(0x04, 0x10);   /* dac osr */
    rc |= es8311_write_reg(0x05, 0x00);   /* adc/dac clk divider */
    rc |= es8311_write_reg(0x0B, 0x00);   /* SYSTEM_REG0B */
    rc |= es8311_write_reg(0x0C, 0x00);   /* SYSTEM_REG0C */
    rc |= es8311_write_reg(0x10, 0x1F);   /* SYSTEM_REG10 */
    rc |= es8311_write_reg(0x11, 0x7F);   /* SYSTEM_REG11 */

    /* ---- 1b. 复位 + 主从选择 ----
     * 0x00 = 0x80：复位数字/时钟管理；bit6(0x40)=0 ⇒ codec 作 I2S **从机**
     *（本板 STM32 出 MCLK/SCK/LRCK）。esp-adf 是读-改-写，照做。 */
    rc |= es8311_write_reg(0x00, 0x80);
    if (es8311_read_reg(0x00, &v) == 0)
    {
        v &= (uint8_t)~0x40u;
        rc |= es8311_write_reg(0x00, v);
    }

    /* ---- 1c. MCLK 来源：来自 MCLK 引脚（bit7=0），不反相（bit6=0）---- */
    rc |= es8311_write_reg(0x01, 0x3F);
    if (es8311_read_reg(0x01, &v) == 0)
    {
        v &= (uint8_t)~0x80u;
        rc |= es8311_write_reg(0x01, v);
    }

    /* ---- 1d. 时钟系数：256fs（pre_div=1, multi=1, adc/dac_div=1, osr=0x10,
     *        bclk_div=4 ⇒ SCK=64fs，lrck=0x00FF ⇒ LRCK=MCLK/256=fs）---- */
    rc |= es8311_write_reg(0x02, 0x00);
    rc |= es8311_write_reg(0x05, 0x00);
    rc |= es8311_write_reg(0x03, 0x10);
    rc |= es8311_write_reg(0x04, 0x10);
    rc |= es8311_write_reg(0x08, 0xFF);           /* lrck_l */
    if (es8311_read_reg(0x07, &v) == 0)           /* lrck_h = 0 */
    {
        v &= (uint8_t)0xC0u;
        rc |= es8311_write_reg(0x07, v);
    }
    if (es8311_read_reg(0x06, &v) == 0)           /* bclk_div = 4 ⇒ 写 3 */
    {
        v = (uint8_t)((v & 0xE0u) | 0x03u);
        rc |= es8311_write_reg(0x06, v);
    }
    if (es8311_read_reg(0x01, &v) == 0)           /* MCLK 不反相 */
    {
        v &= (uint8_t)~0x40u;
        rc |= es8311_write_reg(0x01, v);
    }
    if (es8311_read_reg(0x06, &v) == 0)           /* SCK 不反相 */
    {
        v &= (uint8_t)~0x20u;
        rc |= es8311_write_reg(0x06, v);
    }

    /* ---- 1e. 系统/ADC 收尾 ---- */
    rc |= es8311_write_reg(0x13, 0x10);   /* SYSTEM_REG13 */
    rc |= es8311_write_reg(0x1B, 0x0A);   /* ADC_REG1B */
    rc |= es8311_write_reg(0x1C, 0x6A);   /* ADC_REG1C */
    rc |= es8311_write_reg(0x44, 0x08);   /* GPIO_REG44 */

    /* esp-adf 这里还有 pa_power(true)（GPIO 拉高外部功放使能）——
     * 本模块 NS4150B 无常闭使能脚，跳过。 */

    if (rc == 0)
    {
        s_codec_ready = 1u;
    }
    g_es_rc = rc;
    return rc;
}

int es8311_config_i2s(void)
{
    uint8_t v;
    int rc = 0;

    /* 16 位数据（位[3:2]=11）+ Philips I2S（位[1:0]=00），DAC/ADC 两侧都配 */
    if (es8311_read_reg(0x09, &v) == 0)
    {
        v = (uint8_t)((v | 0x0Cu) & 0xFCu);
        rc |= es8311_write_reg(0x09, v);
    }
    if (es8311_read_reg(0x0A, &v) == 0)
    {
        v = (uint8_t)((v | 0x0Cu) & 0xFCu);
        rc |= es8311_write_reg(0x0A, v);
    }

    g_es_rc = rc;
    return rc;
}

int es8311_start_dac(void)
{
    uint8_t v;
    int rc = 0;

    /* DAC 通路开（0x09 bit6=0），ADC 通路关（0x0A bit6=1）*/
    if (es8311_read_reg(0x09, &v) == 0)
    {
        v &= (uint8_t)~0x40u;
        rc |= es8311_write_reg(0x09, v);
    }
    if (es8311_read_reg(0x0A, &v) == 0)
    {
        v |= 0x40u;
        rc |= es8311_write_reg(0x0A, v);
    }

    /* 音量取**当前 UI 音量**（控制中心滑杆的值，出厂 70 ⇒ 0x85 ≈ -28 dB），
     * 不再写死 0xBF：否则用户在抽屉里把音量拖小、下次起播又被拉回 0 dB。 */
    rc |= es8311_write_reg(0x32, es8311_volume_reg(g_audio_volume));
    rc |= es8311_write_reg(0x17, 0xBF);   /* ADC 侧（本测试不用）*/
    rc |= es8311_write_reg(0x0E, 0x02);
    rc |= es8311_write_reg(0x12, 0x00);   /* SYSTEM_REG12：power up */
    rc |= es8311_write_reg(0x14, 0x1A);
    if (es8311_read_reg(0x14, &v) == 0)
    {
        v &= (uint8_t)~0x40u;             /* 非 DMIC */
        rc |= es8311_write_reg(0x14, v);
    }
    rc |= es8311_write_reg(0x0D, 0x01);
    rc |= es8311_write_reg(0x15, 0x40);
    rc |= es8311_write_reg(0x37, 0x48);   /* DAC_REG37：ramp */
    rc |= es8311_write_reg(0x45, 0x00);

    g_es_rc = rc;
    return rc;
}

int es8311_stop_dac(void)
{
    int rc = 0;

    rc |= es8311_write_reg(0x32, 0x00);
    rc |= es8311_write_reg(0x17, 0x00);
    rc |= es8311_write_reg(0x0E, 0xFF);
    rc |= es8311_write_reg(0x12, 0x02);
    rc |= es8311_write_reg(0x14, 0x00);
    rc |= es8311_write_reg(0x0D, 0xFA);
    rc |= es8311_write_reg(0x15, 0x00);
    rc |= es8311_write_reg(0x37, 0x08);
    rc |= es8311_write_reg(0x45, 0x01);

    g_es_rc = rc;
    return rc;
}

int es8311_set_volume(uint8_t vol)
{
    int rc = es8311_write_reg(0x32, vol);
    g_es_rc = rc;
    return rc;
}

uint8_t es8311_volume_reg(uint32_t vol0_100)
{
    if (vol0_100 > 100u)
    {
        vol0_100 = 100u;
    }
    return (uint8_t)((vol0_100 * 0xBFu) / 100u);
}

int es8311_set_volume_ui(uint32_t vol0_100)
{
    int rc;

    if (s_codec_ready == 0u)
    {
        return -1;                 /* codec 还没起来：不动总线、不污染失败计数 */
    }
    rc = es8311_write_reg(0x32, es8311_volume_reg(vol0_100));
    g_es_rc = rc;
    return rc;
}
