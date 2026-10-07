/* ===========================================================================
 * DMA ↔ D-Cache 一致性双向台架（2026-10-04）
 *
 * 【为什么需要它】
 * 本工程下一步要接 TF 卡（SDMMC + IDMA），而 SDMMC/IDMA 是**总线主设备**，
 * 会绕过 CPU 的 L1 D-Cache 直接读写内存。届时会同时出现两个方向的问题：
 *
 *   方向 ①：CPU 写 → DMA 读    —— 现在安全，因为 FORCEWT=1（写直达内存）
 *   方向 ②：DMA 写 → CPU 读    —— **危险**：D-Cache 里可能还留着旧副本，
 *                                CPU 读到陈旧数据，且完全静默（不报错、不 HardFault）
 *
 * 方向 ② 不能靠"记得先 invalidate"来防 —— 本工程历史上已经吃过一次
 * "以为在防、实际漏了"的亏（阶段 3 的总线路障那两节）。所以在动 SDMMC 之前，
 * 先用**确定性实测**把这两个方向的真实行为量出来，再决定架构：
 *   · 若方向 ② 确实会读到陈旧值 ⇒ 必须划"专用非缓存 DMA 区"（L3），
 *     而不是靠调用方手工维护 cache。
 *
 * 【为什么用 DMA1 MEM2MEM 就够了】
 * CPU 写内存 → DMA 搬到另一块 → CPU 读回比对，这就是"CPU ↔ DMA 数据通路"
 * 的最小完备模型。SDMMC 只是把"搬运的两端"换成 FIFO，缓存语义完全一致。
 * 用 MEM2MEM 做台架的好处：不需要 SD 卡、不需要新增源文件、不占用任何引脚、
 * 不需要中断（用 HAL_DMA_PollForTransfer 阻塞等待）。
 *
 * 【为什么必须用 DMA1/DMA2 而不是 MDMA】
 * 本台架的关键是**复现 CPU 与总线主设备之间的缓存不一致**。DMA1/DMA2 与
 * CPU 共用同一套 L1 D-Cache 一致性域（都不做 cache 维护），语义与 SDMMC IDMA 相同。
 *
 * 【硬件约束（务必遵守，否则 DMA 会静默搬不到数据）】
 *   1. **DMA1/DMA2 无法访问 DTCM（0x2000_0000）** —— TCM 是 CPU 专有，不挂在
 *      总线矩阵的从设备侧。所以缓冲区绝不能来自 board_alloc0（DTCM）。
 *      本台架的缓冲区显式放在 **D2 域 SRAM1（0x3000_0000）**：
 *         · 与 DMA1/DMA2 同域（都在 D2），无跨域延迟；
 *         · 不与 heap1（AXI SRAM，峰值 97%）抢空间；
 *         · MPU region3 把它配成 **CACHEABLE** —— 正是我们要暴露的场景。
 *   2. **缓冲区必须 32 字节对齐**（Cortex-M7 cache line = 32 B），
 *      否则 SCB_InvalidateDCache_by_Addr 会误伤相邻数据的脏行。
 *   3. **MEM2MEM 不能用循环模式**（HAL 头文件明确注明），用 DMA_NORMAL。
 *
 * 【三个用例与预期】（`words = 64`，即 256 B = 8 条 cache line）
 *   A  方向① CPU写→DMA读      ：预期 mismatch = 0     （FORCEWT 保证写直达）
 *   B  方向② DMA写→CPU读(不维护)：预期 mismatch = 64    （全部读到陈旧值）
 *   C  方向② + invalidate      ：预期 mismatch = 0     （证明补救办法有效）
 *
 * B 与 C 成对出现，互为对照：
 *   · 只有 B 失败，才能证明"必须做 cache 维护"不是空话；
 *   · 只有 C 通过，才能证明"invalidate 确实能补救"（而不是恰好两次都对）。
 *
 * 【为什么 B 必然失败（而不是"看运气"）】
 *   · 16 KB D-Cache / 4 路组相联，本台架只占 8 条 line，分散在 8 个不同组，
 *     每组只用 1 路 —— 在 prime 读与复读之间没有任何东西能把它们挤出去；
 *   · prime 读用 volatile 指针，保证真的发 load 指令（不会被优化掉）；
 *   · 中间只夹一次 DMA 搬运，DMA 完全不碰 D-Cache。
 *
 * 用法：`python tools/dma_check.py`（写 g_dma_test=1 触发，自动读回结果并判定）。
 * =========================================================================== */

#ifndef DMA_BENCH_H
#define DMA_BENCH_H

#include <stdint.h>

/* ---- 诊断量（全部在 DTCM 的 .bss，SWD 读不受 D-Cache 影响）---- */

extern volatile uint32_t g_dma_test;        /* 写 1 触发一轮完整测试；跑完自动清 0 */
extern volatile uint32_t g_dma_case;        /* 结果码：0=未跑 1=DMA初始化失败 2=三个用例都跑完 */

extern volatile uint32_t g_dma_init_rc;     /* HAL_DMA_Init 返回值（HAL_OK=0） */
extern volatile uint32_t g_dma_xfer_rc;     /* 最后一次 HAL_DMA_Start 返回值（HAL_OK=0） */
extern volatile uint32_t g_dma_poll_rc;     /* 最后一次 HAL_DMA_PollForTransfer 返回值（HAL_OK=0） */

extern volatile uint32_t g_dma_dcache_on;   /* 1 = D-Cache 已使能（SCB->CCR.DC） */
extern volatile uint32_t g_dma_forcewt;     /* 1 = FORCEWT 已置位（SCB->CACR bit2） */

extern volatile uint32_t g_dma_a_mismatch;  /* 用例 A：CPU 写 → DMA 读，预期 0 */
extern volatile uint32_t g_dma_b_mismatch;  /* 用例 B：DMA 写 → CPU 读（不维护），预期 = words */
extern volatile uint32_t g_dma_c_mismatch;  /* 用例 C：DMA 写 → CPU 读（invalidate），预期 0 */

/* "读到新值"的逐字位图（bit i = 第 i 个字读到了新值 ⇒ 该处没有陈旧副本）。
 * 解释用：32 B/line = 8 字，位图成"8 位一段"⇒ 那一条 cache line 未填充或被换出。 */
extern volatile uint32_t g_dma_a_fresh_lo, g_dma_a_fresh_hi;
extern volatile uint32_t g_dma_b_fresh_lo, g_dma_b_fresh_hi;
extern volatile uint32_t g_dma_c_fresh_lo, g_dma_c_fresh_hi;

extern volatile uint32_t g_dma_src_addr;    /* 源缓冲区实际地址（应落在 0x3000_0000 区） */
extern volatile uint32_t g_dma_dst_addr;    /* 目标缓冲区实际地址 */
extern volatile uint32_t g_dma_words;       /* 每次用例搬运的字数（32 位） */
extern volatile uint32_t g_dma_bytes;       /* 每次用例搬运的字节数 */

/* 写 g_dma_test=1 调用的入口（挂在主循环，与 g_micro_test / g_gram_recheck 同一手法：
 * 不需要重编译、不需要调试器断点）。 */
void run_dma_bench(void);

#endif /* DMA_BENCH_H */
