/* ===========================================================================
 * STM32H743VIT6 链接脚本（GCC / ld）
 *
 * ⚠ 绝对不能从 0x20000000 一铺到底。
 *   H7 的 RAM 分散在三个电源域、地址不连续（RM0433 §2.3）：
 *     0x2000_0000  128 KB  DTCM          <- 栈、.data/.bss、heap0
 *     0x2002_0000 ~ 0x23FF_FFFF          <- **保留区，不可访问**（铺过去就 HardFault）
 *     0x2400_0000  512 KB  AXI SRAM      <- heap1（YMGUI 的 draw buffer / 图片 / 大对象）
 *     0x3000_0000  128 KB  SRAM1  (D2)
 *     0x3002_0000  128 KB  SRAM2  (D2)
 *     0x3004_0000   32 KB  SRAM3  (D2)
 *     0x3800_0000   64 KB  SRAM4  (D3)
 *
 * 本脚本至少显式声明 DTCM 与 AXI SRAM 两段（计划书 §3.4）。
 * 0x9000_0000 是 QSPI 映射区（只读），不进 RAM 段。
 * =========================================================================== */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K   /* STM32H743VIT6: 2 MB */
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
  AXI   (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
  SRAM1 (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
  SRAM2 (rwx) : ORIGIN = 0x30020000, LENGTH = 128K
  SRAM3 (rwx) : ORIGIN = 0x30040000, LENGTH = 32K
  SRAM4 (rwx) : ORIGIN = 0x38000000, LENGTH = 64K
}

/* 栈顶 = DTCM 末端，向下生长（Cortex-M 满递减栈） */
_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  /* ---- 向量表：必须落在 flash 起始，复位后硬件从 0x0800_0000 取 MSP 与 PC ---- */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  /* ---- 代码与只读数据 ---- */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
  .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

  /* ---- 已初始化数据：运行在 DTCM，初值存在 flash ---- */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > DTCM AT> FLASH

  /* ---- IME 大静态池（阶段 5 / T6）----
   * phone_ime 的候选词池/组合状态表/用户统计等约 60 KB，若留在 DTCM 的 .bss
   * 会把 heap0 挤**倒挂**（实测 _heap0_start > _heap0_end）。挪到空闲的 SRAM2。
   * 源码侧用 IME_BSS_ATTR（宏 YMGUI_IME_BSS_SECTION_NAME=bss_ime）指进本段；
   * 本段 NOLOAD，由 board_startup.c 显式清零（_sbss/_ebss 只覆盖 DTCM 的 .bss）。
   * ⚠ 两个位置陷阱（都踩过）：
   *   1. 必须放在 `.bss` 输出段**之前** —— .bss 里的通配 *(.bss*) 会先把它吃掉；
   *   2. 段定义本身带显式 `> SRAM2`，不影响后续裸符号赋值的区域上下文，
   *      但任何**裸**符号赋值（_sbrk_start 等）都不能夹在两个不同区域的段之间。
   */
  .bss_ime (NOLOAD) :
  {
    . = ALIGN(8);
    _sime_bss = .;
    *(.bss_ime)
    *(.bss_ime.*)
    . = ALIGN(8);
    _eime_bss = .;
  } > SRAM2

  /* ---- DMA 台架缓冲区（2026-10-04）----
   * 放在 **D2 域 SRAM1（0x3000_0000）**，理由三条（见 src/dma_bench.c 顶部）：
   *   1. **DMA1/DMA2 无法访问 DTCM**（TCM 是 CPU 专有，不在总线矩阵从设备侧），
   *      所以 DMA 缓冲区绝不能沿用 .bss 所在的 DTCM；SRAM1 与 DMA1/DMA2 同属 D2 域；
   *   2. 不与 heap1（AXI SRAM，峰值 97%）抢空间 —— SRAM1 现在是**完全空闲**的 128 KB；
   *   3. MPU region3 把 0x3000_0000 起 512 KB 配成 **CACHEABLE** ——
   *      这正是台架要复现的场景（DMA 绕过 D-Cache 造成的静默不一致）。
   *
   * ⚠ 与 .bss_ime 同样的两条位置陷阱：必须放在 `.bss` 输出段**之前**
   *   （否则被 `*(.bss*)` 通配吃掉，链接器不报错但静静不生效）；
   *   且本段 NOLOAD，需在 board_startup.c 里手动清零。
   *   缓冲区由源码侧 `__attribute__((aligned(32)))` 保证 32 字节对齐（cache line）。 */
  .bss_dma (NOLOAD) :
  {
    . = ALIGN(32);
    _sdma_bss = .;
    *(.bss_dma)
    *(.bss_dma.*)
    . = ALIGN(32);
    _edma_bss = .;
  } > SRAM1

  /* ---- QSPI 灌库暂存区（2026-10-07，方案 B 配套）----
   * 词组词典 1.27 MB 装不进内部 flash，只能由 host 经 SWD **分块**送进来
   * （见 src/qspi_provision.h 的长注释）。暂存区 16 KB。
   *
   * 为什么也放 SRAM1：
   *   1. 与 .bss_dma 同区、且不抢 heap1（AXI）；SRAM1 有 128 KB，
   *      .bss_dma 只用了 512 B，这里再占 16 KB 无压力；
   *   2. QSPI 的 indirect 收发是 **CPU 轮询 DR**（见 qspi_port.c，无 DMA），
   *      所以源缓冲只要在 CPU 能直访的地址上就行，不存在 DMA 可达性问题；
   *   3. MPU region3 把 0x3000_0000 起配成 cacheable —— 而 CPU 刚写完这块缓冲
   *      就要交给 QSPI 发走，缓存一致性靠 FORCEWT=1（写直达）保证，
   *      与 .bss_dma 台架实测的"0/64 失配"是同一条依据。
   *
   * ⚠ 两条位置陷阱同 .bss_dma：必须在 `.bss` 输出段**之前**（否则被 `*(.bss*)`
   *   通配静静吃掉），且 NOLOAD（16 KB 不占 bin 体积）。
   *   **不需要**在 board_startup.c 清零：每块都由 host 完整填满后才触发搬运。 */
  .bss_prov (NOLOAD) :
  {
    . = ALIGN(32);
    _sprov_bss = .;
    *(.bss_prov)
    *(.bss_prov.*)
    . = ALIGN(32);
    _eprov_bss = .;
  } > SRAM1

  /* ---- 第二块 band buffer（2026-10-04，第 7a 项：flush 与渲染重叠）----
   * 放 **D2 域 SRAM3（0x3004_0000，32 KB）**，此前完全空闲。理由：
   *   1. 与 DMA1 同属 D2 域，DMA 读它不必跨域；且**不占 heap1（AXI）** ——
   *      heap1 刚回收 30 KB，不该再被 20 KB 吃回去；
   *   2. MPU region3 把 0x3000_0000 起 512 KB 配成 cacheable，SRAM3 与 SRAM1 同区；
   *      而 `tools/dma_check.py` 已在该区实测"CPU 写 → DMA 读" **0/64 失配**
   *      （FORCEWT=1 让 CPU 写直达内存）—— 所以"CPU 渲染、DMA 读"这条路
   *      在 SRAM3 上是有实证的，不是推断；
   *   3. 与 .bss_ime / .bss_dma 同样的两条位置陷阱，必须照做：
   *      · 必须放在 `.bss` 输出段**之前**，否则被 `*(.bss*)` 通配静静吃掉；
   *      · 本段 NOLOAD。**本缓冲不需要在 board_startup.c 里清零**：每条 band 交给
   *        DMA 之前都会被 drawObjRec 整块渲染，不存在未初始化读。
   */
  .bss_sram3 (NOLOAD) :
  {
    . = ALIGN(32);
    _ssram3_bss = .;
    *(.bss_sram3)
    *(.bss_sram3.*)
    . = ALIGN(32);
    _esram3_bss = .;
  } > SRAM3

  /* ---- 零初始化数据 ---- */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > DTCM

  . = ALIGN(8);
  _end = .;
  /* newlib 的 _sbrk 引用符号 `end` 作为堆起点，不提供会报
   * `undefined reference to 'end'`。（本工程另外自己实现了 _sbrk，
   * 见 src/board_startup.c；这个符号只为兼容库内引用保留。） */
  PROVIDE(end = .);

  /* ---- newlib sbrk 保留区（16 KB）★必须与 heap0 严格分离★ ----
   *
   * newlib 的 malloc 经 _sbrk 取内存。printf 第一次调用时会用 malloc 给
   * stdout 分配行缓冲，之后**每次 printf 都往那个缓冲区写格式化结果**。
   *
   * ⚠ 实测踩坑（2026-10-03）：这一行原先是 `_heap0_start = ALIGN(8)`，即
   *   _heap0_start 与 _end（= sbrk 堆起点）**重合同一个地址 0x2000_04B0**。
   *   于是 stdio 行缓冲与 YMGUI 的 ctx 落在同一块内存上：
   *     gt9xxx_init() 的 printf("CTP ID:%s\r\n", temp) 把
   *     "CTP ID:1158\r\n" 写进 0x2000_04B8 —— 正好盖掉 ctx->root 与
   *     ctx->top_layer。随后 YMGUI_Inject_Pointer() 读 ctx->root（变成
   *     0x0D383531 这种野指针）→ 精确总线错误 → HardFault → 主循环假死。
   *   保持两者分离后，stdio 与 YMGUI 各用各的内存，互不影响。
   */
  _sbrk_start = ALIGN(8);
  _sbrk_end   = _sbrk_start + 0x4000;          /* 16 KB，仅供 stdio 等零星使用 */

  /* ---- YMGUI 双档堆（计划书 §3.4）----
   *   heap0 = 小/快：DTCM 剩余部分（对象头、样式、事件、脏矩形表）
   *   heap1 = 大/慢：AXI SRAM 整块（draw buffer、图片、字形位图）
   * 由 src/board_alloc.c 的堆分配器使用，不走 newlib malloc。
   */
  _heap0_start = _sbrk_end;                    /* ★ 起点在 sbrk 保留区之后 ★ */
  /* 栈留在 DTCM 顶部，向下生长（_estack = DTCM 末端）。
   * 留 32 KB：YMGUI 的事件派发/命中测试是递归的，深树 + 控件回调嵌套会吃掉不少栈；
   * 32 KB 是留裕量，**不是**某个已复现故障的修复（早期曾把 8 KB 栈当作
   * 注入卡死的原因，后经实测排除：栈加大到 32 KB 后现象不变，真正原因是
   * sbrk 堆与 heap0 重合导致的 ctx 被 printf 输出覆盖）。 */
  _heap0_end   = _estack - 0x8000;

  /* ---- AXI SRAM（512 KB）的用途划分 ----
   * 起始 300 KB：**全帧镜像缓冲**。YMGUI 的 flush_cb 只给分条(band)像素，
   *   而验收/调试需要整屏，所以把每条 band 累积到这里，再用 SWD 读出来还原 PNG。
   *   320 × 480 × 2 B = 307200 B = 0x4B000。
   * 其余 212 KB：heap1（YMGUI 的大对象、图片、draw buffer）。
   */
  _frame_buf     = ORIGIN(AXI);
  _frame_buf_end = ORIGIN(AXI) + 0x4B000;
  _heap1_start   = ORIGIN(AXI) + 0x4B000;
  _heap1_end     = ORIGIN(AXI) + LENGTH(AXI);

  /DISCARD/ :
  {
    *(.note*)
    *(.comment)
  }
}
