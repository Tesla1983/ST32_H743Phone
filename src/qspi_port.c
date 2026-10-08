/* ===========================================================================
 * 阶段 4：W25Q128 QUADSPI 接入实现（indirect + memory-mapped XIP）
 *
 * 命令层与早期 Rust 参照工程的 norflash 驱动同源（那套在同一块板上
 * 实测通过），这里按本工程的 C + HAL 环境重写，并把计划书 §6.2 的三个坑逐条落实。
 *
 * ★计划书 §6.2 的三个必踩坑，逐条对应到本文件的实现★
 *   ① 片选必须自己配：`qspi_pins_init()` 里显式配 PB10 → AF9。
 *   ② 数据寄存器按 8 位访问：`qspi_read()` / `qspi_write_data()` 用
 *      `*(volatile uint8_t *)&QUADSPI->DR`，绝不用 32 位访问。
 *   ③ 事务一次配完，顺序 DLR → CCR → AR：见 `qspi_config_xfer()`。
 *
 * 额外纪律（与 FMC/Touch 一致）：**所有等待都有上限**。厂商源码里是死等，
 * 片选悬空/芯片虚焊时 BUSY 永不清、FIFO 永远空，会把固件永久卡在初始化里
 * —— 外部表现和"板子坏了"一模一样。这里一律有超时，失败往上返回错误码。
 * =========================================================================== */

#include "qspi_port.h"
#include "stm32h7xx_hal.h"

/* ===================== 指令表（厂商 norflash.h 原样）===================== */
#define CMD_WRITE_ENABLE      0x06u
#define CMD_WRITE_DISABLE     0x04u
#define CMD_READ_SR1          0x05u
#define CMD_READ_SR2          0x35u
#define CMD_WRITE_SR1         0x01u
#define CMD_WRITE_SR2         0x31u
#define CMD_FAST_READ_QUAD    0xEBu
#define CMD_PAGE_PROGRAM_QUAD 0x32u
#define CMD_SECTOR_ERASE      0x20u
#define CMD_MANUF_DEVICE_ID   0x90u
#define CMD_JEDEC_ID          0x9Fu
#define CMD_EXIT_QPI          0xFFu
#define CMD_READ_DATA         0x03u    /* 单线读，链路体检用 */

/* ===================== 位域（照 CMSIS stm32h743xx.h 核对）=====================
 * QUADSPI_CCR：INSTRUCTION[7:0] IMODE[9:8] ADMODE[11:10] ADSIZE[13:12]
 *              ABMODE[15:14] ABSIZE[17:16] DCYC[22:18](5 位) DMODE[25:24]
 *              FMODE[27:26] DHHC[30] DDRM[31]
 * QUADSPI_DCR：CKMODE[0] CSHT[10:8] FSIZE[20:16]
 * QUADSPI_CR ：EN[0] ABORT[1] TCEN[3] SSHIFT[4] DFM[6] FTHRES[12:8] PRESCALER[31:24]
 * QUADSPI_SR ：TCF[1] BUSY[5] FLEVEL[13:8]
 * QUADSPI_FCR：CTCF[1] 等
 * ===================================================================== */

/* CR：使能 + 采样移半周期(SSHIFT) + FIFO 阈值 4 + 预分频 1。
 * 预分频 1 ⇒ QSPI CLK = qspi_ker_ck / (1+1) = HCLK3(200MHz)/2 = 100MHz
 * （W25Q128 上限 133MHz；厂商例程同值）。
 * 注意：RCC_D1CCIPR.QUADSPISEL 复位值是 0 = hclk3，本工程没改过它，
 *       所以 qspi_ker_ck = HCLK3 = 200MHz。运行时把该字段读进 g_qspi_ker_sel 复核。 */
#define QSPI_CR_VAL   (QUADSPI_CR_EN | (1u << QUADSPI_CR_SSHIFT_Pos) | \
                       (4u << QUADSPI_CR_FTHRES_Pos) | (1u << QUADSPI_CR_PRESCALER_Pos))

/* DCR：CKMode=1(Mode3，空闲 CLK 高) + CSHT=5(片选高 5 周期) + FSIZE=23(2^24=16MB) */
#define QSPI_DCR_VAL  (QUADSPI_DCR_CKMODE | (5u << QUADSPI_DCR_CSHT_Pos) | \
                       (23u << QUADSPI_DCR_FSIZE_Pos))

/* 超时上限（@400MHz 这类循环约 4 周期一次）
 *   快：≈17ms，够跑完任意一条 QSPI 事务
 *   忙：≈830ms，覆盖扇区擦除（典型 45ms，最大 400ms） */
#define WAIT_FAST_LOOPS  2000000u
#define WAIT_BUSY_LOOPS  100000000u

#define PAGE_SIZE        256u
#define SECTOR_SIZE      4096u
#define ADDR_WIDTH_24BIT 2u          /* ADSIZE 编码：2 = 24 位 */

/* 验收用的抽样块大小 / 块数：全 16MB 覆盖，每块首 256B → 共比对 64KB */
#define VERIFY_CHUNK     256u
#define VERIFY_BLOCK     0x10000u    /* 64KB */
#define VERIFY_BLOCKS    (QSPI_XIP_SIZE / VERIFY_BLOCK)   /* = 256 */

/* 擦写回环落点：字体区尾部的空闲扇区（见 qspi_port.h 的布局说明）。
 * ⚠ 特意**不用** §6.4 指定的"最后扇区 0x00FFF000" —— 那里计划书明确要求留空
 *   （早期参照工程的自检在用），本工程遵循该约定避开。 */
#define VERIFY_LOOP_ADDR QSPI_VERIFY_LOOP_ADDR

/* ===================== 诊断量 ===================== */
volatile uint32_t g_qspi_rc           = 99;
volatile uint32_t g_qspi_jedec        = 0;
volatile uint32_t g_qspi_id90         = 0;
volatile uint32_t g_qspi_sr_snap      = 0;
volatile uint32_t g_qspi_cr           = 0;
volatile uint32_t g_qspi_dcr          = 0;
volatile uint32_t g_qspi_ker_sel      = 0xFFFFFFFFu;
volatile uint32_t g_qspi_mm_ok        = 0;
volatile uint32_t g_qspi_verify_rc    = 0xFFFFFFFFu;
volatile uint32_t g_qspi_cmp_blocks   = 0;
volatile uint32_t g_qspi_cmp_bytes    = 0;
volatile uint32_t g_qspi_cmp_mismatch = 0;
volatile uint32_t g_qspi_first_bad    = 0xFFFFFFFFu;
volatile uint32_t g_qspi_loop_ok      = 0;
volatile uint32_t g_qspi_loop_w0      = 0;
volatile uint32_t g_qspi_loop_r0      = 0;
volatile uint32_t g_qspi_loop_x0      = 0;
volatile uint32_t g_qspi_erase_cnt    = 0;
volatile uint32_t g_qspi_busy_snap    = 0;
volatile uint32_t g_qspi_err_cnt      = 0;

/* 读写回环共用的两块 256B 缓冲 + 整扇区暂存（写前"读-改-擦-写回"要用） */
static uint8_t s_ind[VERIFY_CHUNK];
static uint8_t s_xip[VERIFY_CHUNK];
static uint8_t s_sector[SECTOR_SIZE];

/* 当前是否处于 memory-mapped 模式。indirect 操作前会自动退出映射，
 * 避免"映射模式下发 indirect 命令"这种必错的用法。 */
static uint32_t s_in_mmap = 0;

/* ===================== 引脚与时钟 ===================== */

static void qspi_pins_init(void)
{
    GPIO_InitTypeDef g;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF9_QUADSPI;

    /* CLK = PB2, NCS = PB10
     * ★坑 ①★ NCS 就是这里配的 —— HAL 的 QSPI 初始化**不含片选**，
     *   漏掉它片选悬空，读回永远是 0xFF。 */
    g.Pin = GPIO_PIN_2 | GPIO_PIN_10;
    HAL_GPIO_Init(GPIOB, &g);

    /* IO0 = PD11, IO1 = PD12, IO3 = PD13 */
    g.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOD, &g);

    /* IO2 = PE2 */
    g.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOE, &g);
}

/* ===================== 收发三件套 ===================== */

static int qspi_wait_idle(void)
{
    uint32_t guard = WAIT_FAST_LOOPS;

    while ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0u)
    {
        if (--guard == 0u)
        {
            g_qspi_busy_snap = QUADSPI->SR;
            return QSPI_ERR_COMM;
        }
    }
    return QSPI_OK;
}

static int qspi_wait_done(void)
{
    uint32_t guard = WAIT_FAST_LOOPS;

    while ((QUADSPI->SR & QUADSPI_SR_TCF) == 0u)
    {
        if (--guard == 0u)
        {
            g_qspi_busy_snap = QUADSPI->SR;
            return QSPI_ERR_COMM;
        }
    }
    QUADSPI->FCR = QUADSPI_FCR_CTCF;
    return QSPI_OK;
}

/* 超时收尾：记录 SR 并中止事务。不中止的话外设会一直 BUSY，后面每步都跟着超时。 */
static void qspi_unwedge(void)
{
    uint32_t guard = WAIT_FAST_LOOPS;

    g_qspi_busy_snap = QUADSPI->SR;
    QUADSPI->CR |= QUADSPI_CR_ABORT;
    while (((QUADSPI->SR & QUADSPI_SR_BUSY) != 0u) || ((QUADSPI->CR & QUADSPI_CR_ABORT) != 0u))
    {
        if (--guard == 0u)
        {
            break;
        }
    }
    QUADSPI->FCR = QUADSPI_FCR_CTCF | QUADSPI_FCR_CTEF | QUADSPI_FCR_CSMF | QUADSPI_FCR_CTOF;
}

/**
 * 配置并启动一次事务 —— **一次写完，顺序 DLR → CCR → AR**（★坑 ③★）。
 *
 * 照 HAL 的 QSPI_Config()：先把长度写进 DLR，再写 CCR（含 FMODE，**这一写就启动事务**），
 * 有地址阶段的话最后写 AR。
 * 早先把"配命令"和"配长度"拆成两步，结果是**所有不带地址的事务都通、带 24 位地址的
 * 全不启动**（超时瞬间 SR 里 BUSY=0 且 TCF=0）。
 *
 * mode 位域：bit[1:0] 指令线数 / bit[3:2] 地址线数 / bit[5:4] 地址位宽 / bit[7:6] 数据线数
 * len  = 数据阶段字节数（0 = 无数据阶段，此时不动 DLR）
 * fmode= 0b00 间接写 / 0b01 间接读
 */
static void qspi_config_xfer(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle,
                             uint32_t fmode, uint32_t len)
{
    if (len > 0u)
    {
        QUADSPI->DLR = len - 1u;
    }

    QUADSPI->CCR = (fmode                     << QUADSPI_CCR_FMODE_Pos)     |
                   ((uint32_t)((mode >> 6) & 0x03u) << QUADSPI_CCR_DMODE_Pos)    |
                   ((uint32_t)(dmcycle & 0x1Fu)     << QUADSPI_CCR_DCYC_Pos)     |
                   ((uint32_t)((mode >> 4) & 0x03u) << QUADSPI_CCR_ADSIZE_Pos)   |
                   ((uint32_t)((mode >> 2) & 0x03u) << QUADSPI_CCR_ADMODE_Pos)   |
                   ((uint32_t)((mode >> 0) & 0x03u) << QUADSPI_CCR_IMODE_Pos)    |
                   ((uint32_t)cmd);

    if (((mode >> 2) & 0x03u) != 0u)
    {
        QUADSPI->AR = addr;
    }
}

/* 只发命令、无数据阶段。无数据阶段时配置写完事务立即启动，所以要等 TC 并清标志。 */
static int qspi_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle)
{
    int rc = qspi_wait_idle();
    if (rc != QSPI_OK) { qspi_unwedge(); return rc; }

    qspi_config_xfer(cmd, addr, mode, dmcycle, 0x0u, 0u);

    rc = qspi_wait_done();
    if (rc != QSPI_OK) { qspi_unwedge(); }
    return rc;
}

/* 发命令并读 len 字节。 */
static int qspi_read_data(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle,
                          uint8_t *buf, uint32_t len)
{
    uint32_t i;
    volatile uint8_t *dr8 = (volatile uint8_t *)&QUADSPI->DR;
    int rc = qspi_wait_idle();

    if (rc != QSPI_OK) { qspi_unwedge(); return rc; }

    /* FMODE=01 间接读，这一写就启动事务 */
    qspi_config_xfer(cmd, addr, mode, dmcycle, 0x1u, len);

    /* ★坑 ②★ 必须按 8 位访问 DR。FIFO 是按访问宽度弹数据的：
     * 一次 32 位读会弹走 4 个字节而我只取 1 个，剩下 3 个丢掉，
     * 现象是"第一个字节对、后面全超时"。 */
    for (i = 0; i < len; i++)
    {
        uint32_t guard = WAIT_FAST_LOOPS;

        while ((QUADSPI->SR & QUADSPI_SR_FLEVEL) == 0u)   /* FIFO 里有货才读 */
        {
            if (--guard == 0u)
            {
                qspi_unwedge();
                return QSPI_ERR_COMM;
            }
        }
        buf[i] = *dr8;
    }

    rc = qspi_wait_done();
    if (rc != QSPI_OK) { qspi_unwedge(); }
    return rc;
}

/* 发命令并写 len 字节（长度由 DLR 决定，中途不能改，所以整段一次配好）。 */
static int qspi_write_data(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle,
                           const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    volatile uint8_t *dr8 = (volatile uint8_t *)&QUADSPI->DR;
    int rc = qspi_wait_idle();

    if (rc != QSPI_OK) { qspi_unwedge(); return rc; }

    qspi_config_xfer(cmd, addr, mode, dmcycle, 0x0u, len);

    /* 同理按 8 位写：32 位写会一次塞 4 个字节，长度就不对了 */
    for (i = 0; i < len; i++)
    {
        uint32_t guard = WAIT_FAST_LOOPS;

        while ((QUADSPI->SR & QUADSPI_SR_FLEVEL) >= (32u << QUADSPI_SR_FLEVEL_Pos))
        {
            if (--guard == 0u)
            {
                qspi_unwedge();
                return QSPI_ERR_COMM;
            }
        }
        *dr8 = buf[i];
    }

    rc = qspi_wait_done();
    if (rc != QSPI_OK) { qspi_unwedge(); }
    return rc;
}

/* ===================== W25Q128 命令层 ===================== */

static uint8_t qspi_read_sr(uint8_t regno)
{
    uint8_t cmd = (regno == 2u) ? CMD_READ_SR2 : CMD_READ_SR1;
    uint8_t one = 0xFFu;

    /* 指令 1 线 + 无地址 + **数据 1 线**（DMODE = 0b01 → 模式码的 bit6 必须为 1）。
     * ⚠ 这里写错过一次：DMODE 落在 bit[7:6]，写成 (0u << 6) 就是"无数据阶段"，
     *   而函数却按"间接读 1 字节"配了 FMODE=01 + DLR=0 ⇒ 控制器等一个永远采不到的
     *   数据阶段，**BUSY 一直为 1、FLEVEL 一直为 0**，所有读状态/读 ID 全超时。
     *   现象与"片选悬空"一模一样，极易误判成硬件问题。 */
    if (qspi_read_data(cmd, 0u, (1u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u, &one, 1u) != QSPI_OK)
    {
        return 0xFFu;      /* 通信失败：数据线全高，语义一致 */
    }
    return one;
}

static int qspi_write_sr(uint8_t regno, uint8_t sr)
{
    uint8_t cmd = (regno == 2u) ? CMD_WRITE_SR2 : CMD_WRITE_SR1;

    if (qspi_send_cmd(CMD_WRITE_ENABLE, 0u, (0u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u) != QSPI_OK)
    {
        return QSPI_ERR_COMM;
    }
    /* 写状态寄存器：单线指令 + 无地址 + **单线数据**（同上，bit6 必须为 1） */
    return qspi_write_data(cmd, 0u, (1u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u, &sr, 1u);
}

static int qspi_wait_busy(void)
{
    uint32_t guard = WAIT_BUSY_LOOPS;

    while ((qspi_read_sr(1u) & 0x01u) == 0x01u)
    {
        if (--guard == 0u)
        {
            return QSPI_ERR_COMM;
        }
    }
    return (qspi_read_sr(1u) != 0xFFu) ? QSPI_OK : QSPI_ERR_COMM;
}

static int qspi_exit_qpi(void)
{
    /* 4 线发 0xFF。芯片若本来就工作在标准 SPI，这串在 DI 上是全 1 = 空操作，无害。 */
    return qspi_send_cmd(CMD_EXIT_QPI, 0u, (0u << 6) | (0u << 4) | (0u << 2) | (3u << 0), 0u);
}

/* 置 QE（状态寄存器 2 的 bit1）。不置这一位 IO2/IO3 不当数据线用，四线读必失败。 */
static int qspi_qe_enable(void)
{
    uint8_t sr1 = qspi_read_sr(1u);
    uint8_t sr2 = qspi_read_sr(2u);

    g_qspi_sr_snap = (uint32_t)sr1 | ((uint32_t)sr2 << 8);

    if ((sr1 == 0xFFu) && (sr2 == 0xFFu))
    {
        return QSPI_ERR_COMM;      /* 两条读状态都超时 ⇒ 链路不通 */
    }
    if ((sr2 & 0x02u) == 0u)
    {
        return qspi_write_sr(2u, (uint8_t)(sr2 | 0x02u));
    }
    return QSPI_OK;
}

/* 0x9F 读 JEDEC ID（3 字节，不需要地址）。比 0x90 更通用，做首选判据。 */
static uint32_t qspi_read_jedec(void)
{
    uint8_t b[3] = {0u, 0u, 0u};

    /* 单线指令 + 无地址 + **单线数据 3 字节**（bit6 必须为 1，理由同 read_sr） */
    if (qspi_read_data(CMD_JEDEC_ID, 0u, (1u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u, b, 3u) != QSPI_OK)
    {
        return 0u;
    }
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

/* 0x90 读厂商/器件 ID（2 字节，带 24 位地址 0）。厂商例程用的就是这条。 */
static uint16_t qspi_read_id90(void)
{
    uint8_t b[2] = {0u, 0u};

    if (qspi_read_data(CMD_MANUF_DEVICE_ID, 0u,
                       (1u << 6) | (2u << 4) | (1u << 2) | (1u << 0), 0u, b, 2u) != QSPI_OK)
    {
        return 0u;
    }
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

/* ===================== indirect 读写（对外）===================== */

static int qspi_ensure_indirect(void)
{
    if (s_in_mmap != 0u)
    {
        return qspi_exit_mmap();
    }
    return QSPI_OK;
}

int qspi_read(uint8_t *buf, uint32_t addr, uint32_t len)
{
    int rc;

    if ((buf == NULL) || (len == 0u) || (addr + len > QSPI_XIP_SIZE))
    {
        return QSPI_ERR_PARAM;
    }
    rc = qspi_ensure_indirect();
    if (rc != QSPI_OK) { return rc; }

    /* 0xEB 快速四线读：指令 1 线 + 地址 4 线(24 位) + 6 空周期 + 数据 4 线 */
    rc = qspi_read_data(CMD_FAST_READ_QUAD, addr,
                        (3u << 6) | (ADDR_WIDTH_24BIT << 4) | (3u << 2) | (1u << 0),
                        6u, buf, len);
    if (rc != QSPI_OK) { g_qspi_err_cnt++; }
    return rc;
}

int qspi_erase_sector(uint32_t sector)
{
    int rc;

    if (sector >= (QSPI_XIP_SIZE / SECTOR_SIZE)) { return QSPI_ERR_PARAM; }
    rc = qspi_ensure_indirect();
    if (rc != QSPI_OK) { return rc; }

    /* 指令 1 线 + 地址 1 线(24 位) + 无数据 */
    rc = qspi_send_cmd(CMD_WRITE_ENABLE, 0u, (0u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u);
    if (rc == QSPI_OK) { rc = qspi_wait_busy(); }
    if (rc == QSPI_OK)
    {
        rc = qspi_send_cmd(CMD_SECTOR_ERASE, sector * SECTOR_SIZE,
                           (0u << 6) | (ADDR_WIDTH_24BIT << 4) | (1u << 2) | (1u << 0), 0u);
    }
    if (rc == QSPI_OK) { rc = qspi_wait_busy(); }

    if (rc == QSPI_OK) { g_qspi_erase_cnt++; }
    return rc;
}

/* 页内写（<=256 B，不跨页） */
static int qspi_write_page(const uint8_t *buf, uint32_t addr)
{
    int rc = qspi_send_cmd(CMD_WRITE_ENABLE, 0u, (0u << 6) | (0u << 4) | (0u << 2) | (1u << 0), 0u);

    if (rc != QSPI_OK) { return rc; }

    /* 0x32 四线页写：指令 1 线 + 地址 4 线 + 数据 4 线，无空周期 */
    rc = qspi_write_data(CMD_PAGE_PROGRAM_QUAD, addr,
                         (3u << 6) | (ADDR_WIDTH_24BIT << 4) | (1u << 2) | (1u << 0), 0u, buf, PAGE_SIZE);
    if (rc != QSPI_OK) { return rc; }

    return qspi_wait_busy();
}

/* 自动换页的连续写（不带擦除） */
static int qspi_write_nocheck(const uint8_t *buf, uint32_t addr, uint32_t len)
{
    uint32_t done = 0u;

    while (done < len)
    {
        uint32_t page_left = PAGE_SIZE - ((addr + done) % PAGE_SIZE);
        uint32_t n = len - done;
        uint8_t page[PAGE_SIZE];
        uint32_t i;
        int rc;

        if (n > page_left) { n = page_left; }

        /* 页写固定按整页长度发（DLR 由数据阶段长度决定，这里补齐 0xFF） */
        for (i = 0u; i < PAGE_SIZE; i++)
        {
            page[i] = (i < n) ? buf[done + i] : 0xFFu;
        }
        rc = qspi_write_page(page, addr + done);
        if (rc != QSPI_OK) { return rc; }

        done += n;
    }
    return QSPI_OK;
}

/* 完整写：SPI Flash 只能把 1 写成 0，改任意字节必须整扇区"读-改-擦-写回"。 */
int qspi_write(const uint8_t *buf, uint32_t addr, uint32_t len)
{
    uint32_t a = addr;
    uint32_t rest = len;

    if ((buf == NULL) || (len == 0u) || (addr + len > QSPI_XIP_SIZE))
    {
        return QSPI_ERR_PARAM;
    }
    if (qspi_ensure_indirect() != QSPI_OK) { return QSPI_ERR_STATE; }

    while (rest > 0u)
    {
        uint32_t sector = a / SECTOR_SIZE;
        uint32_t off    = a % SECTOR_SIZE;
        uint32_t n      = SECTOR_SIZE - off;
        uint32_t i;
        int need_erase = 0;
        int rc;

        if (n > rest) { n = rest; }

        /* 先读整扇区（擦完就全是 0xFF 了，必须用擦除前这份，否则会丢掉扇区里其它数据） */
        rc = qspi_read(s_sector, sector * SECTOR_SIZE, SECTOR_SIZE);
        if (rc != QSPI_OK) { return rc; }

        for (i = 0u; i < n; i++)
        {
            if (s_sector[off + i] != buf[len - rest + i])
            {
                need_erase = 1;
                break;
            }
        }

        if (need_erase != 0)
        {
            for (i = 0u; i < n; i++)
            {
                s_sector[off + i] = buf[len - rest + i];
            }
            rc = qspi_erase_sector(sector);
            if (rc != QSPI_OK) { return rc; }
            rc = qspi_write_nocheck(s_sector, sector * SECTOR_SIZE, SECTOR_SIZE);
            if (rc != QSPI_OK) { return rc; }
        }
        /* 不需要擦除说明这段本来就是要写的内容，直接跳过 */

        a    += n;
        rest -= n;
    }
    return QSPI_OK;
}

/* ===================== memory-mapped（XIP）===================== */

int qspi_exit_mmap(void)
{
    uint32_t guard = WAIT_FAST_LOOPS;

    if (s_in_mmap == 0u)
    {
        return QSPI_OK;
    }

    /* CR.ABORT：中止当前（映射）事务并回到间接模式。
     * 它会自动清 0；等到它清掉且 BUSY 落下才算退出完成。 */
    QUADSPI->CR |= QUADSPI_CR_ABORT;
    while (((QUADSPI->CR & QUADSPI_CR_ABORT) != 0u) || ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0u))
    {
        if (--guard == 0u)
        {
            return QSPI_ERR_COMM;
        }
    }
    QUADSPI->FCR = QUADSPI_FCR_CTCF | QUADSPI_FCR_CTEF | QUADSPI_FCR_CSMF | QUADSPI_FCR_CTOF;
    s_in_mmap = 0u;
    return QSPI_OK;
}

int qspi_enter_mmap(void)
{
    uint32_t guard = WAIT_FAST_LOOPS;

    if (s_in_mmap != 0u)
    {
        return QSPI_OK;
    }

    /* XIP 区保持 NOT_CACHEABLE（见 `third_party/BSP/MPU/mpu.c` 里的实测记录：
     * 改成可缓存对帧率零收益，只会多背一层 cache 维护负担）。
     * 因此进入映射前不需要维护 D-Cache —— 每次读都真的落到 QSPI，
     * 这也是 A4「XIP 与 indirect 逐字节一致」能直接成立的前提。 */
    while ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0u)
    {
        if (--guard == 0u)
        {
            return QSPI_ERR_COMM;
        }
    }

    QUADSPI->DLR = 0u;                /* 映射模式不使用 DLR/AR */

    /* 一次写完 CCR（FMODE=0b11 这一写即进入映射模式）：
     *   沿用 0xEB 快读：指令 1 线 + 地址 4 线(24 位) + 6 空周期 + 数据 4 线
     * 位域逐位核对自 CMSIS：FMODE[27:26] DMODE[25:24] DCYC[22:18]
     *                        ADSIZE[13:12] ADMODE[11:10] IMODE[9:8] INSTRUCTION[7:0] */
    QUADSPI->CCR = (0x3u << QUADSPI_CCR_FMODE_Pos)     /* 0b11 = memory-mapped */
                 | (0x3u << QUADSPI_CCR_DMODE_Pos)     /* 0b11 = 四线数据        */
                 | (6u   << QUADSPI_CCR_DCYC_Pos)      /* 6 个空周期            */
                 | (0x2u << QUADSPI_CCR_ADSIZE_Pos)    /* 0b10 = 24 位地址      */
                 | (0x3u << QUADSPI_CCR_ADMODE_Pos)    /* 0b11 = 四线地址       */
                 | (0x1u << QUADSPI_CCR_IMODE_Pos)     /* 0b01 = 单线指令       */
                 | (uint32_t)CMD_FAST_READ_QUAD;

    s_in_mmap = 1u;
    return QSPI_OK;
}

/* ===================== 初始化 ===================== */

int qspi_port_init(void)
{
    uint16_t id90;
    uint32_t jedec;

    qspi_pins_init();

    __HAL_RCC_QSPI_CLK_ENABLE();      /* RCC_AHB3ENR.QSPIEN */

    /* 记录 QSPI 内核时钟选择（0 = hclk3，本工程未改过 → 200MHz，预分频 1 → 100MHz） */
    g_qspi_ker_sel = (RCC->D1CCIPR & RCC_D1CCIPR_QSPISEL_Msk) >> RCC_D1CCIPR_QSPISEL_Pos;

    QUADSPI->CR  = 0u;                /* 先关，保证配置从干净状态开始 */
    QUADSPI->DCR = QSPI_DCR_VAL;      /* CKMODE / CSHT / FSIZE(=23) */
    QUADSPI->CR  = QSPI_CR_VAL;       /* EN + SSHIFT + FTHRES + PRESCALER */

    g_qspi_cr  = QUADSPI->CR;
    g_qspi_dcr = QUADSPI->DCR;

    /* 退 QPI → 置 QE → 读 ID。任一步失败都记错误码返回，不把板子卡住。 */
    if (qspi_exit_qpi() != QSPI_OK)
    {
        g_qspi_rc = QSPI_ERR_COMM;
        return QSPI_ERR_COMM;
    }
    if (qspi_qe_enable() != QSPI_OK)
    {
        g_qspi_rc = QSPI_ERR_COMM;
        return QSPI_ERR_COMM;
    }

    jedec = qspi_read_jedec();
    id90  = qspi_read_id90();
    g_qspi_jedec = jedec;
    g_qspi_id90  = (uint32_t)id90;

    /* 0x9F 是 JEDEC 标准、跟型号无关，作首选；0x90 是厂商例程用法，作兜底。
     * 两条都不符才算失败。 */
    if ((jedec != QSPI_JEDEC_W25Q128) && ((uint32_t)id90 != (uint32_t)QSPI_W25Q128_ID))
    {
        g_qspi_rc = QSPI_ERR_ID;
        return QSPI_ERR_ID;
    }

    g_qspi_rc = QSPI_OK;
    return QSPI_OK;
}

/* ===================== 阶段 4 验收（计划书 §6.6）===================== */

int qspi_port_verify(void)
{
    uint32_t blk;
    uint32_t i;
    int rc;

    g_qspi_cmp_blocks   = 0u;
    g_qspi_cmp_bytes    = 0u;
    g_qspi_cmp_mismatch = 0u;
    g_qspi_first_bad    = 0xFFFFFFFFu;
    g_qspi_loop_ok      = 0u;

    if (qspi_exit_mmap() != QSPI_OK)
    {
        g_qspi_verify_rc = QSPI_ERR_COMM;
        return QSPI_ERR_COMM;
    }

    /* ---- ① 全地址空间抽样比对：每 64KB 块首 256B，XIP 与 indirect 逐字节比 ----
     * 每次进出映射都走一遍真实路径（§6.6 要求的"退出映射/重进映射"顺带被覆盖 256 次）。 */
    for (blk = 0u; blk < VERIFY_BLOCKS; blk++)
    {
        uint32_t off = blk * VERIFY_BLOCK;

        rc = qspi_read(s_ind, off, VERIFY_CHUNK);
        if (rc != QSPI_OK)
        {
            g_qspi_verify_rc = rc;
            return rc;
        }

        rc = qspi_enter_mmap();
        if (rc != QSPI_OK)
        {
            g_qspi_mm_ok = 0u;
            g_qspi_verify_rc = rc;
            return rc;
        }
        for (i = 0u; i < VERIFY_CHUNK; i++)
        {
            s_xip[i] = *(volatile uint8_t *)(QSPI_XIP_BASE + off + i);
        }
        rc = qspi_exit_mmap();
        if (rc != QSPI_OK)
        {
            g_qspi_mm_ok = 0u;
            g_qspi_verify_rc = rc;
            return rc;
        }

        for (i = 0u; i < VERIFY_CHUNK; i++)
        {
            g_qspi_cmp_bytes++;
            if (s_ind[i] != s_xip[i])
            {
                if (g_qspi_first_bad == 0xFFFFFFFFu)
                {
                    g_qspi_first_bad = off + i;
                }
                g_qspi_cmp_mismatch++;
            }
        }
        g_qspi_cmp_blocks++;
    }

    /* ---- ② 擦写回环：最后一个扇区（§6.4 列为"留空"）----
     * 擦 → 写图案 → indirect 读回比 → 退出映射再重进 → XIP 读回比。 */
    {
        uint8_t pat[VERIFY_CHUNK];
        uint8_t rb[VERIFY_CHUNK];
        uint32_t w0;
        uint32_t r0;
        uint32_t x0;
        int same_ind;
        int same_xip;

        for (i = 0u; i < VERIFY_CHUNK; i++)
        {
            pat[i] = (uint8_t)(0xA5u + (uint8_t)i);   /* 好认的花样 */
        }
        w0 = (uint32_t)pat[0] | ((uint32_t)pat[1] << 8) | ((uint32_t)pat[2] << 16) | ((uint32_t)pat[3] << 24);
        g_qspi_loop_w0 = w0;

        rc = qspi_write(pat, VERIFY_LOOP_ADDR, VERIFY_CHUNK);
        if (rc != QSPI_OK)
        {
            g_qspi_loop_ok = 3;                 /* 写阶段就失败了 */
            g_qspi_verify_rc = rc;
            return rc;
        }

        rc = qspi_read(rb, VERIFY_LOOP_ADDR, VERIFY_CHUNK);
        if (rc != QSPI_OK)
        {
            g_qspi_loop_ok = 4;                 /* 读阶段超时 */
            g_qspi_verify_rc = rc;
            return rc;
        }
        same_ind = 1;
        for (i = 0u; i < VERIFY_CHUNK; i++)
        {
            if (rb[i] != pat[i]) { same_ind = 0; }
        }
        r0 = (uint32_t)rb[0] | ((uint32_t)rb[1] << 8) | ((uint32_t)rb[2] << 16) | ((uint32_t)rb[3] << 24);
        g_qspi_loop_r0 = r0;

        /* 退出映射 → 重进映射 → 再用 XIP 读同一地址 */
        if (qspi_exit_mmap() != QSPI_OK) { g_qspi_verify_rc = QSPI_ERR_COMM; return QSPI_ERR_COMM; }
        if (qspi_enter_mmap() != QSPI_OK) { g_qspi_verify_rc = QSPI_ERR_COMM; return QSPI_ERR_COMM; }

        {
            volatile uint8_t *p = (volatile uint8_t *)(QSPI_XIP_BASE + VERIFY_LOOP_ADDR);
            for (i = 0u; i < VERIFY_CHUNK; i++)
            {
                s_xip[i] = p[i];
            }
        }
        same_xip = 1;
        for (i = 0u; i < VERIFY_CHUNK; i++)
        {
            if (s_xip[i] != pat[i]) { same_xip = 0; }
        }
        x0 = (uint32_t)s_xip[0] | ((uint32_t)s_xip[1] << 8) | ((uint32_t)s_xip[2] << 16) | ((uint32_t)s_xip[3] << 24);
        g_qspi_loop_x0 = x0;

        g_qspi_loop_ok = (same_ind != 0 && same_xip != 0) ? 1u : 2u;
    }

    /* 收尾：**停在 memory-mapped 模式**。
     * 这是本工程的稳态 —— XIP 区 0x9000_0000 就是给只读常量用的，
     * 停在映射模式才能直接 `*(const uint8_t*)0x90000000` 读。
     * 反过来若停在间接模式，对 0x9000_0000 的访问是未定义行为（可能返回垃圾）。
     * 需要擦写时不用手工退出：qspi_read / qspi_write / qspi_erase_sector
     * 都会经 qspi_ensure_indirect() 自动先退出映射。 */
    g_qspi_mm_ok = (qspi_enter_mmap() == QSPI_OK) ? 1u : 0u;
    if (g_qspi_mm_ok == 0u)
    {
        g_qspi_verify_rc = QSPI_ERR_COMM;
        return QSPI_ERR_COMM;
    }

    {
        int ok = (g_qspi_cmp_mismatch == 0u) && (g_qspi_loop_ok == 1u);
        g_qspi_verify_rc = ok ? QSPI_OK : QSPI_ERR_STATE;
        return g_qspi_verify_rc;
    }
}
