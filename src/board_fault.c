/* ===========================================================================
 * 内核故障现场快照（实现）
 *
 * 只读内核的故障状态寄存器，不做任何可能再触发故障的事（不打印、不分配内存）。
 * 说明：这里**不采集栈上的异常帧（stacked PC/LR）** —— 那需要在 handler 入口用
 * naked asm 分别判定 MSP/PSP。对本工程的定位需求来说，CFSR 的分类位 + BFAR/MMFAR
 * 的出错地址已经足够（当初就是靠 BFAR=0x0D38353F 直接定位到"读了野对象的
 * obj->state"）；要精确到出错指令，再用 GDB 看栈回溯即可。
 * =========================================================================== */

#include "board_fault.h"
#include "stm32h7xx.h"

board_fault_t g_fault;

void board_fault_capture(uint32_t kind)
{
    g_fault.kind  = kind;
    g_fault.cfsr  = SCB->CFSR;
    g_fault.hfsr  = SCB->HFSR;
    g_fault.mmfar = SCB->MMFAR;
    g_fault.bfar  = SCB->BFAR;
    g_fault.shcsr = SCB->SHCSR;
    g_fault.ccr   = SCB->CCR;
    g_fault.count = g_fault.count + 1u;

    /* magic 最后写：保证上面几项都已写入，读到 magic 就说明快照完整 */
    g_fault.magic = 0xFA017CA7u;
}
