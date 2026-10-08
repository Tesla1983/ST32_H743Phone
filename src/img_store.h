/* ===========================================================================
 * 图库：TF 卡上的 BMP → RGB565 → 灌进 W25Q128 → XIP 直读显示
 *
 * 【它解决什么】
 * 相册面板 284×230 全尺寸 RGB565 = 130 640 B，超过 heap1 的 115 KB 余量；
 * 板上又没有外部 RAM。但 `YMGUI_DrawImg.h:12` 的 `const GYpx* data` 是**只读指针**，
 * 可以指向 QSPI 的 XIP 窗口 0x9000_0000。于是把图片事先转成 RGB565 放进 W25Q128，
 * 显示时**零 RAM 开销**。
 *
 * 【为什么在固件里转，而不是 PC 侧转好灌进去】
 * PC 侧（tools/img2rgb565.py + tools/qspi_provision.py）也能做，但要 PC 和线缆；
 * 固件内转换让用户"往卡里丢一张图就能看"。代价是要处理 §3.2 的 XIP 冲突。
 *
 * 【核心设计：流式，且与图片尺寸无关】
 *   f_read 一行 → 转 RGB565 → 攒满 4 KB → qspi_write → 丢弃 → 下一行
 * 峰值 RAM 只有「4 KB 块缓冲 + 一行源数据（≤2 KB）」，**与图片尺寸无关**，
 * 所以 320×480（RGB565 307 KB）也放得下 —— heap1 的 115 KB 限制不再是约束。
 *
 * 【⚠⚠ 最大的坑：写 W25 必须退出 memory-mapped，而退出后读 XIP 会 BusFault】
 * 字模（0x000000，982 KB）与 IME 词典（0x300000，1.27 MB）都在 XIP 上，UI 每帧都在读。
 * 写 W25 前必须 exit_mmap；**但 qspi_write 只退不恢复**（只有 qspi_erase_sector 会恢复）。
 *
 * 实测（2026-10-08）：在 indirect 模式下读 XIP 窗口 ⇒ **精确 BusFault 升级成 HardFault**，
 *   g_fault.kind = 1、CFSR = 0x8200（PRECISERR + BFARVALID）、BFAR = 0x9001_0000，
 *   主循环就此卡死（g_loop_count 不再增长）。
 *   ⚠ 只**读一次**不会挂（探针 g_img_test=2 曾因此得出"没事"的错误结论），
 *     读上百次必挂 —— 这是一次典型的**假阴性**，靠单点采样下结论会翻车。
 *
 * ⇒ 两条硬规矩：
 *   ① 任何 indirect 写之后、下一次 XIP 读之前，**必须 qspi_enter_mmap()**
 *      （import_bmp 结尾与 img_store_poll 末尾各有一处，后者是兜底）；
 *   ② 导入期间 `g_img_busy = 1`，主循环跳过渲染与触摸（见 main.c 挂载处）。
 *
 * 【为什么 QSPI 必须按 4 KB 块写】
 * qspi_write 内部是「读整个扇区 → 擦 → 改 → 写回」。若每次只写 256 B page，
 * 每个 page 都会触发一次整扇区读改擦写（sector erase 约 45 ms）⇒ 慢几十倍。
 * 所以这里按 **4 KB 对齐的块**攒满再写，一个块正好一个扇区。
 *
 * 【BMP 行序与读写顺序（本文件最容易写错的地方）】
 * BMP 绝大多数是 **bottom-up**（文件里第 0 行 = 图像最底行），而 GYimg 要 top-down。
 * 同时 QSPI 只能按扇区批量写、不能逐行倒着写。
 * ⇒ 解法：**输出块从后往前写**（先写最后一个 4 KB 块），
 *   这样每个块对应的源行区间在文件里是**递增**的 ⇒ 源文件全程**顺序读**，
 *   块内也从后往前填行，连 f_lseek 都不用（详见 img_store.c 的 import_bmp）。
 * =========================================================================== */

#ifndef IMG_STORE_H
#define IMG_STORE_H

#include <stdint.h>

/* ---- W25Q128 上的布局 ----
 * 取 qspi_port.h 布局表的「0x100000 ~ 0x2FFFFF 图片/壁纸常量区」。
 * 实测该区**目前没有任何代码使用**（只在注释里出现）⇒ 可以拿来放图库。
 *   +0x000000  索引扇区 4 KB（每条 16 B，256 条）
 *   +0x001000  数据区（2 MB − 4 KB）
 */
#define IMG_BASE        0x00100000u
#define IMG_IDX_ADDR    (IMG_BASE)
#define IMG_DATA_ADDR   (IMG_BASE + 4096u)
#define IMG_DATA_SIZE   (0x00200000u - 4096u)
#define IMG_ENTRY_BYTES 16u
#define IMG_MAX_SLOTS   256u                /* 4096 / 16 */

#define IMG_MAGIC       0x4947u             /* 'IG'（小端存进 flash） */
#define IMG_FLAG_VALID  0x0001u

#define IMG_PATH_MAX    64u

/* ---- 诊断量（DTCM 的 .bss，SWD 直读）---- */

extern volatile uint32_t g_img_busy;        /* ★1 = 独占期，主循环必须跳过渲染/触摸 */
extern volatile uint32_t g_img_test;        /* 1=导入 BMP  2=XIP 冲突探针  3=清空图库 */
extern volatile uint32_t g_img_rc;          /* 0=成功；非 0 见 img_store.c 的 RC_* */
extern volatile uint32_t g_img_step;        /* 失败时看它卡在哪一步 */
extern volatile uint32_t g_img_slot;        /* 分配到的槽位 */
extern volatile uint32_t g_img_scale;       /* 抽样倍率：1=原尺寸 2=1/2 4=1/4 */
extern volatile uint32_t g_img_src_w;
extern volatile uint32_t g_img_src_h;
extern volatile uint32_t g_img_bpp;         /* 源位深（16 或 24）*/
extern volatile uint32_t g_img_out_w;
extern volatile uint32_t g_img_out_h;
extern volatile uint32_t g_img_out_bytes;   /* 写入 W25 的字节数 = out_w*out_h*2 */
extern volatile uint32_t g_img_used;        /* 导入后图库已用字节 */
extern volatile uint32_t g_img_fs_rc;       /* 最后一个 FRESULT */
extern volatile uint32_t g_img_qspi_rc;     /* 最后一个 QSPI 返回码 */
extern volatile uint32_t g_img_cyc;         /* 整轮 DWT 周期数 */
extern volatile uint32_t g_img_count;       /* ★上一次导入结束后写的图片数快照。
                                             * ⚠ 它不是"当前值"：**复位后是 0**（还没跑过导入），
                                             *   只有 img_store_poll 走完 mode 1 才会刷新。
                                             *   想知道"此刻图库里有多少张"要用 BoardGallery_Count()
                                             *   （它每次实时读索引）。2026-10-08 曾把 0 当成 bug 查。 */

/* 往卡里写文件（测试夹具：把 PC 侧的测试 BMP 分片送进 TF 卡）。
 * 为什么需要：TF 卡插在开发板上，PC 不能直接拷文件；拔卡再插要断电且得人工参与。
 * 于是让板子自己写：主机每次把 ≤4 KB 的一片塞进 s_blk，置 g_img_mk 触发。
 *   1 = 新建（CREATE_ALWAYS）并写本片   2 = 追加（OPEN_APPEND）写本片
 * 每片都 open→write→close，不留打开状态：中途复位最多丢最后一片，不会留半个文件。 */
extern volatile uint32_t g_img_mk;          /* 1=新建写片  2=追加写片 */
extern volatile uint32_t g_img_mk_len;      /* 本片字节数（从缓冲起，≤4096） */
extern volatile uint32_t g_img_mk_rc;       /* 0=成功 */
extern volatile uint32_t g_img_mk_total;    /* 累计写入字节数（应等于文件大小） */

/* QSPI 分步诊断（g_img_test=4）—— 定位"灌库写失败在哪一步"。
 * 两组对照，专门比较"退出映射前有没有做过大量 XIP 读"：
 *   A 组（无 XIP 读）：[0] exit_mmap rc  [1] write 4 KB rc  [2] 回读失配
 *   B 组（先读 XIP）：[3] enter_mmap rc [4] 256 次 XIP 读的校验和
 *                     [5] exit_mmap rc  [6] write 4 KB rc   [7] 回读失配
 * 返回码含义：0 = QSPI_OK，1 = COMM（通信超时），4 = STATE（状态不对）。
 * 为什么要它：直接调 qspi_write 只会返回一个笼统的 QSPI_ERR_COMM，
 * 分不清是"读扇区超时""擦除超时""页写超时"，也分不清是不是 XIP 读引起的。 */
extern volatile uint32_t g_img_qdiag[8];

/* 相册侧采样（诊断"为什么 UI 读到的图片数与 poll 快照不同"）：
 *   g_img_board_count = BoardGallery_Count() 最后一次的返回值
 *   g_img_slot_snap   = 那一刻从 XIP 读到的 slot0/slot1 的 (magic, flags) */
extern volatile uint32_t g_img_board_count;
extern volatile uint32_t g_img_slot_snap[4];

/* XIP 冲突探针（g_img_test=2）的结果：
 * 三个时刻读「偏移 0x300000（IME 词典首，实测 = 0x4D494D59 = "YMIM"）」得到的值。
 * ⚠ 探针地址必须选**已知非 0** 的位置：读字模区首会拿到 0，
 *   三个采样全 0 看起来"没问题"，其实是**什么都没测出来**（第一版就踩了）。 */
extern volatile uint32_t g_img_xip_inmap;    /* ① 正常映射中 */
extern volatile uint32_t g_img_xip_exited;   /* ② 退出映射后（★要测的就是它） */
extern volatile uint32_t g_img_xip_restored; /* ③ 重新进入映射后 */
extern volatile uint32_t g_img_xip_fault;    /* 探针期间有没有新增故障（0/1） */

/* 待导入的文件路径（SWD 写入，NUL 结尾） */
extern char g_img_path[IMG_PATH_MAX];

/* ---- 目录扫描（照片 / 笔记分类的真实文件列表，2026-10-09）----
 *
 * 【为什么不让 UI 直接调 f_readdir】
 *   phone_shell 是 third_party 的演示代码，不该认识 FatFs（与 BoardGallery_*
 *   同一套分层）；且 FILINFO 开了 LFN 后约 280 B，压进 UI 调用栈不划算。
 *   ⇒ 这里扫完缓存成定长表，UI 只取"名字 + 大小"。
 *
 * 【开销与调用时机】一次扫描 = 若干次 f_readdir（每次可能读一个目录扇区），
 *   实测在**毫秒级**，只在进入分类时扫一次，**不每帧扫**。g_scan_cyc 留证。
 *
 * 【⚠ 一张表串行复用】s_scan 只有一份，照片和笔记共用；切分类时重新扫。
 *   UI 不得跨分类混用索引。 */
#define IMG_SCAN_MAX      24u
#define IMG_SCAN_NAME_MAX 48u

#define IMG_PIC_DIR   "/YMGUI/PIC"
#define IMG_NOTE_DIR  "/YMGUI/NOTE"

extern volatile uint32_t g_scan_n;       /* 上次扫描到的条目数 */
extern volatile uint32_t g_scan_cyc;     /* 上次扫描耗时的 DWT 周期数 */
extern volatile uint32_t g_scan_fs_rc;   /* 最后一个 FRESULT */
extern volatile uint32_t g_scan_kind;    /* 上次扫的是哪个分类：1=照片 2=笔记 */

/* 笔记读写的诊断量（SWD 直读，判据用） */
extern volatile int32_t  g_note_rc;      /* 0 = 成功；<0 见 img_store.c 的返回码 */
extern volatile uint32_t g_note_bytes;   /* 上次读/写的字节数 */
extern volatile uint32_t g_note_fs_rc;   /* 最后一个 FRESULT */
extern volatile uint32_t g_note_cmp;     /* 自检（g_img_test=6）往返比对的失配字节数，0 = 完全一致 */

/* 扫描 dir 下后缀为 ext（传小写，如 ".bmp"）的文件，返回条目数；<0 = 失败
 * （目录不存在等，具体看 g_scan_fs_rc）。空目录返回 0。 */
int  img_scan_dir(const char* dir, const char* ext, int kind);
int  img_scan_count(void);
/* 第 i 条的名字（不含目录前缀）与字节数；i 越界返回 -1 */
int  img_scan_name(int i, char* out, int n);
int  img_scan_size(int i, uint32_t* size);
/* 拼出第 i 条的完整路径，给导入/打开用 */
int  img_scan_path(int i, const char* dir, char* out, int n);
/* 确保目录存在（已存在不算失败） */
int  img_scan_mkdir(const char* dir);
/* 触发异步导入第 i 条（置 g_img_test=1，由 img_store_poll 执行）。
 * 返回 0 = 已排队；<0 = 索引越界 / 上一轮还没跑完。 */
int  img_scan_import(int i, const char* dir);

/* 主循环挂载点（写 g_img_test 触发，跑完自动清 0） */
void img_store_poll(void);

/* 写文件夹具的挂载点（写 g_img_mk 触发，跑完自动清 0） */
void img_store_mk_poll(void);

/* ---- 查询接口（供相册 app 经 board 层调用）---- */
int  img_store_count(void);
int  img_store_slot_info(uint32_t slot, uint32_t *off, uint32_t *len,
                         uint32_t *w, uint32_t *h);
/* 返回该槽位像素在 XIP 窗口里的地址；无图返回 0 */
const void *img_store_pixels(uint32_t slot);

#endif /* IMG_STORE_H */
