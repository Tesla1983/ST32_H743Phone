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
/* REG32（DAC 音量）**最后一次写进去的值** + 写入次数。
 * ⚠ 为什么必须有：本工程主机侧 probe-rs 读不到外设、也调不了 C 函数，而"暂停后续播
 *   没声音"这个现象的第一嫌疑就是"DAC 音量停在 0（静音）"。把每次 REG32 写入记在
 *   RAM 里，SWD 一次就能看到起播/续播后音量的**真实落点**（0xFFFF = 本次开机没写过）。*/
volatile uint32_t g_es_reg32_wr = 0xFFFFu;
volatile uint32_t g_es_reg32_n  = 0u;

/* ---- I2C 写失败诊断 + 恢复（2026-10-13：暂停→续播"没声音/音量乱"根因排障）----
 * 【现象】板上实测：一次"起播→暂停→续播"里 wr_ng 会涨几次，且**逐次恶化**
 *   （5 个循环里 wr_ng 累计 3→13→29→50→94），到后面连音量寄存器 REG32 都写不进去
 *   ⇒ 芯片音量停在 0（静音）⇒ "进度条在走但没声音"；只写进去一部分 ⇒ "音量大小混乱"。
 *   而所有失败**都被调用方忽略**（dac_path_regs / ramp 的返回值都是 (void)），
 *   所以既没声音也没人能发现。这两点叠起来就是用户报的那个现象。
 * 【对策】① 每次写失败先**复位 I2C 外设 + 复位 HAL 句柄状态**再重试（最多 3 次）；
 *         ② 记下失败现场（寄存器号 / HAL 状态码 / 最近 8 次）供 SWD 复盘；
 *         ③ ramp 结束后**回读校验**，不对就补写（见 audio_out 的 s_dac_rb_pending）。*/
volatile uint32_t g_es_ng_reg  = 0xFFFFu;  /* 最后一次写失败的寄存器号 */
volatile uint32_t g_es_ng_st   = 0xFFFFu;  /* 该次的 HAL_StatusTypeDef（1=NACK 2=BUSY 3=TIMEOUT）*/
volatile uint32_t g_es_ng_nack = 0u;       /* 三种失败分别计数 */
volatile uint32_t g_es_ng_busy = 0u;
volatile uint32_t g_es_ng_tmo  = 0u;
volatile uint32_t g_es_ng_ring[8];         /* 最近 8 次失败：高 8 位=寄存器号，低 8 位=状态码 */
volatile uint32_t g_es_ng_head = 0u;
volatile uint32_t g_es_retry_ok = 0u;      /* 重试后成功（第一次失败过）的次数 */
volatile uint32_t g_es_probe_retry = 0u;   /* probe 首试没探到、延时后探到的次数 */
volatile uint32_t g_es_fail    = 0u;       /* 重试 3 次仍失败的次数（>0 = 真丢写）*/
volatile uint32_t g_es_fix_wr  = 0u;       /* "回读发现不对 → 补写"的次数 */
volatile uint32_t g_es_recover_pe = 0u;    /* 因 BUSY/TIMEOUT 而做 I2C 软件复位(PE 0→1)的次数 */
volatile uint32_t g_es_readback = 0u;      /* 回读体检执行次数（含起播后那次与每秒那次）*/
/* ★ 2026-10-13：UI 卡死修复新增的两个计数（见 g_es_ng_idr 下方的长说明）*/
volatile uint32_t g_es_bus_clear = 0u;     /* I2C bus clear（9 时钟 + STOP）真正执行过的次数 */
volatile uint32_t g_es_breaker_trip = 0u;  /* I2C 熔断触发次数（连续硬失败过多 ⇒ 快速失败）*/
volatile uint32_t g_es_i2c_req = 0u;       /* 排障钩子：写 1 ⇒ 主循环按 g_es_i2c_hz 重定时 */
volatile uint32_t g_es_i2c_timingr = 0u;   /* 最近一次生效的 I2C1 TIMINGR（重定时诊断）*/
volatile uint32_t g_es_busclear_en = 1u;   /* ★排障开关：1=允许自动 bus clear（默认）；
                                            * 0=禁用（只做 PE 复位）—— 用于验证"单次 IDR 采样
                                            * 误判 SDA 被钳低 ⇒ 误触发 bus clear ⇒ 正反馈"
                                            * 的假设。验证完保持 1。 */
volatile uint32_t g_es_ng_idr  = 0xFFFFu;  /* 失败瞬间 PB6(SCL)/PB7(SDA) 的电平（GPIOB->IDR bit6/bit7）
                                            * 0x40/0x80 = 已释放（有上拉）；缺 0x80 ⇒ SDA 被从机压住 = 总线卡死*/

/* ★★ 2026-10-14（补丁13）：I2C **阻塞时间**诊断 —— "偶发卡死"的量化依据 ★★
 * 【为什么必须量它】"整个 UI 卡住无法滑动"的本质是**主循环单拍被 I2C 阻塞**（I2C 是
 *   阻塞轮询、跑在主循环线程里）。此前只知道"卡"，不知道**卡在哪一笔、卡了多久**。
 *   本轮实测（tools/ui_freeze_repro.py 40 轮真实点击）：第 5 次点击后主循环停转，
 *   同期 `g_es_fail` 从 0 涨到 49、`g_es_breaker_trip` **恒为 0**（熔断器从未触发）。
 *   ⇒ 把"每笔事务耗时"和"绕开了多少次 HAL 的固定等待"直接量出来。 */
volatile uint32_t g_es_tx_us_max = 0u;      /* 单笔 HAL I2C 事务的**最长耗时(µs)** */
volatile uint32_t g_es_slow_n    = 0u;      /* 耗时 > 5 ms 的事务笔数 */
volatile uint32_t g_es_busy_stuck_n = 0u;   /* 进 HAL 之前就发现 BUSY=1（外设卡住）的次数 ——
                                             * 每命中一次就省下 HAL 内部那笔**固定 25 ms** 等待 */
volatile uint32_t g_es_rcc_rst_n = 0u;      /* 对 I2C1 外设做 RCC 强制复位（I2C1RST）的次数 */
volatile uint32_t g_es_bc_fail   = 0u;      /* bus clear 之后 SDA 仍被从机钳住的次数 */
volatile uint32_t g_es_silent_n  = 0u;      /* "静音并确认"成功次数（es8311_dac_become_silent）*/
volatile uint32_t g_es_silent_ng = 0u;      /* "静音未能确认"次数（>0 = 切时钟时刻有爆音风险）*/
volatile uint32_t g_es_silent_vol = 0xFFu;  /* 上面那次确认读到的 REG32（应为 0x00）*/
volatile uint32_t g_es_silent_mute = 0xFFu; /* 上面那次确认读到的 REG31（bit6/5 应为 1）*/
volatile uint32_t g_es_pdown_n   = 0u;      /* es8311_stop_dac()（真·掉电）执行次数 ——
                                             * 补丁13 起**正常播放/停止路径不再调用它**，应恒 0 */
volatile uint32_t g_es_reg01_n   = 0u;      /* REG01（时钟管理）被写的次数 */
volatile uint32_t g_es_reg01_val = 0xFFFFu; /* REG01 最后一次写进去的值（应恒 0x3F）*/
volatile uint32_t g_es_pdown_req = 0u;      /* 排障钩子：写 1 ⇒ 主循环下一拍执行一次"真掉电"
                                             * （用于验证"掉电 → 起播恢复时钟"这条配对，见
                                             *  tools/es8311_clk_check.py 的 K2）*/
volatile uint32_t g_es_nack_sim  = 0u;      /* 排障钩子：0=正常；1=每笔事务直接判失败（模拟对端
                                             * 永不回应）；2=另假装 SDA 被钳住（把 bus clear 与
                                             * 熔断一起演练）。**纯软件、不碰任何电气**，
                                             * 用来把"最坏情况下的单拍阻塞上限"变成可复现的判据。*/

/* 写失败后的 I2C 现场复位（只动 I2C1 自己的寄存器 + HAL 句柄，不碰任何 GPIO 电气；
 * 定义放在 s_hi2c 声明之后，见下面 es8311_write_reg 之前）。 */

static I2C_HandleTypeDef s_hi2c;                          /* I2C1 句柄（bus_clear 里会复位其 State）*/
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

/* ---- I2C 硬失败熔断状态（2026-10-13 UI 卡死修复）----
 * consecutive_fail：连续"全重试仍失败"的笔数；达 ES8311_BREAKER_N 即触发熔断。
 * breaker_tick    ：熔断后 HAL_GetTick() 的冷却起点；冷却期内所有 I2C 立即快速失败。*/
static uint32_t          s_es_consec_fail = 0u;
static uint32_t          s_es_breaker_tick = 0u;
/* ★ 补丁13 新增：
 *   s_es_bc_streak —— 连续"做了 bus clear 却仍没换来一次成功"的次数。达 ES8311_BC_TRIP
 *                     即直接判总线不可用（把熔断阈值填满）—— 见 es8311_bus_clear() 的说明。
 *   s_es_fast_fail —— 1 = 已进入"降级快速失败"模式：上一笔事务耗时 > 5 ms（说明总线/从机
 *                     出问题），此后每笔只用**1 次尝试 + 1 ms 超时**，直到某笔又快又成功地
 *                     完成才退出。作用：把"烂总线"下单拍的 I2C 阻塞上限收缩一个量级。
 *   s_reg31        —— REG31 的**写侧缓存**：官方语义是"读-改-写只动 bit6/5"，但每笔读
 *                     都是一次额外的 NACK 机会（实测 0x31 正是失败最多的寄存器之一）
 *                     ⇒ 改成只在回读体检时刷新缓存、写的时候直接拼值。 */
static uint32_t          s_es_bc_streak = 0u;
static uint8_t           s_es_fast_fail = 0u;
static uint8_t           s_reg31        = 0x00u;

/* ★ SCL 目标频率（Hz）。默认 100 kHz；SWD 写 `g_es_i2c_hz` 后调 es8311_i2c_retiming()
 *   现场改 TIMINGR —— 用于验证"降频能否消除 ~30% 首试 NACK"的电气边际假设。
 *   范围夹在 10~400 kHz（I2C 标准模式上限 400 kHz，下限防手滑写 0 除零）。 */
volatile uint32_t g_es_i2c_hz = 100000u;

/* ====================== I2C1 总线 ====================== */

/* 前置声明：bus_init 末尾的总线自检会用到（定义在各辅助函数集中处，见下方）。 */
static void es8311_bus_clear(void);

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

    /* ★★ 开机总线自检 + 清理（2026-10-13 UI 卡死修复）★★
     * 【为什么开机就要做】I2C 总线卡死（从机把 SDA 钳低）是**从机侧**的状态，
     *   **不会随 MCU 复位而消失** —— 板载/外接 codec 的 5 V 常亮，它的 I2C
     *   状态机停在"正拉着 SDA"就一直拉着。所以"复位板子重来"并不必然回到干净态：
     *   实测就出现过复位后 bus 仍为 0x40（SDA 被钳住），于是固件一开机就每笔 I2C
     *   都耗满超时 ⇒ 起播/续播全废。
     *   ⇒ 上电配置完 I2C 后立刻看一次电平：SDA 被钳住就先 bus clear 再放行。
     *   ⚠ 放在 g_es_bus_rc=0 之后：bus_clear 的前置检查依赖它。 */
    if ((GPIOB->IDR & GPIO_PIN_7) == 0u)
    {
        es8311_bus_clear();          /* SDA 被从机压住 ⇒ 9 时钟 + STOP 把它走完 */
    }
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

    /* ⚠⚠ 目标 SCL 频率 = g_es_i2c_hz（默认 100 kHz，可 SWD 现场下调做电气余量实验）。
     *   【为什么可调】2026-10-13 零探针窗口实测（零探针窗口实验：全程不碰探针、
     *   播放中静置 20 s）：本模块 I2C **首试 NACK 率约 30%**，且成簇爆发（g_es_fail 一次
     *   涨 2~5 笔）、ng_ring 全是 NACK、失败瞬间 g_es_ng_idr=0x40（SDA 被读成低）。
     *   根因指向**电气边际**：模块的 SDA/SCL 上拉**只有 MCU 内部 ~40 kΩ**（板上无外部
     *   上拉），配 100 kHz 时上升沿 (RC) 余量不足 ⇒ 采样点读到的电平还没升到位。
     *   降 SCL 频率 = 给上升沿更多时间 ⇒ 直接验证该假设（纯软件、零硬件风险）。 */
    tot = pclk / ((presc + 1u) * g_es_i2c_hz);
    if (tot < 8u) tot = 8u;
    if (tot > 500u) tot = 500u;          /* SCLH/SCLL 各 8 bit，总周期数上限 ~510 */
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

/* ★ SWD 现场重定时：读 g_es_i2c_hz 重算 TIMINGR 并写进 I2C1（无需重新烧录即可扫多个频率）。
 *   用法（主机）：probe-rs write ... g_es_i2c_hz <Hz>；再 write ... g_es_i2c_req 1；
 *   固件在主循环里消费 g_es_i2c_req 后调用本函数。返回最终生效的 TIMINGR（诊断）。
 *   ⚠ 只在 g_es_bus_rc==0（I2C1 已初始化）时有效；否则直接返回 0。
 *   ⚠ HAL_I2C_Init 会重置外设；调用前后驱动的 retry/breaker 状态无需变动（时序无关）。*/
uint32_t es8311_i2c_retiming(void)
{
    uint32_t hz = g_es_i2c_hz;

    if (g_es_bus_rc != 0u)
    {
        return 0u;
    }
    if (hz < 10000u)  hz = 10000u;      /* 下限 10 kHz：防手滑写 0（除零）+ 防过慢 */
    if (hz > 400000u) hz = 400000u;     /* 上限 400 kHz：I2C 标准模式极限 */
    g_es_i2c_hz = hz;

    s_hi2c.Init.Timing = es8311_i2c_timing(HAL_RCC_GetPCLK1Freq());
    (void)HAL_I2C_Init(&s_hi2c);
    g_es_i2c_timingr = s_hi2c.Init.Timing;
    return s_hi2c.Init.Timing;
}

/* ====================== 寄存器读写 ====================== */

/* 重试次数与间隔。
 * ⚠⚠ 2026-10-13 关键修正：单笔超时从 **50 ms 降到 5 ms**。
 *   【UI 卡死的直接原因】板上实测（无读取窗口测速，排除 probe halt 伪影）：
 *   "快速点播放/暂停"后主循环单拍耗时 **g_tick_ms = 3921 ms**，主循环速率从
 *   77 拍/秒掉到 0~16 拍/秒 ⇒ 用户看到的"整个 UI 卡住无法滑动"。
 *   算式完全吻合：一次起播约 19 笔 I2C，每笔 5 次重试 × 50 ms = 250 ms
 *   ⇒ 19 × 250 ms ≈ 4.75 s。50 ms 这个值对 100 kHz 总线毫无意义
 *   （一字节事务本身只要 ~90 µs，就算从机全程拉伸时钟也就几百 µs），
 *   它只是把"对端不应答"拖成了 UI 级卡顿。改成 5 ms 后最坏 ~19×15 ms ≈ 285 ms。
 * ⚠ 重试次数**保持 5**（补丁9 曾一度降到 3，2026-10-13 复测后改回）：
 *   【为什么改回】零探针窗口实测（零探针窗口实验：reset→起播→完全不再碰
 *   探针、静置 20 s）证明本模块 I2C 链路的**首试 NACK 率约 30%** —— 这是"内部上拉
 *   (~40 kΩ) + 100 kHz"下该模块的固有电气边际，**不是**探针 halt 伪影（零窗口照样
 *   30%），也**不是**补丁9 引入的。既然单笔首试就有 ~30% 会 NACK，那么"5 次重试仍
 *   全灭"的概率 ≈ 0.28⁵ ≈ 0.17%，而"3 次重试仍全灭" ≈ 0.28³ ≈ 2.2% —— **放大约 13 倍**。
 *   这正是 wav_pause_resume_check 的 P6（真丢写 g_es_fail==0）开始间歇 FAIL 的原因：
 *   不是代码回归，是把容错余量削得太薄。
 *   【为什么不会带回 UI 卡死】UI 卡死的真因是**单笔超时 50 ms**（×5 重试 = 250 ms/笔，
 *   一次起播 19 笔 ≈ 4.75 s），与"重试几次"无关；超时已收敛到 5 ms（10 倍），最坏
 *   19×5×5 ms ≈ 475 ms。且卡死态（SDA 被从机钳低）由 es8311_i2c_recover 在**首次**
 *   失败时就读 IDR 检出、立刻 bus_clear 救回并 return，**根本走不满 5 次重试**。
 *   ⇒ 重试数回到 5 只增加"对端偶发 NACK 时多试两次"的容错，不增加卡死态开销。 */
#define ES8311_TIMEOUT_MS   5u
#define ES8311_WR_TRIES     5u
#define ES8311_TRY_MS       2u
#define ES8311_PROBE_TRIES  3u

/* 硬失败熔断：连续 N 次"全重试仍失败"即判定总线不可用 ⇒ 后续调用**立即返回失败**
 * （不做任何 HAL 超时等待、不重试），冷却 ES8311_BREAKER_MS 后再放行一次探测式尝试。
 * 【为什么必须有】实测总线进入"SDA 被从机钳低"的卡死态后，**每一笔**都会耗满
 *   "超时 × 重试"；而 app 的进度刷新、每秒回读体检、确认期都会不停发 I2C ⇒
 *   卡死态被无限延长。熔断把"卡死期间的 I2C 开销"从秒级压到 ~0。 */
#define ES8311_BREAKER_N    6u
#define ES8311_BREAKER_MS   250u

/* ★ 补丁13 新增的三个阈值（"偶发卡死"的收敛手段，全部与 25 ms 那个坑有关）：
 *
 * ① ES8311_SLOW_US / "降级快速失败"
 *    单笔事务耗时超过 5 ms 就说明总线已经不正常（正常一字节事务 ~90 µs，含从机拉伸
 *    时钟也就几百 µs）。此后**每笔只用 1 次尝试 + 1 ms 超时**，直到某笔在 1 ms 内
 *    完成才恢复满重试。为什么按"上一笔的耗时"而不是"失败次数"降级：真正的卡顿源自
 *    **HAL 内部的等待**（见 ②），而不是重试次数本身，所以必须由"已经慢了"这件事触发。
 *
 * ② ES8311_TIMEOUT_FAST_MS —— 降级模式下的单笔超时。
 *    ⚠⚠ 【这是本轮找到的最直接的卡死来源】HAL 的 `HAL_I2C_Master_Transmit()` 开头会
 *    `I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BUSY, SET, I2C_TIMEOUT_BUSY, ...)`，
 *    而 `I2C_TIMEOUT_BUSY = 25 ms` 是 **HAL 源文件里的常量**（stm32h7xx_hal_i2c.c:344），
 *    与本项目传进去的 5 ms **没有任何关系**。总线烂掉时 BSY 会一直置位 ⇒ **每次尝试
 *    都先白等 25 ms**；19 笔起播 × 5 次重试 × 25 ms ≈ 2.4 s ⇒ 主循环停转。
 *    本版两道防线：先自己查 BUSY（命中就绕开 HAL，见 es8311_i2c_busy_stuck），
 *    以及降级模式下把单笔超时压到 1 ms。
 *
 * ③ ES8311_BC_TRIP —— 连续几次 bus clear 没换来成功就判总线不可用。
 *    旧代码在 bus_clear 里把熔断计数清零，而 bus_clear 恰恰只在"SDA 被钳住"时触发
 *    ⇒ 熔断**永远攒不满**（实测复现里 breaker_trip 恒 0、fail 涨到 49）。 */
#define ES8311_SLOW_US          5000u
#define ES8311_TIMEOUT_FAST_MS  1u
#define ES8311_BC_TRIP          3u

/* ---- 前置声明：probe 在这些实现之前就用到它们（bus clear / 熔断 / 计数）----
 * 定义在文件下方（bus clear 需要 GPIO + I2C 寄存器，集中放在一起更清楚）。 */
static void es8311_i2c_recover(HAL_StatusTypeDef st);
static uint32_t es8311_retry_budget(void);
static void es8311_i2c_note(int failed);
static void es8311_i2c_hw_reset(void);
static int  es8311_i2c_busy_stuck(void);
static void es8311_tx_begin(uint32_t *t0);
static void es8311_tx_end(uint32_t t0);

int es8311_probe(uint8_t addr7)
{
    return es8311_probe_to(addr7, 20u);
}

/* ⚠⚠ 探测**必须重试**（2026-10-13 板上定位，第二个真根因）：
 *   `wav_play_start()` 第一行就是 `wav_play_stop()` —— 若当时正在播，它会调
 *   `es8311_stop_dac()` 把 codec 打进 **power-down**；紧接着 `audio_out_start()` 就
 *   `es8311_probe()`。芯片在掉电/上电窗口里**不对自己的 I2C 地址应答**，而旧写法
 *   **一次 NACK 就判"总线上没有 ES8311"** ⇒ `audio_out_start` 返回 -2 ⇒
 *   `wav_play_start` 返回 -22、**I2C 一笔都不写**（实测 g_es_wr_ok 增量为 0）。
 *   表现正是用户报的"点了播放没反应/没声音"；而"第二发就好"也正是因为第二发时
 *   芯片已经走出那个窗口。
 *   ⇒ 与写/读同一套处置：失败先按类型自愈、延时 2 ms 再试，共 3 次；仍失败才判没探到。
 *   ⚠ HAL_I2C_IsDeviceReady 内部那 3 次 Trials 是**背靠背**的（同一窗口内全灭），
 *     所以这里的外层"延时后重试"才是真正起作用的那个。 */
int es8311_probe_to(uint8_t addr7, uint32_t timeout_ms)
{
    uint32_t tries;
    /* ⚠ 超时上限收敛到 ES8311_TIMEOUT_MS：调用方（出货路径）传的是 20 ms，
     *   那对"从机不应答"是纯粹浪费（探测本身 ~90 µs 级别），且会累积成 UI 卡顿。 */
    if (timeout_ms > ES8311_TIMEOUT_MS)
    {
        timeout_ms = ES8311_TIMEOUT_MS;
    }
    tries = es8311_retry_budget();
    if (tries > ES8311_PROBE_TRIES)
    {
        tries = ES8311_PROBE_TRIES;
    }

    for (uint32_t att = 0u; att < tries; ++att)
    {
        uint32_t t0;
        int      ready;

        if (g_es_nack_sim != 0u)
        {
            ready = 0;
        }
        else if (es8311_i2c_busy_stuck() != 0)
        {
            ready = 0;                  /* 外设卡住：不交给 HAL（它要等 25 ms）*/
        }
        else
        {
            es8311_tx_begin(&t0);
            ready = (HAL_I2C_IsDeviceReady(&s_hi2c, (uint16_t)(addr7 << 1), 3u, timeout_ms)
                     == HAL_OK) ? 1 : 0;
            es8311_tx_end(t0);
        }
        if (ready != 0)
        {
            if (att != 0u)
            {
                g_es_probe_retry++;      /* 首试没探到、延时后探到了（诊断量）*/
            }
            es8311_i2c_note(0);
            return 0;
        }
        /* 探测失败同样要看总线电平：SDA 被钳住就做 bus clear（否则后面全卡）。 */
        es8311_i2c_recover(HAL_ERROR);
        if (tries > 1u)                /* ★ 补丁13：与读写同口径，降级态不付这笔延时 */
        {
            HAL_Delay(ES8311_TRY_MS);
        }
    }
    es8311_i2c_note(1);
    return -1;
}

void es8311_set_addr(uint8_t addr7)
{
    s_addr7 = addr7;
}

/* ★★ 2026-10-14（补丁13）：把"偶发卡死"从"靠运气"变成"有上限"的三个小工具 ★★ */

/* 单笔 I2C 事务计时（DWT 周期计数，µs 分辨率；DWT 在本工程常开，见 main.c/audio_out.c）。
 * 统计量落到 g_es_tx_us_max / g_es_slow_n，并按"上一笔快慢"维护降级标志 s_es_fast_fail。 */
static void es8311_tx_begin(uint32_t *t0)
{
    *t0 = DWT->CYCCNT;
}

static void es8311_tx_end(uint32_t t0)
{
    uint32_t us = (uint32_t)((DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u));

    if (us > g_es_tx_us_max)
    {
        g_es_tx_us_max = us;
    }
    if (us > ES8311_SLOW_US)
    {
        g_es_slow_n++;
        s_es_fast_fail = 1u;            /* 一笔就慢了 ⇒ 后面每笔只用 1 次尝试 + 1 ms 超时 */
    }
    /* ⚠ 这里**不**清 s_es_fast_fail：NACK 本身只要 ~200 µs，"快"不代表"好"。
     *   降级态的出口只有一个 —— 某笔**整笔成功**（es8311_i2c_note(0)）。
     *   这很重要：若按"上一笔快不快"来退出降级，NACK 风暴里会反复进出，
     *   每次都先付满"5 次尝试 + 5 × 2 ms 延时"，单拍又会被拖长。 */
}

/* I2C1 外设的**硬件复位**（RCC 的 I2C1RST）。
 *
 * 【为什么不能只写 PE=0/1】PE 复位只清外设的寄存器，**不保证清掉 BUSY 标志**：BUSY 的语义是
 *   "总线上有一次传输还没结束"，当从机把 SDA 钳低时主机永远等不到 STOP，BUSY 就一直置位。
 *   而 HAL 每个事务开头都要等 BUSY 清零（固定 25 ms，见文件上方 ② 的说明）⇒ 卡死放大。
 *   RCC 强制复位把整个外设（含状态机）归零，是 ST 官方给出的确定性手段。
 *
 * 【电气安全】只复位 I2C1 这一个外设的寄存器，**不碰任何 GPIO 的电气配置**、不改变
 *   引脚模式、不驱动任何线；PB6/PB7 的 AF 与上拉保持原样。I2C1 在本工程是 ES8311 独占
 *   （全仓 grep 确认：除 es8311.c 外只有 audio_test.c 的自检会临时关它）。
 *   ⚠ 复位会把 TIMINGR 一起清掉 ⇒ 必须紧跟一次 HAL_I2C_Init 把时序写回。 */
static void es8311_i2c_hw_reset(void)
{
    __HAL_RCC_I2C1_FORCE_RESET();
    for (volatile uint32_t i = 0u; i < 32u; ++i) { }
    __HAL_RCC_I2C1_RELEASE_RESET();

    s_hi2c.State     = HAL_I2C_STATE_READY;
    s_hi2c.ErrorCode = HAL_I2C_ERROR_NONE;
    s_hi2c.Lock      = HAL_UNLOCKED;
    (void)HAL_I2C_Init(&s_hi2c);
    g_es_rcc_rst_n++;
}

/* 进 HAL 之前先看 BUSY：返回 1 = 外设卡住（**本笔不要再交给 HAL**，否则要白等 25 ms）。
 * 命中就先做一次硬件复位；复位后 BUSY 仍未清 ⇒ 判定卡住，让调用方快速失败。 */
static int es8311_i2c_busy_stuck(void)
{
    if ((I2C1->ISR & I2C_ISR_BUSY) == 0u)
    {
        return 0;
    }
    g_es_busy_stuck_n++;
    es8311_i2c_hw_reset();
    if ((I2C1->ISR & I2C_ISR_BUSY) == 0u)
    {
        return 0;                       /* 复位救回来了：本笔照常走 HAL */
    }
    return 1;                           /* 仍卡住：不要再让 HAL 去等 25 ms */
}

/* ★★ I2C 总线恢复：9 个 SCL 脉冲 + STOP（bus clear）— 2026-10-13 UI 卡死根因之一 ★★
 *
 * 【症状与根因】板上实测（i2c_timeline.py，全程只读 IDR 不改电气）：
 *   快速点播放/暂停后总线电平**恒定 0x40** —— SCL(PB6)=1 已释放、SDA(PB7)=0
 *   **被从机永久钳低**；此后每一笔 I2C 都在 "超时 × 重试" 里耗满时间
 *   （实测累计 g_es_fail=900、主循环单拍 3921 ms）⇒ UI 卡死无法滑动。
 *
 * 【为什么 SDA 会被钳住】I2C 从机在"读到地址后、按约定该释放 SDA"的时刻被中途打断
 *   （本工程的高频启停：I2S 时钟开关、codec 电源态切换、以及 probe-rs halt 冻结 CPU
 *   时外设仍在跑），就会停在"正拉着 SDA"的中间态。从机不会自己退出 ⇒ 主机必须按
 *   I2C 规范用 9 个 SCL 脉冲把它"走完"剩余位、再补一个 STOP 复位总线状态机。
 *
 * 【本函数为什么以前不存在、现在必须有】旧的 es8311_i2c_recover() 只处理
 *   HAL_BUSY/TIMEOUT（复位 I2C1 外设寄存器），对 NACK 直接 `return`；而**单片机侧
 *   复位自己的 I2C 外设根本无法让从机松开 SDA** —— 外设复位了，线还是被钳着。
 *   实测证据：g_es_recover_pe=0（从未做过任何恢复）+ bus 恒定 0x40（永久卡死）。
 *   ⇒ 唯一的出路是**软件 bit-bang 时钟脉冲**（把 PB6/PB7 临时切成 GPIO 推挽/开漏，
 *   这是 I2C 总线恢复的标准做法，不是"强灌电流"）。
 *
 * ⚠⚠ 电气安全（严守用户第 0 条禁令）：
 *   · 两个脚**都配成开漏输出（GPIO_MODE_OUTPUT_OD）**，绝不使用推挽。
 *     开漏下主机只能在"释放(写1)"与"拉低(写0)"之间切换，**与从机不可能对顶**
 *     （任何一方拉低都是合法的 I2C 线"或"语义）—— 这是刻意选的保守值：
 *     即使从机正在拉伸 SCL 或钳住 SDA，也不存在对顶电流路径。
 *   · 全程**不主动拉低 SDA**（SDA 始终写 1 = 只释放），只由主机提供 SCL 脉冲。
 *   · 脉冲期间每个电平都读回 IDR 确认，最多 9 个脉冲（I2C 一帧最多 8 数据位 + 1 ACK）。
 *   · 恢复完把 PB6/PB7 **还原成 AF4 开漏**（I2C1 原配置），不留副作用。
 *   · 电流量级：开漏 + 内部上拉 ~40 kΩ ⇒ 单脚最大 VDD/40k ≈ 82 µA，可忽略。 */
static void es8311_bus_clear(void)
{
    GPIO_InitTypeDef g;

    /* ⚠ 前置条件：GPIOB 与 I2C1 的时钟必须已使能（由 es8311_bus_init 打开）。
     *   若总线还没初始化就走到这里，碰 I2C1 寄存器会 BusFault ⇒ 直接返回。 */
    if (g_es_bus_rc != 0u)
    {
        return;
    }

    /* --- 1) SCL/SDA 都切成**开漏**输出 + 上拉；两脚均写 1（释放）。
     *        开漏 = 物理上不可能与从机对顶（见上方电气安全说明）。 --- */
    g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_OUTPUT_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_LOW;          /* 慢速：总线恢复不需要快边沿 */
    g.Alternate = 0;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);   /* 两线释放（高）*/

    /* --- 2) 起始条件：SCL 高时把 SDA 拉低（START）。开漏下"同向拉低"无对顶电流。 --- */
    for (volatile uint32_t i = 0u; i < 200u; ++i) { }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);   /* SDA 低（START）*/
    for (volatile uint32_t i = 0u; i < 200u; ++i) { }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);     /* SDA 释放，交还给从机 */

    /* --- 3) 9 个 SCL 脉冲：每个脉冲后检查 SDA 是否已释放（从机吐完最后一位） --- */
    for (uint32_t clk = 0u; clk < 9u; ++clk)
    {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);   /* SCL 低 */
        for (volatile uint32_t i = 0u; i < 200u; ++i) { }
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);     /* SCL 高空闲（释放）*/
        for (volatile uint32_t i = 0u; i < 200u; ++i) { }

        if ((GPIOB->IDR & GPIO_PIN_7) != 0u)     /* SDA 已被释放（读到高）⇒ 从机走完了 */
        {
            break;
        }
    }

    /* --- 4) STOP 条件：SCL 高时让 SDA 由低→高（释放）—— 复位所有从机的总线状态机 --- */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    for (volatile uint32_t i = 0u; i < 200u; ++i) { }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);         /* SDA 释放 ⇒ STOP */
    for (volatile uint32_t i = 0u; i < 200u; ++i) { }

    /* --- 5) 还原 PB6/PB7 为 AF4 开漏（I2C1 原配置），并把 I2C 外设/handle 归位 --- */
    g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_AF_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    /* I2C 外设软件复位（PE 0→1，≥3 个 APB 周期），清掉可能残留的 BUSY/状态机 */
    CLEAR_BIT(I2C1->CR1, I2C_CR1_PE);
    (void)I2C1->CR1;
    for (volatile uint32_t i = 0u; i < 32u; ++i) { }
    SET_BIT(I2C1->CR1, I2C_CR1_PE);
    s_hi2c.State     = HAL_I2C_STATE_READY;
    s_hi2c.ErrorCode = HAL_I2C_ERROR_NONE;
    s_hi2c.Lock      = HAL_UNLOCKED;
    g_es_recover_pe++;

    /* --- 6) ★ 补丁13：熔断计数**不再清零**，改为"连续几次 bus clear 都没换来成功
     *        就判总线不可用"。 ---
     * 【旧代码错在哪】原来这里是 `s_es_consec_fail = 0; s_es_breaker_tick = 0;`
     *   —— 而本函数**只在"SDA 被从机钳住"时被调用**，那恰好就是熔断器被设计出来要对付的
     *   那种状态。于是：每次失败都触发一次 bus clear ⇒ 计数被清零 ⇒ 永远攒不到
     *   ES8311_BREAKER_N=6 ⇒ **熔断永不触发**。实测（ui_freeze_repro 40 轮真实点击）：
     *   主循环停转、`g_es_fail` 涨到 49，而 `g_es_breaker_trip` 恒为 0 —— 保护形同虚设。
     * 【现在】只有"整笔事务成功"才清计数（es8311_i2c_note(0)）；bus clear 连续做满
     *   ES8311_BC_TRIP 次仍没有任何一笔成功 ⇒ 直接把计数填满阈值，进入冷却半开。 */
    if (++s_es_bc_streak >= ES8311_BC_TRIP)
    {
        s_es_consec_fail  = ES8311_BREAKER_N;
        s_es_breaker_tick = HAL_GetTick();
        g_es_breaker_trip++;
    }

    /* --- 7) 清完再看一眼：SDA 还是低 ⇒ 这次 bus clear 没救回来（量化 it，别猜）--- */
    for (volatile uint32_t i = 0u; i < 2000u; ++i) { }
    if ((GPIOB->IDR & GPIO_PIN_7) == 0u)
    {
        g_es_bc_fail++;
    }
    g_es_bus_clear++;
}

/* 写/读失败后的 I2C 现场处置。**按失败类型分开处理**（2026-10-13 板上实测的分类）：
 *   · HAL_ERROR(NACK)：从机不应答地址。**但必须先看总线电平**：
 *       - IDR 的 SDA 仍为高（0xC0/0x80）：总线正常，只是芯片那一小段时间不响应
 *         （软复位/电源态切换窗口）⇒ 只延时，不动外设。
 *       - IDR 的 SDA 被钳低（0x40）：**总线已卡死** ⇒ 必须做 bus clear，
 *         否则后面每一笔都要耗满"超时 × 重试"（实测正是 UI 卡死的直接原因）。
 *   · HAL_BUSY / HAL_TIMEOUT：外设状态机/时序问题 ⇒ 复位 I2C 外设（PE 0→1）+ 句柄归位。
 *   ⚠ PE 必须**拉低至少 3 个 APB 时钟周期**才算有效复位（RM0433 I2C 章）。 */
static void es8311_i2c_recover(HAL_StatusTypeDef st)
{
    uint32_t idr;

    /* ★ 判据：SDA(PB7) 被压住 = 总线卡死 ⇒ 立刻 bus clear（与失败类型无关）。
     *   放在类型判断**之前**：实测卡死态下 HAL 报的正是 NACK(1)，
     *   若照旧对 NACK 直接 return，就永远走不到恢复。
     * ⚠⚠ 但**必须先给总线一点时间把 SDA 抬起来**：HAL 失败(尤其 NACK 后的 STOP)
     *   返回的**那一刻**，SDA 的上升沿（板上只有 MCU 内部 ~40 kΩ 弱上拉 × 总线电容，
     *   RC 约数 µs）还没到位，此刻读 IDR 会读到 0 ⇒ **误判"总线卡死"** ⇒ 误触发
     *   bus_clear（切 GPIO + 9 脉冲 + PE 复位，会打断正在进行的时序）⇒ 反而制造下一次
     *   失败，形成"误判→bus clear→更多失败"的正反馈。2026-10-13 实测：10 万次读里
     *   bus_clear 触发 187 次、失败率 ~30%、降频无改善 —— 正与该正反馈相符。
     *   ⇒ 采样前先等几个 µs（并连读两次取"稳定态"）。 */
    for (volatile uint32_t i = 0u; i < 2000u; ++i) { }   /* ≈ 20 µs @100 MHz：等 SDA 抬起 */
    idr = GPIOB->IDR & (GPIO_PIN_6 | GPIO_PIN_7);
    /* ★ 补丁13：排障仿真 g_es_nack_sim>=2 ⇒ 假装 SDA 被钳住，把 bus clear 与熔断一起演练
     * （纯软件，不改任何电气；见 g_es_nack_sim 的声明）。 */
    if (g_es_nack_sim >= 2u)
    {
        idr &= ~(uint32_t)GPIO_PIN_7;
    }
    if ((g_es_busclear_en != 0u) && ((idr & GPIO_PIN_7) == 0u))
    {
        es8311_bus_clear();
        return;
    }
    (void)idr;

    if (st == HAL_ERROR)
    {
        return;                       /* NACK 且总线电平正常：等调用方的延时即可 */
    }
    /* ★ 补丁13：BUSY/TIMEOUT 改用**外设硬件复位**（RCC 的 I2C1RST）。
     *   原来只写 PE=0/1 不够 —— BUSY 的语义是"总线上还有一次传输没结束"，SDA 被钳住时
     *   永远等不到 STOP，PE 复位不保证能清它。而 HAL 每个事务开头要等 BUSY（固定 25 ms）
     *   ⇒ 卡死会被逐笔放大。RCC 复位把状态机一并归零，才是确定性的做法。 */
    es8311_i2c_hw_reset();
}

/* 熔断查询：返回**本次调用允许的最大重试次数**（实现"半开"语义，避免误伤正常播放）。
 *   0 = 正常：用 ES8311_WR_TRIES 次重试（能熬过芯片电源态窗口）。
 *   1 = 熔断冷却中：**只允许 1 次快速尝试**（单笔 5 ms），不做重试风暴。
 *      为什么不是"直接返回失败"：冷却期内直接把起播判失败，会把"慢但能用"
 *      变成"点了播放没反应"——那是更差的回归。给一次便宜的机会，
 *      真恢复了立刻转正常；仍失败则继续冷却，代价仅 5 ms。 */
static uint32_t es8311_retry_budget(void)
{
    /* ★ 补丁13：降级快速失败优先 —— 上一笔已经慢了（>5 ms）就只给 1 次机会。
     *   这一条与熔断互补：熔断按"整笔全灭"计数、反应慢半拍；这里按"单笔耗时"即时收敛，
     *   保证"烂总线"下单拍的 I2C 阻塞上限不会因为 19 笔起播而线性放大。 */
    if (s_es_fast_fail != 0u)
    {
        return 1u;
    }
    if (s_es_consec_fail < ES8311_BREAKER_N)
    {
        return ES8311_WR_TRIES;
    }
    if ((HAL_GetTick() - s_es_breaker_tick) >= ES8311_BREAKER_MS)
    {
        /* 冷却结束：放行一次完整重试（半开试探） */
        s_es_consec_fail = ES8311_BREAKER_N - 1u;
        return ES8311_WR_TRIES;
    }
    return 1u;                        /* 冷却中：仅一次快速尝试 */
}

/* 记一次"整笔失败"（所有重试都失败）或一次成功 —— 维护连续失败计数与熔断触发。*/
static void es8311_i2c_note(int failed)
{
    if (failed)
    {
        s_es_consec_fail++;
        /* ★ 补丁13：整笔全灭是"这条链路现在不好使"的强信号 ⇒ 进入降级快速失败。
         *   效果：**第一个**失败的寄存器付 5×(0.2 ms + 2 ms) ≈ 11 ms，其后每一笔只
         *   0.2 ms（1 次尝试、不延时、1 ms 超时）⇒ 一次 19~40 笔的起播/初始化序列
         *   单拍总开销被压在 ~20 ms 量级，而不是 167 ms（实测）或 ~2.4 s（旧代码）。 */
        s_es_fast_fail = 1u;
        if (s_es_consec_fail == ES8311_BREAKER_N)
        {
            s_es_breaker_tick = HAL_GetTick();
            g_es_breaker_trip++;
        }
    }
    else
    {
        s_es_consec_fail = 0u;
        /* ★ 补丁13：只有**真正成功**才退出降级 + 清"bus clear 连击"计数。
         *   （旧代码在 bus_clear 里清 consec_fail，等于自己把熔断关掉，见其说明。） */
        s_es_bc_streak   = 0u;
        s_es_fast_fail   = 0u;
    }
}

/* 重试次数与间隔定义已上移到 es8311_probe_to 之前（probe/write/read 共用）。 */

int es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t           buf[2];
    HAL_StatusTypeDef st = HAL_ERROR;
    uint32_t          t0;
    /* ★ 熔断（半开）：冷却期只允许 1 次快速尝试，绝不拖成 UI 卡顿。
     *   理由见 es8311_retry_budget：直接失败会把"慢但能用"退化成"点了没反应"。 */
    uint32_t tries = es8311_retry_budget();
    /* ★ 补丁13：降级模式下把单笔超时也压到 1 ms（见 ES8311_TIMEOUT_FAST_MS 的说明）。 */
    uint32_t to    = (s_es_fast_fail != 0u) ? ES8311_TIMEOUT_FAST_MS : ES8311_TIMEOUT_MS;

    buf[0] = reg;
    buf[1] = val;

    /* 最多写 tries 次：失败后按类型处置再重试（理由见上）。
     * ⚠ 只在**第一次**失败时记账（否则重试会把失败现场冲掉，复盘时就看不到真因）。
     * ⚠ 失败**必须重试**：调用方（dac_path_regs / 音量 ramp）的历史写法是忽略返回值的，
     *   一旦这几个字节丢了，DAC 通路/音量就会"只写了一半"—— 表现正是用户报的
     *   "没声音"和"音量大小混乱"。 */
    for (uint32_t att = 0u; att < tries; ++att)
    {
        /* ★ 补丁13 的两道防线（顺序有意义）：
         *   ① 排障仿真（g_es_nack_sim）直接判失败 —— 用于复现"对端永不回应"的最坏情况；
         *   ② 自己查 BUSY：命中就绕开 HAL，省下它内部那笔**固定 25 ms** 的等待。 */
        if (g_es_nack_sim != 0u)
        {
            st = HAL_ERROR;
        }
        else if (es8311_i2c_busy_stuck() != 0)
        {
            st = HAL_BUSY;
        }
        else
        {
            es8311_tx_begin(&t0);
            st = HAL_I2C_Master_Transmit(&s_hi2c, (uint16_t)(s_addr7 << 1), buf, 2u, to);
            es8311_tx_end(t0);
        }
        if (st == HAL_OK)
        {
            if (att != 0u)
            {
                g_es_retry_ok++;          /* 第一次失败、重试成功 */
            }
            if (reg == 0x32u)             /* DAC 音量：记值 + 计数（暂停/续播排障用）*/
            {
                g_es_reg32_wr = val;
                g_es_reg32_n++;
            }
            if (reg == 0x01u)             /* ★ 补丁13：时钟管理寄存器（K2 的"起播确实写过"证据）*/
            {
                g_es_reg01_val = val;
                g_es_reg01_n++;
            }
            if (reg == 0x31u)             /* ★ 补丁13：静音寄存器写侧缓存（见 s_reg31 说明）*/
            {
                s_reg31 = val;
            }
            g_es_wr_ok++;
            es8311_i2c_note(0);
            return 0;
        }

        if (att == 0u)
        {
            g_es_ng_reg = reg;
            g_es_ng_st  = (uint32_t)st;
            if (st == HAL_TIMEOUT)      { g_es_ng_tmo++;  }
            else if (st == HAL_BUSY)    { g_es_ng_busy++; }
            else                        { g_es_ng_nack++; }
            g_es_ng_ring[g_es_ng_head & 7u] =
                ((uint32_t)reg << 8) | ((uint32_t)st & 0xFFu);
            g_es_ng_head++;
            g_es_wr_ng++;
            /* 顺手量一眼总线电平（只读 IDR，不改任何电气）：SDA 若被从机压住，
             * 说明是"总线卡死"而不是"从机 NACK 一包"，两者的处置完全不同。 */
            g_es_ng_idr = GPIOB->IDR & (GPIO_PIN_6 | GPIO_PIN_7);
        }

        es8311_i2c_recover(st);
        /* ★ 补丁13：只在"还有下一次尝试"时才付这 2 ms 延时。
         *   这笔延时本来是给芯片"走出 NACK 窗口"用的（实测有效：首试 NACK 后延时再试
         *   基本都能成 —— g_es_retry_ok 在涨）。但降级/熔断态下 tries==1，没有下一次了，
         *   再加延时就是在白白拖长主循环单拍，所以跳过。 */
        if (tries > 1u)
        {
            HAL_Delay(ES8311_TRY_MS);
        }
    }

    g_es_fail++;
    es8311_i2c_note(1);
    return -1;
}

int es8311_read_reg(uint8_t reg, uint8_t *val)
{
    HAL_StatusTypeDef st = HAL_ERROR;
    uint32_t          t0;
    /* ★ 熔断（半开）：冷却期只允许 1 次快速尝试（理由见 es8311_retry_budget）。 */
    uint32_t tries = es8311_retry_budget();
    /* ★ 补丁13：降级模式下单笔超时压到 1 ms（与 write 同口径）。 */
    uint32_t to    = (s_es_fast_fail != 0u) ? ES8311_TIMEOUT_FAST_MS : ES8311_TIMEOUT_MS;

    /* 与写同样要重试：读失败会让调用方**跳过**后续写（config_i2s 就是
     * `if (read == 0) write(...)`），结果是数据格式没配却"看起来成功"——又是
     * 一类只有耳朵能发现的静默故障。 */
    for (uint32_t att = 0u; att < tries; ++att)
    {
        if (g_es_nack_sim != 0u)
        {
            st = HAL_ERROR;
        }
        else if (es8311_i2c_busy_stuck() != 0)
        {
            st = HAL_BUSY;
        }
        else
        {
            es8311_tx_begin(&t0);
            st = HAL_I2C_Master_Transmit(&s_hi2c, (uint16_t)(s_addr7 << 1), &reg, 1u, to);
            if (st == HAL_OK)
            {
                st = HAL_I2C_Master_Receive(&s_hi2c, (uint16_t)(s_addr7 << 1), val, 1u, to);
            }
            es8311_tx_end(t0);
        }
        if (st == HAL_OK)
        {
            g_es_rd_ok++;
            es8311_i2c_note(0);
            return 0;
        }
        if (att == 0u)
        {
            g_es_ng_reg = (uint32_t)reg | 0x100u;   /* 高位置 1 标记"这是读失败" */
            g_es_ng_st  = (uint32_t)st;
            g_es_ng_ring[g_es_ng_head & 7u] =
                (0x01u << 16) | ((uint32_t)reg << 8) | ((uint32_t)st & 0xFFu);
            g_es_ng_head++;
            g_es_rd_ng++;
            g_es_ng_idr = GPIOB->IDR & (GPIO_PIN_6 | GPIO_PIN_7);
        }
        es8311_i2c_recover(st);
        if (tries > 1u)                /* ★ 补丁13：降级/熔断态（tries==1）不付这笔延时 */
        {
            HAL_Delay(ES8311_TRY_MS);
        }
    }
    es8311_i2c_note(1);
    return -1;
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

    /* ---- 1a0. I2C 噪声免疫前置（2026-10-14 补丁11·D2，照抄官方 esp_codec_dev）----
     * 官方 es8311_open() 在**配置最前面**写 0x44=0x08 两次，官方注释两条：
     *   ① "Enhance ES8311 I2C noise immunity" —— 0x44 含 I2C 噪声免疫位；
     *   ② "occasional failures during the first I2C write ⇒ 再写一次保可靠"。
     * 本板 I2C 上拉极弱（仅 MCU 内部 ~40 kΩ、无外部上拉），零探针窗口实测首试 NACK
     * 约 30%，这一笔"早写 + 双写"值得照抄。⚠ 本项目 es8311_write_reg 自带 5 次重试，
     * 此处双写只是额外冗余、不是唯一防线（D2 官方差异，低风险顺带采纳）。 */
    rc |= es8311_write_reg(0x44, 0x08);
    rc |= es8311_write_reg(0x44, 0x08);

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
    /* ★ 2026-10-14（补丁11·D3）：末步 0x44 由 0x08 改为 **0x58**（开内部参考 ADCL+DACR）。
     *   官方 esp_codec_dev 的 es8311_open() 末步正是 0x44=0x58（no_dac_ref=false 默认）；
     *   本项目序列是官方"旧祖源"、官方后来补的这一笔我们缺了。0x58 打开内部参考信号
     *   （ADCL+DACR），改善参考/偏置稳定性（→ 噪声）；是所有官方 ES8311 板卡的默认值。
     *   ⚠ bit3（I2C 噪声免疫）在 0x08 与 0x58 里都是 1，故 D2 那两笔前置写不受影响。 */
    rc |= es8311_write_reg(0x44, 0x58);   /* GPIO_REG44：0x58=开内部参考 ADCL+DACR */

    /* esp-adf 这里还有 pa_power(true)（GPIO 拉高外部功放使能）——
     * 本模块 NS4150B 无常闭使能脚，跳过。 */

    if (rc == 0)
    {
        s_codec_ready = 1u;
    }
    g_es_rc = rc;
    return rc;
}

int es8311_config_i2s(void){
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

/* DAC 通路寄存器序列（音量值参数化）—— es8311_start_dac() 与
 * es8311_dac_start_muted() 共用**同一套顺序**，只有 REG32（DAC 音量）的取值不同。
 * ⚠ 顺序照抄 esp-adf：先开 DAC 通路/电源，最后两级写 REG32。别为了"先静音"
 *   把 REG32 挪到最前面 —— 通路上电期间的分频/OSR 还没配好，写入可能被后续
 *   的 REG12 power-up 覆盖掉（同一段代码两条路都走，顺序只能有一份）。 */
static int dac_path_regs(uint8_t vol_reg)
{
    uint8_t v;
    int rc = 0;

    /* ★★ 2026-10-14（补丁11·D9 配套）：先恢复"时钟管理"，再走 DAC 通路。
     *
     * 【为什么必须在最前面】es8311_stop_dac() 的**完整掉电尾**（补丁11·D9，照抄官方
     *   es8311_suspend）会把 REG00 的 CSM_ON(bit7) 清 0、把 REG01 的 MCLK_ON(bit5)/
     *   BCLK_ON(bit4)/CLKADC_ON(bit3)/CLKDAC_ON(bit2)/ANACLKADC_ON(bit1)/ANACLKDAC_ON(bit0)
     *   全部清 0（位定义见 Linux 内核 sound/soc/codecs/es8311.h）。
     *   而本项目续播**跳过完整 init**（audio_out.c：s_codec_ready 一旦置位就不再跑 init）
     *   ⇒ 若不在这里写回来，芯片就"没有时钟"，数据进不去 DAC ⇒ **续播无声**。
     *   官方 esp_codec_dev 的 es8311_start() 开头正是重写这两笔（REG00/REG01），此处照搬。
     *     0x00=0x80：CSM_ON=1 + MSC=0（I2S 从机；本板 STM32 出 MCLK/SCK/LRCK 作主机）；
     *                RST 位保持 0（不复位数字核，避免多造一个"上电窗口 NACK"）。
     *     0x01=0x3F：MCLK 取引脚(bit7=0)、不反相(bit6=0)、MCLK/BCLK/ADCCLK/DACCLK/
     *                ANACLKADC/ANACLKDAC 全开(bit5..bit0=1) —— 与本项目 init 的收尾值一致。
     *   ⚠ 首次起播（刚 init 完）时这两笔是幂等的，不改变状态；只有 D9 掉电后才真正起作用。 */
    rc |= es8311_write_reg(0x00, 0x80);
    rc |= es8311_write_reg(0x01, 0x3F);

    /* ★ 补丁13：**先数字静音，再上电**（防爆音的通用次序：上电全程 muted）。
     *   为什么补这一笔：芯片复位默认 REG31 = 0x00（未静音）。首次起播时若正好把
     *   模拟通路（REG0E/REG12/REG0D）在一颗"未静音"的芯片上拉起来，输出节点从
     *   断电态跳到工作点的那个台阶会**直接出来**（只是幅度被 REG32=0 压着）。
     *   本函数末尾本来就有一笔解静音，这里补上与之配对的"先静音"，使通路
     *   **在整个上电过程中都处于静音** —— 与官方 es8311_enable(false)→(true)
     *   的次序一致（set_mute(true) → … → set_mute(false)）。 */
    rc |= es8311_set_mute(1);

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

    rc |= es8311_write_reg(0x32, vol_reg);
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

    /* ★ 2026-10-14（补丁12）：**解静音**（REG31 bit6/5 清 0）—— 必须是最后一步。
     * 【为什么必须有】audio_out_stop() 现在会先 `es8311_set_mute(1)` 再停时钟
     *   （消"停止/切歌"那声爆音，见 audio_out.c）。而 REG31 是**芯片内持久**的
     *   ⇒ 不解静音的话，"停过一次之后再也出不了声"。
     * 【为什么放最后】此时：① 通路上电（REG0E/REG12/REG0D）已完成；
     *   ② REG32 刚被写成参数值（起播传 0x00 = -95.5 dB）⇒ 解静音这一步的输出电平
     *   在噪声底以下，听不见；③ 在它之后没有任何后续写入会覆盖。
     *   顺序错误（比如把它放在 REG0D 上电之前）会让"上电瞬间"直接以有效音量出波形。 */
    rc |= es8311_set_mute(0);

    g_es_rc = rc;
    return rc;
}

int es8311_start_dac(void)
{
    /* 音量取**当前 UI 音量**（控制中心滑杆的值，出厂 70 ⇒ 0x85 ≈ -28 dB），
     * 不再写死 0xBF：否则用户在抽屉里把音量拖小、下次起播又被拉回 0 dB。
     * ⚠ 这是"一步到位"的起播（自检固件 audio_test 用它）——音量跳变会带一声轻响。
     *   出货的音乐播放走 es8311_dac_start_muted() + 音量 ramp（见 audio_out.c）。 */
    return dac_path_regs(es8311_volume_reg(g_audio_volume));
}

int es8311_dac_start_muted(void)
{
    /* REG32 写 0（-95.5 dB，等同静音）：把"DAC 从 power-down 醒来"这步跳变压到听不见，
     * 通路接通后再由调用方把音量 ramp 到目标值（见 es8311.h 的说明）。 */
    return dac_path_regs(0x00u);
}

int es8311_dac_mute(void)
{
    int rc;
    /* 只写音量 = 静音。**不写 0x0E/0x12/0x0D/0x14/0x15/0x37/0x45** —— 那些是
     * "关机"序列（es8311_stop_dac）的一部分，会制造 power-down 的 NACK 窗口。
     * 暂停后芯片仍保持通电与通路就绪，所以续播时 dac_path_regs 全是"有效时钟下的写"。
     * ⚠ 仍要过 es8311_write_reg 的重试：单笔也可能撞上别的窗口（比如 probe halt）。 */
    rc = es8311_write_reg(0x32, 0x00);
    g_es_rc = rc;
    return rc;
}

/* ★ 2026-10-14（补丁12）：REG31 的**专用数字静音位** —— 官方 esp_codec_dev /
 *   Linux 内核 es8311 驱动用来消上下电爆音的手段（本工程此前从不碰 0x31）。
 *
 * 【为什么要它，而不是继续用 REG32=0】
 *   REG32=0 是"把音量降到 -95.5 dB"：靠 REG37 的硬件软 ramp 平滑下降，**慢**（实测
 *   从 0 dB 到听感静音约 160~190 ms），适合"暂停"这种可以等的动作。
 *   而"停止/切歌/退出 app"要的是**立刻断**。REG31 的 bit6(DSMMUTE)/bit5(DEMMUTE) 是从
 *   ΣΔ 调制器的输入端把信号钳到 0 —— 生效是即时的，且**时钟还在跑**，调制器能正常
 *   完成这次过渡 → 随后再切时钟就不会有阶跃（见 audio_out.c → audio_out_stop 的说明）。
 *   官方次序 `es8311_enable(false)` = set_mute(true) → 关 pa → suspend()，正是这个顺序。
 *
 * 【位的取法】照官方实现：`v = (v & 0x9F) | (on ? 0x60 : 0)`。
 *   bit7 与 bit4..0 原样保留（本工程从未写过它们，保留最稳），只改 bit6/bit5。
 *   ⚠ 与 REG32 无关、互不覆盖：所以"起播时 REG32=0 + REG31 解静音"两条路都安全。 */
int es8311_set_mute(int on)
{
    uint8_t v;
    int rc;

    /* ★ 补丁13：不再"先读再写"，直接用写侧缓存 s_reg31（由每次成功的 0x31 写入与
     *   es8311_dac_readback 的回读共同维护）。
     * 【为什么改】官方语义确实是"读-改-写只动 bit6/5"，但那一笔**读**每一次都是一次
     *   独立的 NACK 机会 —— 实测失败环里 0x31 出现频率最高（g_es_ng_ring），而它恰好是
     *   "切时钟前最后一道防线"：写丢了就是那声爆音。少一笔读 = 少一次失败机会。
     *   缓存失真不会长期存在：es8311_dac_readback() 每 ~1 s 回读一次并把缓存对齐。 */
    v = (uint8_t)((s_reg31 & 0x9Fu) | (on ? 0x60u : 0x00u));
    rc = es8311_write_reg(0x31, v);
    g_es_rc = rc;
    return rc;
}

/* ★★ 2026-10-14（补丁13）：让 DAC **确定地**变静音，并且**当场确认**。 ★★
 *
 * 【为什么需要"确认"这一步】这是"停时钟之前"的最后一道防线：
 *   若切 I2S 时钟的那一刻 DAC 还挂在有效音量上，ΣΔ 调制器停摆 ⇒ 输出保住一个直流电平
 *   ⇒ 相对上一刻的波形就是一次阶跃 ⇒ **那声爆音**。
 *   而 I2C 是**会失败**的：实测首试 NACK 约 5%~30%，且失败环里 0x31/0x32 命中最多。
 *   "写了就算"在这条路径上等于"有相当概率不静音"。
 *
 * 【动作顺序（每一步都有理由）】
 *   ① REG32 先砍一半 —— 即便后面两笔全丢，切时钟时的跳变也已小 6 dB（兜底）；
 *   ② REG32 = 0    —— 音量到最小（-95.5 dB）；
 *   ③ REG31 = 静音 —— 数字静音是**即时**的，且时钟仍在跑，调制器能正常完成过渡；
 *   ④ 回读 0x32/0x31 **确认**；没确认就补写一轮再验（最多 2 轮）。
 *
 * 【与 es8311_stop_dac() 的分工】本函数**完全不碰电源/时钟寄存器**：
 *   芯片保持"通电 + VMIDSEL 仍工作"，与「暂停」是同一个状态。而 es8311_stop_dac()
 *   会把内部参考 VREF/VMIDSEL 一起断掉 —— 基准塌掉/重建会在输出耦合电容上产生一次
 *   低频台阶（同一声爆音的另一个来源）。所以**正常播放/停止不再调它**。
 *
 * @return 0 = 已确认静音；-1 = 未能确认（调用方应把 g_es_silent_ng 当作"有爆音风险"看）*/
int es8311_dac_become_silent(void)
{
    /* ① 半步降（兜底：万一 ②③ 都丢，跳变也只有原来的一半） */
    if ((g_es_reg32_wr != 0xFFFFu) && (g_es_reg32_wr > 1u))
    {
        (void)es8311_write_reg(0x32u, (uint8_t)(g_es_reg32_wr / 2u));
    }
    /* ②③ 音量归零 + 数字静音 */
    (void)es8311_write_reg(0x32u, 0x00u);
    (void)es8311_set_mute(1);

    /* ④ 确认（最多三轮：前两轮各补写一轮再验；第三轮是给"探针 halt / 芯片电源态窗口"
     *   这种连续多笔 NACK 的极端情形留的 —— 实测 40 轮快速启停里会出现 1 次连两轮都
     *   验不过的情况。确认不了就记 g_es_silent_ng，让"有爆音风险"可见。） */
    for (uint32_t round = 0u; round < 3u; ++round)
    {
        uint8_t vol = 0xFFu;
        uint8_t m   = 0xFFu;
        int ok = (es8311_read_reg(0x32u, &vol) == 0) && (vol == 0u);

        ok = ok && (es8311_read_reg(0x31u, &m) == 0) && ((m & 0x60u) == 0x60u);
        if (ok != 0)
        {
            s_reg31 = m;                /* 缓存对齐（回读到的才是芯片里真实的）*/
            g_es_rb_mute = m;
            /* ⚠ 这里**不**写 g_es_rb_vol / g_es_rb_pwr：那两个是"起播后音量确认期"的口径，
             *   被停/暂停覆盖会让 tools/wav_pause_resume_check.py 的 P3 判据读到 0 而误报。
             *   本函数自己的回读值另存 g_es_silent_vol / g_es_silent_mute。 */
            g_es_silent_vol  = vol;
            g_es_silent_mute = m;
            g_es_silent_n++;
            return 0;
        }
        (void)es8311_write_reg(0x32u, 0x00u);
        (void)es8311_set_mute(1);
    }
    g_es_silent_ng++;
    return -1;
}

int es8311_stop_dac(void)
{
    int rc = 0;

    /* ★ 补丁13：计数 + 说明为什么正常路径不再调它。
     * 【为什么"停止播放"不再走本函数】本函数是**真·掉电**（含 REG0D=0xFC 把内部参考
     *   VREF 断掉）。而"模拟基准塌掉 → 下次上电重新建立"会在输出耦合电容上产生一次
     *   低频台阶 —— 这正是那声爆音的另一个来源。实测证据：走同一套"通电不动电源"策略的
     *   「暂停」路径（补丁10 起）没有爆音抱怨，而走本函数的「停止/切歌」一直有。
     *   ⇒ 停止改为 es8311_dac_become_silent()（只静音、不断电），与暂停同构。
     * 【那谁还在用它】① audio_test.c 的一次性自检；② 排障钩子 g_es_pdown_req
     *   （主循环消费一次）—— 用它继续保住"掉电 → 起播恢复时钟"这条配对的验收覆盖
     *   （tools/es8311_clk_check.py）。 */
    g_es_pdown_n++;

    /* ★ 2026-10-14（补丁11·D8/D9）：照搬官方 esp_codec_dev 的 es8311_suspend()
     *   ——**完整有序掉电尾**，掉电态比旧版"半掉电"更确定、更干净。
     *
     * 【与旧版的差异】
     *   · D8：0x45 由 **0x01 改回 0x00**。旧值 0x01 方向与官方相反（官方 start 与
     *         suspend 都写 0x00），本项目起播路径 dac_path_regs 也写 0x00 ⇒ 旧值 0x01
     *         是旧祖源遗留、无依据。改 0x00 后 stop 与 start 对它一致、不再有无谓跳变。
     *   · D9：补齐官方掉电尾四组：
     *         0x02=0x10 → 0x00         （REG02：时钟分频收尾，终值 0x00 = 本项目 48k 配置值）
     *         0x00=0x00 → 0x1F         （REG00：关 CSM_ON + 置 RST 位）
     *         0x01=0x30 → 0x00         （REG01：关掉全部时钟使能）
     *         0x0D=0xFC                （REG0D：连内部参考 VREF(bit2) 一起断电 = 真·全掉电；
     *                                   旧版停在 0xFA，VREF 仍开着，VMIDSEL=2 仍在工作）
     *       旧版最后那笔 0x37=0x08 已删除：官方 suspend 不写 0x37，而本项目起播
     *       dac_path_regs 必写 0x37=0x48 ⇒ 旧那笔是冗余（写完马上被起播覆盖）。
     *
     * 【⚠⚠ 配套铁律：这些寄存器必须由起播路径写回来】见 dac_path_regs() 开头新增的
     *   REG00=0x80 / REG01=0x3F 恢复。本项目续播跳过完整 init，所以"stop 写了 X ⇒
     *   start 必须恢复 X"是硬不变量；缺了恢复 ⇒ 时钟全关 ⇒ 续播无声。官方 es8311_start()
     *   正是这么配对的（开头重写 REG00/REG01）。 */
    rc |= es8311_write_reg(0x32, 0x00);   /* DAC 音量 → 静音 */
    rc |= es8311_write_reg(0x17, 0x00);   /* ADC 音量 → 0 */
    rc |= es8311_write_reg(0x0E, 0xFF);   /* 关内部块 */
    rc |= es8311_write_reg(0x12, 0x02);   /* REG12：DAC power-down */
    rc |= es8311_write_reg(0x14, 0x00);
    rc |= es8311_write_reg(0x0D, 0xFA);   /* REG0D：关模拟/偏置（VREF 与 VMIDSEL 稍后彻底关）*/
    rc |= es8311_write_reg(0x15, 0x00);
    rc |= es8311_write_reg(0x02, 0x10);   /* D9：时钟分频收尾（官方 suspend 首笔）*/
    rc |= es8311_write_reg(0x00, 0x00);   /* D9：REG00 CSM_ON=0 */
    rc |= es8311_write_reg(0x00, 0x1F);   /* D9：REG00 置 RST 位（官方"定义好的掉电态"）*/
    rc |= es8311_write_reg(0x01, 0x30);   /* D9：REG01 时钟管理收尾 */
    rc |= es8311_write_reg(0x01, 0x00);   /* D9：REG01 关全部时钟使能 ← 起播必须写回 0x3F */
    rc |= es8311_write_reg(0x45, 0x00);   /* D8：原为 0x01（方向与官方相反，已修正）*/
    rc |= es8311_write_reg(0x0D, 0xFC);   /* D9：REG0D 末步——连 VREF 一起断电（真·全掉电）*/
    rc |= es8311_write_reg(0x02, 0x00);   /* D9：REG02 回到本项目 48k 配置值 */

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

/* ---- DAC 通路"回读体检"（2026-10-13 排障用）----
 * 把 DAC 关键寄存器**从芯片里读回来**存进 RAM，供 SWD 查看：
 *   g_es_rb_vol (0x32 DAC 音量) / g_es_rb_pwr (0x12 power) / g_es_rb_dac0e (0x0E)
 * 为什么需要：写出去了 ≠ 芯片收下了。"续播没声音"的第一嫌疑就是"REG32 停在 0"，
 * 但只有回读才能区分"我们没写"和"写了但被芯片/复位吞掉"。
 * 返回 0 = 三次读都成功；<0 = 至少一次 I2C 读失败（-1 表示芯片根本没应答）。*/
volatile uint32_t g_es_rb_vol  = 0xFFFFu;
volatile uint32_t g_es_rb_pwr  = 0xFFFFu;
volatile uint32_t g_es_rb_dac0e = 0xFFFFu;
volatile uint32_t g_es_rb_mute  = 0xFFFFu;   /* REG31 回读值（补丁12：bit6/5 = 0 才算"没被静音"）*/
/* ★ 2026-10-14（补丁11·D9 配套验证）：回读"时钟管理"寄存器，用来在板上**证明**
 *   "stop 的完整掉电尾（D9）关掉的时钟，起播时确实被写回来了" —— 尤其 REG01 必须回到
 *   0x3F（全时钟使能），否则芯片无时钟、DAC 收不到数据 ⇒ 续播无声。
 *   ⚠ 这件事**音量回读验不出来**（REG32/REG12 走 I2C，与音频时钟无关，照样可读），
 *     所以必须单独看这三个量。纯诊断、不参与控制流、失败即置 0xFFFF。 */
volatile uint32_t g_es_rb_clk1 = 0xFFFFu;   /* REG01：期望 0x3F（MCLK/BCLK/ADCCLK/DACCLK 全开）*/
volatile uint32_t g_es_rb_clk2 = 0xFFFFu;   /* REG02：期望 0x00（本项目 48k 分频/倍频）*/
volatile uint32_t g_es_rb_clk6 = 0xFFFFu;   /* REG06：期望与起播前一致（bclk_div=4、SCK 不反相）*/

int es8311_dac_readback(void)
{
    uint8_t  v;
    uint32_t target = es8311_volume_reg(g_audio_volume);
    int      confirmed = 0;

    g_es_readback++;

    /* ★ 补丁11·D9 配套验证：先各回读一次时钟管理寄存器（纯诊断，见 g_es_rb_clk* 说明）。
     *   若 stop_dac 的完整掉电尾把 REG01 清零、而起播没写回，这里会读到 0x00 ⇒ 一眼可见。 */
    if (es8311_read_reg(0x01, &v) == 0) { g_es_rb_clk1 = v; } else { g_es_rb_clk1 = 0xFFFFu; }
    if (es8311_read_reg(0x02, &v) == 0) { g_es_rb_clk2 = v; } else { g_es_rb_clk2 = 0xFFFFu; }
    if (es8311_read_reg(0x06, &v) == 0) { g_es_rb_clk6 = v; } else { g_es_rb_clk6 = 0xFFFFu; }

    /* 最多 3 轮"读 → 不对就补 → 再读"。之所以要补写：I2C 写是**会失败**的（见
     * es8311_write_reg 的说明），而 volume ramp / dac_path_regs 的返回值历史上被忽略
     * ⇒ 一旦音量那几笔丢了，芯片就停在 0（静音）—— 用户听到的"进度条在走但没声音"；
     * 丢一半就是"音量大小混乱"（实测停在 0x78 = ramp 第 7 步）。这里以**回读值为准**做闭环。
     * ⚠ 返回语义：0 = **确认音量已落进芯片**（读成功且值 == 目标、REG12 已上电）；
     *   <0 = 未确认成功。调用方（audio_out 的"确认期"）靠它决定要不要下一拍再来一次。 */
    for (uint32_t round = 0u; round < 3u; ++round)
    {
        int vol_ok = (es8311_read_reg(0x32, &v) == 0);
        if (vol_ok) { g_es_rb_vol = v; } else { g_es_rb_vol = 0xFFFFu; }

        int pwr_ok = (es8311_read_reg(0x12, &v) == 0);
        if (pwr_ok) { g_es_rb_pwr = v; } else { g_es_rb_pwr = 0xFFFFu; }

        if (es8311_read_reg(0x0Eu, &v) == 0) { g_es_rb_dac0e = v; } else { g_es_rb_dac0e = 0xFFFFu; }

        /* ★ 补丁12：REG31 的专用静音位也必须确认是"未静音"。
         *   为什么非查不可：audio_out_stop() 现在会 es8311_set_mute(1) 再停时钟，
         *   而 REG31 是**芯片内持久**状态 —— 起播时那一笔"解静音"若被 NACK 吃掉，
         *   表现就是"停过一次之后再也出不了声"，而音量/电源回读**全都是对的**
         *   （正是本项目最忌讳的"看不出来的坏状态"）。⇒ 纳入终态判据。 */
        int mute_ok = (es8311_read_reg(0x31, &v) == 0);
        if (mute_ok) { g_es_rb_mute = v; } else { g_es_rb_mute = 0xFFFFu; }
        mute_ok = mute_ok && ((g_es_rb_mute & 0x60u) == 0u);
        if (mute_ok != 0)
        {
            /* ★ 补丁13：把 REG31 的**写侧缓存**对齐到芯片里的真实值。
             *   为什么必须有这一步：es8311_set_mute() 已改成"不读、直接用缓存拼值"
             *   （少一笔读 = 少一次 NACK 机会，见其说明）。缓存若永远不校正，一次丢写
             *   就会让缓存与芯片永久分叉。这里每 ~1 s 回读一次 ⇒ 分叉最多存在 1 s。 */
            s_reg31 = (uint8_t)g_es_rb_mute;
        }

        /* 终态判据：音量 == 目标、REG12 的 power-down 位(bit1)为 0、且 REG31 未静音。*/
        if (vol_ok && pwr_ok && mute_ok && (g_es_rb_vol == target) && ((g_es_rb_pwr & 0x02u) == 0u))
        {
            confirmed = 1;
            break;
        }

        /* ---- 分层补写：先便宜、后昂贵（2026-10-13 板上实测定的层次）----
         * 【为什么不能"读失败就整段重写"】I2C 读**会失败**（探针 halt、芯片电源态窗口），
         *   一旦 read != 0 就把回读值当 0xFFFF、立刻 dac_repair() 整段重写（11 笔写）
         *   ⇒ 一次读 NACK 换来 11 笔写，是"写放大"，而写风暴又更容易再丢写。
         * 【为什么也不能"读失败就不补"】实测（tools/wav_pause_resume_check.py，2026-10-13）：
         *   音量 ramp 的**第 8 步（写终值 0x89）**若砸进 NACK 窗口、5 次重试全灭，
         *   REG32 会停在**中间档**（实测 0x78 = 第 7 步值），而那一拍的回读往往也在同一
         *   窗口里失败 ⇒ 此时若不补，音量就**永久停在中间值** —— 这正是用户报的
         *   "音量大小混乱"（表现为听起来比设定音量轻一档，且每次续播轻重不一）。
         * 【所以】第 1、2 轮各补**单笔**（REG32 / REG12 这两个决定听感的寄存器，最省），
         *   末轮仍不对才 dac_repair() 整段重写（覆盖"丢写落在通路别的寄存器"的少见情形）。 */
        if (round < 2u)
        {
            g_es_fix_wr++;              /* 单笔补写也计入修补次数，避免掩盖问题 */
            if (!vol_ok || (g_es_rb_vol != target))
            {
                (void)es8311_write_reg(0x32, (uint8_t)target);
            }
            if (!pwr_ok || ((g_es_rb_pwr & 0x02u) != 0u))
            {
                (void)es8311_write_reg(0x12, 0x00u);
            }
            if (!mute_ok)
            {
                (void)es8311_set_mute(0);   /* 卡在静音：立刻补一次解静音 */
            }
        }
        else
        {
            /* 用"只补那两个寄存器"不够的情形：丢写随机落在 0x09/0x0A/0x0E/0x12/
             * 0x14/0x17/0x32/0x37/0x45 各处，末轮整段重写才闭合得住（计数在
             * es8311_dac_repair 里）。 */
            (void)es8311_dac_repair();
        }
    }
    return confirmed ? 0 : -1;
}

int es8311_dac_repair(void)
{
    g_es_fix_wr++;                     /* 补写计数：回读发现终态不对才会走到这里 */
    return dac_path_regs(es8311_volume_reg(g_audio_volume));
}

int es8311_is_inited(void)
{
    return (s_codec_ready != 0u) ? 1 : 0;
}
