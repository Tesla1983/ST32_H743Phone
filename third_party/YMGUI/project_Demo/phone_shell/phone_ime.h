#ifndef YAOMI_PHONE_IME_H
#define YAOMI_PHONE_IME_H
#include "YMGUI_TextInput.h"
#include "YMGUI_EditView.h"

/* 应用级单实例服务；控件的原生私有数据、事件与析构均保留。 */
#if YAOMI_IME
void PhoneIME_Init(GYCTX ctx);
void PhoneIME_Update(int enabled);
void PhoneIME_Shutdown(void);
int PhoneIME_Hide(void);
int PhoneIME_Visible(void);
/* 登记"自己管键盘避让"的滚动容器根对象。
 * 登记后，落在该容器内的输入框**不再**被 PhoneIME_Show 的上移/压扁避让
 * 改动 —— 那种避让混用绝对与局部坐标，在滚动容器里会把控件挪出视口
 * （详见 phone_ime.c 里 in_scroll_container 的注释）。
 * 未登记的容器行为与改动前完全一致。 */
void PhoneIME_RegisterSelfAvoiding(GYOBJ root);
/* 键盘可见性诊断量：由 phone_shell.c 的 advance() 每帧现算赋值，
 * 禁止在别处增量同步（原因见 phone_ime.c 里该变量的定义处注释）。 */
extern volatile int32_t g_diag_ime_visible;
GYOBJ PhoneIME_Target(void);
GYOBJ PhoneIME_TextInput(GYOBJ parent, GYcoord x, GYcoord y, GYcoord w, GYcoord h, size_t capacity);
GYOBJ PhoneIME_EditView(GYOBJ parent, GYcoord x, GYcoord y, GYcoord w, GYcoord h, size_t capacity);
#if defined(PHONE_SHELL_BOARD)
/* 板级自测（详见 phone_ime.c 末尾注释）：桌面有键盘能直接敲，板上用它替代手点软键盘。 */
extern char PhoneIME_BoardCand[6][24];
extern int PhoneIME_BoardCandCount;
extern char PhoneIME_BoardText[192];
int PhoneIME_BoardFocusFirst(void);   /* 把焦点放到第一个可用输入框，1=成功 */
int PhoneIME_BoardType(const char* letters);  /* 逐字母走 action()，返回被接受的字母数 */
int PhoneIME_BoardSnapshot(void);     /* 把当前页候选抄进 PhoneIME_BoardCand，返回个数 */
int PhoneIME_BoardClearTarget(void);  /* 清空当前目标文本框，让每轮自测基线一致 */
int PhoneIME_BoardCommit(int index);  /* 选第 index 个候选上屏，并把目标文本抄进 PhoneIME_BoardText */
#endif
/* 手机应用统一创建入口：以后新增单行/多行输入框也默认接入。 */
#ifdef PHONE_IME_AUTO_INPUTS
#define YMGUI_Creat_TextInput_Creat PhoneIME_TextInput
#define YMGUI_Creat_EditView_Creat PhoneIME_EditView
#endif
#else
static inline void PhoneIME_Init(GYCTX ctx)
{
	(void)ctx;
}
static inline void PhoneIME_Update(int enabled)
{
	(void)enabled;
}
static inline void PhoneIME_Shutdown(void)
{
}
static inline int PhoneIME_Hide(void)
{
	return 0;
}
static inline int PhoneIME_Visible(void)
{
	return 0;
}
static inline GYOBJ PhoneIME_Target(void)
{
	return NULL;
}
#endif
#endif
