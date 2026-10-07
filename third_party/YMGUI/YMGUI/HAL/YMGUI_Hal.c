#include "YMGUI_Hal.h"
#include "YMGUI_Debug.h"
#include "YMGUI_Event.h"
#include "YMGUI_Obj.h"        //GY_STATE_Editing / GYCTX::focus_obj(光标闪烁置脏用)
#include "YMGUI_Invalidate.h" //YMGUI_Obj_Invalidate
#include "YMGUI_PubDefine.h"
#if YMGUI_ANIM
#include "YMGUI_Anim.h"
#endif

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_Hal.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: HAL 接口的库侧实现。flush 就绪回调 + 输入注入入口
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  *
  * 备注信息：
  * 1.当前地基阶段:FlushReady 为空(同步 flush 无需等待);Inject_* 先打印,GUI 层接入后改为事件分发
  *
  ***************************************************************************************************************************
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief flush 完成通知:清 flush_busy,库据此可复用该 buffer。
  *        异步 DMA 在传输完成中断里调;同步 flush_cb 在函数体末尾调。单缓冲(buf2==NULL)时库不检查此标志,调它无副作用。
  */
void YMGUI_Disp_FlushReady(GYDISP d)
{
	if (d != NULL)
		d->flush_busy = 0;
}

#define GY_FRAME_DONE_SLOT_MAX 4
typedef struct
{
	GYDISP display;
	GYframe_done_cb callback;
} gy_frame_done_slot;

static gy_frame_done_slot s_frame_done_slots[GY_FRAME_DONE_SLOT_MAX];

void YMGUI_Disp_SetFrameDoneCb(GYDISP d, GYframe_done_cb cb)
{
	if (d == NULL)
		return;
	for (uint8 i = 0; i < GY_FRAME_DONE_SLOT_MAX; i++)
	{
		if (s_frame_done_slots[i].display == d)
		{
			s_frame_done_slots[i].callback = cb;
			if (cb == NULL)
				s_frame_done_slots[i].display = NULL;
			return;
		}
	}
	if (cb == NULL)
		return;
	for (uint8 i = 0; i < GY_FRAME_DONE_SLOT_MAX; i++)
	{
		if (s_frame_done_slots[i].display == NULL)
		{
			s_frame_done_slots[i].display = d;
			s_frame_done_slots[i].callback = cb;
			return;
		}
	}
}

void YMGUI_Disp_FrameDone(GYDISP d)
{
	if (d == NULL)
		return;
	for (uint8 i = 0; i < GY_FRAME_DONE_SLOT_MAX; i++)
	{
		if (s_frame_done_slots[i].display == d)
		{
			if (s_frame_done_slots[i].callback != NULL)
				s_frame_done_slots[i].callback(d);
			return;
		}
	}
}

//接收注入事件的上下文(GUI 层注册)
static GYCTX s_inject_ctx = NULL;
static GYkey_filter_cb s_key_filter = NULL;
static void* s_key_filter_user = NULL;

/**
  * @brief 注册接收注入事件的上下文
  */
void YMGUI_Inject_SetCtx(void* ctx)
{
	s_inject_ctx = (GYCTX)ctx;
}

void YMGUI_Inject_SetKeyFilter(GYkey_filter_cb cb, void* user_data)
{
	s_key_filter = cb;
	s_key_filter_user = user_data;
}

/**
  * @brief 指针注入入口:转调事件分发
  */
void YMGUI_Inject_Pointer(GYcoord x, GYcoord y, uint8 pressed)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_Pointer(s_inject_ctx, x, y, pressed);
}

/**
  * @brief 按键注入入口:转调事件分发给焦点对象
  */
void YMGUI_Inject_Key(uint32 key, uint8 pressed)
{
	if (s_key_filter != NULL && s_key_filter(key, pressed != 0, s_key_filter_user))
		return;
	//抬起只供过滤器维护状态，不产生字符输入
	if (pressed && s_inject_ctx != NULL)
		YMGUI_Event_Key(s_inject_ctx, key);
}

void YMGUI_Inject_Wheel(GYcoord x, GYcoord y, int32 delta_x, int32 delta_y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_Wheel(s_inject_ctx, x, y, delta_x, delta_y);
}

uint32 g_ymgui_caret_blink_ms = 300;   //半周期(亮/灭各 300ms)。0=不闪,恒显
uint32 g_ymgui_caret_style    = 1;     //0=竖线(原版) 1=下划线
uint32 g_ymgui_caret_on       = 1;
//诊断:SWD 读变量会暂停内核(DWT 也停),墙上时间不可用于测周期。
//  用"设备端累计毫秒 / 设备端翻转次数"才算得准,两者同时被暂停,比值不受影响。
uint32 g_ymgui_caret_ms    = 0;        //累计喂进来的毫秒
uint32 g_ymgui_caret_flips = 0;        //累计翻转次数

/**
  * @brief 光标闪烁时钟:累计到半周期就翻转 g_ymgui_caret_on,并把焦点控件置脏,
  *        下一次 Refresh 才会真的重画出"亮/灭"两态(不置脏屏幕不会变)。
  *        只有处于编辑态的焦点控件需要重画,其余情况零开销。
  */
void YMGUI_CaretTick(uint32 elapsed_ms)
{
	static uint32 s_acc = 0;
	g_ymgui_caret_ms += elapsed_ms;      //诊断:设备端真实累计时间
	if (g_ymgui_caret_blink_ms == 0)
	{
		g_ymgui_caret_on = 1;
		s_acc = 0;
		return;
	}
	s_acc += elapsed_ms;
	if (s_acc < g_ymgui_caret_blink_ms)
		return;
	//保留余数(而不是清零):清零会把"跨过半周期那一帧的超出量"丢掉,
	//实测半周期被拉长到 300+一帧(9 分钟均值 327ms,长帧时更差)。
	//改成减法后均值回到 300,单次抖动只剩 ±1 帧。
	s_acc -= g_ymgui_caret_blink_ms;
	//上限一个半周期:某帧卡很久(如整屏重绘 60ms+ 连续几帧)也不追补连翻,
	//最多让下一次翻转提前,不会一次性闪好几下。
	if (s_acc > g_ymgui_caret_blink_ms)
		s_acc = g_ymgui_caret_blink_ms;
	g_ymgui_caret_on = (uint32)(g_ymgui_caret_on ^ 1u);
	g_ymgui_caret_flips++;           //诊断:翻转计数
	if (s_inject_ctx != NULL && ((GYCTX)s_inject_ctx)->focus_obj != NULL &&
		(((GYCTX)s_inject_ctx)->focus_obj->state & GY_STATE_Editing))
		YMGUI_Obj_Invalidate(((GYCTX)s_inject_ctx)->focus_obj);
}

void YMGUI_Inject_Tick(uint32 elapsed_ms)
{
	if (s_inject_ctx != NULL)
	{
		#if YMGUI_ANIM
		YMGUI_Anim_Tick(s_inject_ctx, elapsed_ms);
		#endif
		YMGUI_Event_Tick(s_inject_ctx, elapsed_ms);
	}
	YMGUI_CaretTick(elapsed_ms);
}

/**
  * @brief 双击注入入口:转调事件分发(派 GY_EVENT_DoubleClicked 给命中对象)
  */
void YMGUI_Inject_DoubleClick(GYcoord x, GYcoord y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_DoubleClick(s_inject_ctx, x, y);
}

/**
  * @brief 取消当前指针捕获,不产生普通点击
  */
void YMGUI_Inject_PointerCancel(void)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_PointerCancel(s_inject_ctx);
}

/**
  * @brief 上下文请求注入入口:右键/长按等平台输入统一走此语义
  */
void YMGUI_Inject_ContextRequest(GYcoord x, GYcoord y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_ContextRequest(s_inject_ctx, x, y);
}

void YMGUI_Inject_ContextBegin(GYcoord x, GYcoord y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_ContextBegin(s_inject_ctx, x, y);
}

void YMGUI_Inject_ContextMove(GYcoord x, GYcoord y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_ContextMove(s_inject_ctx, x, y);
}

void YMGUI_Inject_ContextEnd(GYcoord x, GYcoord y)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_ContextEnd(s_inject_ctx, x, y);
}

void YMGUI_Inject_ContextCancel(void)
{
	if (s_inject_ctx != NULL)
		YMGUI_Event_ContextCancel(s_inject_ctx);
}

//===========================================================================
// 剪贴板:默认库内静态缓冲(裸机);SDL_LCD 可注册系统剪贴板后端覆盖之
//===========================================================================
#ifndef GY_CLIP_MAX
#define GY_CLIP_MAX 32768  //库内剪贴板缓冲上限(32KB;裸机可用编译宏改小省 RAM)
#endif
static char          s_clip_buf[GY_CLIP_MAX] = { 0 };
static GYclip_set_cb s_clip_set = NULL;
static GYclip_get_cb s_clip_get = NULL;

/**
  * @brief 注册系统剪贴板后端(NULL/NULL 恢复库内静态缓冲)
  */
void YMGUI_Clipboard_SetBackend(GYclip_set_cb set_cb, GYclip_get_cb get_cb)
{
	s_clip_set = set_cb;
	s_clip_get = get_cb;
}

/**
  * @brief 写剪贴板:有后端走后端,否则拷进库内静态缓冲(截断到上限)
  */
void YMGUI_Clipboard_SetText(const char* text)
{
	if (text == NULL)
		text = "";
	if (s_clip_set != NULL)
	{
		s_clip_set(text);
		return;
	}
	uint32 i = 0;
	while (text[i] != '\0' && i < GY_CLIP_MAX - 1)
	{
		s_clip_buf[i] = text[i];
		i++;
	}
	s_clip_buf[i] = '\0';
}

/**
  * @brief 读剪贴板:有后端走后端(NULL 归一为 ""),否则返回库内缓冲
  */
const char* YMGUI_Clipboard_GetText(void)
{
	if (s_clip_get != NULL)
	{
		const char* t = s_clip_get();
		return (t != NULL) ? t : "";
	}
	return s_clip_buf;
}
