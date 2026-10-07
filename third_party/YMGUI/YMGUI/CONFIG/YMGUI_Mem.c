#include "YMGUI_Mem.h"
#include "board_alloc.h"     /* 移植点：接到本板的双档堆（DTCM / AXI SRAM） */
#include <string.h>

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_Mem.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: 内存双档制,统一内存出入口。移植主战场之一:把大小内存映射到不同物理区
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  *
  * ★ 移植改动（STM32H743VIT6 小系统板）★
  *   原版两档都接到 stdlib 的 malloc（桌面端合理）。裸机上没有堆管理，
  *   而且 YMGUI 需要把大小对象**分到不同物理内存**，malloc 做不到这点。
  *   这里改为接到 src/board_alloc.c 的两块独立池：
  *       malloc0 → DTCM    (0x2000_0000, 128 KB, 直挂内核、最快)
  *       malloc1 → AXI SRAM (0x2400_0000, 512 KB 中除去 300 KB 全帧镜像缓冲后的 212 KB)
  *   池边界由 linker/stm32h743vit6.ld 的 _heap0_* / _heap1_* 提供。
  *   原文件已备份为同目录的 YMGUI_Mem.c.vendor_orig，便于日后对照。
  *
  * 备注信息：
  * 1.后缀 0/1 是内存区域编号,不是 calloc 语义;调用方必须自行初始化申请到的内存
  * 2.函数签名与语义保持不变,只换实现 —— 符合库"移植时只改本文件 .c 的函数体"的要求
  *
  *   __  __ ___    ____   __  ___ ____     ______ ______ ______ __  __
  *   \ \/ //   |  / __ \ /  |/  //  _/    /_  __// ____// ____// / / /
  *    \  // /| | / / / // /|_/ / / /       / /  / __/  / /    / /_/ /
  *    / // ___ |/ /_/ // /  / /_/ /       / /  / /___ / /___ / __  /
  *   /_//_/  |_|\____//_/  /_//___/      /_/  /_____/ \____//_/ /_/
  *
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief 小数据缓冲区申请(高速内存区 → DTCM)
  */
void* GY_malloc0(size_t size)
{
	//不清零:0 表示高速内存区。结构体构造函数须显式初始化全部字段。
	return board_alloc0(size);
}

/**
  * @brief 小数据缓冲区重置长度
  */
void* GY_realloc0(void* ptr, size_t size)
{
	return board_realloc0(ptr, size);
}

/**
  * @brief 小数据缓冲区释放
  */
void GY_free0(void* ptr)
{
	board_free0(ptr);
}

/**
  * @brief 大数据缓冲区申请(低速大内存区 → AXI SRAM)
  */
void* GY_malloc1(size_t size)
{
	return board_alloc1(size);
}

/**
  * @brief 大数据缓冲区重置长度
  */
void* GY_realloc1(void* ptr, size_t size)
{
	return board_realloc1(ptr, size);
}

/**
  * @brief 大数据缓冲区释放
  */
void GY_free1(void* ptr)
{
	board_free1(ptr);
}

/**
  * @brief 内存值设置
  */
void GY_memset(void* ptr, int val, size_t size)
{
	memset(ptr, val, size);
}

/**
  * @brief 内存拷贝
  */
void GY_memcpy(void* dst, const void* src, size_t size)
{
	memcpy(dst, src, size);
}
