#ifndef BOARD_ALLOC_H
#define BOARD_ALLOC_H

#include <stddef.h>

/* ===========================================================================
 * YMGUI 的双档内存分配器（对应库里的 GY_malloc0 / GY_malloc1）
 *
 *   alloc0 = 小 / 快：DTCM  —— 对象头、样式、事件、脏矩形表、字形表
 *   alloc1 = 大 / 慢：AXI SRAM —— draw buffer、framebuffer、解码图、字形位图
 *
 * 用两块**独立的池**，各带自己的空闲链表；不走 newlib 的 malloc（裸机没有堆管理）。
 * 每块都支持 alloc / realloc / free，free 时会与相邻空闲块合并。
 * =========================================================================== */

void *board_alloc0(size_t size);
void *board_realloc0(void *ptr, size_t size);
void  board_free0(void *ptr);

void *board_alloc1(size_t size);
void *board_realloc1(void *ptr, size_t size);
void  board_free1(void *ptr);

/* 运行期统计快照（只读，供 SWD 直接读取核对内存用量） */
typedef struct
{
    unsigned total;     /* 池总字节 */
    unsigned used;      /* 当前已分配字节 */
    unsigned peak;      /* 峰值已分配字节 */
    unsigned n_alloc;   /* 累计分配次数 */
} board_heap_stat;

const board_heap_stat *board_heap_stat0(void);
const board_heap_stat *board_heap_stat1(void);

#endif /* BOARD_ALLOC_H */
