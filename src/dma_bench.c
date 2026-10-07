/* ===========================================================================
 * DMA ↔ D-Cache 一致性双向台架 —— 实现
 *
 * 设计说明见 src/dma_bench.h 顶部（为什么做、硬件约束、三个用例的预期）。
 *
 * 这里是实机约束的摘要：
 *   · DMA1 走 **MEM2MEM**（DMAMUX 请求 0，`HAL_DMA_Init` 在
 *     `Direction == DMA_MEMORY_TO_MEMORY` 时会自动把 Request 强制成
 *     `DMA_REQUEST_MEM2MEM`，见 HAL 源码 stm32h7xx_hal_dma.c:418-425）；
 *   · 阻塞等待用 `HAL_DMA_PollForTransfer` + **有限超时**（100 ms），
 *     SysTick_Handler 里有 `HAL_IncTick()`，所以 HAL_GetTick 是活的，
 *     超时判断**不会**被卡成无限循环；
 *   · 缓冲区在 **D2 SRAM1**，DMA1 与它同在 D2 域；
 *   · 缓冲区 32 字节对齐（cache line = 32 B），保证 by_Addr 的 cache 维护
 *     不会误伤相邻数据。
 * =========================================================================== */

#include "stm32h7xx_hal.h"

#include "dma_bench.h"

/* ---- 规模：64 字 = 256 B = 8 条 cache line（16 KB / 4 路组相联，绰绰有余）---- */
#define DMA_BENCH_WORDS   64u
#define DMA_BENCH_BYTES   (DMA_BENCH_WORDS * 4u)

/* ---- 三个图案：两两不同，保证任何一个"陈旧值"都能被比对抓住 ---- */
#define DMA_PAT_A   0xA5A5A5A5u
#define DMA_PAT_B   0x5A5A5A5Au
#define DMA_PAT_C   0x3C3C3C3Cu

/* ---- 缓冲区 → D2 SRAM1（链接脚本的 .bss_dma NOLOAD 段），32 B 对齐 ---- */
#define DMA_BENCH_ATTR   __attribute__((section(".bss_dma"), aligned(32)))

static uint32_t s_src[DMA_BENCH_WORDS] DMA_BENCH_ATTR;
static uint32_t s_dst[DMA_BENCH_WORDS] DMA_BENCH_ATTR;

static DMA_HandleTypeDef s_hdma;
static uint32_t          s_ready;      /* 1 = DMA 已初始化 */

/* prime 读的汇点：必须 volatile，否则 -O2 会把"读了不用"的整段循环删掉，
 * 那样 cache 根本没被填充，用例 B 会假通过。 */
static volatile uint32_t s_sink;

/* ===================== 诊断量（.bss → DTCM，SWD 读不受 cache 影响）===================== */

volatile uint32_t g_dma_test       = 0;             /* 写 1 触发；跑完自动清 0 */
volatile uint32_t g_dma_case       = 0;             /* 0=未跑 1=初始化失败 2=跑完 */

volatile uint32_t g_dma_init_rc    = 0xFFu;         /* HAL_DMA_Init 返回值（HAL_OK = 0）*/
volatile uint32_t g_dma_xfer_rc    = 0xFFu;         /* 最后一次 HAL_DMA_Start 返回值 */
volatile uint32_t g_dma_poll_rc    = 0xFFu;         /* 最后一次 HAL_DMA_PollForTransfer 返回值 */

volatile uint32_t g_dma_dcache_on  = 0;             /* 1 = D-Cache 使能（SCB->CCR.DC）*/
volatile uint32_t g_dma_forcewt    = 0;             /* 1 = FORCEWT（SCB->CACR bit2）*/

volatile uint32_t g_dma_a_mismatch = 0xFFFFFFFFu;   /* A 预期 0（CPU 写 → DMA 读）*/
volatile uint32_t g_dma_b_mismatch = 0xFFFFFFFFu;   /* B 预期 = words（DMA 写 → CPU 读，不维护）*/
volatile uint32_t g_dma_c_mismatch = 0xFFFFFFFFu;   /* C 预期 0（DMA 写 → CPU 读，invalidate）*/

/* "读到新值"的逐字位图（bit i = 第 i 个字读到的是**新值**，即该处没有陈旧副本）。
 * 用途：解释"失配为何不一定等于总字数" —— 32 B/line = 8 字，
 * 位图里应成"8 位一段"出现，从而直接指出哪几条 cache line 被换出/未填充。 */
volatile uint32_t g_dma_a_fresh_lo = 0;
volatile uint32_t g_dma_a_fresh_hi = 0;
volatile uint32_t g_dma_b_fresh_lo = 0;
volatile uint32_t g_dma_b_fresh_hi = 0;
volatile uint32_t g_dma_c_fresh_lo = 0;
volatile uint32_t g_dma_c_fresh_hi = 0;

volatile uint32_t g_dma_src_addr   = 0;             /* 源地址（应在 0x3000_0000 区）*/
volatile uint32_t g_dma_dst_addr   = 0;
volatile uint32_t g_dma_words      = 0;
volatile uint32_t g_dma_bytes      = 0;
volatile uint32_t g_dma_cyc        = 0;             /* 一次 256 B MEM2MEM 搬运的 DWT 周期数 */

/* ===================== 小工具 ===================== */

/* 第 i 个字应该是什么值。用"基值 ^ 位置相关量"，保证同一 i 下不同基值必不相等。 */
static uint32_t pat(uint32_t base, uint32_t i)
{
    return base ^ (i * 0x01010101u);
}

/* CPU 写源缓冲（普通写；FORCEWT=1 时直达内存）*/
static void fill_src(uint32_t base)
{
    uint32_t i;
    for (i = 0u; i < DMA_BENCH_WORDS; i++)
    {
        s_src[i] = pat(base, i);
    }
}

/* 把目标缓冲的当前内容"读一遍"以填充 D-Cache。
 * 用 volatile 指针强制真的发 load；结果写进 s_sink 防止被优化掉。 */
static void prime_dst_cache(void)
{
    volatile const uint32_t *p = (volatile const uint32_t *)s_dst;
    uint32_t acc = 0u;
    uint32_t i;
    for (i = 0u; i < DMA_BENCH_WORDS; i++)
    {
        acc += p[i];
    }
    s_sink = acc;
}

/* CPU 读目标缓冲，统计与期望图案不符的字数。
 * 关键：用 volatile 读 —— 这样读**一定**走 D-Cache 命中路径（这正是要考察的），
 * 且编译器不会把它改成"从别处取已知值"。
 *
 * 同时把"读到**新值**的字"记成位图（fresh_lo/hi）——这是为了解释"为什么失配
 * 不是恰好等于总字数"：位图能直接看出是哪几条 cache line 没有陈旧副本
 * （32 B/line = 8 字 ⇒ 位图里应当成 8 位一段出现）。 */
static uint32_t count_mismatch(uint32_t base, volatile uint32_t *fresh_lo, volatile uint32_t *fresh_hi)
{
    volatile const uint32_t *p = (volatile const uint32_t *)s_dst;
    uint32_t bad = 0u;
    uint32_t lo  = 0u;
    uint32_t hi  = 0u;
    uint32_t i;

    for (i = 0u; i < DMA_BENCH_WORDS; i++)
    {
        if (p[i] != pat(base, i))
        {
            bad++;
        }
        else if (i < 32u)
        {
            lo |= (1u << i);
        }
        else
        {
            hi |= (1u << (i - 32u));
        }
    }

    *fresh_lo = lo;
    *fresh_hi = hi;
    return bad;
}

/* 一次 MEM2MEM 搬运：src → dst。返回 1 = 成功，0 = 失败（原因见诊断量）。 */
static uint32_t dma_copy(void)
{
    HAL_StatusTypeDef rc;
    uint32_t          c0;

    /* 保证此前所有 CPU 写都已落到内存，再让 DMA 去读 —— 这是"提交缓冲区给 DMA"
     * 的标准前序动作（本工程 FORCEWT=1，写本来就直达，但顺序语义靠 DSB 显式化）。 */
    __DSB();

    rc = HAL_DMA_Start(&s_hdma,
                       (uint32_t)(uintptr_t)s_src,
                       (uint32_t)(uintptr_t)s_dst,
                       DMA_BENCH_WORDS);
    g_dma_xfer_rc = (uint32_t)rc;
    if (rc != HAL_OK)
    {
        return 0u;
    }

    c0 = DWT->CYCCNT;
    rc = HAL_DMA_PollForTransfer(&s_hdma, HAL_DMA_FULL_TRANSFER, 100u);
    g_dma_cyc     = DWT->CYCCNT - c0;
    g_dma_poll_rc = (uint32_t)rc;

    __DSB();     /* 传输完成后，确保 DMA 的写对 CPU 可见（before cache maintenance）*/

    return (rc == HAL_OK) ? 1u : 0u;
}

/* ===================== DMA 初始化（首次触发时做一次）===================== */

static uint32_t dma_bench_init(void)
{
    if (s_ready != 0u)
    {
        return 1u;
    }

    /* 时钟：DMA1 在 AHB1(D2)；SRAM1 在 AHB2(D2)。
     * D2 SRAM 复位后默认使能，这里显式打开 —— 不依赖复位默认值，
     * 也避免以后有人改了 RCC 初始化后这里静默失效。 */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_D2SRAM1_CLK_ENABLE();

    s_hdma.Instance                 = DMA1_Stream0;
    s_hdma.Init.Request             = DMA_REQUEST_MEM2MEM;   /* MEM2MEM 时会被强制为 0 */
    s_hdma.Init.Direction           = DMA_MEMORY_TO_MEMORY;  /* ⚠ 不能用循环模式 */
    s_hdma.Init.PeriphInc           = DMA_PINC_ENABLE;
    s_hdma.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;   /* 32 位；缓冲区已 32 B 对齐 */
    s_hdma.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    s_hdma.Init.Mode                = DMA_NORMAL;
    s_hdma.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;  /* 直接模式，与 MEM2MEM 最简组合 */
    s_hdma.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    s_hdma.Init.MemBurst            = DMA_MBURST_SINGLE;
    s_hdma.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    {
        HAL_StatusTypeDef rc = HAL_DMA_Init(&s_hdma);
        g_dma_init_rc = (uint32_t)rc;
        if (rc != HAL_OK)
        {
            return 0u;
        }
    }

    s_ready = 1u;
    return 1u;
}

/* ===================== 主入口 ===================== */

void run_dma_bench(void)
{
    if (g_dma_test == 0u)
    {
        return;                      /* 常态零开销（一次比较）*/
    }
    g_dma_test = 0u;                 /* 先清，保证单次触发 */

    /* 先把两个缓冲区的 cache 行作废，让用例 A 的比对从内存取值（干净起点）。
     * 否则上一轮遗留的缓存内容可能让 A 假通过/假失败。 */
    SCB_InvalidateDCache_by_Addr((uint32_t *)s_src, (int32_t)sizeof(s_src));
    SCB_InvalidateDCache_by_Addr((uint32_t *)s_dst, (int32_t)sizeof(s_dst));
    __DSB();
    __ISB();

    g_dma_words      = DMA_BENCH_WORDS;
    g_dma_bytes      = DMA_BENCH_BYTES;
    g_dma_src_addr   = (uint32_t)(uintptr_t)s_src;
    g_dma_dst_addr   = (uint32_t)(uintptr_t)s_dst;
    g_dma_dcache_on  = ((SCB->CCR & SCB_CCR_DC_Msk) != 0u) ? 1u : 0u;
    g_dma_forcewt    = ((SCB->CACR & SCB_CACR_FORCEWT_Msk) != 0u) ? 1u : 0u;

    if (dma_bench_init() == 0u)
    {
        g_dma_case = 1u;             /* DMA 初始化失败：不去猜后面的结论 */
        return;
    }

    /* ---- 用例 A：方向① CPU 写 → DMA 读（预期失配 0）----
     * CPU 填 src，DMA 搬到 dst，CPU 读 dst 与预期图案比对。
     * 若 FORCEWT 真在起作用（CPU 写直达内存），DMA 读到的就是正确的 A 图案。 */
    fill_src(DMA_PAT_A);
    if (dma_copy() != 0u)
    {
        g_dma_a_mismatch = count_mismatch(DMA_PAT_A, &g_dma_a_fresh_lo, &g_dma_a_fresh_hi);
    }

    /* ---- 用例 B：方向② DMA 写 → CPU 读，**不做任何 cache 维护**（预期失配 = words）----
     * 顺序刻意设计成：先把 dst 读进 cache（prime），再让 DMA 改写内存，
     * 然后 CPU 复读 —— 此时 CPU 命中自己那份**旧副本**，读到的是 A 图案而非 B 图案。
     * 这就是"总线主设备写内存、CPU 读陈旧值"的静默数据损坏模型。 */
    prime_dst_cache();               /* 把 dst 的 A 图案读进 D-Cache */
    fill_src(DMA_PAT_B);
    if (dma_copy() != 0u)
    {
        g_dma_b_mismatch = count_mismatch(DMA_PAT_B, &g_dma_b_fresh_lo, &g_dma_b_fresh_hi);
    }

    /* ---- 用例 C：与 B 完全相同，但**加一次 invalidate**（预期失配 0）----
     * 这是 B 的对照实验：如果 C 也失败，说明问题不在 cache 而在我别处写错了；
     * 只有"B 失败 + C 通过"才能证明 invalidate 是有效补救。 */
    prime_dst_cache();
    fill_src(DMA_PAT_C);
    if (dma_copy() != 0u)
    {
        /* 标准补救动作：作废目标区缓存行（clean 不必要 —— CPU 没写过 dst），
         * 顺序 __DSB → invalidate → __DSB → __ISB，保证后续读一定取自内存。 */
        __DSB();
        SCB_InvalidateDCache_by_Addr((uint32_t *)s_dst, (int32_t)sizeof(s_dst));
        __DSB();
        __ISB();
        g_dma_c_mismatch = count_mismatch(DMA_PAT_C, &g_dma_c_fresh_lo, &g_dma_c_fresh_hi);
    }

    g_dma_case = 2u;                 /* 三个用例都跑完了 */
}
