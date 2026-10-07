/* ===========================================================================
 * GCC 版启动代码 + 向量表（STM32H743VIT6）
 *
 * 为什么自己写：CMSIS 资料包里只有 Keil(arm) 版 startup_stm32h743xx.s，
 * 它用 ARMCC 语法（PROC/ENDP/IMPORT），且 Reset_Handler 调 Keil C 库入口
 * `__main` —— 在 GNU 工具链下链接不过。本文件按 GNU as 语法重写。
 *
 * 中断都先指向 Default_Handler（死循环）。YMGUI 是轮询式、无中断驱动，
 * 需要某个中断时在 C 里定义同名函数即可覆盖（都是 weak 符号）。
 * =========================================================================== */

.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.global __Vectors
.global Reset_Handler
.global Default_Handler

/* ====================== 向量表 ====================== */
.section .isr_vector, "a", %progbits
.type __Vectors, %object
__Vectors:
    .word  _estack                  /* 初始 MSP（链接脚本给：DTCM 末端） */
    .word  Reset_Handler
    /* ---- Cortex-M7 内核异常 ---- */
    .word  NMI_Handler
    .word  HardFault_Handler
    .word  MemManage_Handler
    .word  BusFault_Handler
    .word  UsageFault_Handler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  SVC_Handler
    .word  DebugMon_Handler
    .word  0
    .word  PendSV_Handler
    .word  SysTick_Handler
    /* ---- 外部中断：STM32H743 有 150 个 IRQ（IRQ0..IRQ149）---- */
    .rept  150
    .word  Default_Handler
    .endr
.size __Vectors, . - __Vectors

/* ====================== 复位入口 ====================== */
.section .text.Reset_Handler, "ax", %progbits
.type Reset_Handler, %function
Reset_Handler:
    /* 1) 设栈指针（链接脚本的 _estack = DTCM 末端） */
    ldr   sp, =_estack

    /* 2) 使能 FPU：CPACR 的 CP10/CP11 全访问。
     *    Cortex-M7 复位后 FPU 关闭，此时任何浮点指令都会触发 UsageFault。
     *    应用层要用 float（计划书允许），所以必须在这里打开。 */
    ldr   r0, =0xE000ED88           /* SCB->CPACR */
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)      /* CP10=11, CP11=11 */
    str   r1, [r0]
    dsb
    isb

    /* 3) CMSIS 系统初始化（向量表偏移、SystemCoreClock 更新） */
    bl    SystemInit

    /* 4) 把 .data 从 flash 搬到 DTCM、清 .bss（板级 C 实现，见 board_startup.c） */
    bl    board_init_memory

    /* 5) C 库初始化（__attribute__((constructor)) 等） */
    bl    __libc_init_array

    /* 6) 进 main，不应返回 */
    bl    main

1:  b     1b
.size Reset_Handler, . - Reset_Handler

/* ====================== 默认中断处理 ====================== */
.section .text.Default_Handler, "ax", %progbits
.type Default_Handler, %function
Default_Handler:
    b     .
.size Default_Handler, . - Default_Handler

/* 把所有内核异常与 IRQ 弱绑定到 Default_Handler。
 * `.thumb_set` 让它们拥有正确的 bit0=1（Thumb 状态）。 */
.weak NMI_Handler
.thumb_set NMI_Handler, Default_Handler
.weak HardFault_Handler
.thumb_set HardFault_Handler, Default_Handler
.weak MemManage_Handler
.thumb_set MemManage_Handler, Default_Handler
.weak BusFault_Handler
.thumb_set BusFault_Handler, Default_Handler
.weak UsageFault_Handler
.thumb_set UsageFault_Handler, Default_Handler
.weak SVC_Handler
.thumb_set SVC_Handler, Default_Handler
.weak DebugMon_Handler
.thumb_set DebugMon_Handler, Default_Handler
.weak PendSV_Handler
.thumb_set PendSV_Handler, Default_Handler
.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler
