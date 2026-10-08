/* ===========================================================================
 * TF 卡 / SDMMC1 驱动 + 写读比对自检（路线图 docs/TF_CARD_CACHE_ROADMAP.md L2）
 *
 * 【本阶段的唯一目标】
 * 证明"SDMMC1 的数据通路是对的"—— 用**写→读逐字节比对**的整数结论，
 * 而不是"能列出文件"或"看图对不对"。这是本工程的一贯规矩：
 * 能给出确定整数结论的自测，比任何肉眼判据都可靠。
 *
 * 【为什么这一阶段用轮询模式（HAL_SD_ReadBlocks / HAL_SD_WriteBlocks）】
 *   · 这两个 API 是 **CPU 轮询 SDMMC FIFO** 把数据搬进/搬出内存
 *     （IDMA 只在 *_DMA 变体里启用，已核对 hal_sd.c：
 *       IDMABASE0 / IDMACTRL 只出现在 HAL_SD_ReadBlocks_DMA:1281
 *       与 HAL_SD_WriteBlocks_DMA:1380，轮询版 671 / 856 不碰 IDMA）。
 *   · 于是本阶段**根本不存在 D-Cache 一致性问题**：
 *     CPU 写在 FORCEWT=1 下直达内存，CPU 读也走 cache 但数据本就是 CPU 自己写的。
 *   · 好处是把"卡能不能识别 / 数据通路对不对"与"缓存一致性"两件事彻底解耦，
 *     先拿到一个干净的结论。IDMA 高速模式（L2-4）必须等 L3（非缓存 DMA 区）做完。
 *
 * 【引脚（权威来源：厂商 V1.2 原理图 + 资料包「实验26 SD卡实验」
 *   Drivers/BSP/SDMMC/sdmmc_sdcard.h，不是本工程推测）】
 *   SDMMC1 · 4-bit · AF12：
 *     D0=PC8  D1=PC9  D2=PC10  D3=PC11  CLK=PC12  CMD=PD2
 *   全部上拉（SD 总线要求），Speed=VERY_HIGH。
 *   板上 TF 卡槽**没有**接到 MCU 的卡检测脚（CD 未连），所以有没有卡只能靠
 *   CMD0/CMD8 通信是否成功判断 —— 也就是 HAL_SD_Init 的返回值。
 *
 * 【时钟】
 *   sys_stm32_clock_init(160,5,2,4) ⇒ PLL1_Q = 800MHz/4 = 200MHz；
 *   RCC->D1CCIPR.SDMMCSEL 复位值 0 ⇒ sdmmc_ker_ck = pll1_q_ck = 200MHz。
 *   SDMMC_CK = sdmmc_ker_ck / (2 * ClockDiv)：
 *     初始化阶段 HAL 自己用 SDMMC_INIT_CLK_DIV=0xFA(250) ⇒ 400 kHz（规格要求 ≤400 kHz）；
 *     传输阶段本文件取 ClockDiv=4 ⇒ 25 MHz（SD 卡默认速度上限，取上限即可，
 *     先求稳不追高速；等 L3 做完再谈 50 MHz）。
 *
 * 【⚠ 数据安全：不破坏卡上已有内容】
 *   卡是 32 GB 的 FAT 卡，上面可能有数据。自检**绝不写扇区 0（MBR/分区表）**，
 *   而是挑卡**尾部**的一块（总块数 - 2048，即末尾 1 MB 处，正常文件系统极少用到），
 *   并且：先读出原内容备份 → 写图案 → 读回比对 → **把原内容写回去** → 再读回验证。
 *   即使踩到某段数据，也会被原样恢复（g_sd_restore_mis = 0 就是恢复成功的证据）。
 * =========================================================================== */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>

/* 多扇区用例的扇区数：8 × 512 B = 4 KB。
 * 取 8 而不是路线图里的 100，是为了让"备份 + 图案缓冲"能成对放进 SRAM1
 * （100 扇区需要 100 KB 做备份，白白吃掉整块 SRAM1）；8 扇区已经足够暴露
 * "跨扇区地址递增错误 / 只写了第一个扇区"这类经典故障。 */
#define SD_BENCH_NSEC   8u

/* ---- 诊断量（都在 DTCM 的 .bss，SWD 直读不受 D-Cache 影响）---- */

extern volatile uint32_t g_sd_test;        /* 写 1 触发一轮完整自检；跑完自动清 0 */
extern volatile uint32_t g_sd_state;       /* 0=未跑 1=卡初始化失败 2=自检已跑完 */
extern volatile uint32_t g_sd_init_rc;     /* HAL_SD_Init 返回值（0 = HAL_OK） */
extern volatile uint32_t g_sd_card_type;   /* CardInfo.CardType（0=SDS 1=SDSC 2=SDHC/SDXC） */
extern volatile uint32_t g_sd_block_nbr;   /* 卡的逻辑块总数 */
extern volatile uint32_t g_sd_block_size;  /* 逻辑块大小（应 512） */
extern volatile uint32_t g_sd_cap_mb;      /* 容量 MB */
extern volatile uint32_t g_sd_clk_div;     /* 实际生效的 ClockDiv（25MHz = 4） */

extern volatile uint32_t g_sd_sector;      /* 本轮自检使用的起始扇区（卡尾部） */
extern volatile uint32_t g_sd_bak_rc;      /* 备份：读原内容 返回值 */
extern volatile uint32_t g_sd_write_rc;    /* 写图案 返回值 */
extern volatile uint32_t g_sd_read_rc;     /* 读回 返回值 */
extern volatile uint32_t g_sd_mis1;        /* 用例① 单扇区：失配**字节数**（判据 = 0） */
extern volatile uint32_t g_sd_mis2;        /* 用例② 8 扇区：失配**字节数**（判据 = 0） */
extern volatile uint32_t g_sd_restore_rc;  /* 恢复：把原内容写回 返回值 */
extern volatile uint32_t g_sd_restore_mis; /* 恢复后读回与原内容比对：失配字节数（判据 = 0） */

extern volatile uint32_t g_sd_wr_cyc;      /* 用例② 写 8 扇区的 DWT 周期数 */
extern volatile uint32_t g_sd_rd_cyc;      /* 用例② 读 8 扇区的 DWT 周期数 */
extern volatile uint32_t g_sd_buf_addr;    /* 写图案缓冲地址（应在 SRAM1 0x3000_xxxx） */
extern volatile uint32_t g_sd_card_state;  /* 最后一次 HAL_SD_GetCardState 的返回值 */

/* ---- 第二轮新增：为了定位"第二次跑失败"而加的细粒度证据 ----
 * 第一次跑四项全过、第二次跑用例②的读返回非 OK 且**恢复也跟着失败**
 * （失配 4047 字节 ⇒ 卡上那 8 个扇区被写成了图案、没还原回来）。
 * 这类"间歇失败"正是本工程最警惕的东西，不能靠"再跑一次就好了"糊过去，
 * 必须拿到硬件寄存器层面的证据，所以这里把 HAL 返回码、ErrorCode、
 * SDMMC->STA 全部导出。 */
extern volatile uint32_t g_sd_wr_addr;     /* 写图案缓冲地址 */
extern volatile uint32_t g_sd_rd_addr;     /* 读回缓冲地址 */
extern volatile uint32_t g_sd_bak_addr;    /* 备份缓冲地址（★还原卡内容靠它） */
extern volatile uint32_t g_sd_hal_rc;      /* 最后一次 HAL_SD_*Blocks 返回值：0 OK /1 ERR /2 BUSY /3 TIMEOUT */
extern volatile uint32_t g_sd_errcode;     /* hsd.ErrorCode（HAL 细分错误码） */
extern volatile uint32_t g_sd_sta;         /* 失败瞬间的 SDMMC1->STA（RXOVER/DCRCFAIL/DTIMEOUT...） */
extern volatile uint32_t g_sd_retry;       /* 本轮重试次数 */
extern volatile uint32_t g_sd_step;        /* 卡在哪一步（0 空闲；见 sd_card.c 的 STEP_*） */
extern volatile uint32_t g_sd_hwfc;        /* 1 = 已启用 SDMMC 硬件流控（防 FIFO 上/下溢） */

/* ---- 第三轮新增（L2-4：IDMA 高速模式）----
 * 前置：L3 已把 SRAM1 用 MPU 配成**非缓存**区（判据 dma_check.py 用例② 64→0）。
 * 有了 L3，IDMA 才能安全使用 —— IDMA 是真正的总线主设备写内存，
 * 会绕过 D-Cache；若缓冲区还在 cacheable 区，CPU 随后读到的就是陈旧副本
 * （不报错、不 HardFault 的静默数据损坏，台架已实测过）。
 *
 * 本轮除了"能不能跑通、快多少"，还额外验一件 L3 才会成立的事：
 *   先把读缓冲填成哨兵值并**主动读一遍**（让它在 cache 里有副本），
 *   再让 IDMA 往同一块缓冲搬数据，最后 CPU 直接读 ——
 *   若 SRAM1 仍是 cacheable，CPU 会读到哨兵（g_sd_dma_stale 巨大）；
 *   非缓存下应该一个哨兵字节都读不到（判据 = 0）。 */
extern volatile uint32_t g_sd_dma_wr_cyc;  /* IDMA 写 8 扇区的 DWT 周期数 */
extern volatile uint32_t g_sd_dma_rd_cyc;  /* IDMA 读 8 扇区的 DWT 周期数 */
extern volatile uint32_t g_sd_dma_mis;     /* IDMA 读回 vs 写图案：失配字节数（判据 0） */
extern volatile uint32_t g_sd_dma_stale;   /* IDMA 读后仍等于哨兵的字节数 */
extern volatile uint32_t g_sd_dma_stale_base;
                                           /* ★基线：写图案里**本来就有**多少字节等于哨兵。
                                            * 随机图案 4096 B 中每个字节等于 0x5A 的概率 1/256
                                            * ⇒ 期望 ≈16，实测 15。所以判据不是 stale==0，
                                            *   而是 **stale - base == 0**（见 sd_dma_check.py）。
                                            * 第一版直接判 stale==0，把这 15 字节误报成
                                            * "L3 没生效"，白改了一轮 MPU。 */
extern volatile uint32_t g_sd_dma_hal_rc;  /* 最后一次 *_DMA 返回值 */
extern volatile uint32_t g_sd_dma_sta;     /* IDMA 失败瞬间的 SDMMC1->STA */
extern volatile uint32_t g_sd_dma_errcode; /* IDMA 路径的 hsd.ErrorCode */
extern volatile uint32_t g_sd_dma_irq;     /* SDMMC1_IRQHandler 实际进入次数（>0 才说明中断真通了） */
extern volatile uint32_t g_sd_dma_wrcplt;  /* HAL_SD_TxCpltCallback 次数 */
extern volatile uint32_t g_sd_dma_rdcplt;  /* HAL_SD_RxCpltCallback 次数 */
extern volatile uint32_t g_sd_dma_errcb;   /* HAL_SD_ErrorCallback 次数 */
extern volatile uint32_t g_sd_speed_mode;  /* 0=默认速度(25MHz) 1=高速(50MHz)；2=切换失败 */
extern volatile uint32_t g_sd_dma_buf_addr;/* IDMA 缓冲区地址（应在 SRAM1 0x3000_xxxx） */
extern volatile uint32_t g_sd_dma_clkdiv;  /* IDMA 用例**当时**用的 ClockDiv（25MHz=4 / 50MHz=2） */

/* 失败现场的寄存器快照（首次上板时 IDMA 一上来就错，需要硬件级证据定位）。
 * 下标固定：0=STA 1=DCOUNT 2=DLEN 3=DCTRL 4=MASK 5=IDMACTRL 6=IDMABSIZE 7=IDMABASE0
 * pre = 刚启动 IDMA 之后；post = 出错/超时那一刻。 */
#define SD_DMA_SNAP_N  8u
extern volatile uint32_t g_sd_dma_snap_pre[SD_DMA_SNAP_N];
extern volatile uint32_t g_sd_dma_snap_post[SD_DMA_SNAP_N];
extern volatile uint32_t g_sd_dma_w_rc;        /* IDMA 写用例返回码（0=成功，见 sd_card.c 的 rc 约定） */
extern volatile uint32_t g_sd_dma_r_rc;        /* IDMA 读用例返回码 */
extern volatile uint32_t g_sd_dma_w_errcode;   /* IDMA 写失败瞬间的 hsd.ErrorCode */
extern volatile uint32_t g_sd_dma_r_errcode;   /* IDMA 读失败瞬间的 hsd.ErrorCode */
extern volatile uint32_t g_sd_dma_w_sta;       /* IDMA 写失败瞬间的 STA */
extern volatile uint32_t g_sd_dma_r_sta;       /* IDMA 读失败瞬间的 STA */

/* g_sd_test 的取值 = 模式（写进 g_sd_test 即触发，跑完自动清 0）：
 *   1 = 完整自检（备份 → 单扇区写读比对 → 8 扇区写读比对 → 恢复）
 *   2 = **只恢复**：把内存里的 s_bak 写回 g_sd_sector（内存备份还在时用，最快）
 *   3 = **主机回填后恢复**：主机先把 4 KB 原内容写进 s_bak（地址读 g_sd_bak_addr）、
 *       把扇区号写进 g_sd_sector，再写 3 —— 用于内存备份已随复位丢失的情况
 *       （build/sd_bss_sd.bin 里存着板上缓冲的完整副本）。
 *       写完会读回比对，g_sd_restore_mis = 0 即卡内容已还原。
 *   4 = **IDMA 自检**（当前时钟）：备份 → IDMA 写图案 → IDMA 读回比对
 *       → 顺带验 L3（哨兵法，看 g_sd_dma_stale）→ 恢复原内容
 *   5 = **IDMA + 高速总线**（ClockDiv=2 ⇒ 50MHz）：先切 SDMMC_SPEED_MODE_HIGH，
 *       再跑与 4 完全相同的一组用例，用来量化"IDMA 在高速下的收益"；
 *       切速失败会记在 g_sd_speed_mode=2 并**退回 25MHz 继续跑**，不会卡死。 */
#define SD_TEST_FULL     1u
#define SD_TEST_RESTORE  2u
#define SD_TEST_HOSTFILL 3u
#define SD_TEST_DMA      4u
#define SD_TEST_DMA_HS   5u

/* ---- 驱动接口 ---- */

/* 初始化 SDMMC1 + 识别卡。没插卡/识别失败返回非 0，并把原因记进 g_sd_init_rc。
 * 可在开机时调用一次；失败不影响任何其他功能。 */
int sd_card_init(void);

/* 自检入口（挂在主循环，写 g_sd_test=1 触发）。跑完自动清 g_sd_test。 */
void sd_bench_poll(void);

/* 给后续 FatFs（L2-3）用的块读写接口；扇区地址 = 块地址（SDHC 用块寻址）。 */
int sd_read_blocks(uint32_t sector, uint32_t count, uint8_t *buf);
int sd_write_blocks(uint32_t sector, uint32_t count, const uint8_t *buf);
uint32_t sd_get_block_nbr(void);
uint32_t sd_get_block_size(void);

#endif /* SD_CARD_H */
