/* ===========================================================================
 * YMGUI → STM32H743 的移植桥（显示端口）
 *
 * 库对硬件**唯一**的要求就是 GYdisp::flush_cb(d, area, buf)：
 * 把 buf 里 area 大小的连续像素推给面板。真实硬件在这里做并口/SPI 传输。
 *
 * 本文件的三个职责：
 *   1. 填 GYdisp（尺寸、draw buffer、flush_cb）；
 *   2. flush_cb 里用厂商 lcd.c 的 lcd_set_window + lcd_wr_data 把 band 推给 ST7796；
 *   3. 顺手把每条 band 累积进**全帧镜像缓冲**（AXI SRAM 起始 300 KB），
 *      这样用 SWD 读出来就能还原整屏 PNG 目视核对 —— 没有相机时这是唯一验收手段。
 *
 * 缓冲策略（2026-10-04 第 7a 项起）：**DMA 双缓冲**。
 *   buf1 在 AXI SRAM（board_alloc1），buf2 在 D2 域 SRAM3（链接脚本 .bss_sram3）。
 *   flush_cb 只负责"设窗口 + 起 DMA"，立即返回；DMA 传完后由 wait_cb（轮询 TC 标志）
 *   调 YMGUI_Disp_FlushReady 交还 buffer。于是 CPU 渲染下一条 band 与 DMA 推上一条
 *   **重叠**，一帧的耗时从 render+flush 变成 max(render, flush)。
 *
 *   ⚠ 旧注释说"FMC SRAM 模式没有传输完成信号，无法做异步重叠"——**那个前提是错的**：
 *     8080 异步写靠 FMC 用 HREADY 反压 AHB，DMA 的传输计数照样会走到 0，
 *     TC 标志照样会置起，它就是一个合法的"buffer 可复用"事件。不需要面板给任何信号。
 *     真要担心的只是"起 DMA 之前 CPU 的写有没有落到内存" —— FORCEWT=1 下写本来就
 *     直达，再用 DSB 把顺序语义显式化即可（tools/dma_check.py 已在同区域实测 0/64 失配）。
 *
 *   DMA 用 **MEM2MEM 软件触发**：FMC 没有 DMA 请求线，只能自己驱动。
 * =========================================================================== */

#include "ymgui_port.h"
#include "board_alloc.h"

#include "stm32h7xx_hal.h"
#include "./BSP/LCD/lcd.h"

#include <string.h>
#include <stdint.h>

/* 链接脚本提供：全帧镜像缓冲的边界（AXI SRAM 起始 300 KB） */
extern uint8_t _frame_buf[];
extern uint8_t _frame_buf_end[];

volatile unsigned g_flush_calls  = 0;
volatile unsigned g_flush_pixels = 0;
volatile unsigned g_frame_seq    = 0;

/* 真实 flush_cb 的耗时统计（2026-10-03 新增）。
 * 【为什么要有这个】g_flush_cyc 量的是 ymgui_port_measure_flush() 里那段"裸写循环"，
 * 它**不含**画面真正往屏上推的那条路径：少算了每 band 一次的 set_window、0x2C，
 * 以及全帧镜像 memcpy。只盯 g_flush_cyc 会低估真实成本 —— 两个量一起看才不会看走眼。
 * g_flush_cb_cyc 是累加值，除以 g_flush_cb_calls 得到单次均值。 */
volatile unsigned g_flush_cb_cyc   = 0;
volatile unsigned g_flush_cb_calls = 0;

/* ---- DMA 双缓冲（2026-10-04 第 7a 项）----
 * ⚠ 改造后 g_flush_cb_cyc 的含义变了：它只量"设窗口 + 起 DMA"这段（会掉到几微秒），
 *   **真正在等面板的时间挪到了 wait_cb**，记在 g_flush_wait_cyc。
 *   两个量要一起看，否则会读出"flush 几乎免费"这种假象。 */
volatile unsigned g_flush_wait_cyc   = 0;   /* wait_cb 里等 DMA 的累计周期 */
volatile unsigned g_flush_wait_calls = 0;

volatile unsigned g_lcd_dma_ok       = 0;   /* 1 = DMA 双缓冲已启用（0 = 退回单缓冲同步）*/
volatile unsigned g_lcd_dma_init_rc  = 0xFFu;/* HAL_DMA_Init 返回值（0 = HAL_OK）*/
volatile unsigned g_lcd_dma_poll_rc  = 0;   /* 最近一次等 DMA 的结局：1=等到 2=超时兜底 */
volatile unsigned g_lcd_dma_timeout  = 0;   /* 轮询超时次数（>0 = 这条通路不可靠）*/
volatile unsigned g_lcd_dma_reenable = 0;   /* 起新传输时上一笔还开着而被迫关流的次数（>0 = 上游契约被破坏）*/
volatile unsigned g_lcd_dma_waited   = 0;   /* flush_cb 开头真的等到上一笔的次数 = 重叠真实发生的次数 */
volatile unsigned g_lcd_dma_xfers    = 0;   /* 交给 DMA 的传输次数 */
volatile unsigned g_lcd_dma_pixels   = 0;   /* 交给 DMA 的像素总数 */
volatile unsigned g_lcd_dma_wait_enter = 0; /* wait_cb 被调用次数（含提前返回）——用来证伪"库没调它" */

/* ---- flush 通路开关（2026-10-04）----
 * 0 = CPU 直推        1 = DMA 双缓冲（**出厂默认**）
 *
 * 【为什么默认 DMA】同场景（桌面页）背靠背 A/B，三次连跑：
 *   CPU 直推  18.14 ms / 55.1 FPS （7 257 405 / 7 257 378 cyc —— 两次复测差 27 cyc）
 *   DMA 双缓冲 14.71 ms / 68.0 FPS （5 883 591 cyc）
 *   ⇒ **DMA 快 3.43 ms（−18.9%）**，且面板 GRAM 回读逐像素一致（gram = 0）。
 *   设置页场景同样更快（20.39 → 17.75 ms）。
 *
 * ⚠ 别重犯我踩过的两个测量错误：
 *   1. **跨场景比数字**：本板基准随当前页面变化（桌面 18.14 / 设置页 20.39 /
 *      笔记页历史 23.17）。A/B 必须同一页、背靠背跑。我第一版拿"设置页的 CPU 数字"
 *      去比"桌面页的旧基线 17.78"，得出了"没有收益"的**反结论**，差点因此把本项关掉。
 *   2. 只有**两次复测差 < 50 cyc** 才说明不是噪声（本板很干净，实测就是这量级）；
 *      单跑一次的数字不足以支撑结论。
 *
 * ⚠ 运行期从 1 切到 0 是安全的：flush_cb 开头那次 drain 不受本开关约束，
 *   所以即使切换瞬间有 DMA 在途，也会先等干净再碰 FMC。
 *
 * 【本项踩过的坑，别再犯】窗口命令必须在"等完上一笔 DMA"之后才发 —— 见 flush_cb
 *   开头"顺序要害"那段。命令与在途像素交错过一次，代价是整屏花且不自愈。 */
#ifndef YMGUI_PORT_LCD_DMA
#define YMGUI_PORT_LCD_DMA  1
#endif
volatile unsigned g_lcd_dma_enable = YMGUI_PORT_LCD_DMA;

/* 全帧镜像开关（2026-10-03 P0-2）--------------------------------------------
 *
 * 【镜像缓冲是干什么的】每条 band 推给面板之后，再往 AXI SRAM 里那份 320×480
 * 的副本拷一份。它**不是**显示链路的一部分 —— 唯一的用处是给两个诊断当参照物：
 *   · A2 的 gram_verify：整屏回读面板，与镜像逐像素比对（没有相机的情况下，
 *     这是"像素真的进了面板"的唯一硬证据）；
 *   · tools/grab_ymgui.py 抓屏还原 PNG 目视核对。
 * 代价是每帧 320×480×2 = 300 KB 的纯诊断拷贝。
 *
 * 【两级开关】
 *   · 编译期 YMGUI_PORT_FRAME_MIRROR=0 → 量产固件的最终形态（省掉这段拷贝）；
 *   · 运行时 g_frame_mirror（默认 1）→ 不重新编译就能量"关掉值多少毫秒"。
 *
 * ⚠ 关掉之后 g_gram_mismatch 会失去意义（面板与镜像必然不一致）。
 *   跑 A2 之前必须把 g_frame_mirror 写回 1，并让它跑满一帧让镜像追上。 */
#ifndef YMGUI_PORT_FRAME_MIRROR
#define YMGUI_PORT_FRAME_MIRROR  1
#endif
volatile unsigned g_frame_mirror = YMGUI_PORT_FRAME_MIRROR;

/* ---------------------------------------------------------------------------
 * 热点像素写：**必须内联**
 *
 * LCD_RAM 的物理地址 = 0x6000_0000 + 2^19 × 2 = 0x6010_0000（NE1=CS，A19=RS），
 * 推导见 third_party/BSP/LCD/lcd.h 顶部注释。
 *
 * 【为什么不能调 lcd_wr_data()】
 * 那个函数在 lcd.c 里定义、头文件只声明，是**跨编译单元调用**，-O2 也不会跨越 TU 内联；
 * 函数体里还有一句专门为 -O2 准备的 `data = data;` 人为延时。整屏 153600 次调用
 * ⇒ 每像素多付约 14 个周期 ⇒ 5.5 ms。整帧写入的主循环是全固件最热的一段，
 * 每一笔开销都会被乘以十几万倍。
 *
 * 【为什么去掉人为延时是安全的】
 * 总线侧的写脉宽（25 ns）是 **FMC 外设按 DATAST 寄存器自己拉出来的**，与 CPU 快慢无关；
 * CPU 写得太快时 FMC 会用 HREADY 反向压住 AHB，不会丢数据。真正的判据是
 * ramp / GRAM 逐像素自检 —— 每次改这里都必须复跑 A2（`g_ramp_mismatch` / `g_gram_mismatch` = 0）。 */
#define LCD_RAM_PX  ((volatile uint16_t *)0x60100000u)

static inline void lcd_wr_px_inline(uint16_t v)
{
    *LCD_RAM_PX = v;
}

static GYdisp s_disp;
static GYpx  *s_frame;

/* draw buffer 高度：320 × 32 × 2 B = 20 KB。
 * 库的 band 机制会按此切块；小一点只是 flush 次数多，正确性不受影响。
 * 正式定义在 ymgui_port.h（main.c 的 g_band_rows 实验钩子要拿它当上限）。 */
#ifndef DRAW_BAND_ROWS
#define DRAW_BAND_ROWS  32
#endif

/* ---------------------------------------------------------------------------
 * DMA 双缓冲（2026-10-04 第 7a 项）
 * ------------------------------------------------------------------------- */

/* 第二块 band buffer：D2 域 SRAM3（链接脚本 .bss_sram3，0x3004_0000 起 32 KB）。
 * 32 字节对齐是 DMA 与 cache line 的双重要求。 */
static GYpx s_band2[YMGUI_PORT_W * DRAW_BAND_ROWS]
    __attribute__((section(".bss_sram3"), aligned(32)));

/* 编译期挡住容量不一致：库按 buf_px_cnt 切 band，若 s_band2 比它小就会越界写。
 * DRAW_BAND_ROWS 调大到 48 还装得下（30720 B），64（40960 B）就超了 —— 那时这里报错。 */
#if ((YMGUI_PORT_W * DRAW_BAND_ROWS * 2) > (32 * 1024))
#error "SRAM3(32KB) 装不下第二块 band buffer：请调小 DRAW_BAND_ROWS，或把 s_band2 改放 SRAM1"
#endif

static DMA_HandleTypeDef s_lcd_dma;

/* CPU 直推：同步路径本体，以及"DMA 起不来"时的退路（宁可慢，也不能整块不刷）。
 * 写法与 lcd_wr_px_inline 一致 —— 这条循环是全固件最热的一段，不能退化成跨 TU 调用。 */
static void lcd_push_cpu(const GYpx *restrict buf, uint32_t n)
{
    volatile uint16_t *restrict ram = LCD_RAM_PX;
    const GYpx        *restrict src = buf;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        *ram = src[i];
    }
}

/* 轮询超时上限。一条 band 最多 20480 px × 31.8 ns ≈ 0.65 ms，
 * 给到 50 ms（@400 MHz = 2×10^7 周期）已是两个数量级余量；
 * 真撞到上限说明通路有问题，绝不能死等。用周期而不是毫秒，
 * 是为了不依赖 SysTick/HAL_GetTick（SWD 读变量会暂停内核，但那不影响这里）。 */
#define LCD_DMA_WAIT_TIMEOUT_CYC  (20000000u)

/* 初始化 DMA1_Stream1。**刻意避开 DMA1_Stream0** —— 那是 dma_bench 用的，
 * 两边共用一个流会互相踩（台架手动触发时正好可能和刷屏撞上）。
 * MEM2MEM 模式：HAL 会把请求线强制为 0、改由软件触发 —— FMC 没有 DMA 请求线，
 * 这是唯一可行的驱动方式。 */
/* ⚠⚠ 为什么**逐笔传输自己写寄存器**、而不是反复调 HAL_DMA_Start：
 *   本版 HAL 的 HAL_DMA_Start 成功分支**没有 __HAL_UNLOCK**（见
 *   third_party/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c L638-657：开头
 *   `__HAL_LOCK(hdma)`，成功只置 State=BUSY + 使能流就 return），于是**第一笔成功
 *   之后 Lock 永远卡在 HAL_LOCKED**，此后每次调用都在那个宏里 `return HAL_BUSY`。
 *   板级实测正是如此：g_lcd_dma_start_rc = 2（HAL_BUSY）、xfers 只有 1、
 *   其余 2683 笔全部 fallback 回 CPU 直推 —— 而且帧时间还变慢了（20.39 ms）。
 *   ⇒ 本函数只借 HAL_DMA_Init 把 CR 的位配好（一次性），
 *     之后每笔传输自己写 PAR/M0AR/NDTR + 置 EN、自己轮询 TC 标志。
 *     少一层隐式状态，行为完全确定，也不依赖"PollForTransfer 有没有被调到"。
 */
static int lcd_dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    memset(&s_lcd_dma, 0, sizeof(s_lcd_dma));
    s_lcd_dma.Instance                 = DMA1_Stream1;
    s_lcd_dma.Init.Request             = DMA_REQUEST_MEM2MEM;
    s_lcd_dma.Init.Direction           = DMA_MEMORY_TO_MEMORY;
    /* ⚠ 反直觉但正确：MEM2MEM 下 HAL 的 DMA_SetConfig 把 **SrcAddress 放进 PAR**、
     *   DstAddress 放进 M0AR（本文件起传输时也照此写）。我们要"源缓冲递增、
     *   LCD 寄存器固定"，所以管 PAR 的 PeriphInc 必须是 ENABLE、
     *   管 M0AR 的 MemInc 必须是 DISABLE。
     *   照"外设地址固定"的字面义去配 PINC=DISABLE，会把 LCD 寄存器当成递增源，全错。 */
    s_lcd_dma.Init.PeriphInc           = DMA_PINC_ENABLE;
    s_lcd_dma.Init.MemInc              = DMA_MINC_DISABLE;
    s_lcd_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;  /* GYpx = RGB565 = 16 位 */
    s_lcd_dma.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    s_lcd_dma.Init.Mode                = DMA_NORMAL;               /* MEM2MEM 不允许循环 */
    s_lcd_dma.Init.Priority            = DMA_PRIORITY_HIGH;
    s_lcd_dma.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;     /* 直接模式，最简组合 */
    s_lcd_dma.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    s_lcd_dma.Init.MemBurst            = DMA_MBURST_SINGLE;
    s_lcd_dma.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    g_lcd_dma_init_rc = (unsigned)HAL_DMA_Init(&s_lcd_dma);
    if (g_lcd_dma_init_rc != (unsigned)HAL_OK)
    {
        return -1;
    }
    /* HAL_DMA_Init 的句柄状态我们之后一概不用（不走 HAL 的 Start/Poll），
     * 这里显式解锁一次，免得将来有人误用 HAL 接口时踩到上面那个 Lock 坑。 */
    s_lcd_dma.Lock  = HAL_UNLOCKED;
    s_lcd_dma.State = HAL_DMA_STATE_READY;
    return 0;
}

/* 1 = 有一笔在途（buffer 不能复用）。 */
static volatile uint8_t s_dma_inflight;

/* stream1 在 LISR/LIFCR 里的位域：bit6..bit11（每个流 6 位）。
 * 用裸位而不是逐宏名，是因为错误标志宏名在头文件里叫法不一，写死位域少一层依赖。 */
#define LCD_DMA_LIFCR_S1    (0x3Fu << 6)
#define LCD_DMA_ERR_S1      (((uint32_t)1u << 6) | ((uint32_t)1u << 7) | ((uint32_t)1u << 8))

/* 起一笔传输：写源/目的/长度 → 清标志 → DSB → 使能。
 * 正常模式下 NDTR 归零时硬件自动清 EN，所以重入前不必手动关流。 */
static void lcd_dma_kick(const GYpx *buf, uint32_t n)
{
    DMA_Stream_TypeDef *s = (DMA_Stream_TypeDef *)s_lcd_dma.Instance;

    /* 上一笔若还开着（**不该发生**：库在复用 buffer 之前必须走过 wait_cb 把它等完），
     * 先关流再清标志 —— 否则下面写进去的 PAR/M0AR/NDTR 会和残留在途的传输打架。
     * 正常模式下 NDTR 归零时硬件已自动清 EN，所以这条分支在正常流程里永不进入。 */
    if ((s->CR & DMA_SxCR_EN) != 0u)
    {
        s->CR &= (uint32_t)~DMA_SxCR_EN;
        g_lcd_dma_reenable++;
    }

    s->PAR  = (uint32_t)(uintptr_t)buf;         /* MEM2MEM：PAR  = 源（递增）*/
    s->M0AR = (uint32_t)(uintptr_t)LCD_RAM_PX;  /* MEM2MEM：M0AR = 目的（固定）*/
    s->NDTR = n;
    DMA1->LIFCR = LCD_DMA_LIFCR_S1;             /* ⚠ 必须先清标志再使能，否则读到的 TC 是上一笔的 */
    __DSB();                                    /* 保证此前 CPU 对 buf 的写已到内存 */
    s->CR |= DMA_SxCR_EN;
    s_dma_inflight = 1u;
}

/* 查一笔传输是否结束：1=完成 0=在途 -1=出错。完成/出错都会清标志并落 s_dma_inflight。 */
static int lcd_dma_poll(void)
{
    if ((DMA1->LISR & DMA_LISR_TCIF1) != 0u)
    {
        DMA1->LIFCR    = DMA_LIFCR_CTCIF1;
        s_dma_inflight = 0u;
        return 1;
    }
    if ((DMA1->LISR & LCD_DMA_ERR_S1) != 0u)
    {
        DMA1->LIFCR    = LCD_DMA_ERR_S1;
        s_dma_inflight = 0u;
        return -1;
    }
    return 0;
}

/* 等当前这笔传完。返回 1 = 正常结束，0 = 超时兜底（已关流）。
 * 超时上限一条 band 最多 ~0.65 ms，撞到上限说明通路有问题，绝不能死等。 */
static int lcd_dma_drain(void)
{
    uint32_t t0 = DWT->CYCCNT;

    while (s_dma_inflight != 0u)
    {
        if (lcd_dma_poll() != 0)
        {
            return 1;               /* 完成或出错 */
        }
        if ((DWT->CYCCNT - t0) > LCD_DMA_WAIT_TIMEOUT_CYC)
        {
            g_lcd_dma_timeout++;
            ((DMA_Stream_TypeDef *)s_lcd_dma.Instance)->CR &= (uint32_t)~DMA_SxCR_EN;
            s_dma_inflight = 0u;
            return 0;
        }
    }
    return 1;
}

/* 库在 while(flush_busy) 里反复调本函数，直到 flush_busy 清零。
 * ⚠ 现在这条路**只是安全网**：正常流程下 flush_cb 里已经等过并交还了 buffer，
 *   flush_busy 一直是 0，库根本不会进这个循环（实测 g_lcd_dma_wait_enter = 0）。
 *   留着它是为了"万一库真的进了等待循环"时不会假死 —— 无论如何最后必须
 *   调 YMGUI_Disp_FlushReady，否则 flush_busy 永远为 1，库死循环在 waitFlushIdle。 */
static void lcd_dma_wait_cb(GYDISP d)
{
    uint32_t t0 = DWT->CYCCNT;

    g_lcd_dma_wait_enter++;         /* 诊断：本函数到底有没有被库调到 */

    if (d->flush_busy == 0u)
    {
        return;
    }

    (void)lcd_dma_drain();

    g_flush_wait_cyc   += (unsigned)(DWT->CYCCNT - t0);
    g_flush_wait_calls += 1;
    g_lcd_dma_poll_rc   = 1u;

    YMGUI_Disp_FlushReady(d);
}

/* ---------------------------------------------------------------------------
 * HAL flush 回调 —— 库与硬件之间唯一的连接点
 * ------------------------------------------------------------------------- */
static void lcd_flush_cb(GYdisp *d, const GYrect *area, const GYpx *buf)
{
    uint32_t n;
    uint32_t cb_start = DWT->CYCCNT;   /* 整段 flush_cb 的真实成本（含窗口命令与镜像） */

    /* 防御：宽高非正直接跳过（裁剪后可能出现空区域） */
    if (area->w <= 0 || area->h <= 0)
    {
        YMGUI_Disp_FlushReady(d);
        return;
    }

    /* ★★ 顺序要害：**任何 FMC 访问之前，必须先把上一笔 DMA 等完** ★★
     *
     * 窗口命令（0x2A/0x2B/0x2C）也是经 FMC 发给面板的。上一笔像素还在推时，这些命令
     * 就会**插进像素流里** —— 面板把命令当像素数据收（或把像素当命令收），
     * 结果是整条 band 写到错误位置 / 整屏花。而且它**不会自己恢复**：
     * YMGUI_Refresh 在无脏区时直接早返回，花掉的那一块会一直留在屏上，
     * 直到某次重绘恰好覆盖它。
     *
     * 【为什么两个自检都抓不到（2026-10-04 实际踩到）】
     *   · A2 的 gram 回读是**空闲态**跑的：那时每笔 DMA 早就传完，命令与像素不可能交错
     *     ⇒ 报 0/0；
     *   · 全帧镜像是在本函数里从**源 buffer** 拷的，反映"要画什么"、压根不经过面板
     *     ⇒ 抓屏看着完全正常。
     *   两个"看起来正常"叠在一起，正好把这类**传输层**错误完全藏住。
     *
     * 【第一版就是这么错的】原来把等待放在**起 DMA 之前**（见下面 kick 处），
     * 那只解决了"下一块 buffer 覆盖在传的 buffer"，没管住"窗口命令与在传像素交错"。
     * 现在统一提到最前面：进本函数先等干净，再碰 FMC。 */
    if (s_dma_inflight != 0u)
    {
        (void)lcd_dma_drain();
        g_lcd_dma_waited++;         /* 诊断：真正等到上一笔的次数（= 重叠真实发生的次数）*/
    }

    /* 1) 推给面板：设窗口 → 连续写 GRAM。
     *    lcd_set_window 用的是面板自己记着的 setxcmd/setycmd（ST7796 为 0x2A/0x2B），
     *    末尾已自动写 0x2C 进入写 GRAM 状态。 */
    lcd_set_window((uint16_t)area->x, (uint16_t)area->y,
                   (uint16_t)area->w, (uint16_t)area->h);
    /* ⚠ 这一句不能省：厂商的 lcd_set_window **只发 0x2A/0x2B 设窗口范围，不发 0x2C**。
     * 少了它，数据进不了 GRAM —— 现象是"写进去的东西读不回来"（ramp 自检整屏失配），
     * 而 RAM 里的镜像缓冲看起来完全正常，极易被误判成"读通路坏了"。 */
    lcd_write_ram_prepare();

    /* 逐像素推给面板。这里用内联写而不是 lcd_wr_data()，原因见上方 LCD_RAM_PX 的注释。
     * GYpx 就是 uint16_t（RGB565），与 16 位 FMC 总线一一对应，不存在格式转换。
     *
     * 两条路：
     *   d->buf2 != NULL（库的 dbl 判据，与本文件 s_disp.buf2 严格一致）⇒ 异步 DMA：
     *      发起即返回，FlushReady 交给 lcd_dma_wait_cb。⚠ **绝不能**在这里自己调
     *      FlushReady —— 那等于提前宣告 buffer 空闲，库会在下一条 band 覆盖正在传的那块。
     *   否则 ⇒ 同步 CPU 直推，传完再 FlushReady（原路径，逐像素写）。 */
    n = (uint32_t)area->w * (uint32_t)area->h;
    if ((g_lcd_dma_enable != 0u) && (d->buf2 != NULL))
    {
        /* 异步 DMA：**等待与交还都自己做，不依赖库的 wait_cb**。
         *
         * 【为什么要这样】本工程的 flush 链路上还插着 app 的一层包装：
         *   phone_shell.c 的 PhoneShell_BoardInit 把 disp->flush_cb 换成了
         *   它自己的 phone_flush（软件调暗 + 缩略快照），port 的 lcd_flush_cb 被存在
         *   phone_flush 的 display_flush 里转发调用。于是"谁在什么时候等 DMA"就变得
         *   不确定了：实测 g_lcd_dma_wait_enter = 0（库的 wait_cb 从未被调到）。
         *
         * 【等待已经在函数开头做过】见上面"顺序要害"那段 —— 这里**不再等**：
         *   一是等过之后才碰的 FMC（窗口命令），二是起传输本身不需要额外的等待。
         *
         * 【为什么仍然有重叠】CPU 渲染下一条 band 发生在 **flush_cb 之前**：
         *   引擎先 drawObjRec 填好整块，再调 flush_cb。所以"渲染 N+1"与"DMA 推 N"
         *   天然就是重叠的；函数开头那次等待只在 DMA 比渲染慢时才真的花时间。 */
        lcd_dma_kick(buf, n);
        g_lcd_dma_xfers++;
        g_lcd_dma_pixels += n;
    }
    else
    {
        /* CPU 直推 —— **备用通路**：`g_lcd_dma_enable = 0`、或 DMA 初始化失败
         * （`g_lcd_dma_ok = 0`）时走这里。实测比 DMA 慢约 3.4 ms（见开关处注释），
         * 但它不依赖任何 DMA 状态机，是"至少能出画面"的兜底。 */
        lcd_push_cpu(buf, n);
    }

    /* ⚠ 交还 buffer 统一放在这里、两条路共用 —— 分开写的话，以后改某一条分支时
     *   很容易漏掉一处，而漏掉的后果是库死循环在 while(flush_busy) 里
     *   （现象是整机假死，不是画面错，排查方向会完全跑偏）。 */
    YMGUI_Disp_FlushReady(d);

    /* 2) 镜像进全帧缓冲，供 SWD 抓屏还原 PNG。
     *    band 像素在 buf 里是紧密排列的（pitch = w），而全帧是 320 宽，要逐行拷。
     *    开关语义见上方 g_frame_mirror 的注释（A2 与抓屏需要它开着）。 */
    if (g_frame_mirror != 0u && s_frame != NULL &&
        area->x >= 0 && area->y >= 0 &&
        (area->x + area->w) <= YMGUI_PORT_W &&
        (area->y + area->h) <= YMGUI_PORT_H)
    {
        int row;
        for (row = 0; row < area->h; row++)
        {
            GYpx       *dst = s_frame + (size_t)(area->y + row) * YMGUI_PORT_W + area->x;
            const GYpx *src = buf + (size_t)row * area->w;
            memcpy(dst, src, (size_t)area->w * sizeof(GYpx));
        }
    }

    g_flush_calls++;
    g_flush_pixels += n;

    g_flush_cb_cyc   += (unsigned)(DWT->CYCCNT - cb_start);
    g_flush_cb_calls += 1;

    /* 3) 同步 port：传输完成后通知库"这块 buffer 可以复用了"。
     *    少这一句，库会一直以为 buffer 还在传，永远不再刷下一块。 */
    YMGUI_Disp_FlushReady(d);
}

/* 一轮 Refresh 的所有 band 都推完后触发；这里只用于统计整帧数。 */
static void lcd_frame_done_cb(GYDISP d)
{
    (void)d;
    g_frame_seq++;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */
int ymgui_port_init(void)
{
    /* 全帧镜像缓冲：AXI SRAM 起始 300 KB（320 × 480 × 2 = 307200 B）。
     * 清一遍，避免未初始化时抓屏出现随机噪声被误读成"渲染花了"。 */
    s_frame = (GYpx *)_frame_buf;
    if ((uint8_t *)s_frame + (YMGUI_PORT_W * YMGUI_PORT_H * sizeof(GYpx)) != _frame_buf_end)
    {
        /* 链接脚本的 _frame_buf 区间必须正好装得下一整屏，否则后面算偏移会越界 */
        return -2;
    }
    memset(s_frame, 0, YMGUI_PORT_W * YMGUI_PORT_H * sizeof(GYpx));

    memset(&s_disp, 0, sizeof(s_disp));
    s_disp.hor_res    = YMGUI_PORT_W;
    s_disp.ver_res    = YMGUI_PORT_H;
    s_disp.buf_px_cnt = YMGUI_PORT_W * DRAW_BAND_ROWS;
    s_disp.buf1       = (GYpx *)board_alloc1((size_t)s_disp.buf_px_cnt * sizeof(GYpx));
    s_disp.buf2       = NULL;              /* 下面 DMA 初始化成功才置 s_band2 */
    s_disp.flush_cb   = lcd_flush_cb;
    s_disp.wait_cb    = NULL;
    s_disp.user_data  = NULL;

    if (s_disp.buf1 == NULL)
    {
        return -1;
    }

    /* DMA 双缓冲：初始化失败就**退回单缓冲同步路径**（显示照常，只是没有重叠）。
     * 与全工程"任何一步失败都不能把板子卡住"一致 —— 失败只记进诊断量，不返回错误。 */
    if (lcd_dma_init() == 0)
    {
        s_disp.buf2    = s_band2;          /* 库见到它就切到异步 dbl 路径 */
        s_disp.wait_cb = lcd_dma_wait_cb;
        g_lcd_dma_ok   = 1u;
    }

    YMGUI_Disp_SetFrameDoneCb(&s_disp, lcd_frame_done_cb);
    return 0;
}

GYDISP ymgui_port_disp(void)
{
    return &s_disp;
}

GYpx *ymgui_port_frame(void)
{
    return s_frame;
}

/* ---------------------------------------------------------------------------
 * A1 验收测量（计划书 §0.2：整屏 flush 净耗时 ≤ 53 ms，即 ≥ 19 FPS）
 *
 * 与阶段 0 的测法同源：把镜像缓冲整屏推一遍，用 DWT 周期计数计时。
 * 走的是与 flush_cb 完全相同的写路径（lcd_set_window → prepare → lcd_wr_data），
 * 所以测的就是真实显示管线的净能力。
 * ------------------------------------------------------------------------- */
volatile uint32_t g_flush_cyc  = 0;    /* 整屏推送的 DWT 周期数（400 MHz） */
volatile uint32_t g_flush_nspx = 0;    /* 每像素 ns × 100（便于看小数） */

void ymgui_port_measure_flush(void)
{
    uint32_t n = (uint32_t)YMGUI_PORT_W * (uint32_t)YMGUI_PORT_H;
    uint32_t c0;
    uint32_t c1;
    uint32_t i;

    if (s_frame == NULL)
    {
        return;
    }

    c0 = DWT->CYCCNT;
    lcd_set_window(0, 0, (uint16_t)YMGUI_PORT_W, (uint16_t)YMGUI_PORT_H);
    lcd_write_ram_prepare();
    /* 与 flush_cb 走同一条内联写路径 —— 测量口径必须和真实路径一致，
     * 否则"测量值很好看、实际很慢"会让人得出错误结论。 */
    {
        volatile uint16_t *restrict ram = LCD_RAM_PX;
        const GYpx        *restrict src = s_frame;
        for (i = 0; i < n; i++)
        {
            *ram = src[i];
        }
    }
    c1 = DWT->CYCCNT;

    g_flush_cyc  = c1 - c0;
    /* ns/px × 100 = cyc × 1e8 / 400MHz / n = cyc × 100000000 / (400000000/1000 × n)
     * 化简：= cyc × 1000 / n × ... 直接按 (cyc * 250) / n / 100 → 保守起见用 64 位算 */
    g_flush_nspx = (uint32_t)(((uint64_t)g_flush_cyc * 100000000u) /
                              ((uint64_t)400000u * n));
}

volatile uint32_t g_gram_mismatch  = 0xFFFFFFFFu;
volatile uint32_t g_gram_first_bad = 0;

void ymgui_port_gram_verify(void)
{
    uint32_t i;
    uint32_t n     = (uint32_t)YMGUI_PORT_W * (uint32_t)YMGUI_PORT_H;
    uint32_t mism  = 0;
    uint32_t first = 0;

    if (s_frame == NULL)
    {
        return;
    }

    /* 整屏设窗口 → 发 0x2E 进读模式 → **哑读 1 拍** → 连续读 n 个像素。
     * 哑读这一拍不能省：RGB565 + 16 位并口时控制器第一个字无效
     * （这个坑在早期 Rust 参照工程上踩过；本工程把它固化成哑读一拍）。 */
    lcd_set_window(0, 0, (uint16_t)YMGUI_PORT_W, (uint16_t)YMGUI_PORT_H);
    lcd_wr_regno(0x2E);
    (void)LCD->LCD_RAM;

    for (i = 0; i < n; i++)
    {
        uint16_t v = (uint16_t)LCD->LCD_RAM;
        if (v != s_frame[i])
        {
            if (mism == 0)
            {
                first = i;
            }
            mism++;
        }
    }

    g_gram_mismatch  = mism;
    g_gram_first_bad = first;
}

volatile uint32_t g_ramp_mismatch = 0xFFFFFFFFu;

void ymgui_port_ramp_selftest(void)
{
    uint32_t i;
    uint32_t n    = (uint32_t)YMGUI_PORT_W * (uint32_t)YMGUI_PORT_H;
    uint32_t mism = 0;

    /* ① 整屏写已知图案「像素 i = i & 0xFFFF」 */
    lcd_set_window(0, 0, (uint16_t)YMGUI_PORT_W, (uint16_t)YMGUI_PORT_H);
    lcd_write_ram_prepare();        /* 0x2C：进写 GRAM 模式（厂商 set_window 不含此步） */
    for (i = 0; i < n; i++)
    {
        lcd_wr_data((uint16_t)i);
    }

    /* ② 复位读指针后整屏读回比对 */
    lcd_set_window(0, 0, (uint16_t)YMGUI_PORT_W, (uint16_t)YMGUI_PORT_H);
    lcd_wr_regno(0x2E);
    (void)LCD->LCD_RAM;              /* 哑读 1 拍 */
    for (i = 0; i < n; i++)
    {
        uint16_t v = (uint16_t)LCD->LCD_RAM;
        if (v != (uint16_t)i)
        {
            mism++;
        }
    }
    g_ramp_mismatch = mism;

    /* ③ 恢复面板内容（把镜像缓冲重推一遍），免得本自检破坏后面的比对 */
    if (s_frame != NULL)
    {
        lcd_set_window(0, 0, (uint16_t)YMGUI_PORT_W, (uint16_t)YMGUI_PORT_H);
        lcd_write_ram_prepare();
        for (i = 0; i < n; i++)
        {
            lcd_wr_data(s_frame[i]);
        }
    }
}

/* ===========================================================================
 * 背光硬件 PWM（PB1 = TIM3_CH4 / AF2）—— 步骤 2a 的硬件版，实验性
 *
 * 【要解决什么】软件调暗（phone_shell.c 里 `phone_flush` 叠一层全屏黑罩）现在
 * 仍值 **3.65 ms/帧**（实测 21.42 ms → 关掉调暗 17.77 ms，46.7 → 56.3 FPS）。
 * 背光是 PB1 直连的普通 GPIO、全工程没有任何 TIM/PWM，所以"亮度"一直是靠 CPU
 * 逐像素叠黑换来的。把 PB1 出成硬件 PWM，这笔开销就直接变成 0。
 *
 * 【已经查实的部分】
 *   · **PB1 = TIM3_CH4，复用功能 AF2** —— ST 官方 AF 表（MicroPython 的
 *     ports/stm32/boards/stm32h743_af.csv 是机器可读的同源表）：
 *     `PortB,PB1 , ,TIM1_CH3N ,TIM3_CH4 ,TIM8_CH3N ...` ⇒ AF1=TIM1_CH3N,
 *     **AF2=TIM3_CH4**, AF3=TIM8_CH3N。
 *   · **TIM3 在本工程里完全没被占用**（全仓库搜过：src/ 与 phone_shell/ 都没有
 *     任何 TIM3 使用；delay 走的是 SysTick，不占定时器）。TIM3 在 APB1。
 *   · **不需要引 HAL_TIM**：CMSIS 设备头里 TIM_CCMR2_OC4M_* / TIM_CCER_CC4E_* /
 *     TIM_CR1_CEN / TIM_EGR_UG 等位定义齐全，直接写寄存器即可 ——
 *     既不新增 HAL 模块（省 30~40 KB flash）也不用改 CMakeLists。
 *
 * 【查不实、只能上板试的部分】**背光驱动电路不在仓库里**（docs/ 下四份验收
 * 文档、BSP 注释、qspi_port.h 的引脚表都只有"PB1 = LCD_BL、置高点亮"，
 * 没有任何原理图/驱动方式说明）。所以存在两种可能：
 *   （a）PB1 驱动 MOSFET/三极管/驱动 IC 的 PWM 输入 ⇒ 改占空比就能调亮度，本方案成立；
 *   （b）PB1 是某颗升压/背光芯片的**使能脚** ⇒ PWM 可能表现为闪烁、非线性，或干脆没反应。
 * 本模块就是用来回答这个的，因此一切以"可逆"为前提：
 *   · **默认 mode = 0**：完全不碰 PB1，保持厂商的 GPIO 常亮行为（也是最安全的回退态）；
 *   · 只有 SWD 写 `g_bl_pwm_mode = 1` 才接管；写回 0 立刻恢复全亮；
 *   · **复位后一律回到 mode = 0**（`lcd_init` 会把 PB1 重新配成推挽输出并拉高），
 *     所以任何情况下 `probe-rs reset` 都能把背光恢复到全亮。
 *
 * 【怎么用】
 *   g_bl_pwm_mode  : 0 = GPIO 常亮（默认）   1 = TIM3_CH4 PWM
 *   g_bl_pwm_duty  : 千分比 0..1000（mode=1 时生效）
 *   g_bl_pwm_hz    : 回读：实际 PWM 频率（0 = 当前没在 PWM）
 *   g_bl_pwm_rc    : 0 = 最近一次配置成功；非 0 = 时钟没算对，已自动回退 GPIO
 *   回读寄存器快照（用于从主机侧确认"PB1 真的归 TIM3 了"）：
 *   g_bl_pin_moder / g_bl_pin_afr / g_bl_tim_ccer / g_bl_tim_ccr4 / g_bl_tim_arr / g_bl_tim_psc
 *
 * 【判据】duty = 0 时面板应当**完全黑掉**（背光灭）—— 这是"PWM 真的在控制背光"
 * 最干脆的单一证据；duty = 1000 应与原来的常亮一模一样。两者都不是，说明是 (b)。
 * =========================================================================== */
volatile uint32_t g_bl_pwm_mode = 0;      /* 0 = GPIO 常亮（默认） 1 = PWM */
volatile uint32_t g_bl_pwm_duty = 1000;   /* 千分比 0..1000 */
volatile uint32_t g_bl_pwm_hz   = 0;      /* 回读：实际频率 Hz */
volatile uint32_t g_bl_pwm_rc   = 0;      /* 回读：0 成功 */
/* 寄存器快照（每拍刷新，供主机侧核对 PB1 的归属） */
volatile uint32_t g_bl_pin_moder = 0;
volatile uint32_t g_bl_pin_afr   = 0;
volatile uint32_t g_bl_tim_ccer  = 0;
volatile uint32_t g_bl_tim_ccr4  = 0;
volatile uint32_t g_bl_tim_arr   = 0;
volatile uint32_t g_bl_tim_psc   = 0;

static uint32_t s_bl_mode_applied = 0xFFFFFFFFu;
static uint32_t s_bl_duty_applied = 0xFFFFFFFFu;
static uint32_t s_bl_active       = 0u;      /* 1 = PB1 当前归 TIM3 驱动 */

/* APB1 上的定时器时钟。ST 的通用规则：APB 预分频 ≠ 1 时定时器时钟 = PCLK × 2。
 * 本板 sys.c 是 400MHz 主频，APB1 一般是 100MHz ⇒ 定时器 200MHz。 */
static uint32_t bl_tim3_clk_hz(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1_Msk) != 0u)
    {
        return pclk1 * 2u;
    }
    return pclk1;
}

/* 配好并**启动** TIM3_CH4（占空比一并设好），最后才把 PB1 切给 TIM3。
 * 返回 0 = 成功；-1 = 时钟算不出来，此时一个寄存器都没动过。
 *
 * ★顺序是关键★ 必须在 PB1 还是 GPIO 的时候，就把定时器配好、占空比设好、
 * 输出使能、计数器启动；**最后一步**才切 PB1 的复用。
 * 反过来的话，PB1 会有一小段时间"已经是 AF 但定时器还没在驱动它" ——
 * 那个窗口里引脚没有确定电平，背光会闪一下（甚至短暂灭掉）。
 * 按这个顺序，PB1 一接上 TIM3 就已经是完整正确的 PWM 波形。 */
static int bl_pwm_hw_config(uint32_t duty)
{
    GPIO_InitTypeDef gi;
    uint32_t timclk = bl_tim3_clk_hz();
    uint32_t psc, arr;

    if (timclk < 100000u)                 /* 明显不对就别碰硬件 */
    {
        return -1;
    }

    /* 目标约 2 kHz：先把计数时钟压到 1 MHz，再取 500 个计数一周期。
     * 2 kHz 的理由：远高于可见闪烁频率，又远低于 MOSFET/驱动器的开关上限；
     * 太低（几百 Hz）有些背光驱动会响或抖。psc 是 16 位寄存器，要夹住。 */
    psc = timclk / 1000000u;
    if (psc == 0u)     psc = 1u;
    if (psc > 65536u)  psc = 65536u;
    psc -= 1u;
    arr = 500u - 1u;

    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* 1) 先把定时器彻底停掉、通道输出关掉（此时 PB1 还是 GPIO，动这些不影响它） */
    TIM3->CR1  = 0u;
    TIM3->CCER = 0u;

    /* 2) 定时器参数 + 占空比 */
    TIM3->PSC  = psc;
    TIM3->ARR  = arr;
    TIM3->CCR4 = (uint32_t)(((arr + 1u) * duty) / 1000u);
    /* CH4 配 PWM 模式 1（CNT < CCR4 时输出有效）+ 预装载。
     * CCMR2 低字节归 CH3，这里只重写高字节，不碰别的通道。 */
    TIM3->CCMR2 = (TIM3->CCMR2 & 0x00FFu)
                | (uint32_t)(0x6u << TIM_CCMR2_OC4M_Pos)
                | TIM_CCMR2_OC4PE;
    TIM3->EGR   = TIM_EGR_UG;             /* 立刻把 PSC/ARR 装进影子寄存器 */
    TIM3->SR    = 0u;                     /* 清掉 UG 带出来的标志 */
    TIM3->CR1   = TIM_CR1_ARPE;

    /* 3) 输出使能 + 启动计数（仍在 PB1 是 GPIO 的状态下，波形只在定时器内部） */
    TIM3->CCER |= TIM_CCER_CC4E;
    TIM3->CR1  |= TIM_CR1_CEN;

    /* 4) **最后**把 PB1 交给 TIM3 —— 接上的瞬间波形已经是正确的 */
    gi.Pin       = GPIO_PIN_1;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_LOW;
    gi.Alternate = GPIO_AF2_TIM3;         /* PB1 = TIM3_CH4 */
    HAL_GPIO_Init(GPIOB, &gi);

    g_bl_pwm_hz = timclk / ((psc + 1u) * (arr + 1u));
    return 0;
}

/* 把 PB1 还给 GPIO 常亮（全亮）—— 最安全的回退态，与厂商行为一致。
 * ★顺序同样是关键★：**先把引脚切回 GPIO（ODR 已预置高），再停定时器**。
 * 反过来会留下"AF 但没有驱动"的空窗，背光闪一下。 */
static void bl_pwm_hw_restore(void)
{
    GPIO_InitTypeDef gi;

    /* 1) 先备好 ODR = 高，这样切过去的瞬间就是全亮 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);

    /* 2) 把引脚从 TIM3 手里收回，改成推挽输出 */
    gi.Pin   = GPIO_PIN_1;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gi);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);

    /* 3) 引脚已经不归定时器了，这时再关输出/停表完全无害 */
    TIM3->CCER &= ~TIM_CCER_CC4E;
    TIM3->CR1  &= ~TIM_CR1_CEN;

    g_bl_pwm_hz = 0u;
    s_bl_active = 0u;
}

/**
 * @brief 背光 PWM 每拍调用：g_bl_pwm_mode / g_bl_pwm_duty 变了才动作
 *        （没变时只花两次比较，可以放心每拍调）
 */
void bl_pwm_tick(void)
{
    uint32_t mode = g_bl_pwm_mode;
    uint32_t duty = g_bl_pwm_duty;

    if ((mode != s_bl_mode_applied) || (duty != s_bl_duty_applied))
    {
        s_bl_mode_applied = mode;
        s_bl_duty_applied = duty;

        if (mode == 0u)
        {
            bl_pwm_hw_restore();
            g_bl_pwm_mode = 0u;
            g_bl_pwm_rc   = 0u;
        }
        else
        {
            if (duty > 1000u)
            {
                duty = 1000u;
            }

            if (s_bl_active == 0u)
            {
                if (bl_pwm_hw_config(duty) != 0)
                {
                    /* 时钟算不出来：一个寄存器都没动，把 mode 打回 0，原因留在 rc */
                    g_bl_pwm_rc       = 1u;
                    g_bl_pwm_mode     = 0u;
                    s_bl_mode_applied = 0u;
                }
                else
                {
                    s_bl_active = 1u;
                    g_bl_pwm_rc = 0u;
                }
            }
            else
            {
                /* 只改占空比，不重启定时器。OC4PE 预装载保证新值在下一个更新事件
                 * 才生效，不会切出半个脉冲（2 kHz 下最坏等 0.5 ms，看不见）。 */
                TIM3->CCR4  = (((uint32_t)TIM3->ARR + 1u) * duty) / 1000u;
                g_bl_pwm_rc = 0u;
            }
        }
    }

    /* 寄存器快照：每拍刷新（6 次寄存器读，成本可忽略），
     * 让主机侧能直接确认 PB1 到底归谁、PWM 参数是什么，不必靠"看屏幕亮不亮"。 */
    g_bl_pin_moder = (GPIOB->MODER >> 2u) & 0x3u;   /* PB1 占 MODER 的 bit2:3 */
    g_bl_pin_afr   = (GPIOB->AFR[0] >> 4u) & 0xFu;  /* PB1 占 AFR[0] 的 bit4:7（不是 bit0:3！） */
    g_bl_tim_ccer  = TIM3->CCER;
    g_bl_tim_ccr4  = TIM3->CCR4;
    g_bl_tim_arr   = TIM3->ARR;
    g_bl_tim_psc   = TIM3->PSC;
}

