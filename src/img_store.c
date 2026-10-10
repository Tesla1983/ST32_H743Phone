/* ===========================================================================
 * 图库实现（说明见 img_store.h 顶部，尤其是 BMP 行序与 XIP 冲突那两节）
 *
 * 判据（tools/img_check.py 会自动比，都是整数结论）：
 *   ① 导入：g_img_rc == 0，g_img_out_bytes == out_w*out_h*2
 *   ② 内容：XIP 上读回的 RGB565 与「主机侧独立算出的参考」逐字节失配 == 0
 *   ③ 索引：g_img_count 增加 1，槽位 off/len/w/h 与导入结果一致
 *   ④ XIP 探针：g_img_xip_fault == 0（退出映射期间读 XIP 没炸）
 * =========================================================================== */

#include "img_store.h"

#include "ff.h"
#include "diskio.h"
#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_jpeg.h"
#include "qspi_port.h"
#include "fatfs_port.h"
#include "board_fault.h"
#include <string.h>

/* 前置声明：img_store_poll 的 mode 6（笔记往返自检）位于本文件较后处，
 * 但 poll 在它前面 ⇒ 必须在这里先声明，否则编译器在 poll 里见不到。 */
int BoardNote_Save(int i, const char* text);
int BoardNote_Load(int i, char* out, int n);

/* ---- 失败返回码（g_img_rc）---- */
#define RC_FS_MOUNT    1u
#define RC_OPEN        2u
#define RC_READ_HDR    3u
#define RC_NOT_BMP     4u
#define RC_BPP         5u
#define RC_COMPRESS    6u
#define RC_TOO_WIDE    7u
#define RC_NO_SPACE    8u
#define RC_NO_SLOT     9u
#define RC_QSPI        10u
#define RC_PARAM       11u
#define RC_READ_ROW    12u
#define RC_WRITE_FILE  13u
#define RC_CLOSE       14u
#define RC_JPEG_DEC    15u   /* JPEG：HAL_JPEG_Decode 没返回 HAL_OK */
#define RC_JPEG_SHORT  16u   /* JPEG：解码出的字节数与头里声明的宽高不一致（多半是截断文件） */

/* ---- 步骤编号（g_img_step）---- */
#define STEP_MOUNT   1u
#define STEP_OPEN    2u
#define STEP_HDR     3u
#define STEP_PARSE   4u
#define STEP_ALLOC   5u
#define STEP_IMPORT  6u
#define STEP_INDEX   7u

#define IMG_BLK      4096u                 /* = W25Q128 一个扇区，qspi_write 的粒度 */
#define LINE_MAX     2048u                 /* 一行源数据上限（24bpp ⇒ 宽 ≤ 682） */

/* 块缓冲与行缓冲放 SRAM1（非缓存区，链接脚本 .bss_img）。
 * 放非缓存区不是因为现在需要（QSPI 是 CPU 轮询 FIFO，没有 DMA 参与），
 * 而是跟 SD 侧保持同一条规矩：**凡是给外设当缓冲的内存一律非缓存**，
 * 免得哪天改成 MDMA 时静默踩坑。 */
__attribute__((section(".bss_img"), aligned(32)))
static uint8_t s_blk[IMG_BLK];
__attribute__((section(".bss_img"), aligned(32)))
static uint8_t s_line[LINE_MAX];

static FIL s_fil;
static FIL s_mfil;              /* 写文件夹具专用（与导入用的 s_fil 分开，互不干扰） */

/* ---- 诊断量定义 ---- */
volatile uint32_t g_img_busy   = 0;
volatile uint32_t g_img_test   = 0;
volatile uint32_t g_img_rc     = 0xFFFFFFFFu;
volatile uint32_t g_img_step   = 0;
volatile uint32_t g_img_slot   = 0xFFFFFFFFu;
volatile uint32_t g_img_scale  = 1;
volatile uint32_t g_img_src_w  = 0;
volatile uint32_t g_img_src_h  = 0;
volatile uint32_t g_img_bpp    = 0;
volatile uint32_t g_img_out_w  = 0;
volatile uint32_t g_img_out_h  = 0;
volatile uint32_t g_img_out_bytes = 0;
volatile uint32_t g_img_used   = 0;
volatile uint32_t g_img_fs_rc  = 0xFFFFFFFFu;
volatile uint32_t g_img_qspi_rc = 0;
volatile uint32_t g_img_cyc    = 0;
volatile uint32_t g_img_count  = 0;

volatile uint32_t g_img_mk       = 0;
volatile uint32_t g_img_mk_len   = 0;
volatile uint32_t g_img_mk_rc    = 0xFFFFFFFFu;
volatile uint32_t g_img_mk_total = 0;

volatile uint32_t g_img_qdiag[8] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint32_t g_img_board_count = 0xFFFFFFFFu;
volatile uint32_t g_img_slot_snap[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};

volatile uint32_t g_img_xip_inmap    = 0;
volatile uint32_t g_img_xip_exited   = 0;
volatile uint32_t g_img_xip_restored = 0;
volatile uint32_t g_img_xip_fault    = 0;

char g_img_path[IMG_PATH_MAX] = "/YMGUI/pic.bmp";

/* ===========================================================================
 * 索引表（在 W25Q128 上，经 XIP 直读）
 * 每条 16 B，全部按**小端字节**手工编解码 —— 不用结构体映射，
 * 免得编译器对齐/填充把布局搞偏（flash 上的布局必须逐字节确定）。
 * =========================================================================== */

#define XIP_PTR(addr)  ((const uint8_t *)(0x90000000u + (uint32_t)(addr)))

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* ===========================================================================
 * 查询接口
 * =========================================================================== */

int img_store_count(void)
{
    uint32_t i, n = 0;

    for (i = 0u; i < IMG_MAX_SLOTS; i++)
    {
        const uint8_t *e = XIP_PTR(IMG_IDX_ADDR + i * IMG_ENTRY_BYTES);
        if ((rd16(e) == IMG_MAGIC) && ((rd16(e + 2u) & IMG_FLAG_VALID) != 0u))
        {
            n++;
        }
    }
    return (int)n;
}

int img_store_slot_info(uint32_t slot, uint32_t *off, uint32_t *len,
                        uint32_t *w, uint32_t *h)
{
    const uint8_t *e;

    if (slot >= IMG_MAX_SLOTS) { return 1; }
    e = XIP_PTR(IMG_IDX_ADDR + slot * IMG_ENTRY_BYTES);
    if ((rd16(e) != IMG_MAGIC) || ((rd16(e + 2u) & IMG_FLAG_VALID) == 0u))
    {
        return 2;
    }
    if (off) { *off = rd32(e + 4u); }
    if (len) { *len = rd32(e + 8u); }
    if (w)   { *w   = rd16(e + 12u); }
    if (h)   { *h   = rd16(e + 14u); }
    return 0;
}

const void *img_store_pixels(uint32_t slot)
{
    uint32_t off = 0;

    if (img_store_slot_info(slot, &off, NULL, NULL, NULL) != 0) { return NULL; }
    return (const void *)XIP_PTR(IMG_DATA_ADDR + off);
}

static uint32_t idx_used_in(const uint8_t *base);   /* 定义在下面（导入路径专用） */

/* 已用字节数（= 所有有效条目里 max(off+len)，再向上按 4 KB 对齐）
 *
 * ⚠ 导入路径**不要**用它：它会逐条读 XIP（256 次 AHB 事务），而紧接着就要
 * qspi_write（必须先退出映射）。实测（2026-10-08）导入时走这条路径会让
 * qspi_write 返回 QSPI_ERR_COMM（通信超时），而单独跑 QSPI 分步诊断每步都 OK
 * ⇒ 罪魁就是"退出映射前刚做过大量 XIP 读"。
 * 所以导入改用 idx_used_in() —— 在已经拷到 RAM 的索引副本上算。 */
static uint32_t used_bytes(void)
{
    return idx_used_in(XIP_PTR(IMG_IDX_ADDR));
}

static uint32_t idx_used_in(const uint8_t *base)
{
    uint32_t i, mx = 0;

    for (i = 0u; i < IMG_MAX_SLOTS; i++)
    {
        const uint8_t *e = base + i * IMG_ENTRY_BYTES;
        if ((rd16(e) == IMG_MAGIC) && ((rd16(e + 2u) & IMG_FLAG_VALID) != 0u))
        {
            uint32_t end = rd32(e + 4u) + rd32(e + 8u);
            if (end > mx) { mx = end; }
        }
    }
    return (mx + (IMG_BLK - 1u)) & ~(IMG_BLK - 1u);
}

/* ===========================================================================
 * XIP 冲突探针（g_img_test = 2）
 *
 * 要回答的问题：**退出 memory-mapped 之后，读 0x9000_0000 会发生什么**。
 * 这决定导入期间必须采取什么隔离措施（现在是"主循环跳过渲染+触摸"）。
 *
 * ⚠⚠ 2026-10-08 实测结论（推翻了最初的乐观判断）：
 *   退出映射后**单次**读 XIP 确实不会挂（返回锁存值），于是第一版探针得出"没事"；
 *   但随后在 indirect 模式下**连续读上百次**（扫描索引）就必挂：
 *     HardFault / CFSR=0x8200 / BFAR=0x90010000。
 *   ⇒ **探针的"没挂"是假阴性**。真正可靠的结论是：
 *     "退出映射后任何 XIP 读都不可信，且可能触发总线故障"，
 *     所以规矩是"写完立刻 enter_mmap"，而不是"反正读一下也没事"。
 *
 * 安全措施：
 *   · 探针期间 g_img_busy=1 ⇒ 主循环不渲染，不会有第二条路径同时读 XIP；
 *   · 退出前把 g_img_xip_exited 预置成 0xDEADDEAD。若读操作直接把内核打挂，
 *     这个标记就留在那里，配合 g_img_xip_fault 能判断"是挂在读上"；
 *   · 探针结束（含异常路径判断后）必须 enter_mmap，由 img_store_poll 末尾兜底。
 * =========================================================================== */

/* ★探针读哪个地址：必须是**已知非 0** 的位置，否则三个值都是 0、
 * 什么也测不出来（第一版读字模区首 0x000000 就踩了这个坑：那里正好是 0，
 * 三个采样值全是 0x00000000，看起来"没问题"其实是**假阴性**）。
 * IME 词典区首（偏移 0x300000）实测 = 0x504D4959（"YMIM"），是运行时真会读的
 * 区域之一 —— 拿它当探针，测的正是我们要保护的那种访问。 */
#define XIP_PROBE_OFF  0x00300000u

static void run_xip_probe(void)
{
    volatile uint32_t v;
    uint32_t          faults_before = g_fault.count;

    g_img_xip_inmap    = 0;
    g_img_xip_exited   = 0xDEADDEADu;
    g_img_xip_restored = 0;
    g_img_xip_fault    = 0;

    /* ① 正常映射中读一次（基准） */
    v = *(volatile uint32_t *)XIP_PTR(XIP_PROBE_OFF);
    g_img_xip_inmap = v;

    /* ② 退出映射后再读 —— 本探针要测的就是这一步 */
    (void)qspi_exit_mmap();
    v = *(volatile uint32_t *)XIP_PTR(XIP_PROBE_OFF);
    g_img_xip_exited = v;

    /* ③ 恢复映射后再读，验证能回到正常值 */
    (void)qspi_enter_mmap();
    v = *(volatile uint32_t *)XIP_PTR(XIP_PROBE_OFF);
    g_img_xip_restored = v;

    /* 有没有新增故障：比对故障快照的**累计次数**，比看 magic 更可靠
     * （magic 若是开机前就非 0，看它反而分不清是不是这次造成的）。 */
    g_img_xip_fault = (g_fault.count != faults_before) ? 1u : 0u;
}

/* ===========================================================================
 * BMP → RGB565 → W25Q128
 * =========================================================================== */

/* 24bpp：BGR 字节序；16bpp（BI_RGB）：RGB555，小端 u16 */
static uint16_t px_to_565(const uint8_t *p, uint32_t bpp)
{
    uint32_t r, g, b;

    if (bpp == 24u)
    {
        b = p[0]; g = p[1]; r = p[2];
        return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    /* 16bpp = RGB555：5 位扩到 565 的 6 位绿，用位复制 g<<1 | g>>4 更准 */
    {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        r = (v >> 10) & 0x1Fu;
        g = (v >> 5) & 0x1Fu;
        b = v & 0x1Fu;
        return (uint16_t)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
    }
}

static uint32_t import_bmp(void)
{
    uint8_t hdr[54];
    UINT    br = 0;
    FRESULT fr;
    uint32_t offbits, src_w, src_h, bpp, compress;
    uint32_t src_row_bytes, out_w, out_h, out_len, row_bytes;
    uint32_t slot, slot_off, nblk, used, scale;
    int      topdown;
    uint32_t k, i;

    /* ---- 1) 文件已挂载？ ---- */
    g_img_step = STEP_MOUNT;
    if (fatfs_ensure_mounted() != 0)
    {
        return RC_FS_MOUNT;
    }

    /* ---- 2) 打开 ---- */
    g_img_step = STEP_OPEN;
    g_img_path[IMG_PATH_MAX - 1u] = '\0';
    fr = f_open(&s_fil, g_img_path, FA_READ);
    g_img_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { return RC_OPEN; }

    /* ---- 3) 读头 ---- */
    g_img_step = STEP_HDR;
    fr = f_read(&s_fil, hdr, sizeof(hdr), &br);
    g_img_fs_rc = (uint32_t)fr;
    if ((fr != FR_OK) || (br != sizeof(hdr)))
    {
        (void)f_close(&s_fil);
        return RC_READ_HDR;
    }

    /* ---- 4) 解析 ---- */
    g_img_step = STEP_PARSE;
    if (!((hdr[0] == 'B') && (hdr[1] == 'M'))) { (void)f_close(&s_fil); return RC_NOT_BMP; }

    offbits  = rd32(hdr + 10u);
    src_w    = rd32(hdr + 18u);
    {
        int32_t hh = (int32_t)rd32(hdr + 22u);   /* 有符号：负数 = top-down */
        topdown = (hh < 0) ? 1 : 0;
        src_h   = (hh < 0) ? (uint32_t)(-hh) : (uint32_t)hh;
    }
    bpp      = rd16(hdr + 28u);
    compress = rd32(hdr + 30u);

    g_img_src_w = src_w;
    g_img_src_h = src_h;
    g_img_bpp   = bpp;

    scale = g_img_scale;
    if (!((scale == 1u) || (scale == 2u) || (scale == 4u))) { scale = 1u; }
    if ((src_w == 0u) || (src_h == 0u)) { (void)f_close(&s_fil); return RC_PARAM; }
    if (compress != 0u) { (void)f_close(&s_fil); return RC_COMPRESS; }
    if (!((bpp == 24u) || (bpp == 16u))) { (void)f_close(&s_fil); return RC_BPP; }

    src_row_bytes = ((src_w * (bpp / 8u)) + 3u) & ~3u;   /* BMP 每行按 4 字节对齐 */
    if (src_row_bytes > LINE_MAX) { (void)f_close(&s_fil); return RC_TOO_WIDE; }

    out_w = src_w / scale;
    out_h = src_h / scale;
    if ((out_w == 0u) || (out_h == 0u)) { (void)f_close(&s_fil); return RC_PARAM; }
    row_bytes = out_w * 2u;
    out_len   = row_bytes * out_h;
    nblk      = (out_len + (IMG_BLK - 1u)) / IMG_BLK;

    g_img_out_w     = out_w;
    g_img_out_h     = out_h;
    g_img_out_bytes = out_len;

    /* ---- 5) 分配槽位 ----
     * 【为什么不逐条读 XIP】见 idx_used_in 上方的注释：导入紧接着就要 qspi_write
     * （必须先退出映射），而"退出映射前刚做过大量 XIP 读"实测会让后续命令超时。
     * ⇒ 把整个索引扇区**一次**拷进 s_blk，在 RAM 副本上扫描：
     *   XIP 读从 256 次降到 1 次，且拷完立刻用 __DSB() 把总线上的读排空。
     *   （s_blk 后面还要当输出块缓冲用，所以扫描完必须先用完索引副本。） */
    g_img_step = STEP_ALLOC;
    memcpy(s_blk, (const void *)XIP_PTR(IMG_IDX_ADDR), IMG_BLK);
    __DSB();
    __ISB();
    used = idx_used_in(s_blk);
    if ((used + out_len) > IMG_DATA_SIZE) { (void)f_close(&s_fil); return RC_NO_SPACE; }

    for (slot = 0u; slot < IMG_MAX_SLOTS; slot++)
    {
        const uint8_t *e = s_blk + slot * IMG_ENTRY_BYTES;
        if (!((rd16(e) == IMG_MAGIC) && ((rd16(e + 2u) & IMG_FLAG_VALID) != 0u)))
        {
            break;
        }
    }
    if (slot >= IMG_MAX_SLOTS) { (void)f_close(&s_fil); return RC_NO_SLOT; }
    slot_off = used;
    g_img_slot = slot;

    /* ---- 6) 逐块导入 ----
     *
     * 【为什么输出块要从后往前写】BMP 是 bottom-up（文件第 0 行 = 图像最底行），
     * 而 GYimg 要 top-down。若按输出顺序正着写，源行就得倒着读 ⇒ 每次 f_lseek
     * 往回退，FatFs 会从文件头重走簇链 ⇒ 慢。
     * 反过来：**输出块从最后一个开始写**，则每个块对应的源行区间在文件里是递增的，
     * 块内也从后往前填行 ⇒ 源文件**全程顺序读**，一次回退 lseek 都不需要。
     * （top-down 的 BMP 正相反，所以下面按 topdown 决定遍历方向。）
     */
    g_img_step = STEP_IMPORT;

    /* ★把"退出映射"挪到写循环之外★
     * 之前是让 qspi_write 内部（ensure_indirect）自己去退，结果导入必然在
     * STEP_IMPORT 上返回 QSPI_ERR_COMM；而同样的写操作放在分步诊断里每步都成功。
     * 差别就在于：这里是**紧接着一次 4 KB 的 XIP memcpy + 几百次 f_read** 之后
     * 才第一次退出映射。先把映射显式退掉、并做一次探测读，把"模式切换"这个
     * 动作与"批量写"分开（诊断记录见 g_img_qdiag[3]/[4]）。 */
    {
        int q = qspi_exit_mmap();
        g_img_qdiag[3] = (uint32_t)q;
        q = qspi_read(s_line, IMG_DATA_ADDR + slot_off, 256u);   /* 探测读 */
        g_img_qdiag[4] = (uint32_t)q;
    }

    {
        uint32_t blk_first = topdown ? 0u : (nblk - 1u);
        int      bdir      = topdown ? 1 : -1;
        FSIZE_t  cur       = 0;                       /* 当前文件读位置 */
        int      seeked    = 0;

        for (k = 0u; k < nblk; k++)
        {
            uint32_t blk    = blk_first + (uint32_t)((int)k * bdir);
            uint32_t bstart = blk * IMG_BLK;
            uint32_t bend   = bstart + IMG_BLK;
            uint32_t r0, r1, nrows, ri;

            if (bend > out_len) { bend = out_len; }
            r0    = bstart / row_bytes;
            r1    = (bend - 1u) / row_bytes;
            nrows = r1 - r0 + 1u;

            for (ri = 0u; ri < nrows; ri++)
            {
                /* 行方向同样为"源行递增"服务：bottom-up 时 r 从 r1 递减到 r0 */
                uint32_t r         = topdown ? (r0 + ri) : (r1 - ri);
                uint32_t rowbyte0  = r * row_bytes;
                uint32_t lo        = (bstart > rowbyte0) ? bstart : rowbyte0;
                uint32_t hi        = (bend < (rowbyte0 + row_bytes))
                                     ? bend : (rowbyte0 + row_bytes);
                uint32_t c0        = (lo - rowbyte0) / 2u;
                uint32_t nc        = (hi - lo) / 2u;
                uint32_t src_row   = topdown ? (r * scale)
                                             : ((src_h - 1u) - r * scale);
                FSIZE_t  want      = (FSIZE_t)offbits +
                                     (FSIZE_t)src_row * (FSIZE_t)src_row_bytes;

                if ((!seeked) || (want != cur))
                {
                    fr = f_lseek(&s_fil, want);
                    g_img_fs_rc = (uint32_t)fr;
                    if (fr != FR_OK) { (void)f_close(&s_fil); return RC_READ_ROW; }
                    seeked = 1;
                }
                fr = f_read(&s_fil, s_line, src_row_bytes, &br);
                g_img_fs_rc = (uint32_t)fr;
                if ((fr != FR_OK) || (br != src_row_bytes))
                {
                    (void)f_close(&s_fil);
                    return RC_READ_ROW;
                }
                cur = want + (FSIZE_t)src_row_bytes;

                for (i = 0u; i < nc; i++)
                {
                    uint32_t   x  = c0 + i;
                    uint32_t   sx = x * scale;
                    uint16_t   v;
                    uint8_t   *d  = &s_blk[(lo - bstart) + (i * 2u)];

                    if (bpp == 24u) { v = px_to_565(&s_line[sx * 3u], 24u); }
                    else            { v = px_to_565(&s_line[sx * 2u], 16u); }
                    d[0] = (uint8_t)(v & 0xFFu);          /* 小端：MCU 读出来即正确 u16 */
                    d[1] = (uint8_t)((v >> 8) & 0xFFu);
                }
            }

            {
                int q = qspi_write(s_blk, IMG_DATA_ADDR + slot_off + bstart,
                                   bend - bstart);
                g_img_qspi_rc = (uint32_t)q;
                if (q != QSPI_OK)
                {
                    /* 记下失败发生在第几个块、写的哪个地址 */
                    g_img_qdiag[0] = k;
                    g_img_qdiag[1] = (uint32_t)q;
                    g_img_qdiag[2] = IMG_DATA_ADDR + slot_off + bstart;
                    (void)qspi_enter_mmap();
                    (void)f_close(&s_fil);
                    return RC_QSPI;
                }
                g_img_qdiag[0] = k + 1u;      /* 已成功写入的块数 */
            }
        }
    }
    (void)f_close(&s_fil);

    /* ---- 7) 写索引条目 ---- */
    g_img_step = STEP_INDEX;
    {
        uint8_t e[IMG_ENTRY_BYTES];
        int     q;

        memset(e, 0, sizeof(e));
        wr16(e + 0u,  IMG_MAGIC);
        wr16(e + 2u,  IMG_FLAG_VALID);
        wr32(e + 4u,  slot_off);
        wr32(e + 8u,  out_len);
        wr16(e + 12u, (uint16_t)out_w);
        wr16(e + 14u, (uint16_t)out_h);

        q = qspi_write(e, IMG_IDX_ADDR + slot * IMG_ENTRY_BYTES, IMG_ENTRY_BYTES);
        g_img_qspi_rc = (uint32_t)q;
        if (q != QSPI_OK) { return RC_QSPI; }
    }

    /* ★★ 写完必须**立刻**回到映射模式 ★★
     * qspi_write 内部会退出映射（ensure_indirect），而且**不会自己恢复** ——
     * 只有 qspi_erase_sector 会"擦完再把映射打开"。
     * 在 indirect 模式下读 XIP 窗口会触发**精确 BusFault**：
     *   实测 g_fault.kind=1(HardFault)、CFSR=0x8200(PRECISERR+BFARVALID)、
     *   BFAR=0x90010000 —— 正是下面 used_bytes() 要读的索引区 XIP 地址。
     *   此前"退出映射后读一次 XIP 没事"是**假阴性**：只读 1 次侥幸没挂，
     *   读 256 次（扫描索引）必挂。
     * ⇒ 这条规矩是硬的：**任何 indirect 写之后、下一次 XIP 读之前，必须 enter_mmap**。 */
    (void)qspi_enter_mmap();
    g_img_used = used_bytes();
    return 0u;
}

/* ===========================================================================
 * JPEG 硬解码导入（2026-10-09）—— "没有整帧内存"怎么做
 *
 * 【问题】相册面板 284×230 的 RGB565 全帧 = 130 640 B，而把整帧留在 RAM 里
 *   既不经济也不安全（还要留余量给别的分配）。
 *
 * 【解法：分块解码 + 边转边写】
 *   H743 自带 JPEG 编解码器，输出是**按 MCU 行**增量吐出的：
 *     4:2:0 → 一块 = 宽×16 行，YUV 共 24×宽 字节（厂商「实验30」实测公式）
 *     4:2:2 → 一块 = 宽×8 行，16×宽 字节
 *     4:4:4 → 一块 = 宽×8 行，24×宽 字节
 *   于是：一块 YUV 到手 → 立刻转 RGB565 → 攒进 4 KB 块 → 满了就写 W25Q128
 *   → 最后从 XIP 显示（零 RAM）。
 *   峰值 RAM ≈ 输入 4 KB + YUV 块 7.7 KB ≈ 12 KB，而不是 130 KB。
 *
 * 【为什么不用 MDMA / DMA2D】
 *   解码走 polling（HAL_JPEG_Decode），YCbCr→RGB565 由 CPU 做：65 k 像素
 *   约几毫秒，比再引 MDMA + DMA2D 两个外设（还要开中断槽位）划算得多。
 *
 * 【尺寸硬限制 —— 这也是"没有内存"的另一半答案】
 *   硬件**没有缩放**（JPEG CONFR0~7 无 SCALE 位，DMA2D 也不能缩放），
 *   降采样只能 CPU 做。⇒ 超过上限的图**直接拒绝**，不去换内存布局迁就它。
 *   真要显示大图，正确做法是在 PC 侧预缩到相册尺寸再放进卡里。
 * =========================================================================== */

#define JPEG_IN_BUF    4096u
#define JPEG_MAX_W     320u                  /* 宽度上限（相册面板 284 宽） */
#define JPEG_MAX_H     320u                  /* 高度上限：防止超大图占满 2 MB 图片区 */
#define JPEG_YUV_MAX   (24u * JPEG_MAX_W)    /* 一块 YUV 上限：24×宽（4:2:0/4:4:4） */
#define JPEG_TIMEOUT   3000u                 /* HAL_JPEG_Decode 的超时（ms） */

#define JPEG_SUB_444   0u
#define JPEG_SUB_420   1u
#define JPEG_SUB_422   2u

/* 路径是否以 .jpg / .jpeg 结尾（大小写不敏感）。
 * 放在这里而不是复用 ext_match：后者定义在文件更后面，而 poll 要用它。 */
static int path_is_jpeg(const char* p)
{
    int n = (int)strlen(p);
    int i;
    const char* tail;

    if (n < 4) { return 0; }
    tail = p + (n - 4);
    if (!((tail[0] == '.') && (tail[1] == 'j' || tail[1] == 'J'))) { return 0; }
    /* ".jpg" */
    if ((tail[2] == 'p' || tail[2] == 'P') && (tail[3] == 'g' || tail[3] == 'G')) { return 1; }
    /* ".jpeg"（末 5 字符） */
    if (n >= 5)
    {
        const char* t5 = p + (n - 5);
        if ((t5[0] == '.') && (t5[1] == 'j' || t5[1] == 'J') &&
            (t5[2] == 'p' || t5[2] == 'P') && (t5[3] == 'e' || t5[3] == 'E') &&
            (t5[4] == 'g' || t5[4] == 'G'))
        {
            return 1;
        }
    }
    (void)i;
    return 0;
}

/* 按**文件内容**的魔数判断是真 JPEG 还是 BMP。
 * 返回  1 = JPEG；0 = BMP；-1 = 读不出来/认不出（调用方应回退到看扩展名）。
 *
 * ⚠ 为什么不再只看扩展名（2026-10-10 实测：蓝牙收到的图进不了相册）：
 *   蓝牙那条路上，手机用通用串口助手发的是**裸字节流**，没有本工程的
 *   `YMFILE,<名>,<大小>` 头 ⇒ 对端把名字强制成 `btrecv.bin`，原始的
 *   `.jpg` / `.bmp` 后缀**全丢了**。只看扩展名的话，`.bin` 一律走 import_bmp，
 *   里面装的 JPEG 就被 import_bmp 用"不是 BM 签名"拒掉（RC_NOT_BMP）。
 *   ⇒ 改成按内容判：JPEG 的 SOI 是 FF D8，BMP 是 'BM'。
 *
 * ⚠ 必须用**独立的 FIL**：import_bmp / import_jpeg 自己要用全局 s_fil，
 *   这里若也用 s_fil 会把它们的文件句柄冲掉（本函数在它们**之前**调用）。
 *
 * ⚠ 只在这里读 2 字节然后立刻关掉，不持有句柄、不改动 g_img_step ——
 *   真正的导入随后由 import_bmp/import_jpeg 各自重新打开。
 */
static int sniff_image_kind(const char* p)
{
    FIL     f;
    UINT    br = 0;
    FRESULT fr;
    uint8_t sig[2];

    if (p == NULL || p[0] == 0) { return -1; }
    if (fatfs_ensure_mounted() != 0) { return -1; }

    fr = f_open(&f, p, FA_READ);
    if (fr != FR_OK) { return -1; }

    fr = f_read(&f, sig, sizeof(sig), &br);
    (void)f_close(&f);
    if ((fr != FR_OK) || (br != sizeof(sig))) { return -1; }

    if ((sig[0] == 0xFFu) && (sig[1] == 0xD8u)) { return 1; }   /* JPEG SOI  */
    if ((sig[0] == 'B')  && (sig[1] == 'M'))    { return 0; }   /* BMP 'BM'  */
    return -1;                                                  /* 其它：交给扩展名兜底 */
}

__attribute__((section(".bss_img"), aligned(32)))
static uint8_t s_jin[JPEG_IN_BUF];           /* 输入：从卡顺序读进来的 JPEG 字节流 */
__attribute__((section(".bss_img"), aligned(32)))
static uint8_t s_jyuv[JPEG_YUV_MAX];         /* 输出：硬件吐出的一块 YUV */

typedef struct
{
    FIL      f;
    int      eof;          /* 文件已读完 */
    uint32_t w, h;
    uint32_t sub;          /* JPEG_SUB_* */
    uint32_t blk_h;        /* 一块的行数（420 = 16，422/444 = 8） */
    uint32_t mcu_w;        /* 一个 MCU 的像素宽（420/422 = 16，444 = 8） */
    uint32_t per_mcu;      /* 一个 MCU 的字节数（420 = 384，422 = 256，444 = 192） */
    uint32_t yuv_size;
    uint32_t slot_off;     /* W25 数据区偏移（相对 IMG_DATA_ADDR） */
    uint32_t written;      /* 已写入 W25 的字节数 */
    uint32_t row;          /* 已输出到第几行（0-based） */
    uint32_t blk_n;        /* s_blk 里攒的字节数（复用 BMP 导入那块 4 KB） */
    int      qspi_err;
} JpegCtx;

static JpegCtx s_j;

volatile uint32_t g_jpeg_diag[8] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint32_t g_jpeg_swap    = 0;
volatile uint32_t g_jpeg_dbg     = 0;

/* 中断路径追踪（2026-10-09）—— 一次烧录 3.5 分钟，所以一次把该看的都记下来：
 * [0] 进 JPEG_IRQHandler 的次数      [1] GetDataCallback 次数
 * [2] DataReadyCallback 次数         [3] ISR 里见到的 SR 位（累积）
 * [4] 等待循环里见到的 SR 位（累积）  [5] hjpeg.State
 * [6] hjpeg.ErrorCode                [7] Init 返回 | (Decode_IT 返回 << 8) */
volatile uint32_t g_jpeg_trace[8] = {0, 0, 0, 0, 0, 0, 0, 0};

/* ⚠ g_jpeg_dbg=1 时的**首块 YUV 快照**：色度平面的排列只能靠板上实测的
 * 原始字节来定（猜一次错一次，一次烧录 3.5 分钟）。脚本用 nm 找符号地址后
 * 直接整块读走，与主机侧的 YCbCr 参考对账。产品路径不碰它。 */
__attribute__((section(".bss_img"), aligned(32)))
static uint8_t s_jyuv_snap[JPEG_YUV_MAX];

/* JPEG 句柄与完成标志必须是**文件级**：ISR 要用句柄、回调要置标志。
 * ⚠ 为什么要走中断：HAL_JPEG_Decode(polling) 实测**超时**（g_img_qdiag[5]=3）且
 *   输出缓冲拿不到数据 —— H7 的 JPEG 输出 FIFO 要靠中断（或 MDMA）搬运，
 *   CPU 轮询等标志等不到。厂商「实验30」也是 MDMA 双缓冲 + 中断的路子。 */
static JPEG_HandleTypeDef s_hjpeg;
static volatile int       s_jpeg_done = 0;   /* 1 = 完成，-1 = 出错，0 = 进行中 */

/* JPEG 中断入口：定义在本文件（与 SDMMC1_IRQHandler 定义在 sd_card.c 同一套路，
 * 槽位见 src/startup_gcc.s —— IRQ121）。 */
void JPEG_IRQHandler(void)
{
    g_jpeg_trace[0]++;                 /* 进 ISR 的次数 */
    g_jpeg_trace[3] |= JPEG->SR;       /* ISR 里见到的状态位（累积） */
    HAL_JPEG_IRQHandler(&s_hjpeg);
}

void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef* hjpeg)
{
    (void)hjpeg;
    s_jpeg_done = 1;
}

void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef* hjpeg)
{
    (void)hjpeg;
    s_jpeg_done = -1;
}

/* ---- YCbCr → RGB565（BT.601，整数定点）---- */
static uint16_t jpeg_yuv_px(int y, int cb, int cr)
{
    int c = cb - 128;
    int d = cr - 128;
    int r = y + ((d * 1436) >> 10);            /* 1.402   */
    int g = y - ((c * 352 + d * 731) >> 10);   /* 0.344 / 0.714 */
    int b = y + ((c * 1814) >> 10);            /* 1.772   */

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* 把 nbytes 追加进 s_blk，攒满 4 KB 就写一次 W25。
 * 返回 0 = 正常；非 0 = QSPI 写失败（s_j.qspi_err 里也记一份）。 */
static int jpeg_emit(const uint8_t* src, uint32_t nbytes)
{
    uint32_t i = 0u;

    while (i < nbytes)
    {
        uint32_t room = IMG_BLK - s_j.blk_n;
        uint32_t n    = nbytes - i;

        if (n > room) { n = room; }
        memcpy(s_blk + s_j.blk_n, src + i, n);
        s_j.blk_n += n;
        i         += n;

        if (s_j.blk_n >= IMG_BLK)
        {
            int q = qspi_write(s_blk, IMG_DATA_ADDR + s_j.slot_off + s_j.written,
                               IMG_BLK);
            g_img_qspi_rc = (uint32_t)q;
            if (q != QSPI_OK)
            {
                s_j.qspi_err  = q;
                g_img_qdiag[1] = (uint32_t)q;
                g_img_qdiag[2] = IMG_DATA_ADDR + s_j.slot_off + s_j.written;
                return -1;
            }
            s_j.written += IMG_BLK;
            s_j.blk_n    = 0u;
        }
    }
    return 0;
}

/* ---- 色度上采样 ----
 * 为什么不能直接用最近邻：libjpeg（以及 Pillow 背后的解码器）默认做的是
 * "fancy upsampling"（三角形滤波，权重 3/4 : 1/4）。固件若取最近邻，颜色突变处
 * 会差出 20~40 —— 2026-10-09 实测：整幅平均误差 5.55，但**平坦区平均只有 2.81
 * 且零超差**，超差像素 100% 落在非平坦区 ⇒ 差异完全来自上采样方式。
 * ⇒ 这里照 libjpeg 的权重做，边界夹取到块内（跨 MCU 那一行/列的邻居取不到，
 *   夹取带来的误差只出现在每 16 行/列一次的边界上，肉眼不可见）。
 */
static int chroma_at(const uint8_t* yuv, int base, int cy, int cx)
{
    if (cy < 0) { cy = 0; } else if (cy > 7) { cy = 7; }
    if (cx < 0) { cx = 0; } else if (cx > 7) { cx = 7; }
    return (int)yuv[base + cy * 8 + cx];
}

/* 水平 2:1：偶数像素 (3·近 + 1·左 + 2)/4，奇数像素 (3·近 + 1·右 + 1)/4 */
static int chroma_h(const uint8_t* yuv, int base, int cy, int x)
{
    int i = (x % 16) / 2;
    int a, b;
    if ((x & 1) == 0)
    {
        a = chroma_at(yuv, base, cy, i - 1);
        b = chroma_at(yuv, base, cy, i);
        return (3 * b + a + 2) >> 2;
    }
    a = chroma_at(yuv, base, cy, i);
    b = chroma_at(yuv, base, cy, i + 1);
    return (3 * a + b + 1) >> 2;
}

/* 4:2:0：先水平再垂直，垂直同样用 (3/4,1/4) */
static int chroma_up2(const uint8_t* yuv, int base, int cy, int cx, int r, int x)
{
    int v0 = chroma_h(yuv, base, cy, x);
    (void)cx;
    if ((r & 1) == 0)
    {
        int vm = chroma_h(yuv, base, cy - 1, x);
        return (3 * v0 + vm + 2) >> 2;
    }
    int vp = chroma_h(yuv, base, cy + 1, x);
    return (3 * v0 + vp + 1) >> 2;
}

/* 一块 YUV → 若干行 RGB565 → 流式写 W25。
 *
 * ★ 布局是**按 MCU 交错**的，不是"Y 平面 / Cb 平面 / Cr 平面"三段（2026-10-09
 *   板上实测：32×16 纯红 4:2:0 图，硬件吐出 768 字节 = 2 个 MCU × 384）：
 *     MCU_i = [ Y 4 块 256 B ][ Cb 64 B ][ Cr 64 B ]
 *   Y 的 4 块在 MCU 内按 raster 排：左上、右上、左下、右下。
 *   Cb/Cr 各一个 8×8 块，覆盖整个 MCU（4:2:0 是 16×16 ⇒ 每 2×2 取一个）。
 *   ⇒ 取像素要先定位它落在哪个 MCU，再在 MCU 内定位块。 */
static int jpeg_block_to_rgb(const uint8_t* yuv)
{
    uint32_t w     = s_j.w;
    uint32_t rows  = s_j.blk_h;
    uint32_t r;
    uint8_t  line[2u * JPEG_MAX_W];

    /* 第一块：留一份原始字节（g_jpeg_dbg=1）供脚本对账，并记录 MCU 几何。 */
    if (s_j.row == 0u)
    {
        g_jpeg_diag[0] = yuv[0];                       /* MCU0 的 Y 首字节 */
        g_jpeg_diag[1] = yuv[s_j.per_mcu - 128u];      /* MCU0 的 Cb 首字节 */
        g_jpeg_diag[2] = yuv[s_j.per_mcu - 64u];       /* MCU0 的 Cr 首字节 */
        g_jpeg_diag[3] = yuv[1];
        g_jpeg_diag[4] = s_j.yuv_size;
        g_jpeg_diag[5] = s_j.per_mcu;
        g_jpeg_diag[6] = s_j.sub;

        if (g_jpeg_dbg != 0u)
        {
            memcpy(s_jyuv_snap, yuv, s_j.yuv_size);
        }
    }

    for (r = 0u; r < rows; r++)
    {
        uint32_t x;
        if (s_j.row >= s_j.h) { break; }        /* 最后一块可能超出图像高度 */

        for (x = 0u; x < w; x++)
        {
            int      yy, cb, cr;
            uint16_t v;
            uint32_t mx   = x / s_j.mcu_w;               /* 第几个 MCU（横向） */
            uint32_t base = mx * s_j.per_mcu;
            uint32_t yo, co;

            if (s_j.sub == JPEG_SUB_420)
            {
                /* Y：4 个块，by = 上下、bx = 左右，每块 8×8 */
                uint32_t bx = (x % 16u) / 8u;
                uint32_t by = r / 8u;
                yo = base + (by * 2u + bx) * 64u + (r % 8u) * 8u + (x % 8u);
                co = base + 256u;                /* Cb 块起始（Cr = +64） */
            }
            else if (s_j.sub == JPEG_SUB_422)
            {
                /* Y：2 个块左右并排，每块 8×8；MCU 是 16 宽 × 8 高 */
                uint32_t bx = (x % 16u) / 8u;
                yo = base + bx * 64u + r * 8u + (x % 8u);
                co = base + 128u;                /* Cb 块起始（Cr = +64） */
            }
            else    /* 4:4:4：MCU 就是 8×8，三分量同样大小 */
            {
                yo = base + r * 8u + (x % 8u);
                co = base + 64u + r * 8u + (x % 8u);
            }

            yy = yuv[yo];

            /* 色度上采样：4:2:0 水平+垂直各 2:1，4:2:2 只水平，4:4:4 不需要。
             * 实测顺序是 Cb 在前、Cr 在后（32×16 纯红：偏移 256 起是 85=Cb、
             * 320 起是 255=Cr）；万一遇到相反的工具链，用 g_jpeg_swap 翻。 */
            if (s_j.sub == JPEG_SUB_420)
            {
                int cx = (int)((x % 16u) / 2u);
                int cy = (int)(r / 2u);
                cb = chroma_up2(yuv, (int)co,      cy, cx, (int)r, (int)x);
                cr = chroma_up2(yuv, (int)co + 64, cy, cx, (int)r, (int)x);
            }
            else if (s_j.sub == JPEG_SUB_422)
            {
                cb = chroma_h(yuv, (int)co,      (int)r, (int)x);
                cr = chroma_h(yuv, (int)co + 64, (int)r, (int)x);
            }
            else
            {
                cb = (int)yuv[co];
                cr = (int)yuv[co + 64u];
            }
            if (g_jpeg_swap != 0u) { int t = cb; cb = cr; cr = t; }

            v = jpeg_yuv_px(yy, cb, cr);
            line[2u * x]     = (uint8_t)(v & 0xFFu);
            line[2u * x + 1u] = (uint8_t)((v >> 8) & 0xFFu);
        }

        if (jpeg_emit(line, w * 2u) != 0) { return -1; }
        s_j.row++;
    }
    return 0;
}

/* 从卡上读一"份"输入，并保证交给 HAL 的长度是 **4 的倍数且一个字节不丢**。
 *
 * ⚠⚠ 这是本模块最要命的一条坑（2026-10-09 实测定位，花了三版才抓到）：
 *   HAL_JPEG_Decode_IT / HAL_JPEG_ConfigInputBuffer 内部都会做
 *       InDataLength - (InDataLength % 4)
 *   —— 被截掉的 1~3 字节**直接丢弃，不会留到下一次**。
 *   639 字节的 JPEG 被截成 636，丢掉的正好压着 EOI 结束标记 ⇒ 解码器永远
 *   等不到结束：实测 ISR 进了 22 次、GetData 回调 1 次、**DataReady 一次都没有**、
 *   句柄停在 HAL_JPEG_STATE_BUSY_DECODING(4) 直到超时。表现出来就是"画面全灰"
 *   而字节数还恰好对得上（因为那 1024 字节是收尾那次用垃圾数据补出来的）。
 *
 * ⇒ 解法：不足一个字的尾巴**不能丢**。分两种情形：
 *     · 还没读到文件尾（缓冲被读满）⇒ 用 f_lseek 把尾巴退回去，下次读取时
 *       它自然排在最前面，一个字节不丢，也不需要拼接缓冲；
 *     · 已经读到文件尾（尾部凑不满一个字）⇒ **补零凑到 4 的倍数**送进去。
 *       补在 EOI 之后的字节解码器不认，安全。⚠ 这一支不能也去 lseek 回退：
 *       退回去之后下次还是同一个尾巴，永远凑不满一个字 ⇒ 那几个字节就永远
 *       送不进去了（2026-10-09 第二版就是这么又卡住的）。 */
static uint32_t jpeg_read_aligned(void)
{
    UINT    br  = 0u;
    uint32_t rem;
    FRESULT fr;

    fr = f_read(&s_j.f, s_jin, JPEG_IN_BUF, &br);
    g_img_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { br = 0u; }

    rem = ((uint32_t)br) % 4u;
    if (rem != 0u)
    {
        uint32_t need = 4u - rem;
        if (((uint32_t)br + need) <= JPEG_IN_BUF)
        {
            memset(s_jin + br, 0, need);      /* 文件尾：补零凑够一个字 */
            br += need;
        }
        else
        {
            (void)f_lseek(&s_j.f, f_tell(&s_j.f) - (FSIZE_t)rem);
            br -= rem;
        }
    }
    return (uint32_t)br;
}

/* ---- HAL_JPEG 的两个回调（弱定义，本文件实现即覆盖）----
 * ⚠ 它们是**全局符号**：整个固件只能有一份，也只有本模块在用 JPEG。 */

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef* hjpeg, uint32_t NbDecodedData)
{
    UINT    br = 0u;
    FRESULT fr;

    (void)NbDecodedData;
    g_jpeg_trace[1]++;

    if (s_j.eof)
    {
        /* 长度 0 = 没有更多输入，HAL 会结束解码 */
        HAL_JPEG_ConfigInputBuffer(hjpeg, s_jin, 0u);
        return;
    }

    br = jpeg_read_aligned();          /* 4 字节对齐、不丢尾（见函数头注释） */
    if (br == 0u)
    {
        s_j.eof = 1;
        HAL_JPEG_ConfigInputBuffer(hjpeg, s_jin, 0u);
        return;
    }
    HAL_JPEG_ConfigInputBuffer(hjpeg, s_jin, br);
}

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef* hjpeg, uint8_t* pDataOut,
                                uint32_t OutDataLength)
{
    (void)pDataOut;

    g_img_qdiag[6]++;                                   /* 块计数 */
    g_jpeg_trace[2]++;
    if (s_j.row == 0u) { g_jpeg_diag[7] = OutDataLength; }  /* 首块实际产出 */

    if (jpeg_block_to_rgb(s_jyuv) != 0)
    {
        /* 写失败：把输入也断掉，让解码尽快收尾（错误已记在 s_j.qspi_err） */
        s_j.eof = 1;
        HAL_JPEG_ConfigInputBuffer(hjpeg, s_jin, 0u);
    }
    HAL_JPEG_ConfigOutputBuffer(hjpeg, s_jyuv, s_j.yuv_size);
}

/* ---- 自己解析 JPEG 头（SOF0/1/2）拿宽高与采样 ----
 * 为什么不靠 HAL_JPEG_GetInfo：那样要等 "InfoReady" 回调，而**输出缓冲的大小
 * 必须在发起解码时就给定**，回调里再配已经晚了。自己扫一遍标记更简单可控。 */
static int jpeg_probe_header(const uint8_t* b, uint32_t n,
                             uint32_t* w, uint32_t* h, uint32_t* sub)
{
    uint32_t i = 2u;                      /* 跳过 SOI（FF D8） */

    while (i + 9u < n)
    {
        uint8_t  m;
        uint32_t len;

        if (b[i] != 0xFFu) { i++; continue; }
        m = b[i + 1u];
        /* 无长度字段的标记 */
        if ((m == 0x01u) || (m == 0xD8u) || ((m >= 0xD0u) && (m <= 0xD7u)))
        {
            i += 2u;
            continue;
        }
        if (i + 3u >= n) { break; }
        len = ((uint32_t)b[i + 2u] << 8) | (uint32_t)b[i + 3u];
        if (len < 2u) { break; }

        if ((m == 0xC0u) || (m == 0xC1u) || (m == 0xC2u))   /* SOF0 / SOF1 / SOF2 */
        {
            uint8_t nf;
            *h   = ((uint32_t)b[i + 5u] << 8) | (uint32_t)b[i + 6u];
            *w   = ((uint32_t)b[i + 7u] << 8) | (uint32_t)b[i + 8u];
            nf   = b[i + 9u];
            if (nf < 3u)
            {
                return -2;                /* 灰度 / 分量数异常：本模块只处理 YCbCr 三分量 */
            }
            if (i + 11u < n)
            {
                uint8_t hv = b[i + 11u];
                uint8_t hh = (uint8_t)(hv >> 4);
                uint8_t vv = (uint8_t)(hv & 0x0Fu);
                if ((hh == 2u) && (vv == 2u))      *sub = JPEG_SUB_420;
                else if ((hh == 2u) && (vv == 1u)) *sub = JPEG_SUB_422;
                else                                *sub = JPEG_SUB_444;
            }
            else
            {
                *sub = JPEG_SUB_444;
            }
            return 0;
        }
        if (m == 0xDAu) { return -1; }    /* 到 SOS 还没见着 SOF */
        i += 2u + len;
    }
    return -1;
}

static uint32_t import_jpeg(void)
{
    FRESULT fr;
    UINT    br = 0u;
    uint32_t out_w, out_h, out_len, used, slot, slot_off;
    JPEG_HandleTypeDef hj;
    HAL_StatusTypeDef  hs;

    /* ⚠ s_j 里含 FIL：必须在 f_open **之前**清零 —— 放到 f_open 之后会把刚打开的
     * 文件对象清成 0，实测症状是回调里 f_read 返回 FR_INVALID_OBJECT(9)，
     * 解码只走了 3 块（48 行）就因输入断流收尾（2026-10-09 踩到）。 */
    memset(&s_j, 0, sizeof(s_j));

    /* ---- 1) 挂载 + 打开 ---- */
    g_img_step = STEP_MOUNT;
    if (fatfs_ensure_mounted() != 0) { return RC_FS_MOUNT; }

    g_img_step = STEP_OPEN;
    g_img_path[IMG_PATH_MAX - 1u] = '\0';
    fr = f_open(&s_j.f, g_img_path, FA_READ);
    g_img_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { return RC_OPEN; }

    /* ---- 2) 读开头一段解析头 ---- */
    g_img_step = STEP_HDR;
    br = jpeg_read_aligned();          /* 同样要对齐：HAL 会截掉不足 4 的尾巴 */
    if ((g_img_fs_rc != 0u) || (br < 64u))
    {
        (void)f_close(&s_j.f);
        return RC_READ_HDR;
    }

    /* 头这一段已经读走了，回调从当前文件位置继续读 ⇒ s_j.eof 保持 0。
     * （s_j.f 此时是有效的打开状态，不要再去动它。） */
    g_img_step = STEP_PARSE;
    if (jpeg_probe_header(s_jin, br, &out_w, &out_h, &s_j.sub) != 0)
    {
        (void)f_close(&s_j.f);
        return RC_NOT_BMP;             /* 复用"格式不对"的返回码 */
    }
    if ((out_w == 0u) || (out_h == 0u) || (out_w > JPEG_MAX_W) || (out_h > JPEG_MAX_H))
    {
        (void)f_close(&s_j.f);
        return RC_TOO_WIDE;            /* 超尺寸：硬件不能缩放，直接拒绝 */
    }

    s_j.w = out_w;
    s_j.h = out_h;

    /* ★ 一块的大小要按 **MCU 对齐后的宽度** 算，不是图像宽度（2026-10-09 实测）：
     * 32×16 的 4:2:0 图只有 2 个 MCU（16×16 各一个），硬件吐出 768 字节，布局是
     *   MCU0: Y 4 块(256) + Cb(64) + Cr(64) = 384   MCU1: 同
     * 若宽不是 16 的倍数（如 284 → 18 个 MCU = 288 像素），按 24×w 算会**算小**，
     * 缓冲一满 HAL 就回调，剩下的还留在 FIFO ⇒ 块边界错位、画面花。 */
    if (s_j.sub == JPEG_SUB_422)
    {
        s_j.blk_h    = 8u;
        s_j.mcu_w    = 16u;
        s_j.per_mcu  = 256u;                 /* Y 2 块(128) + Cb(64) + Cr(64) */
    }
    else if (s_j.sub == JPEG_SUB_420)
    {
        s_j.blk_h    = 16u;
        s_j.mcu_w    = 16u;
        s_j.per_mcu  = 384u;                 /* Y 4 块(256) + Cb(64) + Cr(64) */
    }
    else
    {
        s_j.blk_h    = 8u;
        s_j.mcu_w    = 8u;
        s_j.per_mcu  = 192u;                 /* Y(64) + Cb(64) + Cr(64) */
    }
    s_j.yuv_size = ((out_w + s_j.mcu_w - 1u) / s_j.mcu_w) * s_j.per_mcu;
    if (s_j.yuv_size > JPEG_YUV_MAX)
    {
        (void)f_close(&s_j.f);
        return RC_TOO_WIDE;
    }

    out_len = out_w * out_h * 2u;
    g_img_src_w  = out_w;
    g_img_src_h  = out_h;
    g_img_bpp    = 24u;                /* 源是 YCbCr，这里只作标记 */
    g_img_out_w  = out_w;
    g_img_out_h  = out_h;
    g_img_out_bytes = out_len;

    /* ---- 3) 分配槽位（与 BMP 同一套：整段索引拷进 s_blk 再扫）---- */
    g_img_step = STEP_ALLOC;
    memcpy(s_blk, (const void *)XIP_PTR(IMG_IDX_ADDR), IMG_BLK);
    __DSB();
    __ISB();
    used = idx_used_in(s_blk);
    if ((used + out_len) > IMG_DATA_SIZE)
    {
        (void)f_close(&s_j.f);
        return RC_NO_SPACE;
    }
    for (slot = 0u; slot < IMG_MAX_SLOTS; slot++)
    {
        const uint8_t* e = s_blk + slot * IMG_ENTRY_BYTES;
        if (!((rd16(e) == IMG_MAGIC) && ((rd16(e + 2u) & IMG_FLAG_VALID) != 0u)))
        {
            break;
        }
    }
    if (slot >= IMG_MAX_SLOTS)
    {
        (void)f_close(&s_j.f);
        return RC_NO_SLOT;
    }
    slot_off = used;
    g_img_slot = slot;

    /* ---- 4) 解码：退出映射 → 解码（回调里流式写 W25）---- */
    g_img_step = STEP_IMPORT;

    s_j.slot_off = slot_off;
    s_j.written  = 0u;
    s_j.row      = 0u;
    s_j.blk_n    = 0u;
    s_j.qspi_err = 0;
    s_j.eof      = 0;                  /* 允许回调继续读文件（头之后的数据） */

    {
        int q = qspi_exit_mmap();
        g_img_qdiag[3] = (uint32_t)q;
        q = qspi_read(s_line, IMG_DATA_ADDR + slot_off, 256u);   /* 探测读 */
        g_img_qdiag[4] = (uint32_t)q;
    }

    /* 时钟要在 HAL_JPEG_Init 之前开（库不会自己开 AHB3 的 JPGDECEN） */
    __HAL_RCC_JPGDECEN_CLK_ENABLE();

    memset(&s_hjpeg, 0, sizeof(s_hjpeg));
    s_hjpeg.Instance = JPEG;
    hs = HAL_JPEG_Init(&s_hjpeg);

    g_jpeg_trace[7] = (uint32_t)hs;      /* 低 8 位 = HAL_JPEG_Init 的返回值 */

    if (hs == HAL_OK)
    {
        uint32_t sr_acc = 0u, cr_acc = 0u;
        HAL_StatusTypeDef dit;

        s_jpeg_done = 0;
        HAL_NVIC_SetPriority(JPEG_IRQn, 5u, 0u);
        HAL_NVIC_EnableIRQ(JPEG_IRQn);

        dit = HAL_JPEG_Decode_IT(&s_hjpeg, s_jin, br, s_jyuv, s_j.yuv_size);
        g_jpeg_trace[7] |= ((uint32_t)dit << 8);

        if (dit == HAL_OK)
        {
            /* 中断里搬数据，这里只等完工（导入本来就是独占操作，阻塞无妨）。
             * 上限 1 秒（DWT @400 MHz）：超时就当失败，绝不死等。 */
            uint32_t t0 = DWT->CYCCNT;
            while ((s_jpeg_done == 0) && ((DWT->CYCCNT - t0) < 400000000u))
            {
                sr_acc |= JPEG->SR;
                cr_acc |= JPEG->CR;
            }
        }
        g_jpeg_trace[4] = sr_acc;
        g_img_qdiag[0]  = cr_acc;
        HAL_NVIC_DisableIRQ(JPEG_IRQn);
        if (s_jpeg_done <= 0) { hs = HAL_TIMEOUT; }
    }
    g_jpeg_trace[5] = (uint32_t)s_hjpeg.State;
    g_jpeg_trace[6] = (uint32_t)s_hjpeg.ErrorCode;
    g_img_qdiag[5] = (uint32_t)hs;      /* HAL 返回值只作诊断，成败看数据量 */
    g_img_qdiag[7] = (uint32_t)s_jpeg_done;
    (void)HAL_JPEG_DeInit(&s_hjpeg);
    (void)f_close(&s_j.f);

    /* ⚠ HAL 收尾时**不会**为最后一块触发 DataReady：
     * 实测 180 行（4:2:0，11 块 = 176 行 + 最后 4 行）只出来了 176 行，
     * 差的就是不足整块的尾巴 ⇒ 解码返回后手动把缓冲里的残留再处理一次。
     * jpeg_block_to_rgb 内部有 `row >= h 就停`，所以只会补上缺的那几行。 */
    if (s_j.row < s_j.h)
    {
        (void)jpeg_block_to_rgb(s_jyuv);
    }

    /* 收尾：把最后不满 4 KB 的尾巴写掉 */
    if (s_j.blk_n != 0u)
    {
        int q = qspi_write(s_blk, IMG_DATA_ADDR + slot_off + s_j.written, s_j.blk_n);
        g_img_qspi_rc = (uint32_t)q;
        if (q != QSPI_OK)
        {
            s_j.qspi_err   = q;
            g_img_qdiag[1] = (uint32_t)q;
        }
        else
        {
            s_j.written += s_j.blk_n;
        }
        s_j.blk_n = 0u;
    }

    g_img_step = STEP_INDEX;
    (void)qspi_enter_mmap();

    if (s_j.qspi_err != 0) { return RC_QSPI; }

    /* 判据用**数据量**而不是 HAL 返回值：最后一块不触发回调时 HAL 可能返回非 OK，
     * 但只要写入字节数与头里声明的宽高对得上，就是解码完整了。 */
    if (s_j.written != out_len)
    {
        g_img_out_bytes = s_j.written;
        return RC_JPEG_DEC;
    }
    if (s_j.written != out_len)
    {
        /* 解码出来的行数和头里声明的对不上 ⇒ 多半是截断文件，别写坏索引 */
        g_img_out_bytes = s_j.written;
        return RC_JPEG_SHORT;
    }

    /* ---- 5) 写索引条目 ---- */
    {
        uint8_t e[IMG_ENTRY_BYTES];
        int     q;

        memset(e, 0, sizeof(e));
        wr16(e + 0u,  IMG_MAGIC);
        wr16(e + 2u,  IMG_FLAG_VALID);
        wr32(e + 4u,  slot_off);
        wr32(e + 8u,  out_len);
        wr16(e + 12u, (uint16_t)out_w);
        wr16(e + 14u, (uint16_t)out_h);

        q = qspi_write(e, IMG_IDX_ADDR + slot * IMG_ENTRY_BYTES, IMG_ENTRY_BYTES);
        g_img_qspi_rc = (uint32_t)q;
        if (q != QSPI_OK) { return RC_QSPI; }
    }

    (void)qspi_enter_mmap();
    g_img_used = used_bytes();
    return 0u;
}

/* QSPI 分步诊断（g_img_test=4）
 *
 * 分两组，专门用来对比"退出映射前有没有做过大量 XIP 读"：
 *   A 组（q[0..2]）：直接 exit → 写 4 KB → 回读
 *   B 组（q[3..7]）：进映射 → 做 256 次 XIP 读 → exit → 写 4 KB → 回读
 * 若 A 全 0 而 B 的写失败，就证实了"刚读过 XIP 就退出映射 ⇒ 后续命令超时"。
 */
static void run_qspi_diag(void)
{
    uint32_t i;
    int      rc;

    for (i = 0u; i < 8u; i++) { g_img_qdiag[i] = 0xFFFFFFFFu; }

    /* ---------- A 组：不做任何 XIP 读 ---------- */
    rc = qspi_exit_mmap();
    g_img_qdiag[0] = (uint32_t)rc;

    for (i = 0u; i < IMG_BLK; i++) { s_blk[i] = (uint8_t)((i >> 2) & 0xFFu); }
    rc = qspi_write(s_blk, IMG_DATA_ADDR, IMG_BLK);
    g_img_qdiag[1] = (uint32_t)rc;

    rc = qspi_read(s_line, IMG_DATA_ADDR, 256u);
    if (rc == QSPI_OK)
    {
        uint32_t mis = 0u;
        for (i = 0u; i < 256u; i++)
        {
            if (s_line[i] != (uint8_t)((i >> 2) & 0xFFu)) { mis++; }
        }
        g_img_qdiag[2] = mis;
    }
    else
    {
        g_img_qdiag[2] = 0xFFFFFFFEu;      /* 回读本身失败 */
    }

    /* ---------- B 组：先做 256 次 XIP 读（模拟导入时扫描索引） ---------- */
    rc = qspi_enter_mmap();
    g_img_qdiag[3] = (uint32_t)rc;

    {
        uint32_t sum = 0u;
        for (i = 0u; i < IMG_MAX_SLOTS; i++)     /* 256 次，与导入时一样多 */
        {
            const uint8_t *e = XIP_PTR(IMG_IDX_ADDR + i * IMG_ENTRY_BYTES);
            sum += rd16(e) + rd32(e + 4u);
        }
        g_img_qdiag[4] = sum;                     /* 非 0 才说明真的读到了东西 */
    }

    rc = qspi_exit_mmap();
    g_img_qdiag[5] = (uint32_t)rc;

    for (i = 0u; i < IMG_BLK; i++) { s_blk[i] = (uint8_t)((i >> 3) & 0xFFu); }
    rc = qspi_write(s_blk, IMG_DATA_ADDR + IMG_BLK, IMG_BLK);
    g_img_qdiag[6] = (uint32_t)rc;                /* ★若 A 组的 q[1]==0 而这里是 1，根因确认 */

    rc = qspi_read(s_line, IMG_DATA_ADDR + IMG_BLK, 256u);
    if (rc == QSPI_OK)
    {
        uint32_t mis = 0u;
        for (i = 0u; i < 256u; i++)
        {
            if (s_line[i] != (uint8_t)((i >> 3) & 0xFFu)) { mis++; }
        }
        g_img_qdiag[7] = mis;
    }
    else
    {
        g_img_qdiag[7] = 0xFFFFFFFEu;
    }

    (void)qspi_enter_mmap();
}

/* 把一个 ≤4 KB 的片段写进卡上的文件（测试夹具，见 img_store.h 的 g_img_mk） */
static uint32_t write_chunk(int create)
{
    FRESULT fr;
    UINT    bw = 0u;
    uint8_t mode;

    if (fatfs_ensure_mounted() != 0) { return RC_FS_MOUNT; }

    mode = create ? (uint8_t)(FA_WRITE | FA_CREATE_ALWAYS)
                  : (uint8_t)(FA_WRITE | FA_OPEN_APPEND);

    g_img_path[IMG_PATH_MAX - 1u] = '\0';
    fr = f_open(&s_mfil, g_img_path, mode);
    g_img_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { return RC_OPEN; }

    if (g_img_mk_len > IMG_BLK) { g_img_mk_len = IMG_BLK; }   /* 缓冲就这么大 */
    fr = f_write(&s_mfil, s_blk, g_img_mk_len, &bw);
    g_img_fs_rc = (uint32_t)fr;
    if ((fr != FR_OK) || (bw != g_img_mk_len))
    {
        (void)f_close(&s_mfil);
        return RC_WRITE_FILE;
    }

    fr = f_close(&s_mfil);
    g_img_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { return RC_CLOSE; }

    g_img_mk_total += bw;
    return 0u;
}

static void run_clear(void)
{
    /* 索引扇区号 = 地址 / 4096 */
    int q = qspi_erase_sector(IMG_IDX_ADDR / IMG_BLK);

    g_img_qspi_rc = (uint32_t)q;
    g_img_rc      = (q == QSPI_OK) ? 0u : RC_QSPI;
    if (q == QSPI_OK) { g_img_used = 0u; }
}

/* ===========================================================================
 * 主循环挂载点
 * =========================================================================== */

void img_store_poll(void)
{
    uint32_t mode = g_img_test;
    uint32_t c0;

    if (mode == 0u) { return; }
    g_img_test = 0u;

    g_img_rc        = 0xFFFFFFFFu;
    g_img_step      = 0u;
    g_img_slot      = 0xFFFFFFFFu;
    g_img_fs_rc     = 0xFFFFFFFFu;
    g_img_qspi_rc   = 0u;
    g_img_out_bytes = 0u;

    /* ★独占期：主循环在这期间跳过渲染与触摸（XIP 不可读，见文件头说明） */
    g_img_busy = 1u;
    c0 = DWT->CYCCNT;

    if (mode == 1u)
    {
        /* 导入分流：**优先按内容魔数**判 JPEG / BMP，嗅不出来才回退到扩展名。
         *
         * ⚠ 为什么改成嗅探优先（2026-10-10）：蓝牙裸流收到的文件扩展名是 `.bin`
         *   （原始后缀在对端就被丢了，见 sniff_image_kind 的说明）——只看扩展名
         *   的话，这种文件一律被当成 BMP、里面的 JPEG 必然导入失败。
         *
         * ⚠ 回退到扩展名这一支**必须保留**：嗅探只在"文件能打开且至少 2 字节"
         *   时才有结论，读不了时（比如卡没挂上）要退回老行为，
         *   这样 tools/img_check.py 原有的 BMP 流程完全不受影响。 */
        {
            int kind    = sniff_image_kind(g_img_path);
            int is_jpeg = (kind < 0) ? (path_is_jpeg(g_img_path) != 0) : (kind == 1);

            g_img_rc = is_jpeg ? import_jpeg() : import_bmp();
        }
    }
    else if (mode == 2u)
    {
        run_xip_probe();
        g_img_rc = 0u;
    }
    else if (mode == 3u)
    {
        run_clear();
    }
    else if (mode == 4u)
    {
        run_qspi_diag();
        g_img_rc = 0u;
    }
    else if (mode == 5u)
    {
        /* 建好 /YMGUI/PIC 与 /YMGUI/NOTE 并扫一次照片分类 —— 验收脚本
         * （tools/files_check.py）用它准备卡上内容、并核对 g_scan_n。 */
        (void)img_scan_mkdir(IMG_PIC_DIR);
        (void)img_scan_mkdir(IMG_NOTE_DIR);
        g_img_rc = (uint32_t)img_scan_dir(IMG_PIC_DIR, ".bmp", 1);
    }
    else if (mode == 6u)
    {
        /* 笔记往返自检：新建一条 → 读回 → 逐字节比对（含中文，验证 UTF-8 原样）。
         * 判据：g_note_rc == 0、g_note_bytes == 期望长度、g_note_cmp == 0（失配数）。 */
        static const char s[] = "note-selftest\n中文内容 ABC";
        int idx = BoardNote_Save(-1, s);
        g_note_cmp = 0xFFFFFFFFu;
        if (idx < 0)
        {
            g_note_rc = -100 + idx;
        }
        else
        {
            int n = BoardNote_Load(idx, (char*)s_line, (int)LINE_MAX);
            if (n < 0)
            {
                g_note_rc = -200 + n;
            }
            else
            {
                uint32_t want = (uint32_t)strlen(s);
                uint32_t i;
                g_note_cmp = 0u;
                for (i = 0; i < want && i < (uint32_t)n; ++i)
                    if (s_line[i] != (uint8_t)s[i]) g_note_cmp++;
                if ((uint32_t)n != want) g_note_cmp++;
                g_note_rc = (g_note_cmp == 0u) ? 0 : -300;
            }
        }
        g_img_rc = 0u;
    }
    else if (mode == 7u)
    {
        /* 删除 g_img_path 指向的文件。
         * 为什么要有这一条：验收脚本往卡上丢测试文件，跑完必须能收走，
         * 否则第二次跑时基线里已经有它 ⇒ 判据"条目数 +1"永远达不到
         * （2026-10-09 实测卡上残留 selftest.bmp 导致 files_check.py 误报 FAIL）。
         * UI 将来的"删除照片 / 删除笔记"也走这一条。
         * FR_NO_FILE（文件本就不存在）也算达成目的 —— 删除是幂等操作。 */
        if (fatfs_ensure_mounted() == 0)
        {
            FRESULT fr = f_unlink(g_img_path);
            g_img_fs_rc = (uint32_t)fr;
            g_img_rc    = (fr == FR_OK || fr == FR_NO_FILE) ? 0u : (uint32_t)fr;
        }
        else
        {
            g_img_rc = RC_PARAM;
        }
    }
    else
    {
        g_img_rc = RC_PARAM;
    }

    g_img_cyc = DWT->CYCCNT - c0;

    /* 兜底：无论上面走了哪条路径、成功还是失败，返回主线程前**必须**回到映射模式。
     * UI 每帧都要读 XIP（字模 + IME 词典），留在 indirect 模式下读就是 BusFault。 */
    (void)qspi_enter_mmap();

    g_img_busy  = 0u;
    g_img_count = (uint32_t)img_store_count();
}

/* ===========================================================================
 * 目录扫描（照片 / 笔记分类的真实文件列表）—— 说明见 img_store.h
 * =========================================================================== */


typedef struct
{
    char     name[IMG_SCAN_NAME_MAX];
    uint32_t size;
} ScanEntry;

/* 放 SRAM1 的 .bss_img（与块缓冲同一段，非缓存区）。
 * 24 × (48 + 4) = 1 248 B，与块缓冲合计仍在 SRAM1 的 128 KB 内。 */
__attribute__((section(".bss_img")))
static ScanEntry s_scan[IMG_SCAN_MAX];
static int s_scan_n;

volatile uint32_t g_scan_n      = 0;
volatile uint32_t g_scan_cyc    = 0;
volatile uint32_t g_scan_fs_rc  = 0xFFFFFFFFu;
volatile uint32_t g_scan_kind   = 0;

volatile int32_t  g_note_rc     = -1;
volatile uint32_t g_note_bytes  = 0;
volatile uint32_t g_note_fs_rc  = 0xFFFFFFFFu;
volatile uint32_t g_note_cmp    = 0xFFFFFFFFu;

/* 后缀匹配：ext 传小写（".bmp" / ".txt"），名字里的大小写不敏感。 */
static int ext_match(const char* name, const char* ext)
{
    int nl = (int)strlen(name);
    int el = (int)strlen(ext);
    int i;
    const char* p;

    if (nl < el) return 0;
    p = name + (nl - el);
    for (i = 0; i < el; ++i)
    {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != ext[i]) return 0;
    }
    return 1;
}

static void copy_str(char* dst, int n, const char* src)
{
    int i = 0;
    if (n <= 0) return;
    while (src[i] != 0 && i < n - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

int img_scan_dir(const char* dir, const char* ext, int kind)
{
    DIR     d;
    FILINFO fi;
    FRESULT fr;
    uint32_t c0 = DWT->CYCCNT;

    s_scan_n = 0;
    g_scan_kind = (uint32_t)kind;

    /* ⚠ 必须先确保卷已挂载：直接 f_opendir 会拿 FR_NOT_ENABLED(12)
     * —— 卷没有工作区，FatFs 根本不知道该问哪个盘（2026-10-09 实测）。 */
    if (fatfs_ensure_mounted() != 0)
    {
        g_scan_fs_rc = 0xFFFFFFFEu;
        g_scan_cyc   = DWT->CYCCNT - c0;
        return -2;
    }

    memset(&d, 0, sizeof(d));
    memset(&fi, 0, sizeof(fi));

    fr = f_opendir(&d, dir);
    g_scan_fs_rc = (uint32_t)fr;
    if (fr != FR_OK)
    {
        g_scan_n   = 0;
        g_scan_cyc = DWT->CYCCNT - c0;
        return -1;
    }

    for (;;)
    {
        fr = f_readdir(&d, &fi);
        g_scan_fs_rc = (uint32_t)fr;
        if (fr != FR_OK) break;
        if (fi.fname[0] == 0) break;                 /* 目录结束 */
        if (fi.fattrib & AM_DIR) continue;           /* 跳过子目录 */
        if (ext != NULL && ext[0] != 0 && !ext_match(fi.fname, ext)) continue;
        if (s_scan_n >= (int)IMG_SCAN_MAX) break;    /* 表满：后面的不列 */

        copy_str(s_scan[s_scan_n].name, (int)IMG_SCAN_NAME_MAX, fi.fname);
        s_scan[s_scan_n].size = (uint32_t)fi.fsize;
        s_scan_n++;
    }

    (void)f_closedir(&d);
    g_scan_n   = (uint32_t)s_scan_n;
    g_scan_cyc = DWT->CYCCNT - c0;
    return s_scan_n;
}

int img_scan_count(void)
{
    return s_scan_n;
}

int img_scan_name(int i, char* out, int n)
{
    if (i < 0 || i >= s_scan_n || out == NULL || n <= 0) return -1;
    copy_str(out, n, s_scan[i].name);
    return 0;
}

int img_scan_size(int i, uint32_t* size)
{
    if (i < 0 || i >= s_scan_n || size == NULL) return -1;
    *size = s_scan[i].size;
    return 0;
}

int img_scan_path(int i, const char* dir, char* out, int n)
{
    int k = 0;
    int j;

    if (i < 0 || i >= s_scan_n || out == NULL || n <= 0) return -1;
    if (dir != NULL)
    {
        while (dir[k] != 0 && k < n - 1) { out[k] = dir[k]; ++k; }
        if (k > 0 && k < n - 1 && out[k - 1] != '/') { out[k++] = '/'; }
    }
    for (j = 0; s_scan[i].name[j] != 0 && k < n - 1; ++j)
        out[k++] = s_scan[i].name[j];
    out[k] = 0;
    return 0;
}

int img_scan_mkdir(const char* dir)
{
    FRESULT fr;
    char    parent[IMG_PATH_MAX];
    int     k = 0;

    if (dir == NULL) return -1;
    if (fatfs_ensure_mounted() != 0) { g_scan_fs_rc = 0xFFFFFFFEu; return -2; }

    /* ⚠ FatFs 的 f_mkdir **不会级联创建**：/YMGUI 还不存在时直接建 /YMGUI/PIC
     * 会返回 FR_NO_PATH（2026-10-09 实测：第一次进照片分类就是这么失败的）。
     * ⇒ 先保证父目录存在：把 dir 的最后一段去掉再建一次。 */
    while (dir[k] != 0 && k < (int)sizeof(parent) - 1) { parent[k] = dir[k]; ++k; }
    parent[k] = 0;
    while (k > 0 && parent[k - 1] != '/') { --k; parent[k] = 0; }
    while (k > 1 && parent[k - 1] == '/') { --k; parent[k] = 0; }
    if (k > 1)
    {
        fr = f_mkdir(parent);
        g_scan_fs_rc = (uint32_t)fr;
        /* 父目录建不出来就别往下试了（FR_EXIST 是"已存在"，不算错） */
        if (fr != FR_OK && fr != FR_EXIST) return -1;
    }

    fr = f_mkdir(dir);
    g_scan_fs_rc = (uint32_t)fr;
    /* 已存在不算失败（FatFs 对已存在的目录返回 FR_EXIST） */
    return (fr == FR_OK || fr == FR_EXIST) ? 0 : -1;
}

int img_scan_import(int i, const char* dir)
{
    char full[IMG_PATH_MAX];

    if (i < 0 || i >= s_scan_n) return -1;
    /* 上一轮还没跑完就不要覆盖（g_img_test 非 0 = 还有活没做） */
    if (g_img_test != 0u || g_img_busy != 0u) return -2;
    if (img_scan_path(i, dir, full, (int)sizeof(full)) != 0) return -3;

    copy_str(g_img_path, (int)IMG_PATH_MAX, full);
    g_img_test = 1u;            /* 异步：由 img_store_poll 执行 import_bmp */
    return 0;
}

/* 按完整路径排队导入（不查扫描表）。见 img_store.h 的说明。
 * ⚠ 唯一的新增风险是"路径由调用方给"，所以这里要挡掉明显不合法的输入。 */
int img_import_path(const char* full)
{
    if (full == NULL || full[0] == 0) return -1;
    if (full[0] != '/')              return -1;      /* 只收绝对路径，避免拼错目录 */
    if (g_img_test != 0u || g_img_busy != 0u) return -2;
    copy_str(g_img_path, (int)IMG_PATH_MAX, full);
    g_img_test = 1u;
    return 0;
}

/* ===========================================================================
 * board 层接口（phone_shell 的相册 app 用，见 phone_shell_board.h 的说明）
 * =========================================================================== */

/* ---- 照片分类：卡上 /YMGUI/PIC 的真实 .bmp 列表 ----
 * BoardPic_Scan() 只在**进入分类时**调一次（内部会 f_mkdir 保证目录存在），
 * 之后 Count/Name/Size 都读缓存表，不再碰文件系统。 */
int BoardPic_Scan(void)
{
    (void)img_scan_mkdir(IMG_PIC_DIR);
    return img_scan_dir(IMG_PIC_DIR, ".bmp", 1);
}

int BoardPic_Count(void)
{
    return img_scan_count();
}

int BoardPic_Name(int i, char* out, int n)
{
    return img_scan_name(i, out, n);
}

int BoardPic_Size(int i, uint32_t* size)
{
    return img_scan_size(i, size);
}

/* 把第 i 张导入图库（异步：置 g_img_test，由 img_store_poll 跑）。
 * 返回 0 = 已排队；UI 侧看 g_img_busy 归零后读 g_img_rc 判成功。 */
int BoardPic_Import(int i)
{
    return img_scan_import(i, IMG_PIC_DIR);
}

int BoardPic_Busy(void)
{
    return (g_img_busy != 0u || g_img_test != 0u) ? 1 : 0;
}

int BoardPic_LastRc(void)
{
    return (int)g_img_rc;
}

/* ---- 笔记：/YMGUI/NOTE 下的 .txt ----
 * 与照片共用同一张扫描表（同一时刻只服务一个分类，见 img_store.h 的说明）。
 *
 * ⚠ 文件名固定用 ASCII 的 note<N>.txt。原因有两条：
 *   ① FF_CODE_PAGE 已是 437（2026-10-09 从 936 改来，省 178 KB FLASH），
 *      FatFs 不再做 GBK↔UTF-16 转码 ⇒ 中文名自己保证不了往返一致；
 *   ② 笔记正文是 UTF-8 原样读写，不受代码页影响 ⇒ 标题放正文第一行。 */
int BoardNote_Scan(void)
{
    (void)img_scan_mkdir(IMG_NOTE_DIR);
    return img_scan_dir(IMG_NOTE_DIR, ".txt", 2);
}

int BoardNote_Count(void)
{
    return img_scan_count();
}

int BoardNote_Name(int i, char* out, int n)
{
    return img_scan_name(i, out, n);
}

int BoardNote_Size(int i, uint32_t* size)
{
    return img_scan_size(i, size);
}

int BoardNote_Load(int i, char* out, int n)
{
    FIL     f;
    FRESULT fr;
    UINT    br = 0;
    char    full[IMG_PATH_MAX];

    g_note_rc     = -1;
    g_note_bytes  = 0;

    if (i < 0 || i >= img_scan_count() || out == NULL || n <= 1) { g_note_rc = -2; return -2; }
    if (fatfs_ensure_mounted() != 0) { g_note_rc = -6; return -6; }
    if (img_scan_path(i, IMG_NOTE_DIR, full, (int)sizeof(full)) != 0) { g_note_rc = -3; return -3; }

    fr = f_open(&f, full, FA_READ);
    g_note_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { g_note_rc = -4; return -4; }

    fr = f_read(&f, out, (UINT)(n - 1), &br);
    (void)f_close(&f);
    g_note_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { g_note_rc = -5; return -5; }

    out[br] = 0;
    g_note_bytes = br;
    g_note_rc    = 0;
    return (int)br;
}

/* 挑一个没被占用的 note<N>.txt（N 从 0 往上找，最多 100 个）。 */
static int pick_new_note(char* full, int n)
{
    FILINFO fi;
    int     k;
    int     j;

    for (k = 0; k < 100; ++k)
    {
        int p = 0;
        const char* dir = IMG_NOTE_DIR;
        while (dir[p] != 0 && p < n - 1) { full[p] = dir[p]; ++p; }
        if (p < n - 1) full[p++] = '/';
        /* "note" */
        { const char* tag = "note"; j = 0; while (tag[j] != 0 && p < n - 1) { full[p++] = tag[j]; ++j; } }
        /* 十进制 N */
        if (k >= 10 && p < n - 1) full[p++] = (char)('0' + (k / 10));
        if (p < n - 1) full[p++] = (char)('0' + (k % 10));
        { const char* ext = ".txt"; j = 0; while (ext[j] != 0 && p < n - 1) { full[p++] = ext[j]; ++j; } }
        full[p] = 0;

        if (f_stat(full, &fi) != FR_OK)
            return 0;                       /* 不存在 ⇒ 可以用 */
    }
    return -1;
}

int BoardNote_Save(int i, const char* text)
{
    FIL     f;
    FRESULT fr;
    UINT    bw = 0;
    char    full[IMG_PATH_MAX];
    uint32_t len;

    g_note_rc    = -1;
    g_note_bytes = 0;

    if (text == NULL) { g_note_rc = -2; return -2; }
    if (fatfs_ensure_mounted() != 0) { g_note_rc = -8; return -8; }
    len = (uint32_t)strlen(text);

    if (i >= 0)
    {
        if (i >= img_scan_count()) { g_note_rc = -3; return -3; }
        if (img_scan_path(i, IMG_NOTE_DIR, full, (int)sizeof(full)) != 0) { g_note_rc = -4; return -4; }
    }
    else
    {
        if (pick_new_note(full, (int)sizeof(full)) != 0) { g_note_rc = -5; return -5; }
    }

    fr = f_open(&f, full, (uint8_t)(FA_WRITE | FA_CREATE_ALWAYS));
    g_note_fs_rc = (uint32_t)fr;
    if (fr != FR_OK) { g_note_rc = -6; return -6; }

    fr = f_write(&f, text, (UINT)len, &bw);
    g_note_fs_rc = (uint32_t)fr;
    (void)f_close(&f);

    if (fr != FR_OK || bw != (UINT)len) { g_note_rc = -7; return -7; }

    g_note_bytes = bw;
    g_note_rc    = 0;

    /* 重新扫一遍，让"新建"后的索引对得上（调用方可能要立刻列目录） */
    (void)img_scan_dir(IMG_NOTE_DIR, ".txt", 2);
    for (int k = 0; k < img_scan_count(); ++k)
    {
        char nm[IMG_SCAN_NAME_MAX];
        if (img_scan_name(k, nm, (int)sizeof(nm)) == 0)
        {
            /* full 的末段文件名与 nm 相同 ⇒ 它就是刚写的那条 */
            int a = 0, b = 0;
            for (int t = 0; full[t] != 0; ++t)
                if (full[t] == '/') a = t + 1;
            while (full[a + b] != 0 && nm[b] != 0 && full[a + b] == nm[b]) ++b;
            if (full[a + b] == 0 && nm[b] == 0)
                return k;
        }
    }
    return 0;
}

int BoardGallery_Count(void)
{
    int n;

    /* ⚠ 导入期间（XIP 不可读）不要来问 —— 调用方是 UI 线程，那时已被 g_img_busy
     * 挡住不渲染；这里再兜一层底，返回 0 让调用方走降级分支。 */
    if (g_img_busy != 0u) { return 0; }

    n = img_store_count();

    /* 诊断：把"UI 线程这一刻实际读到的索引"留证。
     * 起因：g_img_count（poll 末尾快照）读到 2，而相册里显示的总数是 1 ——
     * 同一个 img_store_count() 两个时刻结果不同，必须先把两次采样都落下来。 */
    g_img_board_count = (uint32_t)n;
    {
        const uint8_t *e0 = XIP_PTR(IMG_IDX_ADDR);
        const uint8_t *e1 = XIP_PTR(IMG_IDX_ADDR + IMG_ENTRY_BYTES);
        g_img_slot_snap[0] = rd16(e0);
        g_img_slot_snap[1] = rd16(e0 + 2u);
        g_img_slot_snap[2] = rd16(e1);
        g_img_slot_snap[3] = rd16(e1 + 2u);
    }
    return n;
}

int BoardGallery_Info(int slot, unsigned int *off, unsigned int *len,
                      unsigned int *w, unsigned int *h)
{
    uint32_t o = 0, l = 0, ww = 0, hh = 0;

    if (g_img_busy != 0u) { return 1; }
    if (img_store_slot_info((uint32_t)slot, &o, &l, &ww, &hh) != 0) { return 2; }
    if (off) { *off = o; }
    if (len) { *len = l; }
    if (w)   { *w   = ww; }
    if (h)   { *h   = hh; }
    return 0;
}

const void *BoardGallery_Pixels(int slot)
{
    if (g_img_busy != 0u) { return NULL; }
    return img_store_pixels((uint32_t)slot);
}

/* 写文件夹具：不走 busy（写卡不涉及 XIP，UI 可以继续跑） */
void img_store_mk_poll(void)
{
    uint32_t mode = g_img_mk;

    if (mode == 0u) { return; }
    g_img_mk = 0u;

    if (mode == 1u)
    {
        g_img_mk_total = 0u;
        g_img_mk_rc = write_chunk(1);
    }
    else if (mode == 2u)
    {
        g_img_mk_rc = write_chunk(0);
    }
    else
    {
        g_img_mk_rc = RC_PARAM;
    }
}
