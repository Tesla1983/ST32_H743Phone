/**
 ****************************************************************************************************					 
 * @file        sys.h
 * @version     V1.0
 * @brief       系统初始化代码(包括时钟配置/中断管理/GPIO设置等)            
 ****************************************************************************************************'
 *
 * V1.0
 * 将头文件包含路径改成相对路径,避免重复设置包含路径的麻烦
 *
 ****************************************************************************************************
 */

#ifndef __SYS_H
#define __SYS_H

#include "stm32h7xx.h"
#include "core_cm7.h"
#include "stm32h7xx_hal.h"


/**
 * SYS_SUPPORT_OS用于定义系统文件夹是否支持OS
 * 0,不支持OS
 * 1,支持OS
 */
#define SYS_SUPPORT_OS         0

#define      ON      1
#define      OFF     0
#define      Write_Through()    do{ *(__IO uint32_t*)0XE000EF9C = 1UL << 2; }while(0)     /* Cache透写模式 */

/******************************************************************************************/

uint8_t get_icahce_sta(void);
uint8_t get_dcahce_sta(void);
void sys_nvic_set_vector_table(uint32_t baseaddr, uint32_t offset);                       /* 设置中断偏移量 */
void sys_cache_enable(void);                                                              /* 使能STM32H7的L1-Cache */
uint8_t sys_stm32_clock_init(uint32_t plln, uint32_t pllm, uint32_t pllp, uint32_t pllq); /* 配置系统时钟 */

/* ---- 时钟初始化诊断量（2026-10-04 新增）------------------------------------
 * 为什么需要：厂商代码里 `while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)){}`
 * 是一个**无超时死等**。VOSRDY 不就绪时（供电模式未配置 / 电压档位握手失败）
 * 固件会永久停在这里，外部表现与"板子坏了"完全一样：所有诊断量停在初值。
 *
 * 加了这几个量之后，"卡在时钟初始化" 与 "时钟初始化成功但后面某步失败" 可以
 * 靠读 RAM 直接区分，不必再靠猜寄存器。
 *
 * g_sysclk_stage 取值（单调递增，读它就知道走到哪一步）：
 *   0xFFFFFFFF 还没进入本函数
 *   0x00000001 已使能 SYSCFG 时钟
 *   0x00000002 已写 PWR_D3CR（VOS 目标档位）
 *   0x00000003 VOSRDY 已就绪
 *   0x00000004 HAL_RCC_OscConfig 成功
 *   0x00000005 HAL_RCC_ClockConfig 成功
 *   0x00000006 HAL_RCCEx_PeriphCLKConfig 成功（全程完成）
 * g_sysclk_err  与返回值一致（0=成功，1=OSC/CLK 配置失败，2=VOSRDY 超时）
 * g_sysclk_vos_wait  VOSRDY 死等实际消耗的毫秒数（超时上限见 sys.c，
 *                     已对齐 HAL 的 PWR_FLAG_SETTING_DELAY = 1000）
 *
 * ⚠ 已知的一处标志位不一致（本工程**故意不改**，仅记录）：
 *   本工程等的是 VOSRDY（PWR_D3CR bit13），
 *   而 HAL 自己的 HAL_PWREx_ConfigSupply()/ControlVoltageScaling()
 *   等的是 ACTVOSRDY（PWR_CSR1 bit13）—— 是两个不同寄存器的不同位。
 *   实测冷启动那次 VOSRDY 超时后，稍后两者都变成 1，
 *   即"实际档位"与"目标档位"最终一致，慢的是上电爬升过程。
 *   ⇒ 保留等 VOSRDY（厂商原始行为）+ 1000 ms 上限即可，不必改标志位。
 */
extern volatile uint32_t g_sysclk_stage;
extern volatile uint32_t g_sysclk_err;
extern volatile uint32_t g_sysclk_vos_wait;

/* 以下为汇编函数 */
void sys_wfi_set(void);              /* 执行WFI指令 */
void sys_intx_disable(void);         /* 关闭所有中断 */
void sys_intx_enable(void);          /* 开启所有中断 */
void sys_msr_msp(uint32_t addr);     /* 设置栈顶地址 */

#endif





