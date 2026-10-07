#include "YMGUI_DrawFill.h"
#include "YMGUI_DrawPx.h"
#include "YMGUI_Geom.h"
#include "YMGUI_Debug.h"

/* ---------------------------------------------------------------------------
 * 32 位双像素写用的别名安全类型（P1-1）
 *
 * draw buffer 的"有效类型"是 GYpx(uint16)，直接 cast 成 uint32* 写会踩**严格别名规则**
 * (strict aliasing)：编译器有权假定两种类型的访问互不相关而乱序。用 GCC 的 may_alias
 * 属性显式告诉它"这个类型可能与其它类型别名"，消除该风险。
 *
 * ⚠ 2026-10-04 步骤 5：该 typedef 已**上移到 YMGUI_DrawPx.h**（hspan 重写也要用它，
 *   而同一 TU 里 typedef 不能重复声明，所以这里只能留说明、不能再定义一遍）。
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * 渲染诊断计数（2026-10-03，仅在 YMGUI_DIAG_FILLPX 打开时编译进来）
 *
 * 【为什么要有它】满负载基准只给出"一帧 61 ms"这个总数，看不出这 61 ms 是
 * "少数几层很贵的绘制"还是"很多层便宜的绘制叠起来"。区分二者直接决定优化方向：
 * 前者要改算法，后者要**削 overdraw**（少画几层、或让层更便宜）。
 * 有了"一帧实际填了多少像素"，除以整屏 153600 就得到 overdraw 倍数；
 * 有了调用次数，就知道分摊到每次调用有多少像素（判断是不是碎调用太多）。
 * 纯计数，不改变任何绘制行为。
 * ------------------------------------------------------------------------- */
#if defined(YMGUI_DIAG_FILLPX)
uint32_t g_diag_fill_px     = 0;   /* 累计填充像素数（裁剪后实际写入的） */
uint32_t g_diag_fill_calls  = 0;   /* Draw_Fill 调用次数 */
uint32_t g_diag_fill_opa_px = 0;   /* 其中走 alpha 混合（opa != 255）的像素数 */
/* 其余图元（文字 / 圆弧 / 线条）的逐像素写入出口，定义在 DrawPx.h 的内联函数里 */
uint32_t g_diag_putpx       = 0;   /* GY_PutPx 实际写入次数 */
uint32_t g_diag_blendpx     = 0;   /* GY_BlendPx 实际写入次数 */
#endif

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_DrawFill.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: 矩形填充图元(软件光栅化)。收屏幕坐标,裁剪到 surface.clip 后写入 draw buffer
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  *
  * 备注信息：
  * 1.opa==255 走直写(向量化友好);opa<255 走读-混合-写回(RGB565 无 alpha,alpha 只在此刻存在)
  *
  ***************************************************************************************************************************
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief 在表面填充矩形区域(带裁剪与混合)
  *        像素混合内核已提升为 GY_MixPx(见 DrawPx.h),与抗锯齿图元共用
  */
void YMGUI_Draw_Fill(GYSURFACE s, const GYrect* area, GYcolor color, GYopa opa)
{
	GYrect draw_area;
	GYpx*  bufp;
	GYpx   fillpx;
	//输入指针判断
	gy_assert(s && area);
	gy_log_explain((s == NULL) || (area == NULL), GY_LOG_PtrI, "表面或区域不存在");
	gy_assert(s->buf);
	gy_log_explain(s->buf == NULL, GY_LOG_PtrI, "表面缓冲区不存在");
	//覆盖度为0直接返回
	if (opa == GY_OPA_TRANSP)
		return;

	//区域裁剪:填充区 ∩ 表面裁剪区(屏幕坐标)
	if (GY_Rect_Intersect(&draw_area, area, &s->clip) == 0)
		return;//完全在裁剪区外
	//再 ∩ buf_area,防 clip 非 buf_area 子集时越界写
	if (GY_Rect_Intersect(&draw_area, &draw_area, &s->buf_area) == 0)
		return;

	bufp = (GYpx*)s->buf;
	fillpx = GY_ColorToPx(color);

#if defined(YMGUI_DIAG_FILLPX)
	{
		uint32_t npx = (uint32_t)draw_area.w * (uint32_t)draw_area.h;
		g_diag_fill_px    += npx;
		g_diag_fill_calls += 1u;
		if (opa != GY_OPA_COVER)
			g_diag_fill_opa_px += npx;
	}
#endif

	/* ★P1-1：内层循环重写（2026-10-03）★
	 *
	 * 【原实现的三个"每像素都付一次"的开销】
	 *   1. `if (opa == GY_OPA_COVER)` 判在内层 —— 每像素一次分支，
	 *      而 opa 在整个调用里是常量（分支预测帮得上，但白白占了发射槽）；
	 *   2. `row[bx]` 里 bx 由 `x - buf_area.x` 现算，再加一次"基址+索引"寻址；
	 *   3. 最要命的：一次只写 **2 字节**。板上实测不透明填充 11.07 cycles/px，
	 *      而纯指针直写的地板也就是 11–12 cycles/px ⇒ 说明成本在**总线事务**上，
	 *      不是循环开销（见 tools/bench.py 微基准 case6/7 的对比数据）。
	 *      一次写 4 字节就能让事务数减半。
	 *
	 * 【这里的做法】
	 *   - opa 分支提到循环外，两条路径各自展开；
	 *   - 循环内改成指针递增（`*p++`），索引计算全部提到循环外；
	 *   - 不透明路径在**目标 4 字节对齐 + 宽度为偶数 + stride 为偶数**时走
	 *     32 位双像素写（`fillpx | fillpx<<16`）。
	 *
	 * 【为什么逐位等价】
	 *   - 覆盖的像素集合 = draw_area ∩ clip ∩ buf_area，与原实现完全一致
	 *     （裁剪逻辑一个字没动）；
	 *   - 双像素写写的两个 16 位半字与"分两次写"完全相同；RGB565 是小端序，
	 *     低半字在前 ⇒ `px | px<<16` 就是 [px][px]；
	 *   - 对齐判断里要求的 `stride` 为偶数是为了保证**每一行的对齐性与首行相同**
	 *     （行步长 = stride×2 字节，stride 偶 ⇒ 步长是 4 的倍数），
	 *     所以这个判断可以提到行循环外，每行不再重复判，也不必每行回退到 16 位路径。
	 */
	{
		int32  stride = (int32)s->stride;
		int    bx0    = (int)draw_area.x - (int)s->buf_area.x;
		int    by0    = (int)draw_area.y - (int)s->buf_area.y;
		int    w      = (int)draw_area.w;
		int    h      = (int)draw_area.h;
		GYpx*  row0   = bufp + (int32)by0 * stride + bx0;
		int    y;

		if (opa == GY_OPA_COVER)
		{
			int    use32 = ((((uint32)row0) & 3u) == 0u) && ((w & 1) == 0) && ((stride & 1) == 0);
			uint32 f32   = (uint32)fillpx | ((uint32)fillpx << 16);
			for (y = 0; y < h; y++)
			{
				GYpx* p = row0 + (int32)y * stride;
				if (use32)
				{
					gy_u32_alias_t* pw = (gy_u32_alias_t*)p;
					int n = w >> 1;
					while (n != 0)
					{
						*pw++ = f32;
						--n;
					}
				}
				else
				{
					int n = w;
					while (n != 0)
					{
						*p++ = fillpx;
						--n;
					}
				}
			}
		}
		else
		{
			for (y = 0; y < h; y++)
			{
				GYpx* p = row0 + (int32)y * stride;
				int   n = w;
				while (n != 0)
				{
					*p = GY_MixPx(*p, color, opa); //读-混合-写回
					++p;
					--n;
				}
			}
		}
	}
}
