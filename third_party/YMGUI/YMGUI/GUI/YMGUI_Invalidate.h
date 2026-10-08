#ifndef YMGUI_INVALIDATE_H
#define YMGUI_INVALIDATE_H

#include "YMGUI_PubType.h"
#include "YMGUI_Obj.h"

//===========================================================================
// per-object 绘制耗时统计(诊断,默认关闭)
//   打开方式:编译时定义 YMGUI_DIAG_OBJTIME=1
//   作用:drawObjRec 给每个被访问到的对象记一张表 —— draw_cb 自身耗时(self)、
//        含后代的整棵子树耗时(sub)、调用次数 —— 用来回答"这一帧到底是谁在吃时间"。
//   代价:每个对象每次绘制多两次 DWT 读 + 一次表查找(线性,表很小)。
//   不改任何绘制行为,纯计时。
//===========================================================================
#if defined(YMGUI_DIAG_OBJTIME)

#ifndef YMGUI_DIAG_OBJ_N
#define YMGUI_DIAG_OBJ_N   72      //表容量(装不下的对象计数进 g_objtime_ovf)
#endif

typedef struct
{
	void*   obj;    //对象地址(本次测量期内不变,可当 ID)
	void*   cb;     //draw_cb 地址(nm 可解析成函数名;NULL = 纯容器,只做裁剪/递归)
	GYcoord x, y, w, h; //该对象的屏幕绝对区域(第一见到时记下)
	uint32  self;   //draw_cb 自身累计耗时(cycles,**不含**后代)
	uint32  sub;    //含后代的整棵子树累计耗时(cycles)
	uint32  calls;  //draw_cb 被调用次数
}GYDiagObjTime;

extern GYDiagObjTime g_objtime[YMGUI_DIAG_OBJ_N];
extern uint32        g_objtime_n;    //已占用槽数
extern uint32        g_objtime_ovf;  //表满后漏记的对象访问次数(>0 说明该调大容量)
//总开关:0 = 完全不计时(只留一次分支判断,开销可忽略),1 = 计时。
//做成运行时而不是编译期,是为了**同一份固件**能量出"计时本身吃掉多少"
//(分别跑一次基准相减即可),不用重新编译。
extern uint32        g_objtime_on;

//清零(开始新一轮测量前调)
void YMGUI_Diag_ObjTimeReset(void);

#endif // YMGUI_DIAG_OBJTIME

//===========================================================================
// 失效/脏矩形系统(保留模式发动机)
//   控件状态变 → Invalidate → 向上传播 + 合并脏区 → Refresh 只重绘脏区
//===========================================================================

//标记对象需要重绘(算出其屏幕绝对区域,并入 ctx 脏矩形)
void YMGUI_Obj_Invalidate(GYOBJ obj);
//标记一块屏幕区域需要重绘(并入 ctx 脏矩形)
void YMGUI_Ctx_InvalidateArea(GYCTX ctx, const GYrect* area);

//刷新一帧:合并脏矩形 → 按 buf_px_cnt 切 band → 逐 band 重绘相交对象 → flush
//  无脏区时直接返回(保留模式空闲不耗)
void YMGUI_Refresh(GYCTX ctx);

//===========================================================================
// 绘制规模诊断（2026-10-09，常驻编译）
//   用来把"脏区有多大""裁剪剔掉了多少"变成可读的整数，判断优化值不值得做。
//   ⚠ 无脏区时**保留上一帧的值**（Refresh 直接 return），不是"当前值"。
//===========================================================================
extern uint32 g_inv_px;      //本帧脏区总面积（像素）；满屏 320×480 = 153 600
extern uint32 g_inv_cnt;     //本帧脏区块数（>1 说明有多块互不接触的脏区）
extern uint32 g_inv_frames;  //累计有脏区的帧数
extern uint32 g_draw_visit;  //本帧访问到的对象次数（含被裁剪掉的）
extern uint32 g_draw_draw;   //本帧真正调用 draw_cb 的次数（visit − draw = 被裁剪挡掉的）
//峰值版（写 0 清零）：取"这段时间里最糟糕的一帧"。测操作期间的规模必须用它 ——
//  普通版只留最近一帧，操作后等一会儿就被时间刷新的小脏区盖掉了。
extern uint32 g_inv_px_max;
extern uint32 g_visit_max;
extern uint32 g_draw_max;

#endif // !YMGUI_INVALIDATE_H
