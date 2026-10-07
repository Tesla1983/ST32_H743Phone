#include "phone_quick.h"

/* 注册失败时原实现往 stderr 打一行。裸机上没有控制台，而且 fprintf/stderr 会把
 * newlib stdio 的缓冲（连带 malloc/_sbrk）拖进来 —— 本工程已经在阶段 3 吃过这个亏
 * （见 linker/stm32h743vit6.ld 里"sbrk 区必须与 heap0 分离"的说明）。
 * 所以板级构建下改成只累加一个供 SWD 读的计数，返回值/行为完全不变。 */
#if defined(PHONE_SHELL_BOARD)
#include <stdint.h>
volatile uint32_t g_quick_invalid_cnt = 0;
#define PHONE_QUICK_REPORT(name)  do { (void)(name); g_quick_invalid_cnt++; } while (0)
#else
#include <stdio.h>
#define PHONE_QUICK_REPORT(name)  fprintf(stderr, "Invalid quick setting: %s\n", (name))
#endif

#define PHONE_QUICK(name, source) extern const PhoneQuick PhoneQuick_##name;
#include "phone_quick_catalog.def"
#undef PHONE_QUICK
int PhoneQuick_RegisterDefaults(void)
{
	static int initialized;
	static int valid = 1;
	if (initialized)
		return valid;
	initialized = 1;
#define PHONE_QUICK(name, source)                              \
	if (!PhoneQuick_Register(&PhoneQuick_##name))              \
	{                                                          \
		PHONE_QUICK_REPORT(#name);                             \
		valid = 0;                                             \
	}
#include "phone_quick_catalog.def"
#undef PHONE_QUICK
	return valid;
}
