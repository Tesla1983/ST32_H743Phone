#include "YMGUI_Invalidate.h"
#include "YMGUI_Geom.h"
#include "YMGUI_Debug.h"
#include "YMGUI_PubDefine.h"
#include "YMGUI_Hal.h"

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_Invalidate.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: 失效/脏矩形系统 + Refresh。与 band 刷新管线咬合:只重绘脏区,逐 band flush
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  *
  * 备注信息：
  * 1.脏区当前用单个合并包围盒(简单起见);后续可换成多矩形列表减少重绘面积
  * 2.空闲无脏区时 Refresh 直接返回 —— 保留模式的省电关键
  *
  ***************************************************************************************************************************
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief 把一块屏幕区域并入 ctx 脏矩形(取包围盒并集)
  */
/**
  * @brief 两矩形是否相交或相邻(用于决定是否合并;相邻也合并以减少碎片)
  */
static int rectTouch(const GYrect* a, const GYrect* b)
{
	//在任一轴上完全分离(留 0 间隙即相邻)则不接触
	if (a->x > b->x + b->w || b->x > a->x + a->w)
		return 0;
	if (a->y > b->y + b->h || b->y > a->y + a->h)
		return 0;
	return 1;
}

/**
  * @brief 取两矩形的包围盒并集,写回 dst
  */
static void rectUnion(GYRECT dst, const GYrect* a, const GYrect* b)
{
	GYcoord x1 = GYMin(a->x, b->x);
	GYcoord y1 = GYMin(a->y, b->y);
	GYcoord x2 = GYMax(a->x + a->w, b->x + b->w);
	GYcoord y2 = GYMax(a->y + a->h, b->y + b->h);
	dst->x = x1;
	dst->y = y1;
	dst->w = x2 - x1;
	dst->h = y2 - y1;
}

/**
  * @brief 把整个脏区列表塌缩成单个包围盒(列表满时的退化路径)
  */
static void collapseAll(GYCTX ctx, const GYrect* extra)
{
	GYrect box = ctx->inv_areas[0];
	for (uint8 i = 1; i < ctx->inv_cnt; i++)
		rectUnion(&box, &box, &ctx->inv_areas[i]);
	if (extra != NULL)
		rectUnion(&box, &box, extra);
	ctx->inv_areas[0] = box;
	ctx->inv_cnt = 1;
}

/**
  * @brief 把一块屏幕区域并入 ctx 脏矩形列表,维护"互不重叠"不变式
  *        新区若与已有块接触则反复合并成一块(连通分量),否则新占一槽;
  *        槽满则塌缩为单包围盒
  */
void YMGUI_Ctx_InvalidateArea(GYCTX ctx, const GYrect* area)
{
	gy_assert(ctx && area);
	gy_log_explain((ctx == NULL) || (area == NULL), GY_LOG_PtrI, "上下文或区域不存在");
	if (ctx == NULL || area == NULL || ctx->destroying)
		return;
	if (area->w <= 0 || area->h <= 0)
		return;

	GYrect merged = *area;
	//反复扫描:凡与 merged 接触的已有块都并进来并从列表移除,直到没有新的接触
	int changed = 1;
	while (changed)
	{
		changed = 0;
		for (uint8 i = 0; i < ctx->inv_cnt;)
		{
			if (rectTouch(&merged, &ctx->inv_areas[i]))
			{
				rectUnion(&merged, &merged, &ctx->inv_areas[i]);
				//移除第 i 块(用末尾块填补)
				ctx->inv_areas[i] = ctx->inv_areas[ctx->inv_cnt - 1];
				ctx->inv_cnt--;
				changed = 1;
			}
			else
			{
				i++;
			}
		}
	}

	//merged 现在与列表里所有块都不接触,占一个新槽
	if (ctx->inv_cnt < GY_INV_MAX)
	{
		ctx->inv_areas[ctx->inv_cnt] = merged;
		ctx->inv_cnt++;
	}
	else
	{
		//列表满:塌缩为单包围盒(退化,仍正确只是多刷些)
		collapseAll(ctx, &merged);
	}
}

/**
  * @brief 标记对象需要重绘(算屏幕绝对区域并入脏矩形)
  */
void YMGUI_Obj_Invalidate(GYOBJ obj)
{
	GYrect abs;
	if (obj == NULL || obj->ctx == NULL || obj->ctx->destroying)
		return;
	if (obj->state & GY_STATE_Hidden)
		return;
	YMGUI_Obj_GetAbsArea(obj, &abs);
	YMGUI_Ctx_InvalidateArea(obj->ctx, &abs);
}

/* ---------------------------------------------------------------------------
 * per-object 绘制耗时统计（2026-10-03 P0-0，仅在 YMGUI_DIAG_OBJTIME 打开时编译）
 *
 * 【为什么要有它】整帧基准只给得出"一帧 41 ms"这个总数。靠"像素数 × 图元单价"
 * 反推出来的占比是不可靠的 —— 那样只能估出 Draw_Fill 一共花了多少，
 * 剩下来的"其余"是谁完全看不见（报告 §2.1 里那 25% 就是这么来的）。
 * 所以直接在 drawObjRec 里逐个对象打点：
 *   self = 该对象 draw_cb 自己的耗时（**不含**后代）→ 找出"哪个绘制最贵"；
 *   sub  = 含后代的整棵子树耗时 → 找出"哪个容器底下的东西最贵"。
 * 两个分开记，是因为父与子的耗时不能混：父的 self 不含子，子的 sub 又是父 sub 的一部分。
 *
 * 【代价】每个对象每次绘制多两次 DWT 读 + 一次线性表查找（表只有 48 项）。
 * 约每帧 0.05 ms 量级，**不改任何绘制行为**，只是计时。
 * ------------------------------------------------------------------------- */
#if defined(YMGUI_DIAG_OBJTIME)

//DWT->CYCCNT（Cortex-M 的周期计数器）。这里不引 CMSIS 头，避免库依赖具体厂商 SDK。
//非 ARM（宿主 SDL 构建）退化成常量 0 —— 计时无意义但照样编得过。
#if defined(__ARM_ARCH) || defined(__arm__)
#define GY_DIAG_NOW()   (*(volatile uint32*)0xE0001004u)
#else
#define GY_DIAG_NOW()   0u
#endif

GYDiagObjTime g_objtime[YMGUI_DIAG_OBJ_N];
uint32        g_objtime_n   = 0;
uint32        g_objtime_ovf = 0;
uint32        g_objtime_on  = 1;

void YMGUI_Diag_ObjTimeReset(void)
{
	uint32 i;
	for (i = 0; i < YMGUI_DIAG_OBJ_N; i++)
	{
		g_objtime[i].obj   = NULL;
		g_objtime[i].cb    = NULL;
		g_objtime[i].x = g_objtime[i].y = g_objtime[i].w = g_objtime[i].h = 0;
		g_objtime[i].self  = 0;
		g_objtime[i].sub   = 0;
		g_objtime[i].calls = 0;
	}
	g_objtime_n   = 0;
	g_objtime_ovf = 0;
}

//取 obj 在表里的槽位；第一次见到就新开一槽；表满返回 -1（并计 ovf）
static int diagObjSlot(GYOBJ obj, const GYrect* abs)
{
	int i;
	for (i = 0; i < (int)g_objtime_n; i++)
	{
		if (g_objtime[i].obj == (void*)obj)
			return i;
	}
	if (g_objtime_n >= YMGUI_DIAG_OBJ_N)
	{
		g_objtime_ovf++;
		return -1;
	}
	i = (int)g_objtime_n++;
	g_objtime[i].obj   = (void*)obj;
	g_objtime[i].cb    = (void*)obj->draw_cb;
	g_objtime[i].x     = abs->x;
	g_objtime[i].y     = abs->y;
	g_objtime[i].w     = abs->w;
	g_objtime[i].h     = abs->h;
	g_objtime[i].self  = 0;
	g_objtime[i].sub   = 0;
	g_objtime[i].calls = 0;
	return i;
}

#endif // YMGUI_DIAG_OBJTIME

/**
 * @brief 递归绘制:若对象绝对区域与本 band 裁剪区相交,则调其 draw_cb,再递归子对象
 *        slot_out:per-object 计时用的槽位输出(非诊断构建恒为 NULL)
 */
static void drawObjRec(GYOBJ obj, GYSURFACE s);   /* 前向声明:子对象递归要走计时外壳 */

static void drawObjRecInner(GYOBJ obj, GYSURFACE s, int* slot_out)
{
	GYrect abs, hit;
	if (obj->state & GY_STATE_Hidden)
		return;
	YMGUI_Obj_GetAbsArea(obj, &abs);
#if defined(YMGUI_DIAG_OBJTIME)
	if (slot_out != NULL)
		*slot_out = diagObjSlot(obj, &abs);
#endif
	//与当前 band 裁剪区相交才画(裁剪由 draw_cb 内的图元再次收窄)
	if (GY_Rect_Intersect(&hit, &abs, &s->clip))
	{
		if (obj->draw_cb != NULL)
		{
#if defined(YMGUI_DIAG_OBJTIME)
			if (slot_out != NULL)      /* slot_out 非 NULL ⇔ 本次要计时 */
			{
				uint32 d0 = GY_DIAG_NOW();
				obj->draw_cb(obj, s, &abs);
				if (*slot_out >= 0)
				{
					g_objtime[*slot_out].self  += GY_DIAG_NOW() - d0;
					g_objtime[*slot_out].calls += 1u;
				}
			}
			else
#endif
			{
				obj->draw_cb(obj, s, &abs);
			}
		}
	}
	//子对象递归。若本对象开 ClipChildren,把 surface.clip 收窄到本对象区域,
	//递归结束后恢复(滚动容器:超出区域的子项被裁掉)
	GYrect saved_clip = s->clip;
	if (obj->state & GY_STATE_ClipChildren)
	{
		GYrect newclip;
		if (!GY_Rect_Intersect(&newclip, &abs, &s->clip))
			return;//本对象完全在裁剪区外,子项也不可见
		s->clip = newclip;
	}
	GYOBJ c = obj->child_head;
	while (c != NULL)
	{
		drawObjRec(c, s);          /* 走计时外壳，子树的耗时才能记进各自的槽位 */
		c = c->sibling;
	}
	s->clip = saved_clip;//恢复
}

/**
  * @brief drawObjRec 的计时外壳:包住整棵子树,把 sub(含后代)记进同一槽位。
  *        诊断关闭时它退化成一次直接调用,不留任何开销。
  */
static void drawObjRec(GYOBJ obj, GYSURFACE s)
{
#if defined(YMGUI_DIAG_OBJTIME)
	if (g_objtime_on)               /* 关掉时只剩这一次判断，开销可忽略 */
	{
		uint32 t0   = GY_DIAG_NOW();
		int    slot = -1;
		drawObjRecInner(obj, s, &slot);
		if (slot >= 0)
			g_objtime[slot].sub += GY_DIAG_NOW() - t0;
		return;
	}
#endif
	drawObjRecInner(obj, s, NULL);
}

/**
  * @brief 自旋等到上一次异步 flush 完成(flush_busy 清零)。有 wait_cb 就调它(裸机可 __WFI 省电),
  *        否则忙等。同步 flush_cb 在返回前已调 FlushReady,故此处立即通过。
  */
static void waitFlushIdle(GYDISP disp)
{
	while (disp->flush_busy)
	{
		if (disp->wait_cb != NULL)
			disp->wait_cb(disp);
	}
}

/**
  * @brief 刷新单个脏矩形:裁到屏幕 → 按 buf 容量切 band → 逐 band 重绘 → flush
  *        pcur 指向"下一条 band 该用哪块 buffer"的游标(双缓冲在 buf1/buf2 间 ping-pong;
  *        单缓冲恒为 buf1)。跨脏矩形沿用同一游标,让重叠不在矩形边界断开。
  */
static void refreshOneRect(GYCTX ctx, GYDISP disp, const GYrect* rect, GYpx** pcur)
{
	//脏区裁到屏幕范围
	GYrect screen = {0, 0, disp->hor_res, disp->ver_res};
	GYrect dirty;
	if (!GY_Rect_Intersect(&dirty, rect, &screen))
		return;

	int dbl = (disp->buf2 != NULL);//双缓冲开关

	//每块 buffer 容量 buf_px_cnt。若脏区宽 > 容量,必须横向也切块,否则一行就越界。
	//tile_w = 一块的最大宽度(不超过脏区宽,也不超过容量)
	uint32 cap = disp->buf_px_cnt;
	GYcoord tile_w = (GYcoord)((cap < (uint32)dirty.w) ? cap : (uint32)dirty.w);
	if (tile_w < 1)
		tile_w = 1;

	GYsurface s;

	//横向按 tile_w 切列
	for (GYcoord tx = dirty.x; tx < dirty.x + dirty.w; tx += tile_w)
	{
		GYcoord tw = (tx + tile_w <= dirty.x + dirty.w) ? tile_w : (dirty.x + dirty.w - tx);
		s.stride = tw;//buffer 每行 = 本列块宽度
		//本列块每条 band 能容纳的行数 = 容量 / 块宽
		GYcoord band_h = (GYcoord)(cap / (uint32)tw);
		if (band_h < 1)
			band_h = 1;
		//纵向按 band_h 切行
		for (GYcoord y = dirty.y; y < dirty.y + dirty.h; y += band_h)
		{
			GYcoord bh = (y + band_h <= dirty.y + dirty.h) ? band_h : (dirty.y + dirty.h - y);
			s.buf = *pcur;//本条 band 渲染进当前 buffer
			s.buf_area.x = tx;
			s.buf_area.y = y;
			s.buf_area.w = tw;
			s.buf_area.h = bh;
			s.clip = s.buf_area;

			//从根开始重绘所有与本块相交的对象
			drawObjRec(ctx->root, &s);
			//再画顶层(弹出层):在 root 之后 → 永远盖在其上
			if (ctx->top_layer != NULL)
				drawObjRec(ctx->top_layer, &s);

			if (dbl)
			{
				//双缓冲:发起本块 flush 前先等上一块传完(任一时刻至多 1 个在途传输),
				//然后置忙、发起(异步 port 立即返回),切到另一块 → CPU 渲染下条 band 与本块 DMA 重叠
				waitFlushIdle(disp);
				disp->flush_busy = 1;
				disp->flush_cb(disp, &s.buf_area, s.buf);
				*pcur = (*pcur == disp->buf1) ? disp->buf2 : disp->buf1;
			}
			else
			{
				//单缓冲:同步推给面板(唯一碰硬件处)。游标恒为 buf1
				disp->flush_cb(disp, &s.buf_area, s.buf);
			}
		}
	}
}

/**
  * @brief 刷新一帧:遍历脏矩形列表,每块各自 band 刷新。无脏区直接返回
  *        脏块互不重叠 → 不会重复混合同一像素
  */
void YMGUI_Refresh(GYCTX ctx)
{
	gy_assert(ctx);
	gy_log_explain(ctx == NULL, GY_LOG_PtrI, "上下文不存在");
	if (ctx->inv_cnt == 0)
		return;//无脏区,空闲不耗

	GYDISP disp = (GYDISP)ctx->disp;
	gy_assert(disp && disp->buf1 && disp->flush_cb);

	//双缓冲:入口先排空上帧可能仍在途的 DMA(否则本帧首块会覆盖正在传的 buffer);
	//游标从 buf1 起,穿过本帧所有脏矩形,让 ping-pong 重叠不在矩形边界断开
	GYpx* cur = disp->buf1;
	if (disp->buf2 != NULL)
		waitFlushIdle(disp);

	for (uint8 i = 0; i < ctx->inv_cnt; i++)
		refreshOneRect(ctx, disp, &ctx->inv_areas[i], &cur);

	//双缓冲:等本帧最后一块传完再返回,保证 Refresh 返回后所有 buffer 空闲(下帧可安全复用)
	if (disp->buf2 != NULL)
		waitFlushIdle(disp);

	YMGUI_Disp_FrameDone(disp);

	ctx->inv_cnt = 0;//本帧脏区已处理
}
