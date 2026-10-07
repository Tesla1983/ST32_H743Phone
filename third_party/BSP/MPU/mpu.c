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
    /* 保护整个DTCM,共128K字节 */
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
    
    /* 保护整个SRAM1~SRAM3,共512K字节 */
    mpu_set_protection( 0x30000000,                 /* 基地址 */
                        MPU_REGION_SIZE_512KB,      /* 长度 */
                        MPU_REGION_NUMBER3, 0,      /* NUMER3,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */
                        
    /* 保护整个SRAM4,共64K字节 */
    mpu_set_protection( 0x38000000,                 /* 基地址 */
                        MPU_REGION_SIZE_64KB,       /* 长度 */
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
                        
    /* 保护SDRAM区域,共32M字节 */
    mpu_set_protection( 0xC0000000,                 /* 基地址 */
                        MPU_REGION_SIZE_32MB,       /* 长度 */
                        MPU_REGION_NUMBER6, 0,      /* NUMER6,允许指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_CACHEABLE,       /* 允许cache */
                        MPU_ACCESS_BUFFERABLE);     /* 允许缓冲 */

                        
    /* 保护整个NAND FLASH区域,共256M字节 */
    mpu_set_protection( 0x80000000,                 /* 基地址 */
                        MPU_REGION_SIZE_256MB,      /* 长度 */
                        MPU_REGION_NUMBER7, 1,      /* NUMER7,禁止指令访问 */
                        MPU_REGION_FULL_ACCESS,     /* 全访问 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_NOT_CACHEABLE,   /* 禁止cache */
                        MPU_ACCESS_NOT_BUFFERABLE); /* 禁止缓冲 */

    /* ★移植新增（阶段 4）★ QUADSPI memory-mapped 只读常量区
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
                        MPU_REGION_NUMBER0, 1,      /* NUMER0,禁止指令访问(XN=1) */
                        MPU_REGION_PRIV_RO,         /* 只读 */
                        MPU_ACCESS_NOT_SHAREABLE,   /* 禁止共用 */
                        MPU_ACCESS_NOT_CACHEABLE,   /* 禁止cache */
                        MPU_ACCESS_NOT_BUFFERABLE); /* 禁止缓冲 */
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




