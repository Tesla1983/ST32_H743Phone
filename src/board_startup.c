/* ===========================================================================
 * 板级最早期初始化：C 运行时环境搭建
 *
 * 由 src/startup_gcc.s 的 Reset_Handler 调用，**早于任何 C 全局变量可用之前**
 * ——此时 .data 还没搬、.bss 还没清，所以本文件里不能引用任何已初始化的全局
 * 变量（只能用局部变量 + 链接脚本提供的符号地址）。
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>        /* ptrdiff_t（_sbrk 的签名用） */

/* 链接脚本 linker/stm32h743vit6.ld 提供的符号 */
extern uint32_t _sidata;   /* .data 段在 flash 里的初值起始 */
extern uint32_t _sdata;    /* .data 段在 RAM(DTCM) 的起始 */
extern uint32_t _edata;    /* .data 段结束 */
extern uint32_t _sbss;     /* .bss 起始 */
extern uint32_t _ebss;     /* .bss 结束 */
extern uint32_t _sime_bss; /* IME 大静态池段（.bss_ime → SRAM2）起始 */
extern uint32_t _eime_bss; /* 同上，结束 */
extern uint32_t _sdma_bss; /* DMA 台架缓冲区段（.bss_dma → SRAM1）起始 */
extern uint32_t _edma_bss; /* 同上，结束 */
extern uint32_t _ssd_bss;  /* TF 卡数据缓冲段（.bss_sd → SRAM1）起始 */
extern uint32_t _esd_bss;  /* 同上，结束 */

void board_init_memory(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst;

    /* 1) 把 .data 的初值从 flash 搬到 DTCM */
    for (dst = &_sdata; dst < &_edata;)
    {
        *dst++ = *src++;
    }

    /* 2) 清 .bss */
    for (dst = &_sbss; dst < &_ebss;)
    {
        *dst++ = 0u;
    }

    /* 3) 清 IME 大静态池段（NOLOAD，不进固件镜像，必须这里清零） */
    for (dst = &_sime_bss; dst < &_eime_bss;)
    {
        *dst++ = 0u;
    }

    /* 4) 清 DMA 台架缓冲区段（NOLOAD → SRAM1）。
     * ⚠ 类型上它是 uint32_t[]（4 字节），与这里按 uint32_t 步进的清零方式一致；
     *   若以后往该段放非 4 字节对齐的类型，这里的循环要跟着改。 */
    for (dst = &_sdma_bss; dst < &_edma_bss;)
    {
        *dst++ = 0u;
    }

    /* 5) 清 TF 卡数据缓冲段（NOLOAD → SRAM1）。
     * 逻辑上缓冲每次都会被代码填满（备份扇区也是先读后比），清零并非必需；
     * 这里做是为了让"没跑过自检时读到的内容"是确定的全 0，而不是上电随机值，
     * 免得把随机内容误读成"卡里读出的数据"。 */
    for (dst = &_ssd_bss; dst < &_esd_bss;)
    {
        *dst++ = 0u;
    }
}

/* ===========================================================================
 * 供 newlib 的 __libc_init_array 使用。
 *
 * 我们用 `-nostartfiles`（启动由 src/startup_gcc.s 自己管），此时 crt0 不再提供
 * `_init`/`_fini`，而 __libc_init_array 会无条件调用它们 —— 缺了就报
 * `undefined reference to '_init'`。
 * 本工程没有 C++ 全局构造与 .init 段，给两个空实现即可。
 * =========================================================================== */
void _init(void) {}
void _fini(void) {}

/* ===========================================================================
 * newlib 的 _sbrk：malloc 的底层取内存入口。
 *
 * 为什么必须自己实现（★这是本工程一个已被实测复现的真实故障★）：
 *   库里的 _sbrk 用链接脚本符号 `end` 当堆起点。而链接脚本原先把
 *   `_heap0_start` 也设成 `ALIGN(8)`，两者落在**同一个地址 0x2000_04B0** ——
 *   于是 newlib 的 stdio 缓冲区与 YMGUI 的堆**重叠**。
 *   现象：gt9xxx_init() 里那句 printf("CTP ID:%s\r\n", temp) 的输出
 *   "CTP ID:1158\r\n" 被写进 0x2000_04B8，正好覆盖 ctx->root / ctx->top_layer；
 *   之后 YMGUI_Inject_Pointer() 解引用被破坏的 ctx->root → 精确总线错误
 *   （BFAR=0x0D38353F）→ HardFault → 主循环假死。
 *
 * 现在改成从链接脚本里**独立的** `_sbrk_start.._sbrk_end`（16 KB）取内存，
 * 与 YMGUI 的 heap0 完全分离；池耗尽返回 -1，malloc 会返回 NULL，
 * **绝不会越界写坏别的内存**。
 *
 * 说明：这里不回填 errno —— newlib 的 _malloc_r 只判 `(char*)-1` 这个返回值，
 *      不依赖 errno。
 * =========================================================================== */
extern uint8_t _sbrk_start[];
extern uint8_t _sbrk_end[];

static uint8_t *s_sbrk_cur;      /* 位于 .bss，由 board_init_memory 清零 */

void *_sbrk(ptrdiff_t incr)
{
    uint8_t  *prev;
    uintptr_t next;

    if (s_sbrk_cur == NULL)
    {
        s_sbrk_cur = _sbrk_start;
    }

    prev = s_sbrk_cur;

    if (incr <= 0)
    {
        return (void *)prev;     /* 查询当前 break（负增量不支持） */
    }

    /* 向上取整到 8 字节，保证后续返回的每一段都对齐 */
    next = ((uintptr_t)prev + (uintptr_t)incr + 7u) & ~(uintptr_t)7u;

    if (next > (uintptr_t)_sbrk_end)
    {
        return (void *)-1;       /* 池耗尽，拒绝分配 */
    }

    s_sbrk_cur = (uint8_t *)next;
    return (void *)prev;
}
