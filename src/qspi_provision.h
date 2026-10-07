/* ===========================================================================
 * QSPI 分块灌库（2026-10-07，方案 B 配套）
 *
 * 【要解决的问题】词组词典 phrases_xip.bin 是 **1.27 MB**，而内部 flash 只剩
 * ~508 KB —— 装不下，所以不能像 gb2312 字模那样"objcopy 内嵌 + 开机自灌"
 * （font_provision.c 就是那条路）。1.27 MB 也不可能一次性进任何一块 RAM
 * （最大连续空区是 SRAM1 的 127.5 KB）。
 *
 * 【做法】host 经 SWD 把数据**分块**写进 SRAM1 的暂存区，固件再把这一块
 * 写进 W25Q128；循环 ~80 次（16 KB/块）。全部由 host 脚本 tools/qspi_provision.py
 * 驱动，本文件只提供"搬一块"的能力和一组可读写状态字。
 *
 * 【为什么不做成开机自灌】灌库是一次性的产线动作，不是每次上电都要做的事。
 * 编进默认构建会永久占掉 16 KB SRAM1 和一点 flash；所以由
 * CMake option `YMGUI_QSPI_PROVISION`（默认 OFF）控制。
 * 灌完之后：关掉该 option 重新编出货形态，词典**留在外部 flash 里不动**。
 *
 * ⚠ 灌库会**退出 XIP 映射模式**（indirect 才能擦写），写完必须重新进映射，
 *   否则整屏中文与词典一起消失 —— 见 qspi_provision_poll 末尾。
 * =========================================================================== */

#ifndef QSPI_PROVISION_H
#define QSPI_PROVISION_H

#include <stdint.h>

/* 单次能搬的最大字节数（= 暂存区大小）。host 必须与这里一致，脚本从
 * g_prov_buf_size 读回实际值并自检，不靠两边硬编码对齐。
 *
 * 【为什么是 120 KB 而不是 16 KB】每块都要 host 经 gdb 送一次数据，而
 * `probe-rs gdb` server 与 `probe-rs` CLI **不能同时占用探针**（实测：CLI 一开，
 * server 那条连接就 "Target disconnected"），所以 server 得起起停停，
 * 每块的固定开销 ~10 s。16 KB ⇒ 80 块 ≈ 13 分钟；120 KB ⇒ **11 块 ≈ 2 分钟**。
 * SRAM1 有 128 KB、只被 .bss_dma 占 512 B，放得下；本段 NOLOAD，不进 bin。 */
#define QSPI_PROV_CHUNK (120u * 1024u)

/* 状态机：0 空闲 / 1 忙 / 2 上一个块成功 / 3 上一个块失败 */
extern volatile uint32_t g_prov_state;
/* 主机写 1 触发一次搬运；固件跑完清 0。（g_prov_addr / g_prov_len 要先写好） */
extern volatile uint32_t g_prov_cmd;
/* 本次搬运的目标 QSPI 字节地址与长度（长度 <= QSPI_PROV_CHUNK） */
extern volatile uint32_t g_prov_addr;
extern volatile uint32_t g_prov_len;
/* 最近一次 qspi_write 的返回码（0 = QSPI_OK） */
extern volatile uint32_t g_prov_rc;
/* 累计：已写字节 / 已完成块数 / 擦除扇区数 */
extern volatile uint32_t g_prov_done;
extern volatile uint32_t g_prov_chunks;
extern volatile uint32_t g_prov_erase;
/* 暂存区地址与大小 —— host 写数据前先读这两个，避免"写到了别处" */
extern volatile uint32_t g_prov_buf_addr;
extern volatile uint32_t g_prov_buf_size;
/* 端到端复核：主机写完最后一块后写 `g_prov_verify = 1`，固件从 **XIP** 把
 * [g_prov_addr, +g_prov_len) 读回来算 CRC32 放进 g_prov_crc（跑完清 0）。
 * ⚠ 走 XIP 而不是 indirect 读是刻意的：要验的是"运行时那条读路径通不通"。 */
extern volatile uint32_t g_prov_verify;
extern volatile uint32_t g_prov_crc;

/* 1 = 本固件编入了灌库支持；0 = 没有（脚本据此直接报"请先打开 option"） */
extern volatile uint32_t g_prov_enabled;

/* 在主循环里调用：看到 g_prov_cmd==1 就搬一块。非灌库构建里是空函数。 */
void qspi_provision_poll(void);

#endif /* QSPI_PROVISION_H */
