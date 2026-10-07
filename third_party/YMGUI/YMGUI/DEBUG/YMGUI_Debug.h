#ifndef YMGUI_DEBUG_H
#define YMGUI_DEBUG_H

#include <stdio.h>
#include "YMGUI_PubType.h"

/* ★移植配置（STM32H743 裸机）★ 三个开关原值都是 1，本移植全部置 0。
 *
 * YMGUI_DEBUG_PRINT=1 会让 gy_log_print 展开成 **printf**；裸机上 printf 会经
 * newlib 的 malloc/_sbrk 给 stdout 分配行缓冲。本工程不需要串口打印（调试量都
 * 做成全局变量用 SWD 读），所以关闭，避免引入无谓的 stdio 依赖与开销。
 *
 * ⚠ 更正一条早期写错过的结论（2026-10-03 实测）：曾把"YMGUI_Inject_Pointer()
 *   一调用主循环就卡死"归因到打开这几个开关，并写成"关掉后恢复正常"。
 *   **这是错的** —— 实测关闭这三个开关后故障依旧。真正原因是 newlib 的 sbrk 堆
 *   与 YMGUI 的 heap0 落在同一地址（详见 linker/stm32h743vit6.ld 与
 *   src/board_startup.c 的 _sbrk），与这几个开关无关。
 *   现两者已分离；这些开关保持关闭是因为**不需要**，而不是因为它们是故障源。 */
#define YMGUI_ASSERT_DEBUG 0 //断言调试
#define YMGUI_DEBUG_MODE   0 //调试模式(log_explain)
#define YMGUI_DEBUG_PRINT  0 //直接打印(裸机走 SWD 读变量，不用串口)

//调试信息打印输出
#if YMGUI_DEBUG_PRINT
#define gy_log_print(...) printf(__VA_ARGS__)
#else
#define gy_log_print(...) //取消输出
#endif

//调试日志信息类型(位掩码,可组合;对标 YMCV CVEVNLOG)
typedef enum
{
	GY_LOG_OK     = 0x00, //运行正常
	GY_LOG_PtrI   = 0x01, //输入指针为空
	GY_LOG_PtrO   = 0x02, //输出指针为空
	GY_LOG_PtrIO  = 0x03, //输入&输出指针为空
	GY_LOG_TypeI  = 0x04, //输入类型错误
	GY_LOG_TypeO  = 0x08, //输出类型错误
	GY_LOG_TypeIO = 0x0C, //输入&输出类型错误
	GY_LOG_Mem0   = 0x10, //小内存申请失败
	GY_LOG_Mem1   = 0x20, //大内存申请失败
	GY_LOG_ParamI = 0x40, //输入参数错误
}GYEVNLOG;

#if YMGUI_ASSERT_DEBUG
//断言:x 为假时报告 __FILE__/__LINE__
#define gy_assert(x) ((x) ? (void)0U : gy_assert_fail_inform((uint8*)__FILE__, __LINE__))
void gy_assert_fail_inform(uint8* failfile, uint32 failline);
#else
#define gy_assert(x)
#endif

#if YMGUI_DEBUG_MODE
//解释:cond 为真时输出中文提示 + 错误码
#define gy_log_explain(logthis, logtype, logtips) ((logthis) ? gy_logout_inform((uint8*)(logtips), (logtype)) : (void)0U)
void gy_logout_inform(uint8* tips, GYEVNLOG event);
#else
#define gy_log_explain(logthis, logtype, logtips)
#endif

#endif // !YMGUI_DEBUG_H
