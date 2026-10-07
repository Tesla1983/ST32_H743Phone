#include "YMGUI_DrawArc.h"
#include "YMGUI_DrawPx.h"
#include "YMGUI_Trig.h"
#include "YMGUI_Debug.h"

/**
  ***************************************************************************************************************************
  *	@FileName:    YMGUI_DrawArc.c
  *	@Author:      yaomimaoren
  *	@Date:        2026-07-01
  *	@Description: 中点画圆(整数 8 对称)/实心圆盘/定点三角表画弧。全整数,无 FPU
  *	@Version:     1.0
  *
  ***************************************************************************************************************************
  * Copyright (C), 2026-2036, YAOMI Tech. Co., Ltd.
  ***************************************************************************************************************************/

/**
  * @brief 画一条水平跨度(实心圆盘用)
  *
  * ★2026-10-04 步骤 5：裁剪从"每像素一遍"提到"整条跨度一遍"★
  *
  * 【原来的代价】原实现是 `for (x = x1; x <= x2; x++) GY_PutPx(...)`。
  * GY_PutPx 虽然被内联，但 GCC **没有把循环不变量提出去** —— 反汇编
  * build/ymgui-h743.elf 的 YMGUI_Draw_CircleFill 可见回边落回 0x8018710，
  * 每个像素都要重新从内存读 clip.x/clip.w/clip.y/clip.h、
  * buf_area.x/buf_area.y/buf_area.w/buf_area.h、stride 共 9 个字段并重跑
  * 8 个比较分支，外加一次 g_diag_putpx 的全局读-改-写：
  *
  *     8018710: ldrsh r5,[sl,#12]      ; clip.x
  *     801871e: ldrsh r6,[sl,#16]      ; clip.w
  *     8018728: ldrsh r0,[sl,#14]      ; clip.y
  *     801873a: ldrh  r0,[sl,#4]       ; buf_area.x
  *     801874a: ldrh  r0,[sl,#6]       ; buf_area.y
  *     8018760: ldrsh ip,[sl,#10]      ; buf_area.h
  *     801876a: ldrh  r0,[sl,#20]      ; stride
  *     801877e: strh.w r3,[r0,r6,lsl#1]; ← 真正的一笔存储
  *     8018782: str.w  ip,[r5]         ; g_diag_putpx++
  *     801878a: bge   8018710          ; 下一像素，全部重来
  *
  * 约 24 条指令/像素。而同一个仓库里的 YMGUI_Draw_Fill 早就做对了
  * （裁剪一次 + 指针递增 + 32 位双像素写）。本函数是**引擎内唯一还在逐像素
  * 重复裁剪的实心跨度出口**，而它正好是实心圆盘的全部像素 ——
  * phone_ui.c 的画廊画 r=31/94/116 三个装饰圆，实心核约 73000 像素，
  * 每次重绘在这里烧掉约 1.75M 周期 ≈ 4.4 ms。
  *
  * 【改法】照 Draw_Fill 的口径：先把 [x1,x2] 与 clip ∩ buf_area 求交一次，
  * 取出该行行首指针，然后走紧循环；对齐且长度为偶数时用 32 位双像素写。
  *
  * 【为什么逐位等价】写入的像素集合 = [x1,x2] ∩ clip ∩ buf_area（与原实现
  * 逐像素判定的结果完全一致，边界都是半开区间）；32 位路径写下的两个相同
  * 半字与分两次写 16 位完全相同（RGB565 小端，低半字在前）。
  * 诊断计数改为**一次累加 n**（原来是每像素 +1，总数不变）——
  * 与 DrawImg.c 里 `g_diag_putpx += nwr` 的做法一致，语义不变、开销没了。
  */
static void hspan(GYSURFACE s, GYcoord x1, GYcoord x2, GYcoord y, GYpx px)
{
	int32 lo = (int32)x1;
	int32 hi = (int32)x2;
	int32 n;
	GYpx* p;

	/* 1) 行是否在 clip / buf_area 的纵向范围内（两者只判一次，与 x 无关） */
	{
		int32 cy0 = (int32)s->clip.y;
		int32 cy1 = cy0 + (int32)s->clip.h;
		int32 by  = (int32)y - (int32)s->buf_area.y;
		if ((int32)y < cy0 || (int32)y >= cy1)
			return;
		if (by < 0 || by >= (int32)s->buf_area.h)
			return;

		/* 2) 横向求交：clip 一次、buf_area 一次 */
		{
			int32 cx0 = (int32)s->clip.x;
			int32 cx1 = cx0 + (int32)s->clip.w;
			int32 bx0 = (int32)s->buf_area.x;
			int32 bx1 = bx0 + (int32)s->buf_area.w;
			if (lo < cx0) lo = cx0;
			if (hi >= cx1) hi = cx1 - 1;
			if (lo < bx0) lo = bx0;
			if (hi >= bx1) hi = bx1 - 1;
			if (hi < lo)
				return;
			/* 3) 行首指针：到这里 lo 已保证落在 [buf_area.x, buf_area.x+w) 内 */
			p = (GYpx*)s->buf + by * (int32)s->stride + (lo - bx0);
		}
	}

	n = hi - lo + 1;

#if defined(YMGUI_DIAG_FILLPX)
	/* 与原逐像素 GY_PutPx 的累计值完全等值，只是从"每像素一次 RMW"变成"每跨度一次" */
	g_diag_putpx += (uint32)n;
#endif

	if (((((uint32)p) & 3u) == 0u) && ((n & 1) == 0))
	{
		gy_u32_alias_t* pw = (gy_u32_alias_t*)p;
		uint32 f32 = (uint32)px | ((uint32)px << 16);
		do { *pw++ = f32; } while ((n -= 2) != 0);
	}
	else
	{
		do { *p++ = px; } while (--n != 0);
	}
}

#if YMGUI_ANTIALIAS
/**
  * @brief 圆的 8 对称点各以覆盖度 opa 混合(圆描边抗锯齿用)
  */
static void put8AA(GYSURFACE s, GYcoord cx, GYcoord cy, GYcoord x, GYcoord y, GYcolor color, GYopa opa)
{
	if (opa == GY_OPA_TRANSP)
		return;
	GY_BlendPx(s, cx + x, cy + y, color, opa);
	GY_BlendPx(s, cx - x, cy + y, color, opa);
	GY_BlendPx(s, cx + x, cy - y, color, opa);
	GY_BlendPx(s, cx - x, cy - y, color, opa);
	GY_BlendPx(s, cx + y, cy + x, color, opa);
	GY_BlendPx(s, cx - y, cy + x, color, opa);
	GY_BlendPx(s, cx + y, cy - x, color, opa);
	GY_BlendPx(s, cx - y, cy - x, color, opa);
}

/**
  * @brief 双线性 splat:把一个 16.16 亚像素点按四邻权重混合(细弧抗锯齿用)
  */
static void splatPx(GYSURFACE s, int32 fx16, int32 fy16, GYcolor color)
{
	GYcoord xi = (GYcoord)(fx16 >> 16);
	GYcoord yi = (GYcoord)(fy16 >> 16);
	uint32  fxr = (uint32)(fx16 & 0xFFFF) >> 8;//0..255
	uint32  fyr = (uint32)(fy16 & 0xFFFF) >> 8;
	uint32  ix = 255u - fxr;
	uint32  iy = 255u - fyr;
	GY_BlendPx(s, xi,     yi,     color, (GYopa)((ix * iy) >> 8));
	GY_BlendPx(s, xi + 1, yi,     color, (GYopa)((fxr * iy) >> 8));
	GY_BlendPx(s, xi,     yi + 1, color, (GYopa)((ix * fyr) >> 8));
	GY_BlendPx(s, xi + 1, yi + 1, color, (GYopa)((fxr * fyr) >> 8));
}
#endif

/**
  * @brief 中点画圆(1px 描边,8 对称)
  */
void YMGUI_Draw_Circle(GYSURFACE s, GYcoord cx, GYcoord cy, GYcoord r, GYcolor color)
{
	gy_assert(s && s->buf);
	gy_log_explain((s == NULL) || (s->buf == NULL), GY_LOG_PtrI, "表面或缓冲区不存在");
	if (r <= 0)
		return;
#if YMGUI_ANTIALIAS
	//Wu 圆:第一八分圆逐 x 算精确 y = sqrt(r^2-x^2),主/邻像素按小数分覆盖度,8 对称
	for (int32 x = 0; ; x++)
	{
		int32 val = (int32)r * r - x * x;
		if (val < 0) val = 0;
		//sqrt 放大 256 倍取 8 位小数:isqrt(val<<16)=sqrt(val)*256
		uint32 sq = GY_Isqrt((uint32)val << 16);
		int32  yi = (int32)(sq >> 8);
		int32  frac = (int32)(sq & 0xFF);
		if (x > yi)                     //越过八分圆边界(x==y)即止,余靠对称
			break;
		//真实边界在 yi..yi+1 之间(距离 frac/256),邻居像素取 yi+1(外侧)
		put8AA(s, cx, cy, (GYcoord)x, (GYcoord)yi,       color, (GYopa)(255 - frac));
		put8AA(s, cx, cy, (GYcoord)x, (GYcoord)(yi + 1), color, (GYopa)frac);
	}
#else
	GYpx px = GY_ColorToPx(color);
	int32 x = 0, y = r;
	int32 d = 1 - r;
	while (x <= y)
	{
		GY_PutPx(s, cx + x, cy + y, px);
		GY_PutPx(s, cx - x, cy + y, px);
		GY_PutPx(s, cx + x, cy - y, px);
		GY_PutPx(s, cx - x, cy - y, px);
		GY_PutPx(s, cx + y, cy + x, px);
		GY_PutPx(s, cx - y, cy + x, px);
		GY_PutPx(s, cx + y, cy - x, px);
		GY_PutPx(s, cx - y, cy - x, px);
		x++;
		if (d < 0)
		{
			d += 2 * x + 1;
		}
		else
		{
			y--;
			d += 2 * (x - y) + 1;
		}
	}
#endif
}

/**
  * @brief 实心圆盘(每条对称行用水平跨度填满)
  */
void YMGUI_Draw_CircleFill(GYSURFACE s, GYcoord cx, GYcoord cy, GYcoord r, GYcolor color)
{
	gy_assert(s && s->buf);
	gy_log_explain((s == NULL) || (s->buf == NULL), GY_LOG_PtrI, "表面或缓冲区不存在");
	if (r <= 0)
		return;
	GYpx px = GY_ColorToPx(color);
#if YMGUI_ANTIALIAS
	//距离场式抗锯齿:只把"距边界≥0.5px 的内部"填实心,边缘 1px 环按到圆心
	//距离算覆盖度(cov = r*256 + 128 - dist*256)。各方向一致 → 上下扁平处也平滑
	int32 r256  = (int32)r * 256;
	int32 core2 = (2 * (int32)r - 1) * (2 * (int32)r - 1);//(2r-1)^2 = (r-0.5)^2 * 4
	for (int32 dy = -r; dy <= r; dy++)
	{
		GYcoord y = (GYcoord)(cy + dy);
		//实心核半宽 = floor(sqrt((r-0.5)^2 - dy^2)):用 4 倍整数避开小数
		int32 t = core2 - 4 * dy * dy;
		int32 startDx;//边界羽化扫描起点
		if (t >= 0)
		{
			int32 solidHalf = (int32)(GY_Isqrt((uint32)t) >> 1);//= floor(sqrt(t)/2)
			hspan(s, (GYcoord)(cx - solidHalf), (GYcoord)(cx + solidHalf), y, px);
			startDx = solidHalf + 1;
		}
		else
		{
			startDx = 0;//仅最上/最下行无实心核,从中心起羽化(去掉硬"奶头")
		}
		for (int32 dx = startDx; dx <= (int32)r + 1; dx++)
		{
			int32  dist2 = dx * dx + dy * dy;
			uint32 d256  = GY_Isqrt((uint32)dist2 << 16);//dist * 256
			int32  cov   = r256 + 128 - (int32)d256;
			if (cov <= 0)
				break;
			GYopa opa = (cov >= 255) ? (GYopa)255 : (GYopa)cov;
			GY_BlendPx(s, (GYcoord)(cx + dx), y, color, opa);
			if (dx > 0)
				GY_BlendPx(s, (GYcoord)(cx - dx), y, color, opa);
		}
	}
#else
	int32 x = 0, y = r;
	int32 d = 1 - r;
	while (x <= y)
	{
		hspan(s, cx - x, cx + x, cy + y, px);
		hspan(s, cx - x, cx + x, cy - y, px);
		hspan(s, cx - y, cx + y, cy + x, px);
		hspan(s, cx - y, cx + y, cy - x, px);
		x++;
		if (d < 0)
		{
			d += 2 * x + 1;
		}
		else
		{
			y--;
			d += 2 * (x - y) + 1;
		}
	}
#endif
}

/**
  * @brief 画圆弧:按 1 度步进,用定点三角表算点坐标
  *        坐标 = center + r * (cos, sin) >> Q15。0度=右,90度=下(屏幕 y 向下)
  */
void YMGUI_Draw_Arc(GYSURFACE s, GYcoord cx, GYcoord cy, GYcoord r, int32 start_deg, int32 end_deg, GYcolor color)
{
	gy_assert(s && s->buf);
	gy_log_explain((s == NULL) || (s->buf == NULL), GY_LOG_PtrI, "表面或缓冲区不存在");
	if (r <= 0)
		return;
	if (end_deg < start_deg)
		end_deg += 360;
#if YMGUI_ANTIALIAS
	//按弧长细分(每步端点间距<1px),用 deg64 定点三角 + 双线性 splat
	int32 total_deg = end_deg - start_deg;
	int32 steps = total_deg * (r / 32 + 1);
	if (steps < total_deg) steps = total_deg;
	if (steps < 1) steps = 1;
	int32 start64 = start_deg * 64;
	int32 span64 = total_deg * 64;
	for (int32 i = 0; i <= steps; i++)
	{
		int32 a64 = start64 + (int32)((int64)span64 * i / steps);
		int32 c = GY_Cos64(a64);//Q15
		int32 sn = GY_Sin64(a64);
		//圆心+r*(cos,sin),结果留 16.16 亚像素:Q15 值 <<1 得 16.16 的 r 倍分数
		int32 fx16 = ((int32)cx << 16) + (int32)(((int64)r * c) << (16 - GY_TRIG_SHIFT));
		int32 fy16 = ((int32)cy << 16) + (int32)(((int64)r * sn) << (16 - GY_TRIG_SHIFT));
		splatPx(s, fx16, fy16, color);
	}
#else
	GYpx px = GY_ColorToPx(color);
	for (int32 a = start_deg; a <= end_deg; a++)
	{
		//r * cos/sin,Q15 定点 → 右移 15 位回整数
		GYcoord dx = (GYcoord)(((int32)r * GY_Cos(a)) >> GY_TRIG_SHIFT);
		GYcoord dy = (GYcoord)(((int32)r * GY_Sin(a)) >> GY_TRIG_SHIFT);
		GY_PutPx(s, cx + dx, cy + dy, px);
	}
#endif
}

/**
  * @brief 画粗弧环(r_in..r_out 扇环,无缝隙)
  *        角度按外圆弧长自适应细分(每步外端点间距 < 1px),每个角度沿半径从 r_in 到 r_out
  *        连续画点 → 周向(细分)+径向(逐半径)双向都不留缝
  */
void YMGUI_Draw_ArcThick(GYSURFACE s, GYcoord cx, GYcoord cy, GYcoord r_in, GYcoord r_out, int32 start_deg, int32 end_deg, GYcolor color)
{
	gy_assert(s && s->buf);
	gy_log_explain((s == NULL) || (s->buf == NULL), GY_LOG_PtrI, "表面或缓冲区不存在");
	if (r_out < r_in)
	{
		GYcoord t = r_in; r_in = r_out; r_out = t;
	}
	if (r_out <= 0)
		return;
	if (r_in < 0)
		r_in = 0;
	GYpx px = GY_ColorToPx(color);
	if (end_deg < start_deg)
		end_deg += 360;
	int32 total_deg = end_deg - start_deg;

#if YMGUI_ANTIALIAS
	//距离场式:逐像素按"到圆心距离"给内外两边覆盖度(两条弧边都平滑),
	//角度用半平面叉积判扇形(端帽硬切,弧短不显)。cos/sin 内部自带取模
	int32 r_out256 = (int32)r_out * 256;
	int32 r_in256  = (int32)r_in * 256;
	int32 cosS = GY_Cos(start_deg), sinS = GY_Sin(start_deg);//Q15
	int32 cosE = GY_Cos(end_deg),   sinE = GY_Sin(end_deg);
	int32 lim  = (GYcoord)(r_out + 1);
	/* ★2026-10-03 优化：先用平方比较滤掉环带外的像素，再开方★
	 *
	 * 【原实现的浪费】这个距离场写法对包围盒 (2·lim+1)² 个像素**逐个调
	 * GY_Isqrt()**。以 phone_ui.c 的典型调用 (r_in=12, r_out=14) 为例：
	 * 包围盒 31×31 = 961 个像素，而真正落在环带内的只有 π(14²−12²) ≈ 163 个
	 * —— 也就是说 **83% 的开方算完就被下面 `cov <= 0` 丢弃了**。
	 * 板上实测：单次 Draw_ArcThick 要 118 729 cycles (0.297 ms)，
	 * 竟然和填满整条 320×32 的 band (0.284 ms) 一样贵。
	 *
	 * 【这里的做法】开方前先用**平方**比较判一次：只有可能落在环带内的像素
	 * 才去开方。判据取的是原 cov 判据的**宽松超集**，不会漏画任何像素：
	 *   原判据 cov>0 ⇒ dist < r_out + 0.5  且  dist > r_in − 0.5
	 *   这里放行 ⇒ dist² ≤ (r_out+1)²       且  dist² ≥ (r_in−1)²
	 * 因为 r_out+1 > r_out+0.5、r_in−1 < r_in−0.5，放行的范围严格更大，
	 * 所以是**等价**的（多算几个像素，结果一致；不会少画）。
	 * 代价只是两次 32 位乘法 + 两次比较，远小于一次整数开方。 */
	uint32 out2 = (uint32)(r_out + 1) * (uint32)(r_out + 1);
	uint32 in2  = (r_in > 1) ? ((uint32)(r_in - 1) * (uint32)(r_in - 1)) : 0u;
	for (int32 dy = -lim; dy <= lim; dy++)
	{
		uint32 dy2 = (uint32)(dy * dy);
		for (int32 dx = -lim; dx <= lim; dx++)
		{
			uint32 rr = dy2 + (uint32)(dx * dx);
			if (rr > out2 || rr < in2)
				continue;                   /* 环带外：省掉一次 GY_Isqrt */
			//径向覆盖度:外边 covOut、内边 covIn,取小(环带交集)
			uint32 d256 = GY_Isqrt(rr << 16);//dist*256
			int32  covOut = r_out256 + 128 - (int32)d256;
			int32  covIn  = (r_in > 0) ? ((int32)d256 - (r_in256 - 128)) : 255;
			int32  cov = (covOut < covIn) ? covOut : covIn;
			if (cov <= 0)
				continue;
			//角度扇形判定(叉积:起边左侧 & 终边右侧;>180度取并集)
			if (total_deg < 360)
			{
				int64 crS = (int64)cosS * dy - (int64)sinS * dx;
				int64 crE = (int64)cosE * dy - (int64)sinE * dx;
				int32 inSector = (total_deg <= 180)
					? ((crS >= 0) && (crE <= 0))
					: ((crS >= 0) || (crE <= 0));
				if (!inSector)
					continue;
			}
			GYopa opa = (cov >= 255) ? (GYopa)255 : (GYopa)cov;
			GY_BlendPx(s, (GYcoord)(cx + dx), (GYcoord)(cy + dy), color, opa);
		}
	}
#else
	//细分步数:外圆周每度弧长约 r_out * (pi/180) ≈ r_out/57。要每步 <1px,
	//则每度需 r_out/57 步 → 总步数 = 度数 *(r_out/32+1),换算成 deg64 增量
	int32 steps = total_deg * (r_out / 32 + 1);//每度至少 (r_out/32+1) 步,足够密
	if (steps < total_deg)
		steps = total_deg;
	if (steps < 1)
		steps = 1;
	int32 start64 = start_deg * 64;
	int32 span64 = total_deg * 64;

	for (int32 i = 0; i <= steps; i++)
	{
		int32 a64 = start64 + (int32)((int64)span64 * i / steps);
		int32 c = GY_Cos64(a64);//Q15
		int32 sn = GY_Sin64(a64);
		//沿半径逐像素填(径向连续)
		for (GYcoord rr = r_in; rr <= r_out; rr++)
		{
			GYcoord dx = (GYcoord)(((int32)rr * c) >> GY_TRIG_SHIFT);
			GYcoord dy = (GYcoord)(((int32)rr * sn) >> GY_TRIG_SHIFT);
			GY_PutPx(s, cx + dx, cy + dy, px);
		}
	}
#endif
}
