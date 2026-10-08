/* ===========================================================================
 * TF 卡 / SDMMC1 驱动 + 写读比对自检（详细设计理由见 sd_card.h 顶部）
 *
 * 一句话：本文件只回答一个整数问题 —— "写进去的 N 个字节，读回来有 N 个相同吗"。
 *
 * 【第二轮改动（2026-10-08）】第一次跑四项全过、第二次跑却出现
 * "用例② 多块读返回非 OK、恢复也跟着失败"，即一次操作失败后卡/外设进入了坏状态，
 * 且卡上那 8 个扇区被写成图案没还原回来。为定位它，本轮加了三样东西：
 *   1. **寄存器级证据**：HAL 返回码 / ErrorCode / 失败瞬间的 SDMMC1->STA 全部导出；
 *   2. **硬件流控**：HardwareFlowControl = ENABLE —— FIFO 满/空时 SDMMC 自动暂停时钟，
 *      这是轮询模式（CPU 搬 FIFO，期间还会被 SysTick 打断）防上/下溢的标准做法；
 *   3. **失败重试 + 重新初始化**，并把重试次数导出（g_sd_retry）。
 * 另外补了模式 2/3：把备份内容写回卡，用于"自检失败导致卡内容被改"后的**还原**。
 * =========================================================================== */

#include "sd_card.h"
#include "stm32h7xx_hal.h"
#include <string.h>

/* ---- 诊断量（DTCM 的 .bss；SWD 直读，不受 D-Cache 影响）---- */
volatile uint32_t g_sd_test        = 0;
volatile uint32_t g_sd_state       = 0;
volatile uint32_t g_sd_init_rc     = 0xFFFFFFFFu;
volatile uint32_t g_sd_card_type   = 0;
volatile uint32_t g_sd_block_nbr   = 0;
volatile uint32_t g_sd_block_size  = 0;
volatile uint32_t g_sd_cap_mb      = 0;
volatile uint32_t g_sd_clk_div     = 0;
volatile uint32_t g_sd_sector      = 0;
volatile uint32_t g_sd_bak_rc      = 0xFFFFFFFFu;
volatile uint32_t g_sd_write_rc    = 0xFFFFFFFFu;
volatile uint32_t g_sd_read_rc     = 0xFFFFFFFFu;
volatile uint32_t g_sd_mis1        = 0xFFFFFFFFu;
volatile uint32_t g_sd_mis2        = 0xFFFFFFFFu;
volatile uint32_t g_sd_restore_rc  = 0xFFFFFFFFu;
volatile uint32_t g_sd_restore_mis = 0xFFFFFFFFu;
volatile uint32_t g_sd_wr_cyc      = 0;
volatile uint32_t g_sd_rd_cyc      = 0;
volatile uint32_t g_sd_buf_addr    = 0;
volatile uint32_t g_sd_card_state  = 0xFFFFFFFFu;

volatile uint32_t g_sd_wr_addr     = 0;
volatile uint32_t g_sd_rd_addr     = 0;
volatile uint32_t g_sd_bak_addr    = 0;
volatile uint32_t g_sd_hal_rc      = 0xFFFFFFFFu;
volatile uint32_t g_sd_errcode     = 0xFFFFFFFFu;
volatile uint32_t g_sd_sta         = 0;
volatile uint32_t g_sd_retry       = 0;
volatile uint32_t g_sd_step        = 0;
volatile uint32_t g_sd_hwfc        = 0;

volatile uint32_t g_sd_dma_wr_cyc    = 0;
volatile uint32_t g_sd_dma_rd_cyc    = 0;
volatile uint32_t g_sd_dma_mis       = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_stale     = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_stale_base = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_hal_rc    = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_sta       = 0;
volatile uint32_t g_sd_dma_errcode   = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_irq       = 0;
volatile uint32_t g_sd_dma_wrcplt    = 0;
volatile uint32_t g_sd_dma_rdcplt    = 0;
volatile uint32_t g_sd_dma_errcb     = 0;
volatile uint32_t g_sd_speed_mode    = 0;
volatile uint32_t g_sd_dma_buf_addr  = 0;
volatile uint32_t g_sd_dma_clkdiv    = 0;

volatile uint32_t g_sd_dma_snap_pre[SD_DMA_SNAP_N]  = {0};
volatile uint32_t g_sd_dma_snap_post[SD_DMA_SNAP_N] = {0};
volatile uint32_t g_sd_dma_w_rc      = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_r_rc      = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_w_errcode = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_r_errcode = 0xFFFFFFFFu;
volatile uint32_t g_sd_dma_w_sta     = 0;
volatile uint32_t g_sd_dma_r_sta     = 0;

/* IDMA 完成信号：中断回调里置 1，主循环里等它。
 * 为什么不用 hsd->State 轮询替代：State 由 HAL_SD_IRQHandler 收尾时改，
 * 用它也行，但加一个自己的标志能让"中断到底进没进"这件事**单独可见**
 * （g_sd_dma_irq 计数 + 回调计数三者互证，缺一个就说明链路断了）。 */
static volatile uint32_t s_dma_done    = 0;
static volatile uint32_t s_dma_is_err  = 0;

#define DMA_SENTINEL  0x5Au             /* 哨兵：见 sd_card.h 里 g_sd_dma_stale 的说明 */

/* 步骤编号：失败时看 g_sd_step 就知道卡在哪一步 */
#define STEP_IDLE       0u
#define STEP_BAK_READ   1u
#define STEP_1_WRITE    2u
#define STEP_1_READ     3u
#define STEP_8_WRITE    4u
#define STEP_8_READ     5u
#define STEP_RESTORE_W  6u
#define STEP_RESTORE_R  7u
#define STEP_DMA_WRITE  8u
#define STEP_DMA_READ   9u

/* ---- 缓冲区 ----
 * 【为什么读写缓冲必须在 AXI SRAM（.bss_sd_dma），而备份可以在 SRAM1】
 * 上板实测（2026-10-08）：把 IDMA 缓冲放 SRAM1 时，传输**一个字节都没搬动** ——
 *   写：ErrorCode=0x10 TX_UNDERRUN   读：ErrorCode=0x20 RX_OVERRUN
 *   IDMACTRL=1、IDMABASE0=0x30001200（地址正确）、DCOUNT 只走了 28/4096 字节。
 * 根因（ST AN5200；NuttX 的 stm32_sdmmc.c 引用同一条）：
 *   **SDMMC1（D1 域）的 IDMA 只能访问 D1 域内存 = AXI SRAM；
 *     SRAM1/2/3 在 D2 域、SRAM4 在 D3 域，它都到不了。SDMMC2（D2 域）才支持 SRAM1/2/3。**
 * 本板卡座硬件锁死在 SDMMC1 引脚（PC8~11/PC12/PD2），换不到 SDMMC2 ⇒ 只能搬缓冲。
 *
 * s_bak 只参与"备份/恢复"，这两步走**轮询**（CPU 搬 FIFO，不经 IDMA），
 * 所以留在 SRAM1 完全没问题，也省下 4 KB 的 AXI 空间。 */
#define SD_SECTOR_BYTES  512u
__attribute__((section(".bss_sd_dma"), aligned(32)))
static uint8_t s_wr[SD_BENCH_NSEC * SD_SECTOR_BYTES];   /* 写图案（IDMA 要读它 ⇒ 必须 AXI） */
__attribute__((section(".bss_sd_dma"), aligned(32)))
static uint8_t s_rd[SD_BENCH_NSEC * SD_SECTOR_BYTES];   /* 读回（IDMA 要写它 ⇒ 必须 AXI） */
__attribute__((section(".bss_sd"), aligned(32)))
static uint8_t s_bak[SD_BENCH_NSEC * SD_SECTOR_BYTES];  /* 原内容备份（只走轮询 ⇒ 可留 SRAM1） */

static SD_HandleTypeDef          g_sd_handle;
static HAL_SD_CardInfoTypeDef    s_card_info;
static int                       s_ready = 0;          /* 1 = 卡已识别且可用 */

/* ===========================================================================
 * HAL 的底层钩子（HAL_SD_Init 内部会回调；库里是 __weak，这里给强定义）
 * 引脚来自厂商「实验26 SD卡实验」的 sdmmc_sdcard.h，与 V1.2 原理图一致。
 * =========================================================================== */
void HAL_SD_MspInit(SD_HandleTypeDef *hsd)
{
    GPIO_InitTypeDef gpio;

    if (hsd->Instance != SDMMC1)
    {
        return;
    }

    __HAL_RCC_SDMMC1_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDMMC1;

    /* D0..D3 = PC8..PC11，CLK = PC12 */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* CMD = PD2 */
    gpio.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* L2-4：IDMA 的完成/错误是靠 SDMMC1 全局中断回调 HAL_SD_IRQHandler 的，
     * 不使能 NVIC 的话 *_DMA 会一直卡在 BUSY。
     * 优先级取 5：低于本工程所有"业务"优先级概念（无 RTOS，只有一个 SysTick），
     * 但要保证不会被长时间关中断的临界区饿死 —— 本工程没有长临界区，5 足够。 */
    HAL_NVIC_SetPriority(SDMMC1_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
}

/* ===========================================================================
 * L2-4：SDMMC1 中断与 HAL 回调
 *
 * 向量槽位在 startup_gcc.s 里为 IRQ49 单开（原来 150 个 IRQ 全是 Default_Handler），
 * 这里的 SDMMC1_IRQHandler 就是那个槽位的强定义。
 * =========================================================================== */
void SDMMC1_IRQHandler(void)
{
    g_sd_dma_irq++;
    HAL_SD_IRQHandler(&g_sd_handle);
}

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    g_sd_dma_wrcplt++;
    s_dma_done = 1u;
}

void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    g_sd_dma_rdcplt++;
    s_dma_done = 1u;
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)
{
    /* ★必须在这一刻抓 ErrorCode：HAL_SD_IRQHandler 之后还会清标志、Abort，
     * 等操作返回时再读就只剩 0 了（首轮调试就是这么被误导过一轮）。 */
    g_sd_dma_errcode = hsd->ErrorCode;
    g_sd_dma_sta     = SDMMC1->STA;
    hsd->ErrorCode   = HAL_SD_ERROR_NONE;   /* 清掉，免得污染下一次操作 */
    (void)hsd;
    g_sd_dma_errcb++;
    s_dma_is_err = 1u;
    s_dma_done   = 1u;
}

/* 抓一组 SDMMC 寄存器：出错瞬间与启动瞬间各抓一次，两者对照才看得出"有没有动过"。 */
static void snap(volatile uint32_t *dst)
{
    dst[0] = SDMMC1->STA;
    dst[1] = SDMMC1->DCOUNT;
    dst[2] = SDMMC1->DLEN;
    dst[3] = SDMMC1->DCTRL;
    dst[4] = SDMMC1->MASK;
    dst[5] = SDMMC1->IDMACTRL;
    dst[6] = SDMMC1->IDMABSIZE;
    dst[7] = SDMMC1->IDMABASE0;
}

/* ===========================================================================
 * 初始化
 * =========================================================================== */
int sd_card_init(void)
{
    g_sd_handle.Instance                 = SDMMC1;
    g_sd_handle.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    g_sd_handle.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    g_sd_handle.Init.BusWide             = SDMMC_BUS_WIDE_4B;
    /* 硬件流控：FIFO 满/空时 SDMMC 自己暂停 SDMMC_CK。
     * 轮询模式下数据是 **CPU 搬 FIFO**，而 CPU 还会被 SysTick 等中断打断，
     * 关掉流控时一旦 CPU 没及时服务 FIFO 就会 RXOVER/TXUNDERR（STA 里能看到）。
     * 厂商例程是 DISABLE（它的场景是关中断跑），本工程不关中断，故必须打开。 */
    g_sd_handle.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    g_sd_handle.Init.ClockDiv            = 4u;      /* 200MHz/(2*4) = 25MHz */

    /* DeInit 让重复调用（开机一次 + 失败重试）保持幂等 */
    (void)HAL_SD_DeInit(&g_sd_handle);

    g_sd_clk_div = (uint32_t)g_sd_handle.Init.ClockDiv;
    g_sd_hwfc    = (g_sd_handle.Init.HardwareFlowControl ==
                    SDMMC_HARDWARE_FLOW_CONTROL_ENABLE) ? 1u : 0u;
    g_sd_init_rc = (uint32_t)HAL_SD_Init(&g_sd_handle);
    if (g_sd_init_rc != (uint32_t)HAL_OK)
    {
        s_ready = 0;
        return 1;
    }

    if (HAL_SD_GetCardInfo(&g_sd_handle, &s_card_info) != HAL_OK)
    {
        g_sd_init_rc = 0xFFFFFFFEu;                  /* 识别成功但拿不到卡信息 */
        s_ready = 0;
        return 2;
    }

    g_sd_card_type  = (uint32_t)s_card_info.CardType;
    g_sd_block_nbr  = s_card_info.LogBlockNbr;
    g_sd_block_size = s_card_info.LogBlockSize;
    g_sd_cap_mb     = (uint32_t)(((uint64_t)s_card_info.LogBlockNbr *
                                  (uint64_t)s_card_info.LogBlockSize) >> 20);
    /* 缓冲地址开机即自报：主机（tools/sd_check.py / 还原脚本）**不用猜段内布局**。
     * 实测踩过：同一段里三个缓冲的实际排列顺序与源码书写顺序并不一致
     * （段起点 0x30000200 是 s_bak，而 s_wr 在 0x30002200），猜地址会写错地方。 */
    g_sd_wr_addr  = (uint32_t)s_wr;
    g_sd_rd_addr  = (uint32_t)s_rd;
    g_sd_bak_addr = (uint32_t)s_bak;
    g_sd_buf_addr = (uint32_t)s_wr;

    s_ready         = 1;
    return 0;
}

uint32_t sd_get_block_nbr(void)  { return s_ready ? s_card_info.LogBlockNbr  : 0u; }
uint32_t sd_get_block_size(void) { return s_ready ? s_card_info.LogBlockSize : 0u; }

/* ===========================================================================
 * 块读写（后续 FatFs 的 diskio 就架在这两个函数上）
 * =========================================================================== */
static int wait_transfer_ready(void)
{
    uint32_t guard = 200000u;                        /* 纯计数上限；不依赖 SysTick */

    while (guard-- != 0u)
    {
        uint32_t st = (uint32_t)HAL_SD_GetCardState(&g_sd_handle);
        g_sd_card_state = st;
        if (st == (uint32_t)HAL_SD_CARD_TRANSFER)
        {
            return 0;
        }
    }
    return 1;
}

/* 统一出入口：所有 HAL 调用都从这里走，好把"失败现场"完整记下来。 */
static int sd_op(int is_write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    HAL_StatusTypeDef st;

    g_sd_hal_rc  = 0xFFFFFFFFu;
    g_sd_errcode = 0xFFFFFFFFu;

    if (!s_ready) { return 1; }

    /* 每次操作前先确认卡空闲 —— 上一步若是多块写，卡可能还在 programming */
    if (wait_transfer_ready() != 0)
    {
        g_sd_hal_rc = 0xFFFFFFF0u;                   /* 卡一直不回到 TRANSFER */
        return 3;
    }

    if (is_write)
    {
        st = HAL_SD_WriteBlocks(&g_sd_handle, (const uint8_t *)buf, sector, count, 2000u);
    }
    else
    {
        st = HAL_SD_ReadBlocks(&g_sd_handle, buf, sector, count, 2000u);
    }

    g_sd_hal_rc  = (uint32_t)st;
    g_sd_errcode = g_sd_handle.ErrorCode;
    if (st != HAL_OK)
    {
        /* 立刻快照状态寄存器：RXOVER / TXUNDERR / DCRCFAIL / DTIMEOUT 就在这里 */
        g_sd_sta = SDMMC1->STA;
    }

    if (st != HAL_OK) { return 2; }
    if (wait_transfer_ready() != 0)
    {
        g_sd_hal_rc = 0xFFFFFFF1u;                   /* 传输返回 OK 但卡不回 TRANSFER */
        return 4;
    }
    return 0;
}

int sd_read_blocks(uint32_t sector, uint32_t count, uint8_t *buf)
{
    return sd_op(0, sector, count, buf);
}

int sd_write_blocks(uint32_t sector, uint32_t count, const uint8_t *buf)
{
    return sd_op(1, sector, count, (uint8_t *)buf);
}

/* ===========================================================================
 * L2-4：IDMA 通路（必须先做完 L3：SRAM1 已是非缓存区）
 *
 * 与轮询版 sd_op 的唯一区别是搬数据的人换了：
 *   轮询版 = CPU 在中断打扰下手工搬 SDMMC FIFO（慢，且忙等占满 CPU）；
 *   IDMA   = SDMMC 内置 DMA 直接读写 SRAM1，CPU 只等一个完成中断。
 * 后者是"真正的总线主设备写内存"，所以**必须**有 L3 兜底，否则会静默读到陈旧数据。
 * =========================================================================== */
static int sd_op_dma(int is_write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    HAL_StatusTypeDef st;
    uint32_t c0;

    g_sd_dma_hal_rc  = 0xFFFFFFFFu;
    g_sd_dma_errcode = 0xFFFFFFFFu;
    g_sd_dma_sta     = 0u;

    if (!s_ready) { return 1; }

    if (wait_transfer_ready() != 0)
    {
        g_sd_dma_hal_rc = 0xFFFFFFF0u;               /* 卡一直不回到 TRANSFER */
        return 3;
    }

    /* 启动前清掉所有静态标志：否则上一轮（例如轮询读）残留的 DATAEND/DCRCFAIL
     * 会让 HAL_SD_IRQHandler 一进来就走错误分支，表现为"传输刚发起就报错"。 */
    __HAL_SD_CLEAR_FLAG(&g_sd_handle, SDMMC_STATIC_FLAGS);

    s_dma_done   = 0u;
    s_dma_is_err = 0u;

    if (is_write)
    {
        st = HAL_SD_WriteBlocks_DMA(&g_sd_handle, (const uint8_t *)buf, sector, count);
    }
    else
    {
        st = HAL_SD_ReadBlocks_DMA(&g_sd_handle, buf, sector, count);
    }

    g_sd_dma_hal_rc = (uint32_t)st;
    if (st != HAL_OK)
    {
        g_sd_dma_errcode = g_sd_handle.ErrorCode;
        g_sd_dma_sta     = SDMMC1->STA;
        if (is_write) { g_sd_dma_w_rc = 2u; g_sd_dma_w_errcode = g_sd_dma_errcode; g_sd_dma_w_sta = g_sd_dma_sta; }
        else          { g_sd_dma_r_rc = 2u; g_sd_dma_r_errcode = g_sd_dma_errcode; g_sd_dma_r_sta = g_sd_dma_sta; }
        snap(g_sd_dma_snap_post);
        return 2;
    }
    snap(g_sd_dma_snap_pre);

    /* 等中断回调收尾。上限 0.5 s（DWT @400MHz，32 位回绕不影响差值比较）。
     * 不用 HAL_GetTick：SysTick 是 1 ms 粒度，够不着"超时但没完全超时"的诊断价值。 */
    c0 = DWT->CYCCNT;
    while (s_dma_done == 0u)
    {
        if ((DWT->CYCCNT - c0) > 200000000u) { break; }
    }

    snap(g_sd_dma_snap_post);
    if (is_write)
    {
        g_sd_dma_w_rc      = 0u;
        g_sd_dma_w_errcode = g_sd_dma_errcode;
        g_sd_dma_w_sta     = g_sd_dma_sta;
    }
    else
    {
        g_sd_dma_r_rc      = 0u;
        g_sd_dma_r_errcode = g_sd_dma_errcode;
        g_sd_dma_r_sta     = g_sd_dma_sta;
    }

    if (!s_dma_done)
    {
        g_sd_dma_hal_rc = 0xFFFFFFF2u;               /* 0.5 s 内没等到任何中断 */
        g_sd_dma_sta    = SDMMC1->STA;
        if (is_write) { g_sd_dma_w_sta = g_sd_dma_sta; g_sd_dma_w_rc = 5u; }
        else          { g_sd_dma_r_sta = g_sd_dma_sta; g_sd_dma_r_rc = 5u; }
        (void)HAL_SD_Abort(&g_sd_handle);
        return 5;
    }
    if (s_dma_is_err)
    {
        if (is_write) { g_sd_dma_w_rc = 6u; }
        else          { g_sd_dma_r_rc = 6u; }
        (void)HAL_SD_Abort(&g_sd_handle);
        return 6;
    }
    if (wait_transfer_ready() != 0)
    {
        g_sd_dma_hal_rc = 0xFFFFFFF1u;               /* 传输完成但卡不回 TRANSFER */
        if (is_write) { g_sd_dma_w_rc = 4u; }
        else          { g_sd_dma_r_rc = 4u; }
        return 4;
    }
    return 0;
}

/* 带一次重试的 IDMA 操作：失败就整卡重新初始化再来一遍。
 * 与轮询版 sd_op_retry 同规格 —— 抖动不应当让自检误报，但 g_sd_retry 会记下来。 */
static int sd_op_dma_retry(int is_write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    int rc = sd_op_dma(is_write, sector, count, buf);
    if (rc != 0)
    {
        g_sd_retry++;
        (void)sd_card_init();
        rc = sd_op_dma(is_write, sector, count, buf);
    }
    return rc;
}

/* ===========================================================================
 * 自检用例
 * =========================================================================== */

/* 确定性伪随机图案（LCG）。为什么不用固定值：全 0x00 / 全 0xFF 这类图案
 * 会被"卡没写进去、读回的是擦除态"骗过去 —— 随机图案 + 逐字节比对没有这种巧合。
 * 每个扇区用不同种子 ⇒ 还能顺带暴露"8 个扇区写了同一份内容"这种地址不递增的故障。 */
static void fill_pattern(uint8_t *buf, uint32_t bytes, uint32_t seed)
{
    uint32_t v = seed ^ 0xA5A5A5A5u;
    uint32_t i;

    for (i = 0u; i < bytes; i++)
    {
        v = v * 1103515245u + 12345u;
        buf[i] = (uint8_t)(v >> 16);
    }
}

static uint32_t count_diff(const uint8_t *a, const uint8_t *b, uint32_t bytes)
{
    uint32_t n = 0u, i;
    for (i = 0u; i < bytes; i++)
    {
        if (a[i] != b[i]) { n++; }
    }
    return n;
}

/* 带一次重试的读/写：失败就重新初始化整张卡再来一次。
 * 为什么重试：第一次跑通、第二次跑挂，说明是**间歇**问题；
 * 但重试不是"解决"，只是让自检不至于一碰到抖动就误报，
 * 真正的原因要看 g_sd_sta / g_sd_errcode / g_sd_retry。 */
static int sd_op_retry(int is_write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    int rc = sd_op(is_write, sector, count, buf);
    if (rc != 0)
    {
        g_sd_retry++;
        (void)sd_card_init();                        /* 重新走一遍识别流程 */
        rc = sd_op(is_write, sector, count, buf);
    }
    return rc;
}

/* ---- 模式 2/3：把 s_bak 写回 g_sd_sector（8 扇区）并读回验证 ---- */
static void run_restore(void)
{
    g_sd_step = STEP_RESTORE_W;
    g_sd_restore_rc  = (uint32_t)sd_op_retry(1, g_sd_sector, SD_BENCH_NSEC, s_bak);
    g_sd_step = STEP_RESTORE_R;
    memset(s_rd, 0, SD_BENCH_NSEC * SD_SECTOR_BYTES);
    (void)sd_op_retry(0, g_sd_sector, SD_BENCH_NSEC, s_rd);
    g_sd_restore_mis = count_diff(s_bak, s_rd, SD_BENCH_NSEC * SD_SECTOR_BYTES);
    g_sd_step = STEP_IDLE;
}

static void run_bench(void)
{
    uint32_t total, sector, c0;

    g_sd_state   = 0u;
    g_sd_retry   = 0u;
    g_sd_sta     = 0u;
    g_sd_buf_addr = (uint32_t)s_wr;
    g_sd_wr_addr  = (uint32_t)s_wr;
    g_sd_rd_addr  = (uint32_t)s_rd;
    g_sd_bak_addr = (uint32_t)s_bak;

    /* 卡没起来就现场重试一次（允许"先开机后插卡"的用法） */
    if (!s_ready)
    {
        if (sd_card_init() != 0)
        {
            g_sd_state = 1u;
            return;
        }
    }

    total = s_card_info.LogBlockNbr;
    /* 测试扇区取卡**尾部**（末尾约 1 MB 处）—— 绝不碰扇区 0（MBR/分区表），
     * 也尽量避开 FAT 文件系统通常占用的前部区域。 */
    sector = (total > 4096u) ? (total - 2048u) : (total / 2u);
    g_sd_sector = sector;

    /* ---- 备份原内容（后面要原样写回去）---- */
    g_sd_step = STEP_BAK_READ;
    g_sd_bak_rc = (uint32_t)sd_op_retry(0, sector, SD_BENCH_NSEC, s_bak);
    if (g_sd_bak_rc != 0u)
    {
        g_sd_state = 2u;                /* 读都读不了，后面的用例没有意义 */
        g_sd_step  = STEP_IDLE;
        return;
    }

    /* ---- 用例①：单扇区 512 B 写 → 读 → 逐字节比对 ---- */
    fill_pattern(s_wr, SD_SECTOR_BYTES, 0x9E3779B9u ^ (sector * 2654435761u));
    g_sd_step = STEP_1_WRITE;
    g_sd_write_rc = (uint32_t)sd_op_retry(1, sector, 1u, s_wr);
    memset(s_rd, 0, SD_SECTOR_BYTES);
    g_sd_step = STEP_1_READ;
    g_sd_read_rc = (uint32_t)sd_op_retry(0, sector, 1u, s_rd);
    g_sd_mis1 = count_diff(s_wr, s_rd, SD_SECTOR_BYTES);

    /* ---- 用例②：连续 8 扇区（4 KB）写 → 读 → 逐字节比对 ---- */
    {
        uint32_t i;
        for (i = 0u; i < SD_BENCH_NSEC; i++)
        {
            fill_pattern(s_wr + i * SD_SECTOR_BYTES, SD_SECTOR_BYTES,
                         0x9E3779B9u ^ ((sector + i) * 2654435761u));
        }
        g_sd_step = STEP_8_WRITE;
        c0 = DWT->CYCCNT;
        sd_op_retry(1, sector, SD_BENCH_NSEC, s_wr);
        g_sd_wr_cyc = DWT->CYCCNT - c0;

        memset(s_rd, 0, SD_BENCH_NSEC * SD_SECTOR_BYTES);
        g_sd_step = STEP_8_READ;
        c0 = DWT->CYCCNT;
        g_sd_read_rc |= ((uint32_t)sd_op_retry(0, sector, SD_BENCH_NSEC, s_rd)) << 8;
        g_sd_rd_cyc = DWT->CYCCNT - c0;

        g_sd_mis2 = count_diff(s_wr, s_rd, SD_BENCH_NSEC * SD_SECTOR_BYTES);
    }

    /* ---- 收尾：把原内容写回去，并验证恢复成功（对卡做到无损）---- */
    run_restore();

    g_sd_state = 2u;
}

/* ---- 模式 4/5：IDMA 自检（同时验 L3 的非缓存 DMA 区是否真的生效）---- */
static void run_dma_bench(uint32_t hs_mode)
{
    uint32_t total, sector, c0, i, rc;
    uint32_t mis = 0u, stale = 0u;
    volatile uint32_t acc = 0u;

    g_sd_state        = 0u;
    g_sd_retry        = 0u;
    g_sd_sta          = 0u;
    g_sd_dma_irq      = 0u;
    g_sd_dma_wrcplt   = 0u;
    g_sd_dma_rdcplt   = 0u;
    g_sd_dma_errcb    = 0u;
    g_sd_dma_mis      = 0xFFFFFFFFu;
    g_sd_dma_stale    = 0xFFFFFFFFu;
    g_sd_dma_stale_base = 0xFFFFFFFFu;
    g_sd_dma_wr_cyc   = 0u;
    g_sd_dma_rd_cyc   = 0u;
    g_sd_dma_w_rc     = 0xFFFFFFFFu;
    g_sd_dma_r_rc     = 0xFFFFFFFFu;
    g_sd_dma_w_errcode = 0xFFFFFFFFu;
    g_sd_dma_r_errcode = 0xFFFFFFFFu;
    g_sd_dma_w_sta    = 0u;
    g_sd_dma_r_sta    = 0u;
    g_sd_dma_buf_addr = (uint32_t)s_wr;
    g_sd_wr_addr      = (uint32_t)s_wr;
    g_sd_rd_addr      = (uint32_t)s_rd;
    g_sd_bak_addr     = (uint32_t)s_bak;

    if (!s_ready)
    {
        if (sd_card_init() != 0)
        {
            g_sd_state = 1u;
            return;
        }
    }

    /* ---- 高速总线（仅模式 5）----
     * HAL_SD_ConfigSpeedBusOperation 只发 CMD6 让**卡**进 High-Speed，
     * 主机侧的分频要自己改（200MHz/(2*2)=50MHz，SD 卡 High-Speed 上限 50MHz）。
     * 切速失败不致命：记 g_sd_speed_mode=2，留在 25MHz 把用例跑完。 */
    if (hs_mode != 0u)
    {
        if (HAL_SD_ConfigSpeedBusOperation(&g_sd_handle, SDMMC_SPEED_MODE_HIGH) == HAL_OK)
        {
            SDMMC1->CLKCR = (SDMMC1->CLKCR & ~(uint32_t)SDMMC_CLKCR_CLKDIV) | 2u;
            g_sd_clk_div     = 2u;
            g_sd_speed_mode  = 1u;
        }
        else
        {
            g_sd_speed_mode = 2u;
        }
    }
    else
    {
        g_sd_speed_mode = 0u;
    }
    g_sd_dma_clkdiv = g_sd_clk_div;

    total  = s_card_info.LogBlockNbr;
    sector = (total > 4096u) ? (total - 2048u) : (total / 2u);
    g_sd_sector = sector;

    /* ---- 备份原内容（无论用例成败，最后都要原样写回）---- */
    g_sd_step   = STEP_BAK_READ;
    g_sd_bak_rc = (uint32_t)sd_op_retry(0, sector, SD_BENCH_NSEC, s_bak);
    if (g_sd_bak_rc != 0u)
    {
        g_sd_state = 2u;
        g_sd_step  = STEP_IDLE;
        return;
    }

    for (i = 0u; i < SD_BENCH_NSEC; i++)
    {
        fill_pattern(s_wr + i * SD_SECTOR_BYTES, SD_SECTOR_BYTES,
                     0x9E3779B9u ^ ((sector + i) * 2654435761u));
    }

    /* ---- IDMA 写 8 扇区 ---- */
    g_sd_step = STEP_DMA_WRITE;
    c0 = DWT->CYCCNT;
    rc = (uint32_t)sd_op_dma_retry(1, sector, SD_BENCH_NSEC, s_wr);
    g_sd_dma_wr_cyc = DWT->CYCCNT - c0;
    g_sd_write_rc   = rc;

    /* ---- IDMA 读 8 扇区 ----
     * 先把读缓冲填成哨兵并**真读一遍**（volatile 累加，不会被优化掉），
     * 制造"这块内存此刻在 cache 里有副本"的条件；随后 IDMA 从背后改写它。
     * 若 SRAM1 还是 cacheable 且没人 invalidate，下面读到的会整片是哨兵。 */
    memset(s_rd, DMA_SENTINEL, SD_BENCH_NSEC * SD_SECTOR_BYTES);
    for (i = 0u; i < SD_BENCH_NSEC * SD_SECTOR_BYTES; i += 32u)
    {
        acc += s_rd[i];
    }
    (void)acc;

    g_sd_step = STEP_DMA_READ;
    c0 = DWT->CYCCNT;
    rc = (uint32_t)sd_op_dma_retry(0, sector, SD_BENCH_NSEC, s_rd);
    g_sd_dma_rd_cyc = DWT->CYCCNT - c0;
    g_sd_read_rc    = rc;

    for (i = 0u; i < SD_BENCH_NSEC * SD_SECTOR_BYTES; i++)
    {
        if (s_rd[i] != s_wr[i]) { mis++; }
        if (s_rd[i] == DMA_SENTINEL) { stale++; }
    }
    /* 基线：图案本身里就有多少字节等于哨兵。随机图案下期望 ≈ 4096/256 = 16。
     * 没有它，判据 stale==0 会把这十几个"天然巧合"误报成缓存陈旧
     * （2026-10-08 实测：mis 已为 0，stale 却是 15，白改了一轮 MPU）。 */
    {
        uint32_t base = 0u;
        for (i = 0u; i < SD_BENCH_NSEC * SD_SECTOR_BYTES; i++)
        {
            if (s_wr[i] == DMA_SENTINEL) { base++; }
        }
        g_sd_dma_stale_base = base;
    }
    g_sd_mis2      = mis;        /* 判据一：IDMA 读回 == 写进去的图案 */
    g_sd_dma_mis   = mis;
    g_sd_dma_stale = stale;      /* 判据二：仍读到哨兵的字节数（需与 base 比较） */

    /* ---- 收尾：原样写回；然后把总线速度退回默认 ----
     * 退速的理由：50MHz 是"测得通"而不是"默认可信"，让它留在默认 25MHz，
     * 后续 FatFs / 轮询用例就不会被本轮的高速实验牵连。 */
    run_restore();

    if (g_sd_speed_mode == 1u)
    {
        (void)HAL_SD_ConfigSpeedBusOperation(&g_sd_handle, SDMMC_SPEED_MODE_DEFAULT);
        SDMMC1->CLKCR = (SDMMC1->CLKCR & ~(uint32_t)SDMMC_CLKCR_CLKDIV) | 4u;
        g_sd_clk_div = 4u;
    }

    g_sd_state = 2u;
    g_sd_step  = STEP_IDLE;
}

void sd_bench_poll(void)
{
    uint32_t mode = g_sd_test;

    if (mode == 0u)
    {
        return;
    }
    g_sd_test = 0u;

    if (mode == SD_TEST_FULL)
    {
        run_bench();
    }
    else if (mode == SD_TEST_DMA)
    {
        run_dma_bench(0u);
    }
    else if (mode == SD_TEST_DMA_HS)
    {
        run_dma_bench(1u);
    }
    else /* SD_TEST_RESTORE / SD_TEST_HOSTFILL —— 两者动作相同：写回 s_bak */
    {
        g_sd_state   = 0u;
        g_sd_retry   = 0u;
        g_sd_buf_addr = (uint32_t)s_wr;
        g_sd_wr_addr  = (uint32_t)s_wr;
        g_sd_rd_addr  = (uint32_t)s_rd;
        g_sd_bak_addr = (uint32_t)s_bak;
        if (!s_ready && (sd_card_init() != 0))
        {
            g_sd_state = 1u;
            return;
        }
        run_restore();
        g_sd_state = 2u;
    }
}
