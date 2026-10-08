/* ===========================================================================
 * 板载 RTC 实现 —— 设计意图与判据见 rtc_clock.h 顶部。
 *
 * ⚠⚠ 改本文件前必须知道的三件事（都踩过或查过手册）
 *
 * 1. **不要无条件重设 RTC 时钟源**。
 *    `HAL_RCCEx_PeriphCLKConfig()` 选 RTC 源时会置 `RCC_BDCR.BDRST`，**复位整个
 *    备份域** —— 也就是说，对一块"VDD 一直没断、RTC 正在走、BKP 里存着标记"的板子
 *    再选一次源，会把刚攒下的时间和标记全部清掉，正好把本模块要解决的问题制造出来。
 *    ⇒ 先读 `RCC->BDCR.RTCSEL`：非 0 = 备份域已配过 ⇒ 只使能 RTCEN，跳过选源。
 *
 * 2. **HAL_RTC_GetTime / HAL_RTC_GetDate 必须成对、且顺序固定**。
 *    H7 的 RTC 日历寄存器走影子寄存器，读时间会锁住影子；只有随后的读日期
 *    才解锁并装载下一次采样。反过来（先 GetDate）不会解锁 ⇒ 之后读到的是陈旧值，
 *    表现为"分钟不动"。见 RM0433 RTC 章节与 HAL 里 GetTime 的自带注释。
 *
 * 3. **预分频要和时钟源匹配**，否则走时快慢不对：
 *    LSE 32768 Hz → (127+1)×(255+1) = 32768 ✓
 *    LSI 32000 Hz → (127+1)×(249+1) = 32000 ✓
 *    LSI 是内部 RC，精度约 ±5%（一天能差一小时），只作 LSE 起振失败时的兜底，
 *    真实时间仍会被 $DT 每 10 s 校准回来。
 * =========================================================================== */

#include "rtc_clock.h"

#include <string.h>              /* memset */

#include "stm32h7xx_hal.h"

/* 备份域标记：用来区分"备份域从未配过"与"VDD 没断、RTC 一直在走"。
 * 'YMGR' —— 与其它模块的魔数风格一致（图库索引用 0x4947）。 */
#define RTC_BKP_MAGIC   0x594D4752u
#define RTC_LSE_WAIT_MS 1500u      /* 等 LSE 起振的上限；实测本板 <300 ms */

volatile int32_t  g_rtc_rc    = -1;
volatile int32_t  g_rtc_src   = 2;
volatile int32_t  g_rtc_valid = 0;
volatile uint32_t g_rtc_sets  = 0;

static RTC_HandleTypeDef s_rtc;
static int s_ok    = 0;   /* 0x01：HAL_RTC_Init 成功 */
static int s_valid = 0;   /* 时间可信（BKP 标记 + 年份合理） */

/* ------------------------------------------------------------------------- */
static int year_is_sane(int y)
{
    /* 备份域掉电后 RTC 从 2000-01-01 起算；BKP 被随机值命中也不该更早。
     * 下界取 2024（本工程成立的时间），上界 2099 防溢出到 2100 年。 */
    return (y >= 2024 && y <= 2099);
}

/* 读一次日历并判断可信度（写进 s_valid）。 */
static void evaluate_valid(void)
{
    RTC_DateTypeDef d;
    RTC_TimeTypeDef t;

    s_valid = 0;
    if (!s_ok)
        return;
    if (HAL_RTCEx_BKUPRead(&s_rtc, RTC_BKP_DR0) != RTC_BKP_MAGIC)
        return;
    HAL_RTC_GetTime(&s_rtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&s_rtc, &d, RTC_FORMAT_BIN);   /* 必须跟在 GetTime 之后 */
    if (year_is_sane(2000 + (int)d.Year))
        s_valid = 1;
}

/* ------------------------------------------------------------------------- */
int Rtc_Init(void)
{
    uint32_t sel;
    uint32_t t;

    g_rtc_rc    = -1;
    g_rtc_src   = 2;
    g_rtc_valid = 0;
    s_ok        = 0;
    s_valid     = 0;

    HAL_PWR_EnableBkUpAccess();

    sel = RCC->BDCR & RCC_BDCR_RTCSEL;

    if (sel == 0u)
    {
        /* 备份域还没配过（首次上电 / 曾断电）：现在选时钟源。
         * LSE 优先 —— 板上有 32.768 kHz 晶振（原理图 Y1，PC14/PC15）。 */
        uint32_t src = 0u;              /* 0 = LSE，1 = LSI */

        __HAL_RCC_LSE_CONFIG(RCC_LSE_ON);
        t = 0u;
        while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == 0u && t < RTC_LSE_WAIT_MS)
        {
            HAL_Delay(1);
            ++t;
        }

        if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == 0u)
        {
            /* 晶振没起来（未焊/坏/负载电容不对）：关掉它再回退 LSI，
             * 免得留一个悬空的振荡器白耗电。 */
            __HAL_RCC_LSE_CONFIG(RCC_LSE_OFF);
            __HAL_RCC_LSI_ENABLE();     /* H7 的 LSI 是 ENABLE/DISABLE 而不是 CONFIG(state) */
            t = 0u;
            while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0u && t < 200u)
            {
                HAL_Delay(1);
                ++t;
            }
            src = 1u;
            if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0u)
            {
                g_rtc_rc = -2;          /* LSE / LSI 都不行 ⇒ RTC 不可用 */
                return -2;
            }
        }

        {
            /* ⚠ 整体零初始化：该结构字段很多，HAL 在 PeriphClockSelection 的
             *   各个分支上会读不同的成员，栈上垃圾会让它配出意外时钟。 */
            RCC_PeriphCLKInitTypeDef p;
            memset(&p, 0, sizeof(p));
            p.PeriphClockSelection = RCC_PERIPHCLK_RTC;
            p.RTCClockSelection    = (src == 0u) ? RCC_RTCCLKSOURCE_LSE
                                                : RCC_RTCCLKSOURCE_LSI;
            if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK)
            {
                g_rtc_rc = -3;
                return -3;
            }
        }

        __HAL_RCC_RTC_ENABLE();
        g_rtc_src = (int32_t)src;
    }
    else
    {
        /* 备份域已配过：**只使能 RTCEN，绝不重选时钟源**（重选会 BDRST 清备份域，
         * 把正在走的 RTC 时间一起清掉 —— 见文件头注释 1）。 */
        __HAL_RCC_RTC_ENABLE();
        if (sel == RCC_RTCCLKSOURCE_LSE)
            g_rtc_src = 0;
        else if (sel == RCC_RTCCLKSOURCE_LSI)
            g_rtc_src = 1;
        else
            g_rtc_src = 2;              /* HSE 分频源（本板不用），按未知处理 */
    }

    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.Instance             = RTC;
    s_rtc.Init.HourFormat      = RTC_HOURFORMAT_24;
    s_rtc.Init.AsynchPrediv    = 127u;
    s_rtc.Init.SynchPrediv     = (g_rtc_src == 0) ? 255u : 249u;   /* 见文件头注释 3 */
    s_rtc.Init.OutPut          = RTC_OUTPUT_DISABLE;
    s_rtc.Init.OutPutRemap     = RTC_OUTPUT_REMAP_NONE;
    s_rtc.Init.OutPutPolarity  = RTC_OUTPUT_POLARITY_HIGH;
    s_rtc.Init.OutPutType      = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&s_rtc) != HAL_OK)
    {
        g_rtc_rc = -4;
        return -4;
    }

    s_ok  = 1;
    g_rtc_rc = 0;
    evaluate_valid();
    g_rtc_valid = s_valid;
    return 0;
}

int Rtc_IsValid(void)
{
    return s_valid;
}

void Rtc_SetLocal(int year, int mon, int mday, int hh, int mm, int ss, int wday)
{
    RTC_DateTypeDef d;
    RTC_TimeTypeDef t;

    if (!s_ok)
        return;
    if (!year_is_sane(year))
        return;                          /* 不合理的时间不写进去 */

    t.Hours            = (uint8_t)hh;
    t.Minutes          = (uint8_t)mm;
    t.Seconds          = (uint8_t)ss;
    t.SubSeconds       = 0;
    t.TimeFormat       = RTC_HOURFORMAT12_AM;   /* 24 小时制下该字段被忽略 */
    t.DayLightSaving   = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation   = RTC_STOREOPERATION_RESET;

    d.Year    = (uint8_t)(year - 2000);
    d.Month   = (uint8_t)mon;
    d.Date    = (uint8_t)mday;
    d.WeekDay = (uint8_t)((wday >= 1 && wday <= 7) ? wday : 1);

    HAL_PWR_EnableBkUpAccess();
    /* 顺序：先写时间再写日期（与读相反）。两次写各自进/出 init mode。 */
    (void)HAL_RTC_SetTime(&s_rtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_SetDate(&s_rtc, &d, RTC_FORMAT_BIN);
    HAL_RTCEx_BKUPWrite(&s_rtc, RTC_BKP_DR0, RTC_BKP_MAGIC);

    s_valid = 1;
    g_rtc_valid = 1;
    g_rtc_sets++;
}

int Rtc_GetLocal(int* year, int* mon, int* mday, int* hh, int* mm, int* ss, int* wday)
{
    RTC_DateTypeDef d;
    RTC_TimeTypeDef t;

    if (!s_ok)
        return -1;

    HAL_RTC_GetTime(&s_rtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&s_rtc, &d, RTC_FORMAT_BIN);   /* 必须跟在其后，见文件头注释 2 */

    if (year)  *year = 2000 + (int)d.Year;
    if (mon)   *mon  = (int)d.Month;
    if (mday)  *mday = (int)d.Date;
    if (hh)    *hh   = (int)t.Hours;
    if (mm)    *mm   = (int)t.Minutes;
    if (ss)    *ss   = (int)t.Seconds;
    if (wday)  *wday = (int)d.WeekDay;
    return 0;
}
