#ifndef YMGUI_DRAWPX_H
#define YMGUI_DRAWPX_H

#include "YMGUI_PubType.h"
#include "YMGUI_Surface.h"
#include "YMGUI_PubDefine.h"

//===========================================================================
// 内部共享:单像素写入(裁剪到 surface.clip + band 偏移 + 可选混合)
//   line/circle/arc 等图元都用它,保证裁剪逻辑一处实现
//===========================================================================

//---------------------------------------------------------------------------
// ★2026-10-04 步骤 5：强制内联 + 别名安全类型（本工程移植轮）★
//
// 【为什么要 always_inline】这两个函数本来是 `static inline`，但 GCC -O2 在**热路径上
// 拒绝内联**：反汇编 build/ymgui-h743.elf 可见 GY_BlendPx 被编成两份 264 字节的本地
// 真函数（08018434 / 08018e30），调用点恰好是三个最热的抗锯齿图元：
//     YMGUI_Draw_CircleFill 的羽化环 / YMGUI_Draw_ArcThick / YMGUI_Draw_Line（×2）
// 于是每个抗锯齿像素都要多付一次 AAPCS 调用（压栈/传参/返回预测），再在函数体里
// 重做一遍 6 项裁剪校验。176877 个混合像素/帧里只要有 1/3 走这三条路，
// 光是调用开销就是 1~3 ms 量级。
//
// 【风险与取舍】强制内联会让这几个调用点展开（约 +800 B text），而 M7 的 I-Cache
// 只有 16 KB。所以只对**逐像素出口**强制内联 —— 它们本来就在紧循环里，
// 展开省下的是"每像素一次调用"，收益远大于几百字节。若 text 吃紧要回退，
// 先看反汇编确认。
//---------------------------------------------------------------------------
#if defined(__GNUC__)
#define GY_ALWAYS_INLINE  static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define GY_ALWAYS_INLINE  static __forceinline
#else
#define GY_ALWAYS_INLINE  static inline
#endif

//---------------------------------------------------------------------------
// 32 位双像素写用的别名安全类型。
// （原来私有在 YMGUI_DrawFill.c；步骤 5 重写的 hspan 也要用它，故提到这里共用。
//   同一个 TU 里 typedef 不能重复声明，所以 DrawFill.c 里那份必须同时删掉。）
// 直接 cast 成 uint32* 写会踩**严格别名规则**(strict aliasing)，用 may_alias 显式豁免。
//---------------------------------------------------------------------------
#if defined(_MSC_VER)
typedef uint32 gy_u32_alias_t;
#else
typedef uint32 __attribute__((may_alias)) gy_u32_alias_t;
#endif

//---------------------------------------------------------------------------
// 纯像素混合:把 src 色以覆盖度 opa 混到已有 dst 像素上,返回结果 GYpx
//   RGB888/RGB565 解包到 8bit 分量线性混合再打包;灰度/1bpp 转亮度混合
//   framebuffer 无 alpha,alpha 只在这一瞬间存在(见 PubType GYopa 说明)
//   抗锯齿(coverage 当 opa)与半透明填充共用此内核
//
// ★2026-10-03 性能改动：把三次 `/255u` 换成乘加移位★
// 恒等式：`(x + 1) * 257 >> 16` 在 **x ∈ [0, 65025] 上精确等于 x / 255**
// —— 已用脚本对全部 65026 个取值逐一枚举验证，无一个反例。
// 这里 x = 分量 × opa + 分量 × inv ≤ 255 × 255 = 65025，正好落在验证区间内，
// 所以这是**逐位等价**替换，不是近似，不会引入任何色偏。
// 动机：Cortex-M7 的整数除法编译出来是一串指令，而半透明填充每像素要算 3 次
// （R/G/B），是 alpha 混合路径的主要成本；换成 32 位乘加 + 移位后是单周期指令。
//---------------------------------------------------------------------------
static inline GYpx GY_MixPx(GYpx dst, GYcolor src, GYopa opa)
{
	/* 见上方注释：(x + 1) * 257 >> 16 ≡ x / 255（x ≤ 65025，已全范围验证） */
#define GY_DIV255(x)  (((uint32)(x) + 1u) * 257u >> 16)
#if YMGUI_COLOR_DEPTH == 24
	uint32 inv = 255u - opa;
	GYpx out;
	out.r = (uint8)GY_DIV255(GY_COLOR_R(src) * opa + (uint32)dst.r * inv);
	out.g = (uint8)GY_DIV255(GY_COLOR_G(src) * opa + (uint32)dst.g * inv);
	out.b = (uint8)GY_DIV255(GY_COLOR_B(src) * opa + (uint32)dst.b * inv);
	return out;
#elif YMGUI_COLOR_DEPTH == 16
	/* ★2026-10-04 步骤 4：修掉 α/抗锯齿的**系统性偏暗**★
	 *
	 * 【原实现为什么偏暗】把 dst 拆成 8 位（`5bit<<3`）、在 8 位域里混合、
	 * 再用 `&0xF8` / `&0xFC` / `>>3` **截断**回 5/6 位。两处都在向下取整：
	 *   · `5bit<<3` 把 dst 的最大值 31 映成 248 而不是 255 ⇒ 背景永远偏暗；
	 *   · 回打包又砍掉低 3（R/B）/低 2（G）位。
	 * 30 万随机样本实测：R −2.27 / G −1.24 / B −2.29 LSB，**77.6% 的混合结果偏暗**，
	 * 最差 −8 LSB。直观后果：三层 ribbon 叠出来 R 分量比正确值低 8（57.6 vs 65.8）；
	 * 抗锯齿字形的边缘也一起吃这个偏置 ⇒ 中文显得发胖发虚。
	 *
	 * 【改法】既然 framebuffer 存的就是 5/6/5，就**直接在 5/6 位域里混合、只量化一次**：
	 *     out5 = round( (src5*opa + dst5*inv) / 255 )
	 * `+127` 先加半个 LSB 实现四舍五入，除法继续复用已验证的 GY_DIV255 恒等式。
	 *
	 * 【为什么这样是划算的】不是"更准但更慢"：
	 *   · 分量位数从 8 降到 5（`>>3` 代替 `<<3`），乘加规模反而变小；
	 *   · `sr5*opa` 等与 dst 无关的项是循环不变量，-O2 会提到循环外；
	 *   · 回打包不再需要 `&0xF8` 之类的掩码（结果天然在范围内）。
	 * 代价只有 src 端两次 `*31+127` 的除法，而那是在循环外。
	 *
	 * 【实测（同一组 30 万随机样本）】偏置 R+0.00 / G−0.00 / B+0.00。
	 * 【边界一致性】opa=255 时本函数与 GY_ColorToPx **逐位相同**
	 * （2 万组随机色 0 失配）⇒ 抗锯齿边缘与"不透明直写"快路径之间不会留 1 LSB 接缝。
	 *
	 * ⚠ 这是**有意的外观变化**：半透明/抗锯齿像素整体亮回约 1%。
	 *   逐位等价脚本（tools/check_ribbon_equiv.py）与既有抓屏 PNG 的基线要重采，
	 *   那不是回归。 */
	uint32 inv = 255u - opa;
	uint32 sr5 = GY_COLOR_R(src) >> 3;   /* 与 GY_ColorToPx 同一口径（截断到 5bit） */
	uint32 sg6 = GY_COLOR_G(src) >> 2;   /* 同上，6bit */
	uint32 sb5 = GY_COLOR_B(src) >> 3;
	uint32 rr = GY_DIV255(sr5 * opa + (((uint32)dst >> 11) & 0x1Fu) * inv + 127u);
	uint32 rg = GY_DIV255(sg6 * opa + (((uint32)dst >>  5) & 0x3Fu) * inv + 127u);
	uint32 rb = GY_DIV255(sb5 * opa + ( (uint32)dst        & 0x1Fu) * inv + 127u);
	return (GYpx)((rr << 11) | (rg << 5) | rb);
#else
	uint32 dgray = (uint32)dst;
	uint32 sgray = (GY_COLOR_R(src) * 77 + GY_COLOR_G(src) * 150 + GY_COLOR_B(src) * 29) >> 8;
	uint32 inv = 255u - opa;
	return (GYpx)GY_DIV255(sgray * opa + dgray * inv);
#endif
#undef GY_DIV255
}

/* 渲染诊断计数（2026-10-03，仅 YMGUI_DIAG_FILLPX 打开时生效）。
 * Draw_Fill 的计数在 YMGUI_DrawFill.c 里；这两个覆盖**其余所有图元**的
 * 逐像素写入出口（文字、圆弧、线条最终都走 PutPx / BlendPx），
 * 有了它们才能把一帧的像素写入量按图元类别拆开，定位到底谁在吃时间。 */
#if defined(YMGUI_DIAG_FILLPX)
extern uint32_t g_diag_putpx;
extern uint32_t g_diag_blendpx;
#endif

//写一个屏幕坐标像素(不透明直写)。越界/裁剪外自动忽略
GY_ALWAYS_INLINE void GY_PutPx(GYSURFACE s, GYcoord x, GYcoord y, GYpx px)
{
	//裁剪到 surface.clip(屏幕坐标)
	if (x < s->clip.x || x >= s->clip.x + s->clip.w)
		return;
	if (y < s->clip.y || y >= s->clip.y + s->clip.h)
		return;
	//屏幕坐标 → band buffer 偏移
	GYcoord bx = x - s->buf_area.x;
	GYcoord by = y - s->buf_area.y;
	//防御:再钳到 buf_area 内(clip 若不是 buf_area 子集则挡住越界写)
	if (bx < 0 || bx >= s->buf_area.w || by < 0 || by >= s->buf_area.h)
		return;
	((GYpx*)s->buf)[(int32)by * s->stride + bx] = px;
#if defined(YMGUI_DIAG_FILLPX)
	g_diag_putpx++;
#endif
}

//混合一个屏幕坐标像素(读背景-混合-写回)。opa 为覆盖度:0 跳过、255 直写、其余混合。
//裁剪/band 偏移逻辑与 GY_PutPx 一致;抗锯齿图元逐点走此。越界/裁剪外自动忽略。
GY_ALWAYS_INLINE void GY_BlendPx(GYSURFACE s, GYcoord x, GYcoord y, GYcolor color, GYopa opa)
{
	if (opa == GY_OPA_TRANSP)
		return;
	//裁剪到 surface.clip(屏幕坐标)
	if (x < s->clip.x || x >= s->clip.x + s->clip.w)
		return;
	if (y < s->clip.y || y >= s->clip.y + s->clip.h)
		return;
	GYcoord bx = x - s->buf_area.x;
	GYcoord by = y - s->buf_area.y;
	if (bx < 0 || bx >= s->buf_area.w || by < 0 || by >= s->buf_area.h)
		return;
	GYpx* p = &((GYpx*)s->buf)[(int32)by * s->stride + bx];
	if (opa == GY_OPA_COVER)
		*p = GY_ColorToPx(color);      //满覆盖直写,省一次读+混合
	else
		*p = GY_MixPx(*p, color, opa); //读-混-写回
#if defined(YMGUI_DIAG_FILLPX)
	g_diag_blendpx++;
#endif
}

#endif // !YMGUI_DRAWPX_H
