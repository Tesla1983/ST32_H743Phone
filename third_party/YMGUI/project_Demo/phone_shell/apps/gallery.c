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
	GYOBJ btn_del;          /* 删除按钮（两段式二次确认） */
	GYOBJ btn_clear;        /* 清空图库按钮（两段式二次确认） */
	GYOBJ hint;             /* 图片下方那行提示（复用它显示"再点一次…"） */
	int gallery_index;
	int gallery_total;
	int del_arm;            /* 1 = 删除已按过一次，等二次确认 */
	int clear_arm;          /* 1 = 清空已按过一次，等二次确认 */
} AppState;
static AppState state;

static void on_gallery(GYOBJ btn);
static void on_delete(GYOBJ btn);
static void on_clear(GYOBJ btn);
static void gallery_refresh_state(void);   /* 按 QSPI 真实图库状态刷新 UI（首建 + 每次打开共用） */
static void app_show(const char* argument); /* PhoneApp.show：打开相册时刷新计数，避免陈旧计数 */

/* 删除按钮的两种外观（二次确认用：点一次变红并改字，再点才真删） */
#define DEL_BTN_IDLE  RGB(147, 112, 136)
#define DEL_BTN_ARMED RGB(200, 76, 76)

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

/* 把 0..9999 的十进制无前导零（但 0 输出 "0"）写进 p，返回末尾指针。 */
static char* put_uint(char* p, int v)
{
	int d = 1, t = v;
	if (v == 0) { *p++ = '0'; return p; }
	while (t >= 10) { d *= 10; t /= 10; }
	while (d > 0) { *p++ = (char)('0' + v / d); v %= d; d /= 10; }
	return p;
}

static void refresh_caption(void)
{
	static char buf[64];
	if (state.gallery_total > 0)
	{
		/* 手工拼：本工程不引入 snprintf（省体积，也避免 newlib 的堆依赖）。
		 * 中文必须整段写成字符串常量再 memcpy，绝不能逐字节写 `*p++ = '卡';`，
		 * 否则多字节字符常量会被截断成非法字节（2026-10-08 实测）。
		 *
		 * ⚠ 不再缀「 张卡上图片」（2026-10-11 上板实测）：caption 标签 276 px、
		 *   title 字体（数字步进 17 / 空格 8），原串 "1 / 4 张卡上图片 · 剩1772KB"
		 *   文本实测 ~294 px 超出标签 ⇒ 末尾单位 "B" 被**整字裁掉**，屏幕上只剩
		 *   「剩1772K」。去掉 5 个汉字后 ~196 px，即便索引/总数都是 2 位也远 <276。 */
		char* p = buf;
		int   n = state.gallery_index + 1;
		if (n >= 10) { *p++ = (char)('0' + n / 10); }
		*p++ = (char)('0' + (n % 10));
		*p++ = ' '; *p++ = '/'; *p++ = ' ';
		n = state.gallery_total;
		if (n >= 10) { *p++ = (char)('0' + n / 10); }
		*p++ = (char)('0' + (n % 10));
		*p = 0;                                  /* 先收尾：容量段取不到时也不留野字节 */

		{
			int fr = BoardGallery_FreeBytes();
			if (fr >= 0)
			{
				if (fr < 1024)
				{
					/* 中文走字符串常量 + memcpy，安全 */
					static const char full[] = " · 已满";
					memcpy(p, full, sizeof(full));
				}
				else
				{
					static const char sep[] = " · 剩";
					memcpy(p, sep, sizeof(sep)); p += sizeof(sep) - 1;
					p = put_uint(p, fr / 1024);
					*p++ = 'K'; *p++ = 'B'; *p = 0;
				}
			}
		}
		PhoneUI_text_set(state.gallery_caption, buf);
	}
}

/* 跳到 from 及其之后的第一个有效槽位；一个都没有就置 0。 */
static void next_of(int from)
{
	int s = BoardGallery_Next(from);
	state.gallery_index = (s < 0) ? 0 : s;
}

/* 把删除 / 清空两个按钮都恢复成常态（解除二次确认）。切图、删完、清空完、删失败都要调它。 */
static void delete_disarm(void)
{
	if (state.del_arm == 0 && state.clear_arm == 0) { return; }
	state.del_arm = 0;
	state.clear_arm = 0;
	PhoneUI_button_set(state.btn_del, "删除");
	YMGUI_Obj_SetBgColor(state.btn_del, DEL_BTN_IDLE);
	PhoneUI_button_set(state.btn_clear, "清空");
	YMGUI_Obj_SetBgColor(state.btn_clear, DEL_BTN_IDLE);
	PhoneUI_text_set(state.hint, "示例风景 / Yaomi 相册");
}

static void on_delete(GYOBJ btn)
{
	(void)btn;
	int next = -1;
	int rc;

	if (state.gallery_total <= 0) { return; }   /* 示例图模式：没有可删的真图 */

	/* ---- 第一次按：只进入确认态，不动数据（同时解除清空确认态）---- */
	if (state.del_arm == 0)
	{
		state.del_arm = 1;
		state.clear_arm = 0;
		PhoneUI_button_set(state.btn_clear, "清空");
		YMGUI_Obj_SetBgColor(state.btn_clear, DEL_BTN_IDLE);
		PhoneUI_button_set(state.btn_del, "确认删除?");
		YMGUI_Obj_SetBgColor(state.btn_del, DEL_BTN_ARMED);
		PhoneUI_text_set(state.hint, "再点一次删除当前这张");
		return;
	}

	/* ---- 第二次按：真删 ----
	 * ⚠ 必须用 BoardGallery_Delete 回传的 next 当新下标，不能沿用旧下标：
	 *   槽位号是物理槽号，删掉中间一张后索引会出现空洞。 */
	rc = BoardGallery_Delete(state.gallery_index, &next);
	state.del_arm = 0;
	PhoneUI_button_set(state.btn_del, "删除");
	YMGUI_Obj_SetBgColor(state.btn_del, DEL_BTN_IDLE);

	if (rc != 0)
	{
		PhoneUI_text_set(state.hint, rc == 1 ? "图库忙，稍后再试" : "删除失败");
		return;
	}

	state.gallery_total = BoardGallery_Count();
	if (next < 0 || state.gallery_total <= 0)
	{
		/* 删空了：退回原来的示例色块 */
		state.gallery_total = 0;
		state.gallery_index = 0;
		state.gallery_panel->draw_cb = PhoneUI_gallery_draw;
		PhoneUI_text_set(state.gallery_caption, "01 / 珊瑚");
		PhoneUI_text_set(state.hint, "示例风景 / Yaomi 相册");
	}
	else
	{
		state.gallery_index = next;
		refresh_caption();
		PhoneUI_text_set(state.hint, "示例风景 / Yaomi 相册");
	}
	YMGUI_Obj_Invalidate(state.gallery_panel);
}

static void on_clear(GYOBJ btn)
{
	(void)btn;
	int rc;

	if (state.gallery_total <= 0) { return; }   /* 示例图模式：没有可清的真图 */

	/* ---- 第一次按：只进入确认态，不动数据（同时解除删除确认态）---- */
	if (state.clear_arm == 0)
	{
		state.clear_arm = 1;
		state.del_arm = 0;
		PhoneUI_button_set(state.btn_del, "删除");
		YMGUI_Obj_SetBgColor(state.btn_del, DEL_BTN_IDLE);
		PhoneUI_button_set(state.btn_clear, "确认清空?");
		YMGUI_Obj_SetBgColor(state.btn_clear, DEL_BTN_ARMED);
		PhoneUI_text_set(state.hint, "再点一次清空全部图片");
		return;
	}

	/* ---- 第二次按：真清（擦索引扇区，数据区不擦）---- */
	rc = BoardGallery_Clear();
	state.clear_arm = 0;
	PhoneUI_button_set(state.btn_clear, "清空");
	YMGUI_Obj_SetBgColor(state.btn_clear, DEL_BTN_IDLE);

	if (rc != 0)
	{
		PhoneUI_text_set(state.hint, rc == 1 ? "图库忙，稍后再试" : "清空失败");
		return;
	}

	/* 清空成功：退回示例态（与删空同一套） */
	state.gallery_total = 0;
	state.gallery_index = 0;
	state.gallery_panel->draw_cb = PhoneUI_gallery_draw;
	PhoneUI_text_set(state.gallery_caption, "01 / 珊瑚");
	PhoneUI_text_set(state.hint, "示例风景 / Yaomi 相册");
	YMGUI_Obj_Invalidate(state.gallery_panel);
}

static void on_gallery(GYOBJ btn)
{
	(void)btn;
	state.gallery_total = BoardGallery_Count();

	delete_disarm();                    /* 切图 = 放弃删除 / 清空，顺手解除确认态 */

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

	/* ⚠ 不能用 (index+1) % total：删过图之后槽位会有空洞，取模会落在空槽上
	 *   （表现为画面空白）。交给 BoardGallery_Next 跳过空洞。 */
	next_of(state.gallery_index + 1);
	refresh_caption();
	YMGUI_Obj_Invalidate(state.gallery_panel);
}

/* 按 QSPI 真实图库状态刷新 UI（首建 + 每次打开共用）。
 * ⚠ 必须每次打开都重算：打开 App 只显隐 view，.create 只在开机跑一次，
 *   若在 .create 算一次就完事，计数会陈旧，使 on_clear/on_delete 的
 *   `gallery_total<=0` 早退 —— 清空/删除功能直接失效。 */
static void gallery_refresh_state(void)
{
	delete_disarm();                            /* 复位两段式确认态 */
	state.gallery_total = BoardGallery_Count();
	state.gallery_index = 0;

	if (state.gallery_panel)
		state.gallery_panel->draw_cb = (state.gallery_total > 0)
		                               ? gallery_draw_photo
		                               : PhoneUI_gallery_draw;

	if (state.gallery_total > 0)
		refresh_caption();
	else
		PhoneUI_text_set(state.gallery_caption, "01 / 珊瑚");

	YMGUI_Obj_Invalidate(state.gallery_panel);
}

/* PhoneApp.show：打开相册时刷新计数（避免陈旧计数阻塞清空/删除） */
static void app_show(const char* argument)
{
	(void)argument;
	gallery_refresh_state();
}

static void app_create(GYOBJ view)
{
	state = (AppState){0};
	PhoneUI_app_header(view, "相册", "收藏安静的风景");

	state.gallery_panel = PhoneUI_panel(view, 18, 65, 284, 230, RGB(196, 105, 113));
	/* 默认给矢量日落画，刷新时再按真实计数决定是否切到 XIP 取像素的回调 */
	state.gallery_panel->draw_cb = PhoneUI_gallery_draw;
	state.gallery_panel->event_cb = NULL;
	state.gallery_caption = PhoneUI_left_label(view, 22, 307, 276, "01 / 珊瑚", INK, 3);
	state.hint = PhoneUI_left_label(view, 23, 345, 275, "示例风景 / Yaomi 相册", MUTED, 2);
	/* 底部一行拆成三个：左"下一张"、中"删除"、右"清空"
	 * （各 88 宽 + 10 间距 ×2：18→106→116→204→214→302，正好铺满 284） */
	PhoneUI_button(view, 18, 373, 88, 34, "下一张", on_gallery, RGB(147, 112, 136));
	state.btn_del = PhoneUI_button(view, 116, 373, 88, 34, "删除", on_delete, DEL_BTN_IDLE);
	state.btn_clear = PhoneUI_button(view, 214, 373, 88, 34, "清空", on_clear, DEL_BTN_IDLE);

	gallery_refresh_state();                     /* 按真实计数刷新面板绘制回调 / 下标 / 计数 */
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
	.key = "gallery", .title = "相册", .color = RGB(239, 165, 72), .icon = ICON_GALLERY, .create = app_create, .show = app_show, .inspect = app_inspect, .destroy = app_destroy};
