#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"
#include "YMGUI_DrawImg.h"
#include "YMGUI_Invalidate.h"

/* 相册（2026-10-08 改造）
 *
 * 原来这里只有 3 个纯色块 + 一幅矢量日落画，**没有任何真实图片**。
 * 现在：图库里有图（W25Q128 上，由「TF 卡 BMP → RGB565 → 灌库」得来）就显示真图，
 * 没有就退回原来的色块 —— 卡没插、图库为空都不会变成空白页。
 *
 * 【为什么不占 RAM】BoardGallery_Pixels 返回的是 **QSPI 的 XIP 地址**，
 * 而 YMGUI_DrawImg.h 里 `GYimg.data` 是 `const GYpx*`（只读指针）⇒ 图像数据
 * **整幅留在 flash 里**，显示时不进 RAM。面板 284×230 全尺寸 RGB565 = 130 640 B
 * 本来是放不进 heap1 余量（115 KB）的，靠这条路才放得下。
 */

typedef struct
{
	GYOBJ gallery_panel, gallery_caption;
	int gallery_index;
	int gallery_total;
} AppState;
static AppState state;

static void on_gallery(GYOBJ btn);

/* ---- 有真图时的面板绘制 ---- */
static void gallery_draw_photo(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	/* 裁剪处理与 PhoneUI_gallery_draw 完全一致（照抄那段）——
	 * 面板带 20 px 圆角，必须先把 surface 的 clip 收进 area 与clip 的交集。 */
	GYsurface clipped = *surface;
	int x0 = clipped.clip.x > area->x ? clipped.clip.x : area->x;
	int y0 = clipped.clip.y > area->y ? clipped.clip.y : area->y;
	int x1 = clipped.clip.x + clipped.clip.w < area->x + area->w ? clipped.clip.x + clipped.clip.w : area->x + area->w;
	int y1 = clipped.clip.y + clipped.clip.h < area->y + area->h ? clipped.clip.y + clipped.clip.h : area->y + area->h;
	const GYpx* px;

	if (x1 <= x0 || y1 <= y0)
		return;
	clipped.clip = (GYrect){(GYcoord)x0, (GYcoord)y0, (GYcoord)(x1 - x0), (GYcoord)(y1 - y0)};

	/* 底色先铺一层（图片没盖住的地方不至于露出上一帧） */
	PhoneUI_rounded(&clipped, area, obj->bg_color, 20, GY_OPA_COVER);

	px = (const GYpx*)BoardGallery_Pixels(state.gallery_index);
	if (px)
	{
		unsigned int off = 0, len = 0, w = 0, hh = 0;
		if (BoardGallery_Info(state.gallery_index, &off, &len, &w, &hh) == 0 && w > 0 && hh > 0)
		{
			GYimg  img = {px, (GYcoord)w, (GYcoord)hh, 0, 0};
			GYrect dst = *area;          /* 拉伸铺满面板（最近邻，Draw_ImgScaled 的行为） */
			YMGUI_Draw_ImgScaled(&clipped, &img, dst);
			return;
		}
	}
	/* 取不到像素（例如导入中）：保持底色，不画花屏 */
}

static void refresh_caption(void)
{
	static char buf[32];
	if (state.gallery_total > 0)
	{
		/* 手工拼：本工程不引入 snprintf（省体积，也避免 newlib 的堆依赖）。
		 *
		 * ⚠ 中文部分**必须**整段写成字符串常量再 memcpy，绝不能逐字节写
		 *   `*p++ = '卡';` —— 多字节字符常量（UTF-8 的"卡"= E5 8D A1）赋给 char
		 *   只会留下**最低一个字节**（0xA1），把 UTF-8 序列截断成非法字节，
		 *   YMGUI 的文本控件渲染出来就是残缺/空白。
		 *   2026-10-08 实测踩到：相册页只显示出 "1 / 1"，"卡上图片"四个字整段不见。 */
		static const char suffix[] = " 张卡上图片";
		char* p = buf;
		int   n = state.gallery_index + 1;
		if (n >= 10) { *p++ = (char)('0' + n / 10); }
		*p++ = (char)('0' + (n % 10));
		*p++ = ' '; *p++ = '/'; *p++ = ' ';
		n = state.gallery_total;
		if (n >= 10) { *p++ = (char)('0' + n / 10); }
		*p++ = (char)('0' + (n % 10));
		memcpy(p, suffix, sizeof(suffix));       /* 含结尾 '\0' */
		PhoneUI_text_set(state.gallery_caption, buf);
	}
}

static void on_gallery(GYOBJ btn)
{
	(void)btn;
	state.gallery_total = BoardGallery_Count();

	if (state.gallery_total <= 0)
	{
		/* 降级：图库为空（卡没插 / 还没导入）时退回原来的三维色块轮换 */
		state.gallery_index = (state.gallery_index + 1) % 3;
		static const GYcolor colors[3] = {RGB(196, 105, 113), RGB(83, 155, 173), RGB(149, 128, 204)};
		static const char* names[3] = {"01 / 珊瑚", "02 / 海岸", "03 / 暮色"};
		PhoneUI_text_set(state.gallery_caption, names[state.gallery_index]);
#if YMGUI_ANIM
		if (YMGUI_Anim_BgColorTo(state.gallery_panel, colors[state.gallery_index], 350, GY_ANIM_SMOOTH))
			return;
#endif
		YMGUI_Obj_SetBgColor(state.gallery_panel, colors[state.gallery_index]);
		return;
	}

	state.gallery_index = (state.gallery_index + 1) % state.gallery_total;
	refresh_caption();
	YMGUI_Obj_Invalidate(state.gallery_panel);
}

static void app_create(GYOBJ view)
{
	state = (AppState){0};
	PhoneUI_app_header(view, "相册", "收藏安静的风景");
	state.gallery_total = BoardGallery_Count();

	state.gallery_panel = PhoneUI_panel(view, 18, 65, 284, 230, RGB(196, 105, 113));
	/* 有真图就用自己的绘制回调（从 XIP 取像素）；没有就用原来的矢量日落画 */
	state.gallery_panel->draw_cb = (state.gallery_total > 0)
	                               ? gallery_draw_photo
	                               : PhoneUI_gallery_draw;
	state.gallery_panel->event_cb = NULL;
	state.gallery_caption = PhoneUI_left_label(view, 22, 307, 276, "01 / 珊瑚", INK, 3);
	PhoneUI_left_label(view, 23, 345, 275, "示例风景 / Yaomi 相册", MUTED, 2);
	PhoneUI_button(view, 18, 373, 284, 34, "下一张风景   >", on_gallery, RGB(147, 112, 136));

	if (state.gallery_total > 0)
	{
		state.gallery_index = 0;
		refresh_caption();
	}
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "gallery_panel"))
		return (intptr_t)state.gallery_panel;
	if (!strcmp(name, "gallery_caption"))
		return (intptr_t)state.gallery_caption;
	if (!strcmp(name, "gallery_index"))
		return (intptr_t)state.gallery_index;
	if (!strcmp(name, "gallery_total"))
		return (intptr_t)state.gallery_total;
	return 0;
}

const PhoneApp PhoneApp_gallery = {
	.key = "gallery", .title = "相册", .color = RGB(239, 165, 72), .icon = ICON_GALLERY, .create = app_create, .inspect = app_inspect, .destroy = app_destroy};
