/* ===========================================================================
 * FatFs 移植层实现（设计理由见 fatfs_port.h 顶部）
 *
 * 两件事：
 *   1. diskio —— 把 FatFs 的块设备请求接到 src/sd_card.c 的轮询读写上；
 *   2. 文件系统自检 —— 建目录 / 写文件 / 读回比对 / 删除，全部给出整数结论。
 * =========================================================================== */

#include "fatfs_port.h"
/* stm32h7xx_hal.h 会带进 core_cm7.h —— DWT 周期计数器（本文件用它给整轮自检计时）
 * 就在那里声明。只引 ff.h / diskio.h 的话会报 'DWT' undeclared。 */
#include "stm32h7xx_hal.h"
#include "ff.h"
#include "diskio.h"
#include "sd_card.h"
#include <string.h>

/* ---- 诊断量 ---- */
volatile uint32_t g_fs_test       = 0;
volatile uint32_t g_fs_state      = 0;
volatile uint32_t g_fs_mount_rc   = 0xFFFFFFFFu;
volatile uint32_t g_fs_fstype     = 0;
volatile uint32_t g_fs_mkdir_rc   = 0xFFFFFFFFu;
volatile uint32_t g_fs_open_rc    = 0xFFFFFFFFu;
volatile uint32_t g_fs_write_rc   = 0xFFFFFFFFu;
volatile uint32_t g_fs_written    = 0;
volatile uint32_t g_fs_close_rc   = 0xFFFFFFFFu;
volatile uint32_t g_fs_size       = 0;
volatile uint32_t g_fs_open2_rc   = 0xFFFFFFFFu;
volatile uint32_t g_fs_read_rc    = 0xFFFFFFFFu;
volatile uint32_t g_fs_readn      = 0;
volatile uint32_t g_fs_mis        = 0xFFFFFFFFu;
volatile uint32_t g_fs_unlink_rc  = 0xFFFFFFFFu;
volatile uint32_t g_fs_free_kb    = 0;
volatile uint32_t g_fs_getfree_rc = 0xFFFFFFFFu;
volatile uint32_t g_fs_cyc        = 0;

/* FatFs 的工作对象。放 DTCM 的 .bss（轮询模式下没有 DMA 可达性要求）：
 *   FATFS 含 512 B 的扇区窗口 win[]，FIL 含 512 B 的私有读写窗口 buf[]。 */
static FATFS s_fs;
static FIL   s_fil;
static uint8_t s_wtext[FS_TEXT_BYTES];
static uint8_t s_rtext[FS_TEXT_BYTES];

/* ===========================================================================
 * diskio —— FatFs 的底层块设备接口
 * =========================================================================== */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0u)
    {
        return STA_NOINIT;
    }
    return (sd_get_block_nbr() != 0u) ? (DSTATUS)0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0u)
    {
        return STA_NOINIT;
    }
    if (sd_get_block_nbr() == 0u)
    {
        (void)sd_card_init();                 /* 允许"先开机后插卡" */
    }
    return (sd_get_block_nbr() != 0u) ? (DSTATUS)0 : STA_NODISK;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0u) || (buff == NULL) || (count == 0u))
    {
        return RES_PARERR;
    }
    return (sd_read_blocks((uint32_t)sector, (uint32_t)count, buff) == 0)
           ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0u) || (buff == NULL) || (count == 0u))
    {
        return RES_PARERR;
    }
    return (sd_write_blocks((uint32_t)sector, (uint32_t)count, buff) == 0)
           ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0u)
    {
        return RES_PARERR;
    }

    switch (cmd)
    {
    case CTRL_SYNC:
        /* 本工程的写是同步的（sd_write_blocks 等到卡回 TRANSFER 才返回），
         * 没有"待处理的写"需要冲刷。 */
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (buff == NULL) { return RES_PARERR; }
        *((LBA_t *)buff) = (LBA_t)sd_get_block_nbr();
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == NULL) { return RES_PARERR; }
        *((WORD *)buff) = (WORD)sd_get_block_size();
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* 擦除块大小：只被 f_mkfs 用（FF_USE_MKFS 已关），给 1 即可 */
        if (buff == NULL) { return RES_PARERR; }
        *((DWORD *)buff) = 1u;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

/* ===========================================================================
 * 自检
 * =========================================================================== */

/* 确定性文本：可打印 ASCII，便于在卡上用读卡器一眼看出对不对；
 * 内容随位置变化 ⇒ 任何"整段内容错位/重复"都会被逐字节比对抓出来。 */
static void build_text(void)
{
    static const char head[] =
        "YMGUI TF card filesystem self-test\r\n"
        "line format: %04u ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\n";
    uint32_t i, n;

    n = 0u;
    for (i = 0u; i < sizeof(head) - 1u && n < FS_TEXT_BYTES; i++)
    {
        s_wtext[n++] = (uint8_t)head[i];
    }
    while (n < FS_TEXT_BYTES)
    {
        /* 每行 64 字节，行首写入该行序号（十进制），其余是可打印字符 */
        char tmp[16];
        uint32_t k = 0u;
        uint32_t line = n / 64u;

        tmp[k++] = (char)('0' + ((line / 1000u) % 10u));
        tmp[k++] = (char)('0' + ((line / 100u) % 10u));
        tmp[k++] = (char)('0' + ((line / 10u) % 10u));
        tmp[k++] = (char)('0' + (line % 10u));
        tmp[k++] = ' ';
        for (i = 0u; i < k && n < FS_TEXT_BYTES; i++)
        {
            s_wtext[n++] = (uint8_t)tmp[i];
        }
        for (i = 0u; i < 58u && n < FS_TEXT_BYTES; i++)
        {
            s_wtext[n++] = (uint8_t)(0x21u + ((n * 7u + i * 13u) % 94u));
        }
        if (n < FS_TEXT_BYTES) { s_wtext[n++] = (uint8_t)'\r'; }
        if (n < FS_TEXT_BYTES) { s_wtext[n++] = (uint8_t)'\n'; }
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

static void run_fs_bench(void)
{
    FRESULT  fr;
    UINT     bw = 0u, br = 0u;
    DWORD    fre = 0u;
    FATFS   *fsp = NULL;
    uint32_t c0  = DWT->CYCCNT;

    g_fs_state = 0u;

    /* ---- 挂载 ---- */
    fr = f_mount(&s_fs, "0:", 1);          /* 1 = 立即挂载（不延迟） */
    g_fs_mount_rc = (uint32_t)fr;
    if (fr != FR_OK)
    {
        g_fs_state = 1u;
        g_fs_cyc   = DWT->CYCCNT - c0;
        return;
    }
    g_fs_fstype = (uint32_t)s_fs.fs_type;

    /* ---- 剩余空间 ---- */
    fr = f_getfree("0:", &fre, &fsp);
    g_fs_getfree_rc = (uint32_t)fr;
    if ((fr == FR_OK) && (fsp != NULL))
    {
        uint32_t csize = (fsp->csize != 0u) ? (uint32_t)fsp->csize : 1u;
        g_fs_free_kb = (uint32_t)(((uint64_t)fre * (uint64_t)csize * 512u) / 1024u);
    }

    /* ---- 建目录（已存在返回 FR_EXIST = 8，属正常） ---- */
    g_fs_mkdir_rc = (uint32_t)f_mkdir(FS_TEST_DIR);

    /* ---- 写文件 ---- */
    build_text();
    fr = f_open(&s_fil, FS_TEST_PATH, (BYTE)(FA_WRITE | FA_CREATE_ALWAYS));
    g_fs_open_rc = (uint32_t)fr;
    if (fr == FR_OK)
    {
        fr = f_write(&s_fil, s_wtext, FS_TEXT_BYTES, &bw);
        g_fs_write_rc = (uint32_t)fr;
        g_fs_written  = (uint32_t)bw;
        g_fs_size     = (uint32_t)f_size(&s_fil);   /* 必须在 close 之前读 */
        g_fs_close_rc = (uint32_t)f_close(&s_fil);
    }

    /* ---- 读回比对 ---- */
    memset(s_rtext, 0, FS_TEXT_BYTES);
    fr = f_open(&s_fil, FS_TEST_PATH, (BYTE)FA_READ);
    g_fs_open2_rc = (uint32_t)fr;
    if (fr == FR_OK)
    {
        fr = f_read(&s_fil, s_rtext, FS_TEXT_BYTES, &br);
        g_fs_read_rc = (uint32_t)fr;
        g_fs_readn   = (uint32_t)br;
        (void)f_close(&s_fil);
    }
    if ((g_fs_open2_rc == 0u) && (g_fs_read_rc == 0u))
    {
        uint32_t n = (g_fs_readn < FS_TEXT_BYTES) ? g_fs_readn : FS_TEXT_BYTES;
        g_fs_mis = count_diff(s_wtext, s_rtext, n);
        if (n != FS_TEXT_BYTES)
        {
            g_fs_mis += (FS_TEXT_BYTES - n);        /* 读少了也算失配 */
        }
    }

    /* ---- 清理：删掉测试文件（目录保留，给后续持久化用） ---- */
    g_fs_unlink_rc = (uint32_t)f_unlink(FS_TEST_PATH);

    g_fs_cyc   = DWT->CYCCNT - c0;
    g_fs_state = 2u;
}

void fatfs_bench_poll(void)
{
    if (g_fs_test == 0u)
    {
        return;
    }
    g_fs_test = 0u;
    run_fs_bench();
}

int fatfs_ensure_mounted(void)
{
    /* 已挂载：s_fs.fs_type 非 0（FatFs 挂载成功后会填它），直接复用。
     * 未挂载：按 run_fs_bench 同样的参数挂一次（"0:" + 立即挂载）。 */
    if (s_fs.fs_type != 0u)
    {
        return 0;
    }
    return (int)f_mount(&s_fs, "0:", 1);
}
