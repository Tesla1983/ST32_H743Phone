#include "YMGUI_DrawImg.h"
#include "YMGUI_DrawPx.h"
#include "YMGUI_Geom.h"
#include "YMGUI_Debug.h"

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_DrawImg.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: 图片 blit。native GYpx 数据直拷,可选 colorkey 透明。裁剪+band偏移
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief 把图片 blit 到 surface(左上角屏幕 x,y)
  */
void YMGUI_Draw_Img(GYSURFACE s, GYIMG img, GYcoord x, GYcoord y)
{
	gy_assert(s && s->buf && img && img->data);
	gy_log_explain((s == NULL) || (img == NULL), GY_LOG_PtrI, "表面或图片不存在");

	//图片占用屏幕矩形,先与裁剪区求交(只遍历可见部分)
	GYrect imgr = {x, y, img->w, img->h};
	GYrect vis;
	if (!GY_Rect_Intersect(&vis, &imgr, &s->clip))
		return;

	for (GYcoord sy = vis.y; sy < vis.y + vis.h; sy++)
	{
		GYcoord iy = sy - y;//图片内行
		const GYpx* srow = img->data + (int32)iy * img->w;
		for (GYcoord sx = vis.x; sx < vis.x + vis.w; sx++)
		{
			GYcoord ix = sx - x;//图片内列
			GYpx p = srow[ix];
			if (img->use_key && GY_PxEqual(p, img->key))
				continue;//透明色跳过
			GY_PutPx(s, sx, sy, p);
		}
	}
}

/**
  * @brief 把图片缩放 blit 到目标屏幕矩形 dst(定点最近邻)
  *   遍历 dst∩clip 的每个屏幕像素,反算源像素:
  *     ix = (sx - dst.x) * src_w / dst.w  (dst.h 同理)
  *   用 16.16 定点步进(step = (src<<16)/dst),避免每像素乘除;累加取整。
  */
void YMGUI_Draw_ImgScaled(GYSURFACE s, GYIMG img, GYrect dst)
{
	gy_assert(s && s->buf && img && img->data);
	gy_log_explain((s == NULL) || (img == NULL), GY_LOG_PtrI, "表面或图片不存在");
	if (s == NULL || img == NULL || img->data == NULL)
		return;
	if (dst.w <= 0 || dst.h <= 0 || img->w <= 0 || img->h <= 0)
		return;

	//目标矩形先与裁剪区求交(只遍历可见部分)
	GYrect vis;
	if (!GY_Rect_Intersect(&vis, &dst, &s->clip))
		return;
	//再 ∩ buf_area:原实现靠 GY_PutPx 的兜底检查挡越界;这里把求交提前,
	//后面就能用指针直接写而不用每像素重复判 6 次边界(见下方 P1-2 注释)
	if (!GY_Rect_Intersect(&vis, &vis, &s->buf_area))
		return;

	//16.16 定点采样步长:每前进一个目标像素,源坐标推进 step
	int32 stepx = (int32)(((int64)img->w << 16) / dst.w);
	int32 stepy = (int32)(((int64)img->h << 16) / dst.h);

	/* ★P1-2：内层重写（2026-10-03）★
	 *
	 * 【原实现的两个每像素成本】
	 *   1. 源列号 `sxf = ((sx - dst.x) * stepx + stepx/2) >> 16` —— 每个像素一次
	 *      **64 位乘法**（stepx 是 16.16 定点，乘积会超 32 位）。M7 上 64 位乘要
	 *      多指令且不可流水，是缩放 blit 卖到 58.8 cycles/px 的直接原因之一；
	 *   2. 每个像素走一次 `GY_PutPx()` —— 6 次边界比较 + 减法 + 乘法算索引，
	 *      而这些判断对**同一行内连续的一整段**来说是恒真的常量。
	 *
	 * 【这里的做法】
	 *   - 源列号改**定点累加器**：acc 初始 = (起始列偏移 × stepx + 半步)，每像素
	 *     只做一次 `acc += stepx`（一条 64 位加法），不再有乘法；
	 *   - 行首指针 + `*d++` 直写，裁剪已经在上面一次性做完。
	 *
	 * 【为什么逐位等价】
	 *   - 采样公式没变：acc 就是原式分子，每步 +stepx，>>16 后取整 —— 逐列相同；
	 *     钳位 [0, w-1] 也照做，且钳位**不改累加器**（原式每列独立重算，同理）；
	 *   - 写入区域 = dst ∩ clip ∩ buf_area 后逐像素直写：原先这些像素在 GY_PutPx 里
	 *     恰好全部通过边界检查（vis 已是 clip 与 buf_area 的子集），写入值也相同；
	 *   - colorkey 跳过时指针照常前进 ⇒ 目标位置一一对应。 */
	{
		const GYpx* imgdata = (const GYpx*)img->data;
		GYpx*       bufp    = (GYpx*)s->buf;
		int32       stride  = (int32)s->stride;
		int32       iw      = (int32)img->w;
		GYcoord     sy;

		for (sy = vis.y; sy < vis.y + vis.h; sy++)
		{
			//源行:(sy - dst.y) 个目标像素对应的源行(定点后取整),+半步做四舍五入近似
			int32 syf = (int32)(((int64)(sy - dst.y) * stepy + (stepy >> 1)) >> 16);
			if (syf < 0) syf = 0;
			if (syf >= img->h) syf = img->h - 1;
			const GYpx* srow = imgdata + (int32)syf * iw;
			GYpx* d = bufp + (int32)(sy - s->buf_area.y) * stride
			                + (int32)(vis.x - s->buf_area.x);
			int64 acc = (int64)(vis.x - dst.x) * stepx + (stepx >> 1);
			int   n   = (int)vis.w;
#if defined(YMGUI_DIAG_FILLPX)
			uint32 nwr = 0;
#endif
			while (n != 0)
			{
				int32 sxf = (int32)(acc >> 16);
				acc += stepx;
				if (sxf < 0) sxf = 0;
				else if (sxf >= iw) sxf = iw - 1;
				{
					GYpx p = srow[sxf];
					if (!(img->use_key && GY_PxEqual(p, img->key)))
					{
						*d = p;
#if defined(YMGUI_DIAG_FILLPX)
						++nwr;
#endif
					}
				}
				++d;
				--n;
			}
#if defined(YMGUI_DIAG_FILLPX)
			g_diag_putpx += nwr;
#endif
		}
	}
}
