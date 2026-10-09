/* ===========================================================================
 * FatFs 移植层（路线图 L2-3）
 *
 * 【它解决什么】
 * L2-2 证明了"扇区读写的数据通路是对的"，但扇区不是文件。要让 16 个 APP
 * 从"演示态"变成"有真实数据"（笔记/设置/截屏落盘），必须有文件系统。
 *
 * 【为什么选 FatFs】
 * 厂商资料包「实验27 FATFS实验」自带 ST 分发的 FatFs（R0.12c 系），
 * 与本项目同为 HAL + 裸机环境，配置可直接复用；许可为 FatFs 的 BSD 1-Clause，
 * 随固件分发没有障碍。源码拷到 third_party/FatFs/source，只改 ffconf.h，
 * **diskio 自己写**（厂商那份绑定他们自己的 SD 驱动，这里改接 sd_card.c）。
 *
 * 【关键配置（相对厂商原值的改动，理由都写在 ffconf.h 对应行附近）】
 *   FF_USE_LFN   3 → **1**：3 是"LFN 缓冲放堆"，要 ff_memalloc ⇒ 走 newlib malloc，
 *                          而本工程的 _sbrk 池只有 16 KB 且刻意与 YMGUI 堆隔离；
 *                          改 1 = 静态缓冲（FF_MAX_LFN=255），不碰 malloc。
 *   FF_FS_NORTC  0 → **1**：本工程**没有接 RTC**（时间甚至是 UI 里的硬编码字符串），
 *                          留 0 会要求实现 get_fattime()，给出的是假时间反而误导。
 *   FF_USE_MKFS  1 → 0：不需要在固件里格式化用户的卡 —— 那是最危险的一个功能，
 *                          不编进去就不会被误触发。
 *   FF_VOLUMES   2 → 1：只有一张卡。
 *   FF_CODE_PAGE 936 → **437**（2026-10-09 改，省 178 KB FLASH）：
 *                          936 是双字节（GBK）转码表，在 ffunicode.c 里占
 *                          1894~7350 行 ≈ 170 KB 纯 .rodata；437 是单字节表，12 行。
 *                          **代价：FatFs 不再认得中文字符**（不做 GBK↔UTF-16 转码）。
 *                          实测边界（2026-10-09 板上，不是猜的）：
 *                          · ASCII 路径一切照旧（建目录 / 写 / 读 / 删 全 OK）；
 *                          · **新建 GBK 中文名能成功**，STM32 自己写→自己读
 *                            往返字节一致（437 的 SBCS 表 0x80~0xFF 映射可逆，
 *                            "风景照.bmp" 读回仍是 B7 E7 BE B0 D5 D5 2E 62 6D 70），
 *                            f_open / f_unlink 也都能用；
 *                          · **但 LFN 里存的不是汉字 Unicode** —— 0xB7 经 uc437 表
 *                            映射成 U+2556（制表符）、0xE7 → U+03C4
 *                            ⇒ 卡拔到 PC 上显示乱码，PC 建的中文名 STM32 读不回原字节。
 *                          ⇒ 所以代价是"**卡与 PC 的中文名不互通**"，不是"STM32 不能用中文名"。
 *                            本工程所有落盘路径仍一律 ASCII（见 img_store.c 的
 *                            IMG_PIC_DIR / IMG_NOTE_DIR / note<N>.txt）。
 *   FF_FS_EXFAT  1（保留）：32 GB 的卡很可能是 exFAT，关掉就挂不上。
 *
 * 【数据安全】
 * 自检只在卡上建一个「/YMGUI」目录，在里面写一个测试文件、读回比对、
 * ⚠ 注释里写这个目录名时**不要用两个星号把它夹起来**（像 ** /YMGUI ** 那样）：
 *   星号加斜杠会提前闭合本块注释，编译器随后把中文当代码解析 —— 2026-10-08 实测踩到，
 *   报的是 "unknown type name" / "stray '\343'" 这种完全看不出原因的错。
 * 然后**删除该文件**。不格式化、不动任何既有文件，也不写扇区级数据。
 * =========================================================================== */

#ifndef FATFS_PORT_H
#define FATFS_PORT_H

#include <stdint.h>

/* 测试文件路径（建在 /YMGUI 下；目录保留，给后续持久化用） */
#define FS_TEST_DIR   "/YMGUI"
#define FS_TEST_PATH  "/YMGUI/fs_selftest.txt"
#define FS_TEXT_BYTES 1024u          /* 写 1 KB：跨簇，能暴露簇链问题 */

/* ---- 诊断量（DTCM 的 .bss，SWD 直读）---- */

extern volatile uint32_t g_fs_test;        /* 写 1 触发一轮文件系统自检 */
extern volatile uint32_t g_fs_state;       /* 0=未跑 1=挂载失败 2=跑完 */
extern volatile uint32_t g_fs_mount_rc;    /* f_mount 的 FRESULT（0=FR_OK） */
extern volatile uint32_t g_fs_fstype;      /* 1=FAT12 2=FAT16 3=FAT32 4=exFAT */
extern volatile uint32_t g_fs_mkdir_rc;    /* f_mkdir("/YMGUI")（FR_EXIST=8 也算正常） */
extern volatile uint32_t g_fs_open_rc;     /* 写方式打开 */
extern volatile uint32_t g_fs_write_rc;
extern volatile uint32_t g_fs_written;     /* 实际写入字节数（应 = FS_TEXT_BYTES） */
extern volatile uint32_t g_fs_close_rc;
extern volatile uint32_t g_fs_size;        /* 文件大小（f_size） */
extern volatile uint32_t g_fs_open2_rc;    /* 读方式打开 */
extern volatile uint32_t g_fs_read_rc;
extern volatile uint32_t g_fs_readn;       /* 实际读出字节数 */
extern volatile uint32_t g_fs_mis;         /* ★判据：读回与写入内容逐字节失配数（0） */
extern volatile uint32_t g_fs_unlink_rc;   /* 删除测试文件 */
extern volatile uint32_t g_fs_free_kb;     /* 卡剩余空间 KB */
extern volatile uint32_t g_fs_getfree_rc;
extern volatile uint32_t g_fs_cyc;         /* 整轮 DWT 周期数 */

/* 自检入口（挂在主循环，写 g_fs_test=1 触发） */
void fatfs_bench_poll(void);

/* 供其它模块（图库导入、笔记落盘等）复用同一份挂载：
 * 已挂载就直接返回 0，未挂载则 f_mount 一次。返回 FRESULT（0 = FR_OK）。
 * 为什么不各模块自己 f_mount：FATFS 对象里有 512 B 的扇区窗口 win[]，
 * 每个模块各来一份既浪费 RAM，又会在多处各持一份"当前簇链"状态，容易互相踩。 */
int fatfs_ensure_mounted(void);

#endif /* FATFS_PORT_H */
