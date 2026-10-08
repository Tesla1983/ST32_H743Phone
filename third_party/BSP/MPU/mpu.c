/**
 ****************************************************************************************************
 * @file        mpu.c
 * @version     V1.0
 * @brief       MPU内存保护 驱动代码
 ****************************************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:    STM32H743VIT6小系统板
 *
 ****************************************************************************************************
 */
 
#include "./BSP/MPU/mpu.h"
#include "./BSP/LED/led.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "board_fault.h"    /* ★移植改动：故障现场快照（见 src/board_fault.h） */
 
 
/**
 * @brief       设置某个区域的MPU保护
 * @param       baseaddr:MPU保护区域的基址(首地址)
 * @param       size:MPU保护区域的大小(必须是32的倍数,单位为字节),可设置的值参考:CORTEX_MPU_Region_Size
 * @param       rnum:MPU保护区编号,范围:0~7,最大支持8个保护区域,可设置的值参考：CORTEX_MPU_Region_Number
 * @param       de:禁止指令访问;0,允许指令访问;1,禁止指令访问
 * @param       ap:访问权限,访问关系如下:可设置的值参考：CORTEX_MPU_Region_Permission_Attributes
 *   @arg       MPU_REGION_NO_ACCESS,无访问（特权&用户都不可访问）
 *   @arg       MPU_REGION_PRIV_RW,仅支持特权读写访问
 *   @arg       MPU_REGION_PRIV_RW_URO,禁止用户写访问（特权可读写访问）
 *   @arg       MPU_REGION_FULL_ACCESS,全访问（特权&用户都可访问）
 *   @arg       MPU_REGION_PRIV_RO,仅支持特权读访问
 *   @arg       MPU_REGION_PRIV_RO_URO,只读（特权&用户都不可以写）
 * @note        详见:STM32H7编程手册.pdf,4.6.6节,Table 93.
 * @param       sen  : 是否允许共用;MPU_ACCESS_NOT_SHAREABLE,不允许;MPU_ACCESS_SHAREABLE,允许
 * @param       cen  : 是否允许cache;MPU_ACCESS_NOT_CACHEABLE,不允许;MPU_ACCESS_CACHEABLE,允许
 * @param       ben  : 是否允许缓冲;MPU_ACCESS_NOT_BUFFERABLE,不允许;MPU_ACCESS_BUFFERABLE,允许
 * @retval      0,成功.
 *              其他,错误.
 */
uint8_t mpu_set_protection(uint32_t baseaddr, uint32_t size, uint32_t rnum, uint8_t de, uint8_t ap, uint8_t sen, uint8_t cen, uint8_t ben)
{
    MPU_Region_InitTypeDef mpu_region_init_handle;             /* MPU初始化句柄 */
    HAL_MPU_Disable();                                         /* 配置MPU之前先关闭MPU,配置完成以后再使能MPU */

    mpu_region_init_handle.Enable = MPU_REGION_ENABLE;         /* 使能该保护区域 */
    mpu_region_init_handle.Number = rnum;                      /* 设置保护区域 */
    mpu_region_init_handle.BaseAddress = baseaddr;             /* 设置基址 */
    mpu_region_init_handle.Size = size;                        /* 设置保护区域大小 */
    mpu_region_init_handle.SubRegionDisable = 0X00;            /* 禁止子区域 */
    mpu_region_init_handle.TypeExtField = MPU_TEX_LEVEL0;      /* 设置类型扩展域为level0 */
    mpu_region_init_handle.AccessPermission = (uint8_t)ap;     /* 设置访问权限 */
    mpu_region_init_handle.DisableExec = de;                   /* 是否允许指令访问 */
    mpu_region_init_handle.IsShareable = sen;                  /* 是否允许共用 */
    mpu_region_init_handle.IsCacheable = cen;                  /* 是否允许cache */
    mpu_region_init_handle.IsBufferable = ben;                 /* 是否允许缓冲 */
    HAL_MPU_ConfigRegion(&mpu_region_init_handle);             /* 配置MPU */
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);                    /* 开启MPU */
    return 0;
}

/**
 * @brief       设置需要保护的存储块
 * @note        必须对部分存储区域进行MPU保护,否则可能导致程序运行异常
 *              比如MCU屏不显示,摄像头采集数据出错等等问题...
 * @param       无
 * @retval      无
 */
void mpu_memory_protection(void)
{
    /* ★移植改动（2026-10-08，路线图 L3）★ SRAM1 = **专用非缓存 DMA 区** —— region0
     *
     * 【⚠ 2026-10-09 更正：下面这段"编号小的优先级高"是**错的**，已按 ARM 文档改正】
     *   ARMv7-M ARM（DDI0403）原文："Where there is an overlap between two regions,
     *   the register with the **highest region number** takes priority."；
     *   Cortex-M7 TRM 更直白："7 Highest priority, when 8 regions are implemented."
     *   即 **编号越大优先级越高**（与 Linux/RTOS 里"编号小优先级高"的直觉相反）。
     *   当初这么写没被抓出来，是因为 SRAM1 恰好**不与任何其它 region 重叠**
     *   —— 原来的 region3 是 0x30000000 起 512KB（覆盖 SRAM1），现已缩成
     *   SRAM2 单独的 128KB（0x30020000 起），不再与 SRAM1 相交，
     *   于是 region0 无论优先级高低都照样生效，`dma_check.py` 也就照样 64→0。
     *   **结论的后果**：如果哪天要给某个"被大 region 覆盖"的小区域改属性，
     *   必须给它**更大的编号**，不是更小的（L2-4 的 AXI 尾部 8 KB 就是这么踩的）。
     *
     * 【为什么是"非缓存"而不是"记得手工 invalidate"】
     * `tools/dma_check.py` 已实测：DMA 写内存后 CPU 读，**64/64 个字全部读到陈旧值**，
     * 且不报错、不 HardFault —— 漏一次 invalidate 就是静默数据损坏。
     * 把 DMA 缓冲所在的整块 SRAM1 配成非缓存，等于让一致性由 **MPU 保证**，
     * 而不是靠每个调用方"记得做"。这是接 SDMMC IDMA（L2-4）之前必须完成的前置。
     *
     * 【属性为什么不走公共的 mpu_set_protection()】
     * 那个函数把 TypeExtField 固定成 MPU_TEX_LEVEL0，而 TEX=000/C=0/B=0 是
     * **Strongly-ordered**（最严、也最慢）。DMA 缓冲要的是
     * "Normal memory, Non-cacheable" ⇒ **TEX=001, C=0, B=0**，故这里直接调
     * HAL_MPU_ConfigRegion 配，不动公共函数签名以免影响其它调用点。
     *
     *   XN=1（DisableExec） 不从 DMA 区取指
     *   FULL_ACCESS         DMA 区必须能写（不能设 RO）
     *   SHAREABLE           CPU 与 DMA 是两套总线主设备，语义上就是共享
     */
    {
        MPU_Region_InitTypeDef r;

        r.Enable           = MPU_REGION_ENABLE;
        r.Number           = MPU_REGION_NUMBER0;
        r.BaseAddress      = 0x30000000u;             /* SRAM1（D2 域，128KB） */
        r.Size             = MPU_REGION_SIZE_128KB;
        r.SubRegionDisable = 0x00u;
        r.TypeExtField     = MPU_TEX_LEVEL1;          /* TEX=001 + C=0/B=0 ⇒ Normal 非缓存 */
        r.AccessPermission = MPU_REGION_FULL_ACCESS;
        r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
        r.IsShareable      = MPU_ACCESS_SHAREABLE;
        r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
        r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;

        HAL_MPU_Disable();
        HAL_MPU_ConfigRegion(&r);
        HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    }

    /* 保护整个DTCM,共128K字节（编号回到厂商原始的 region1：DTCM 不与任何其它
     * region 重叠，编号取哪个都不影响它生效） */
    mpu_set_protection( 0x20000000,                 /* 基地址 */
                        MPU_REGION_SIZE_128KB,      /* 长度 */
                        MPU_REGION_NUMBER1, 0,      /* NUMER1,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */

    /* 保护整个AXI SRAM,共512K字节 */
    mpu_set_protection( 0x24000000,                 /* 基地址 */
                        MPU_REGION_SIZE_512KB,      /* 长度 */
                        MPU_REGION_NUMBER2, 0,      /* NUMER2,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */
    
    /* ★移植改动（L3 重排）★ 原来的 region3 是"SRAM1~SRAM3 共 512KB 可缓存"，
     * 现在 SRAM1 已单独由 region0 配成非缓存（见本函数开头），这里只覆盖
     * **SRAM2（0x30020000，128KB）**，长度按 2 的幂且基址按长度对齐（硬件要求）。 */
    mpu_set_protection( 0x30020000,                 /* 基地址（SRAM2） */
                        MPU_REGION_SIZE_128KB,      /* 长度 */
                        MPU_REGION_NUMBER3, 0,      /* NUMER3,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */

    /* ★移植改动（L3 重排）★ SRAM3（0x30040000，32KB）—— 第二块 band buffer 在这里。
     * 编号从"SRAM4 的 4"改成 4 给 SRAM3，SRAM4 挪去 region7。 */
    mpu_set_protection( 0x30040000,                 /* 基地址（SRAM3） */
                        MPU_REGION_SIZE_32KB,       /* 长度 */
                        MPU_REGION_NUMBER4, 0,      /* NUMER4,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */
                        
    /* 保护MCU LCD屏所在的FMC区域,共64M字节
     * （本工程实测：该区域的 XN 位取 0 或 1 都不影响 FMC 读通路，故保持厂商原值 0。
     *   真正导致"整屏 GRAM 读回全失配"的原因是 flush 前漏发 0x2C，见 src/ymgui_port.c。） */
    mpu_set_protection( 0x60000000,                 /* 基地址 */
                        MPU_REGION_SIZE_64MB,       /* 长度 */
                        MPU_REGION_NUMBER5, 0,      /* NUMER5,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_NOT_CACHEABLE,   /* 禁止cache */
                        MPU_ACCESS_NOT_BUFFERABLE); /* 禁止缓冲 */
                        
    /* ★移植改动（L3 重排）★ 原 region6 是 **SDRAM（0xC0000000）**，本板**没有**这个器件，
     * 编号回收给下面的 QSPI（原在 region0）。不访问这片地址，落到默认图也无所谓。 */

    /* ★移植改动（L3 重排）★ 原 region7 是 **NAND（0x80000000）**，本板同样没有，
     * 编号回收给 SRAM4（原在 region4）—— 见本函数末尾。 */

    /* ★移植新增（阶段 4）★ QUADSPI memory-mapped 只读常量区
     * ★L3 重排：编号由 region0 改为 **region6**（region0 让给 SRAM1 非缓存 DMA 区）。
     *   属性一字未改：仍然 NOT_CACHEABLE + 只读 + XN，理由见下面那段 2026-10-03 的记录。
     *   0x9000_0000~0x9FFF_FFFF = QUADSPI bank1 的 256MB 窗口
     *   （本板 DCR.FSIZE=23 只映射 16MB，MPU 区域按整个窗口设，反正不会越界访问）。
     *
     * 为什么必须显式加这一条：`HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT)` 会置
     * PRIVDEFENA=1，**未命中任何区域的地址会落到 ARM 默认存储图** ——
     * 0x8000_0000~0x9FFF_FFFF 在默认图里是"可缓存 Normal"。那样 XIP 读会被
     * D-Cache 缓存：擦写 Flash 后必须手工维护 cache，否则读到陈旧数据，
     * 而且"XIP 与 indirect 逐字节比对"的验收也会被 cache 干扰（第一次读就不同）。
     *
     *   MPU_REGION_PRIV_RO         只读 —— 常量区就该写不动，误写立刻 MemManage
     *   NOT_CACHEABLE / NOT_BUFFERABLE  每次读都真的走 QSPI，与 indirect 结果逐字节可比，
     *                                   不需要任何 cache 维护
     *   de=1（XN）                 不从 XIP 区取指（这里只放数据）
     *
     * 【2026-10-03 试过改成可缓存，已验证无收益，遂回退】
     *   假设：每画一个中文字都要真的走一遍 QSPI 取字模，是渲染慢的主因。
     *   做法：本区改 CACHEABLE|BUFFERABLE，并在 `qspi_enter_mmap()` 开头
     *         `SCB_CleanInvalidateDCache()` 形成"每次进映射都清一次"的闭环。
     *   结果：**满负载帧基准 64.80 ms → 64.69 ms，纹丝不动（15.4 → 15.5 FPS，纯噪声）**。
     *   结论：字模读取**不在**渲染的开销构成里，改动只带来 cache 维护负担、不带收益 ⇒ 回退。
     *   保留这段记录，避免以后重复踩同一条思路。 */
    mpu_set_protection( 0x90000000,                 /* QUADSPI bank1 窗口 */
                        MPU_REGION_SIZE_256MB,      /* 长度 */
                        MPU_REGION_NUMBER6, 1,      /* NUMER6,禁止指令访问(XN=1) */
                        MPU_REGION_PRIV_RO,         /* 只读 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_NOT_CACHEABLE,   /* 禁止cache */
                        MPU_ACCESS_NOT_BUFFERABLE); /* 禁止缓冲 */

    /* ★移植改动（2026-10-08，路线图 L2-4）★ AXI SRAM **尾部 8 KB** = SDMMC1 的
     * IDMA 专用缓冲（链接脚本 .bss_sd_dma，见 linker/stm32h743vit6.ld），
     * 配成 **Normal 非缓存** —— 与 region0（SRAM1）同样的理由与同样的属性：
     *   · IDMA 是独立于 CPU 的总线主设备，它写进来的数据 CPU 的 D-Cache **看不见**，
     *     缓存着就是静默读到旧值（`tools/dma_check.py` 已实测 64/64 字全陈旧）；
     *   · 让 MPU 保证一致性，而不是靠每个调用方"记得 invalidate"。
     *
     * 【为什么必须占编号 7（最大）】
     * 整块 AXI（512KB，0x24000000 起）由 region2 配成 CACHEABLE，这 8 KB 在它里面。
     * ARMv7-M 的规则是 **编号越大优先级越高**（见本函数顶部那段更正），
     * 所以要压住 region2，只能取比 2 大的编号 —— 取最大的 7 最稳
     * （第一版按"编号小优先级高"的错误理解放在 region1，**压不住**，
     *   实测哨兵残留 3438/4096 字节，IDMA 写完 CPU 仍读到 cache 里的旧副本）。
     * SRAM4 那条 region 因此被删除（本工程从不访问 SRAM4，落默认图无影响）。
     *
     * 【为什么是 0x2407E000 / 8 KB】
     *   0x24000000 + 512KB = 0x24080000（AXI 末端），减 8 KB ⇒ **0x2407E000**。
     *   链接脚本里 .bss_sd_dma 就钉在这里（nm 实测 s_rd=0x2407E000、
     *   s_wr=0x2407F000），两边必须一致 —— 手算第一版写成 0x2407C000（差 8 KB），
     *   那样 MPU 盖的是 heap1 里的区域，IDMA 缓冲反而是可缓存的。
     * MPU 区域要求"基址按长度对齐、长度是 2 的幂"：0x7E000 = 63 × 8 KB ⇒ 对齐，成立。 */
    {
        MPU_Region_InitTypeDef r;

        r.Enable           = MPU_REGION_ENABLE;
        r.Number           = MPU_REGION_NUMBER7;     /* 最大编号 ⇒ 压得住 region2 的 AXI 512KB */
        r.BaseAddress      = 0x2407E000u;            /* AXI 尾部 8 KB（与链接脚本 .bss_sd_dma 一致） */
        r.Size             = MPU_REGION_SIZE_8KB;
        r.SubRegionDisable = 0x00u;
        r.TypeExtField     = MPU_TEX_LEVEL1;          /* TEX=001 + C=0/B=0 ⇒ Normal 非缓存 */
        r.AccessPermission = MPU_REGION_FULL_ACCESS;
        r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
        r.IsShareable      = MPU_ACCESS_SHAREABLE;
        r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
        r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;

        HAL_MPU_Disable();
        HAL_MPU_ConfigRegion(&r);
        HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    }

    /* SRAM4（0x38000000，64KB）的 region 已删除：8 条 region 用满，
     * 而本工程从不访问 SRAM4（落 PRIVDEFENA 的默认图即可）。
     * 若将来要用，必须先把某条不重叠的 region 腾出来再补回。 */
}


/**
 * @brief     MemManage错误处理中断
 * @note      进入此中断以后,将无法恢复程序运行!!
 * @param     无
 * @retval    无
 */
void MemManage_Handler(void)
{
    LED1(0);                            /* 点亮LED1(绿灯) */
    /* ★移植改动★ 先把故障现场存进 g_fault 再软复位。
     * 原版这里只打印+复位，复位后什么线索都不剩；现在即使软复位，
     * g_fault 仍留在 SRAM 里可被 SWD 读到（kind=2 即 MemManage）。 */
    board_fault_capture(2u);
    /* ★移植改动：原版这里是"打印 Mem Access Error"的那句串口输出。
     * 裸机上 printf 会经 newlib malloc/_sbrk 分配 stdout 缓冲，而该缓冲曾与本工程的
     * YMGUI 堆(heap0)落在同一地址，会踩坏 ctx；且中断上下文里做串口输出也不合适。 */
    delay_ms(1000);
    /* ★移植改动：原版这里是"提示 Soft Reseting"的那句串口输出，原因同上。 */
    delay_ms(1000);
    NVIC_SystemReset();                 /* 软复位 */
}




