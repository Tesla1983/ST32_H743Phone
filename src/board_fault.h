/* ===========================================================================
 * 内核故障现场快照
 *
 * 为什么需要：Cortex-M 的 fault 处理里，厂商模板（stm32h7xx_it.c / mpu.c）
 * 一律是 `while(1)` 或 `NVIC_SystemReset()`。**故障与"死循环"在外部完全无法区分**
 * —— 只能看到一个不再增长的计数器。
 *
 * 本工程就为此吃过一次大亏：`YMGUI_Inject_Pointer()` 一调用主循环就停，
 * 先被误判为"递归死循环"，白排查了四个方向（栈/递归/环/printf）。
 * 实际是 BusFault 升级成的 HardFault。若当时有这份快照，一眼就能看出是故障、
 * 且能直接拿到出错地址（BFAR）。
 *
 * 用法：故障停机后，用 SWD 读 g_fault：
 *   magic == 0xFA017CA7  ⇒ 确实发生过故障（为 0 则是干净的）
 *   kind                 ⇒ 1=HardFault 2=MemManage 3=BusFault 4=UsageFault 5=NMI
 *   cfsr 分解            ⇒ bit0 MMARVALID / bit8 BFARVALID / bit9 PRECISERR ...
 *   bfar/mmfar           ⇒ 出错的数据地址
 * =========================================================================== */

#ifndef BOARD_FAULT_H
#define BOARD_FAULT_H

#include <stdint.h>

typedef struct
{
    volatile uint32_t magic;    /* 0xFA017CA7 = 已捕获过故障 */
    volatile uint32_t kind;     /* 1=Hard 2=MemManage 3=Bus 4=Usage 5=NMI */
    volatile uint32_t cfsr;     /* SCB->CFSR：可分解 MMFSR / BFSR / UFSR */
    volatile uint32_t hfsr;     /* SCB->HFSR：bit30 = FORCED（被升级为 HardFault）*/
    volatile uint32_t mmfar;    /* SCB->MMFAR：MemManage 出错地址 */
    volatile uint32_t bfar;     /* SCB->BFAR：BusFault 出错地址 */
    volatile uint32_t shcsr;    /* SCB->SHCSR：哪些 fault 被单独使能 */
    volatile uint32_t ccr;      /* SCB->CCR：UNALIGN_TRP / DIV_0_TRP 等 */
    volatile uint32_t count;    /* 累计进入次数 */
} board_fault_t;

extern board_fault_t g_fault;

/* 由各 fault handler 调用：把内核故障寄存器落到 g_fault，然后由调用方停机。 */
void board_fault_capture(uint32_t kind);

#endif /* BOARD_FAULT_H */
