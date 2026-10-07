/* ===========================================================================
 * GB2312 字库灌入 QSPI（计划书 §6.4 布局 + §6.5 字体接入）
 *
 * 字库本体是 `tools/gb2312_glyphs.bin`（982016 B = 7672 字 × 128 B，
 * 第 i 字在 i×128 偏移，16×16 4bpp）。它要被放到 QSPI 偏移 0，
 * 于是 memory-mapped 之后 `0x9000_0000 + i*128` 就是第 i 个字的点阵。
 *
 * 载荷从哪来：用 `objcopy -I binary` 把 .bin 做成一个 `.rodata` 目标文件编进
 * **内部 flash**，当作"出厂载荷"。这样不需要任何主机侧烧写工具，固件自带灌库能力，
 * 换板/换片都能自己灌好，且可重复、可验证。
 *
 * 为什么需要记账扇区：Flash 有擦写寿命（10 万次），不能每次上电都重灌 982 KB
 * （约 20 s + 240 次擦除）。所以在一个空闲扇区放 64 B 的头 {magic,size,crc}：
 *   头匹配 → 从 XIP 读回重算 CRC 复核 → 通过就跳过写入；
 *   否则    → 擦 + 分块写 + 写头 + **逐字节 memcmp 回读复核**。
 *
 * 记账扇区的位置：字模区（0~1 MB）尾部的空闲处 0x000F_F000。
 * 字库只占 0x0000_0000~0x000E_FBFF，其后到 1 MB 都是空的，所以这**不占用**
 * 计划书 §6.4 分配给字模/图片/词典/文件系统的任何区域；也**避开了** §6.4
 * 明确要求留空的最后扇区 0x00FF_F000。
 * =========================================================================== */

#ifndef FONT_PROVISION_H
#define FONT_PROVISION_H

#include <stdint.h>

/* 确保字库在 QSPI 里可用。返回 0 = 可用（原本就在，或本次写好了）；
 * 非 0 = 不可用（此时 phone 壳会退回内置的少量 CJK 字模，界面不至于崩）。 */
int font_provision_ensure(void);

/* 调试用：强制重灌（忽略记账头）。 */
void font_provision_set_force(int force);

/* 从 **XIP（memory-mapped）** 读回一段并算 CRC32（IEEE 802.3，与 zlib.crc32 一致）。
 * 原先是 font_provision.c 内部的 static；灌库台架要用它做"写完之后再读一遍"的
 * 端到端复核，所以对外暴露 —— 校验走的是**真正运行时的那条读路径**（XIP），
 * 而不是 indirect 读，这样"灌库成功但 XIP 读不通"也能被抓出来。
 * 调用前必须已进入映射模式。 */
uint32_t font_crc32_xip(uint32_t off, uint32_t n);

/* ---- 供 SWD 阅读 ---- */
extern volatile uint32_t g_font_ok;         /* 1 = 字库可用 */
extern volatile uint32_t g_font_written;    /* 1 = 本次上电真的写了 QSPI */
extern volatile uint32_t g_font_rc;         /* 0 = 成功；非 0 见 font_provision.c 的错误码注释 */
extern volatile uint32_t g_font_src_size;   /* 内嵌载荷大小（期望 982016 = 0xEFC00） */
extern volatile uint32_t g_font_crc_hdr;    /* 记账头里存的 CRC32 */
extern volatile uint32_t g_font_crc_calc;   /* 从 QSPI 读回重算的 CRC32（应与上面相等） */
extern volatile uint32_t g_font_mismatch;   /* 回读 memcmp 的失配字节数（目标 0） */
extern volatile uint32_t g_font_erase_cnt;  /* 本次擦除的扇区数 */
extern volatile uint32_t g_font_cmp_bytes;  /* 本次回读比对的字节数（目标 982016） */

/* 内嵌载荷的边界符号（由 objcopy -I binary 生成，见 CMakeLists.txt） */
extern const uint8_t _binary_gb2312_bin_start[];
extern const uint8_t _binary_gb2312_bin_end[];

#endif /* FONT_PROVISION_H */
