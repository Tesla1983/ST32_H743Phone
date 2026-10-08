/* ===========================================================================
 * 板载 RTC（LSE 32.768 kHz）—— 让时间/日期在**复位后立刻可用**
 *
 * 【为什么需要：旧方案的结构性缺陷】
 *   原来时间全靠 ESP32 的 $DT 帧 + 板载 tick 递推（src/uart_link.c 的
 *   current_clock）。$DT 每 10 s 一帧 ⇒ **每次复位都要重新等首帧**，
 *   这段时间界面只能显示占位串；$DT 断流（ESP32 掉电/重启）就更久。
 *   而且递推只累加"当日秒数"，日期永远停在最后一帧 $DT 给的日期。
 *   ⇒ 板上有 32.768 kHz 晶振（原理图 Y1，PC14/PC15）就该用起来。
 *
 * 【掉电能保持吗：不能，且这是硬件的限制，不是软件的】
 *   原理图 `STM32H743VIT6 V1.2_SCH.pdf` 里 VBAT 经 BAT54C 从 VDD 取电，
 *   **没有后备电池/超级电容**。所以：
 *     · VDD 一直在（按复位键、按 RESET、SWD reset、看门狗复位）⇒ 备份域不复位
 *       ⇒ **RTC 与 BKP 寄存器都保持**，重启后立即有正确时间日期；
 *     · 真断电（拔 USB / 断电开关）⇒ 备份域丢失，RTC 归零，BKP 标记清零
 *       ⇒ 上电后回到"等首帧 $DT"（RTC 有效标志为假，UI 显示占位串）。
 *   想真正掉电保持得焊一颗纽扣电池到 VBAT——本工程不做（改硬件不属软件范围）。
 *
 * 【判据（SWD 直接读，不需要屏幕）】
 *   g_rtc_rc      = 0   初始化成功；<0 见 rtc_clock.c 的返回码
 *   g_rtc_src     = 0   用的是 LSE（32.768 kHz 晶振，准）
 *                 = 1   用的是 LSI（内部 RC，起振失败时的回退，精度差）
 *                 = 2   两者都失败（RTC 不可用，链路退化为旧的 tick 递推）
 *   g_rtc_valid   = 1   时间可信（曾被 $DT 校准过）
 *   g_rtc_sets    校准次数（= 收到的有效 $DT 帧数）
 * =========================================================================== */

#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <stdint.h>

extern volatile int32_t  g_rtc_rc;      /* 0 = 成功 */
extern volatile int32_t  g_rtc_src;     /* 0 = LSE，1 = LSI，2 = 都失败 */
extern volatile int32_t  g_rtc_valid;   /* 1 = 时间可信 */
extern volatile uint32_t g_rtc_sets;    /* 校准次数 */

/* 初始化：使能 LSE（失败回退 LSI）→ 选 RTC 时钟源 → HAL_RTC_Init。
 * 返回 0 成功。**会阻塞最多约 2 s**（等 LSE 起振），因此必须在 HAL_Init()
 * 之后、GUI 起来之前调用一次。 */
int Rtc_Init(void);

/* 时间是否可信：备份域标记（本次上电后是否被校过）+ 年份合理性双重判据。
 * 断电重启后未校时 ⇒ 返回 0，调用方应显示占位而不是 2000-01-01。 */
int Rtc_IsValid(void);

/* 用 ESP32 的 $DT 校准（本地时间，CST-8，**不要再加减时区**）。
 * wday = ISO 8601：1=周一 … 7=周日；传 0 或越界值表示未知，存 1 占位。 */
void Rtc_SetLocal(int year, int mon, int mday, int hh, int mm, int ss, int wday);

/* 读当前本地时间/日期。返回 0 = 成功；非 0 = RTC 不可用，输出参数不动。
 * ⚠ 内部按"先 GetTime 再 GetDate"的顺序成对调用 HAL —— 这是 H7 RTC 影子
 *   寄存器的要求，反过来读会拿到陈旧日期（ST 参考手册 RM0433 的 RTC 章节，
 *   HAL 的 HAL_RTC_GetTime 里也有对应注释）。 */
int  Rtc_GetLocal(int* year, int* mon, int* mday, int* hh, int* mm, int* ss, int* wday);

#endif /* RTC_CLOCK_H */
