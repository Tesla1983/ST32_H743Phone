/* ===========================================================================
 * GB2312 字库灌入 QSPI —— 实现
 *
 * 错误码（g_font_rc）：
 *   0xFF..FF 初始值（没跑）
 *   0        成功
 *   1        内嵌载荷大小与布局约定不符（说明 .bin 被换过却没同步 QSPI_FONT_BLOB_SIZE）
 *   2        读记账头失败（QSPI 通信问题）
 *   3        写数据失败
 *   4        写记账头失败
 *   5        回读比对失败（g_font_mismatch > 0）
 * =========================================================================== */

#include "font_provision.h"
#include "qspi_port.h"

#include <string.h>

#define FONT_MAGIC   0x47423233u    /* "GB23" */
#define FONT_HDR_LEN 64u            /* 一个扇区里只用前 64 字节 */

typedef struct
{
    uint32_t magic;
    uint32_t size;
    uint32_t crc;
    uint32_t reserved[13];          /* 凑满 64 B，给将来留位 */
} FontHeader;

volatile uint32_t g_font_ok        = 0;
volatile uint32_t g_font_written   = 0;
volatile uint32_t g_font_rc        = 0xFFFFFFFFu;
volatile uint32_t g_font_src_size  = 0;
volatile uint32_t g_font_crc_hdr   = 0;
volatile uint32_t g_font_crc_calc  = 0;
volatile uint32_t g_font_mismatch  = 0;
volatile uint32_t g_font_erase_cnt = 0;
volatile uint32_t g_font_cmp_bytes = 0;

static int s_force;

void font_provision_set_force(int force)
{
    s_force = force;
}

/* ---- CRC32（IEEE 802.3，反射，多项式 0xEDB88320）----
 * 用 16 项半字节表：比全 256 项表省 960 B rodata，速度约为逐位法的 4 倍。
 * 结果与 Python 的 zlib.crc32() 一致（已用 982016 B 的字库交叉核对过）。 */
static const uint32_t s_crc_nib[16] =
{
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
    }
    return crc;
}

/* 从 XIP（memory-mapped）读回算 CRC，用来复核记账头。
 * 调用前必须已进入映射模式 —— 见 font_provision_ensure()。 */
static uint32_t crc32_of_xip(uint32_t off, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)(QSPI_XIP_BASE + off);
    uint32_t       crc = 0xFFFFFFFFu;
    uint32_t       left = n;

    while (left > 0u)
    {
        uint32_t step = (left > 4096u) ? 4096u : left;

        crc = crc32_update(crc, p, step);
        p += step;
        left -= step;
    }
    return crc ^ 0xFFFFFFFFu;
}

int font_provision_ensure(void)
{
    const uint8_t *src;
    uint32_t       src_size;
    FontHeader     hdr;
    int            rc;

    g_font_ok        = 0;
    g_font_written   = 0;
    g_font_mismatch  = 0;
    g_font_erase_cnt = 0;
    g_font_cmp_bytes = 0;

    src      = _binary_gb2312_bin_start;
    src_size = (uint32_t)(_binary_gb2312_bin_end - _binary_gb2312_bin_start);
    g_font_src_size = src_size;

    /* 载荷大小必须与布局约定一致：第 i 字在 i×128，整块放在字模区起始。 */
    if (src_size != QSPI_FONT_BLOB_SIZE)
    {
        g_font_rc = 1;
        return 1;
    }

    /* ---- 1) 读记账头 ---- */
    rc = qspi_read((uint8_t *)&hdr, QSPI_HOUSEKEEP_ADDR, (uint32_t)sizeof(hdr));
    if (rc != QSPI_OK)
    {
        g_font_rc = 2;
        return 2;
    }
    g_font_crc_hdr = hdr.crc;

    /* ---- 2) 头匹配就复核 CRC，通过则跳过（省 Flash 寿命）---- */
    if ((s_force == 0) && (hdr.magic == FONT_MAGIC) && (hdr.size == src_size))
    {
        if (qspi_enter_mmap() == QSPI_OK)
        {
            g_font_crc_calc = crc32_of_xip(0u, src_size);
            if (g_font_crc_calc == hdr.crc)
            {
                g_font_ok = 1;
                g_font_rc = 0;
                return 0;
            }
        }
    }

    /* ---- 3) 需要写：擦 + 写数据（qspi_write 内部按扇区做读-改-擦-写回）---- */
    g_font_written = 1;
    {
        uint32_t erase_before = g_qspi_erase_cnt;

        rc = qspi_write(src, 0u, src_size);
        g_font_erase_cnt = g_qspi_erase_cnt - erase_before;   /* 本次灌库擦了多少扇区 */
    }
    if (rc != QSPI_OK)
    {
        g_font_rc = 3;
        return 3;
    }

    /* ---- 4) 写记账头（CRC 由**源载荷**算出；跳过分支用它复核 QSPI 内容）---- */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FONT_MAGIC;
    hdr.size  = src_size;
    hdr.crc   = crc32_update(0xFFFFFFFFu, src, src_size) ^ 0xFFFFFFFFu;
    g_font_crc_hdr = hdr.crc;

    rc = qspi_write((const uint8_t *)&hdr, QSPI_HOUSEKEEP_ADDR, (uint32_t)sizeof(hdr));
    if (rc != QSPI_OK)
    {
        g_font_rc = 4;
        return 4;
    }

    /* ---- 5) 回读复核：进映射后**逐字节**比对（比 CRC 更强，能看到具体差在哪）---- */
    if (qspi_enter_mmap() != QSPI_OK)
    {
        g_font_rc = 5;
        return 5;
    }
    {
        const uint8_t *xip = (const uint8_t *)QSPI_XIP_BASE;
        uint32_t       i;

        for (i = 0; i < src_size; i++)
        {
            if (xip[i] != src[i])
            {
                g_font_mismatch++;
            }
        }
        g_font_cmp_bytes = src_size;
        g_font_crc_calc  = crc32_of_xip(0u, src_size);
    }

    if (g_font_mismatch != 0u)
    {
        g_font_rc = 5;
        return 5;
    }

    g_font_ok = 1;
    g_font_rc = 0;
    return 0;
}
