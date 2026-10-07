/* ===========================================================================
 * YMGUI 双档内存分配器实现
 *
 * 算法：**带边界合并的空闲链表**（first-fit + 立即合并前后邻）。
 *
 * 块布局（8 字节对齐）：
 *     +0  uint32 size   本块总大小（**含**头部），低 3 位恒为 0
 *     +4  uint32 free   1 = 空闲（同时用于检测重复释放）
 *     +8  Block *next   空闲链表指针（仅空闲块有效）
 *     +16 payload       <= 返回给调用者的地址
 *
 * 为什么不用 newlib 的 malloc：裸机没有系统调用，_sbrk 在 nosys.specs 下永远失败；
 * 而且 YMGUI 需要**把大小对象分到不同物理内存**（DTCM 快 / AXI 大），
 * malloc 做不到这点。
 *
 * 池的边界由链接脚本提供：_heap0_* 在 DTCM，_heap1_* 在 AXI SRAM。
 * =========================================================================== */

#include "board_alloc.h"

#include <stdint.h>
#include <string.h>

/* 链接脚本 linker/stm32h743vit6.ld 提供 */
extern uint8_t _heap0_start[];
extern uint8_t _heap0_end[];
extern uint8_t _heap1_start[];
extern uint8_t _heap1_end[];

#define ALIGN8(x)   (((uint32_t)(x) + 7u) & ~7u)
#define MIN_SPLIT   64u          /* 分裂后剩余小于此值就不再分裂，避免碎块 */

typedef struct Block
{
    uint32_t      size;   /* 总大小（含头部） */
    uint32_t      free;   /* 1 = 空闲 */
    struct Block *next;   /* 空闲链表 */
} Block;

#define HDR_SIZE  ((uint32_t)((sizeof(Block) + 7u) & ~7u))

typedef struct
{
    Block   *free_head;
    uint8_t *base;
    uint32_t total;
    uint32_t used;
    uint32_t peak;
    uint32_t n_alloc;
} Heap;

static Heap            g_h0;
static Heap            g_h1;
static board_heap_stat g_st0;
static board_heap_stat g_st1;
static int             g_inited;

static void heap_init(Heap *h, uint8_t *base, uint8_t *end)
{
    uint32_t total = (uint32_t)(end - base);

    h->base      = base;
    h->total     = total & ~7u;
    h->used      = 0;
    h->peak      = 0;
    h->n_alloc   = 0;

    /* 整段作为一个大空闲块 */
    Block *b = (Block *)base;
    b->size  = h->total;
    b->free  = 1;
    b->next  = NULL;
    h->free_head = b;
}

static void *heap_alloc(Heap *h, uint32_t size)
{
    uint32_t need;
    Block   *prev = NULL;
    Block   *b;

    if (size == 0)
    {
        return NULL;
    }

    need = ALIGN8(size) + HDR_SIZE;
    if (need < MIN_SPLIT)
    {
        need = MIN_SPLIT;
    }
    need = ALIGN8(need);

    for (b = h->free_head; b != NULL; prev = b, b = b->next)
    {
        if (b->size < need)
        {
            continue;
        }

        /* 剩余够放一个块就分裂，否则整块给出去 */
        if (b->size - need >= MIN_SPLIT)
        {
            Block *nb = (Block *)((uint8_t *)b + need);
            nb->size = b->size - need;
            nb->free = 1;
            nb->next = b->next;

            if (prev != NULL) { prev->next = nb; } else { h->free_head = nb; }
            b->size = need;
        }
        else
        {
            if (prev != NULL) { prev->next = b->next; } else { h->free_head = b->next; }
        }

        b->free = 0;
        h->used += b->size;
        if (h->used > h->peak)
        {
            h->peak = h->used;
        }
        h->n_alloc++;

        return (uint8_t *)b + HDR_SIZE;
    }

    return NULL;   /* 池耗尽 */
}

static void heap_free(Heap *h, void *ptr)
{
    Block *b;
    Block *prev;
    Block *cur;
    uint8_t *p;

    if (ptr == NULL)
    {
        return;
    }

    b = (Block *)((uint8_t *)ptr - HDR_SIZE);
    p = (uint8_t *)b;

    /* 越界保护：指针不在本池内就直接忽略，避免写坏别的内存 */
    if (p < h->base || p >= h->base + h->total)
    {
        return;
    }
    if (b->free)
    {
        return;   /* 重复释放，忽略 */
    }

    b->free = 1;
    h->used -= b->size;

    /* 按地址顺序插回空闲链表 */
    prev = NULL;
    cur  = h->free_head;
    while (cur != NULL && cur < b)
    {
        prev = cur;
        cur  = cur->next;
    }
    b->next = cur;
    if (prev != NULL) { prev->next = b; } else { h->free_head = b; }

    /* 与后邻合并 */
    if (b->next != NULL && (uint8_t *)b + b->size == (uint8_t *)b->next)
    {
        b->size += b->next->size;
        b->next  = b->next->next;
    }
    /* 与前邻合并 */
    if (prev != NULL && (uint8_t *)prev + prev->size == (uint8_t *)b)
    {
        prev->size += b->size;
        prev->next  = b->next;
    }
}

static void *heap_realloc(Heap *h, void *ptr, uint32_t size)
{
    Block   *b;
    uint32_t cap;
    void    *np;

    if (ptr == NULL)
    {
        return heap_alloc(h, size);
    }
    if (size == 0)
    {
        heap_free(h, ptr);
        return NULL;
    }

    b   = (Block *)((uint8_t *)ptr - HDR_SIZE);
    cap = b->size - HDR_SIZE;

    /* 原块够用就直接复用（不收缩，避免额外分裂开销） */
    if (cap >= size)
    {
        return ptr;
    }

    /* 不够就新开一块、拷过去、释放旧的 */
    np = heap_alloc(h, size);
    if (np == NULL)
    {
        return NULL;
    }
    memcpy(np, ptr, cap);
    heap_free(h, ptr);
    return np;
}

/* ---------------- 对外接口 ---------------- */

static void ensure_init(void)
{
    if (g_inited)
    {
        return;
    }
    g_inited = 1;
    heap_init(&g_h0, _heap0_start, _heap0_end);
    heap_init(&g_h1, _heap1_start, _heap1_end);
}

void *board_alloc0(size_t size)
{
    ensure_init();
    return heap_alloc(&g_h0, (uint32_t)size);
}

void *board_realloc0(void *ptr, size_t size)
{
    ensure_init();
    return heap_realloc(&g_h0, ptr, (uint32_t)size);
}

void board_free0(void *ptr)
{
    ensure_init();
    heap_free(&g_h0, ptr);
}

void *board_alloc1(size_t size)
{
    ensure_init();
    return heap_alloc(&g_h1, (uint32_t)size);
}

void *board_realloc1(void *ptr, size_t size)
{
    ensure_init();
    return heap_realloc(&g_h1, ptr, (uint32_t)size);
}

void board_free1(void *ptr)
{
    ensure_init();
    heap_free(&g_h1, ptr);
}

const board_heap_stat *board_heap_stat0(void)
{
    ensure_init();
    g_st0.total   = g_h0.total;
    g_st0.used    = g_h0.used;
    g_st0.peak    = g_h0.peak;
    g_st0.n_alloc = g_h0.n_alloc;
    return &g_st0;
}

const board_heap_stat *board_heap_stat1(void)
{
    ensure_init();
    g_st1.total   = g_h1.total;
    g_st1.used    = g_h1.used;
    g_st1.peak    = g_h1.peak;
    g_st1.n_alloc = g_h1.n_alloc;
    return &g_st1;
}
