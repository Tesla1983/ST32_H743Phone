#ifndef PHONE_UI_H
#define PHONE_UI_H

#include "YMGUI_PubDefine.h"
#include "YMGUI_Hal.h"
#include "YMGUI_Mem.h"
#include "YMGUI_Obj.h"
#include "YMGUI_Invalidate.h"
#include "YMGUI_DrawFill.h"
#include "YMGUI_DrawArc.h"
#include "YMGUI_DrawLine.h"
#include "YMGUI_Button.h"
#include "YMGUI_Bar.h"
#include "YMGUI_Anim.h"
#include "YMGUI_Checkbox.h"
#include "YMGUI_Switch.h"
#include "YMGUI_Slider.h"
#include "YMGUI_DrawImg.h"
#include "YMGUI_DrawPx.h"
#include "YMGUI_Event.h"
#include "YMGUI_Geom.h"
#include "phone_font.h"
#include "YMGUI_TextView.h"
#include "YMGUI_Joystick.h"
#include "YMGUI_MsgBox.h"
#define PHONE_IME_AUTO_INPUTS
#include "phone_ime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 320
#define H 480

/* 快捷开关面板的背景截屏（"拉下通知栏时看到的那张当前画面"）。
 *
 * 全尺寸 320×480 native 像素 = 300 KB；小 RAM 目标申请不到时（本板给 YMGUI 的
 * 大对象池 heap1 只有 212 KB）退到 1/4 线性降采样：80×120 = 19.2 KB。
 * 显示端 preview_image 是 160×240 的 GY_IMG_FIT 图片控件，本来就在缩放显示，
 * 所以 1/4 源图放大上去观感几乎无差 —— 这正是"内存不足就当不了背景"的替代方案。
 *
 * 注意：这是**降级路径**，不是新契约。申请得到全尺寸就走全尺寸（桌面/自检走那条，
 * 逐像素精确的断言因此仍然成立）；两条路的比例必须让抓屏侧也知道，
 * 由 PhoneHost_SetCaptureShift() 传递（见 phone_shade.c 的用法）。 */
#define PHONE_BACKDROP_SHIFT 2
#define PHONE_BACKDROP_W     (W >> PHONE_BACKDROP_SHIFT)
#define PHONE_BACKDROP_H     (H >> PHONE_BACKDROP_SHIFT)

#define RGB(r, g, b) GY_ARGB(0xFF, (r), (g), (b))
#define INK RGB(33, 40, 57)
#define MUTED RGB(121, 128, 145)
#define PAPER RGB(246, 247, 251)
#define WHITE RGB(255, 255, 255)
#define ACCENT RGB(91, 103, 222)
typedef struct
{
	char value[160];
	GYcolor color;
	uint8 large, left;
} PhoneText;
typedef struct
{
	char value[96];
	void (*clicked)(GYOBJ);
	uint8 icon;
} PhoneButton;
enum
{
	ICON_NONE,
	ICON_TASK,
	ICON_MUSIC,
	ICON_GALLERY,
	ICON_SETTINGS,
	ICON_CLOCK,
	ICON_WEATHER,
	ICON_NOTES,
	ICON_CALCULATOR,
	ICON_PHONE,
	ICON_MESSAGES,
	ICON_CONTACTS,
	ICON_RECORDER,
	ICON_FILES,
	ICON_GYROSCOPE,
	ICON_CAMERA,
	ICON_BROWSER,
	ICON_HOME,
	ICON_RECENTS,
	ICON_POWER,
	ICON_BACK,
	ICON_PLAY,
	ICON_PAUSE,
	ICON_NEXT,
	ICON_PREVIOUS
};

void PhoneUI_rounded(GYSURFACE surface, const GYrect* area, GYcolor color, int radius, GYopa opa);
void PhoneUI_glass_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
int PhoneUI_text_width(const char* value, uint8 large);
void PhoneUI_draw_text(GYSURFACE surface, const char* value, GYcolor color,
					   uint8 large, int cx, int top);
void PhoneUI_text_set(GYOBJ obj, const char* value);
GYOBJ PhoneUI_label_ex(GYOBJ parent, int x, int y, int w, const char* value,
					   GYcolor color, uint8 large);
GYOBJ PhoneUI_label(GYOBJ parent, int x, int y, int w, const char* value, GYcolor color);
/* 计算器：SWD 可触发的格式化探针（写 g_calc_probe_v → g_calc_probe=1 → 读 g_calc_probe_text）。 */
void PhoneCalc_BoardProbe(void);
GYOBJ PhoneUI_left_label(GYOBJ parent, int x, int y, int w, const char* value, GYcolor color, uint8 size);
void PhoneUI_button_set(GYOBJ obj, const char* value);
void PhoneUI_button_icon(GYOBJ obj, uint8 icon);
void PhoneUI_icon_draw(GYSURFACE surface, uint8 icon, int cx, int cy, GYcolor tint);
void PhoneUI_icon_tile_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
void PhoneUI_button_event(GYOBJ obj, GYEvent event);
GYOBJ PhoneUI_button(GYOBJ parent, int x, int y, int w, int h,
					 const char* value, GYbtn_clicked_cb cb, GYcolor color);
void PhoneUI_set_pos(GYOBJ obj, GYcoord x, GYcoord y);
void PhoneUI_move(GYOBJ obj, GYcoord x, GYcoord y, uint32 duration);
void PhoneUI_gallery_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
void PhoneUI_switch_draw(GYOBJ obj, GYSURFACE s, const GYrect* a);
void PhoneUI_slider_draw(GYOBJ obj, GYSURFACE s, const GYrect* a);
GYOBJ PhoneUI_panel(GYOBJ parent, int x, int y, int w, int h, GYcolor color);
void PhoneUI_weather_draw(GYOBJ obj, GYSURFACE s, const GYrect* a);
void PhoneUI_app_header(GYOBJ view, const char* title, const char* subtitle);
void PhoneUI_phone_input_draw(GYOBJ obj, GYSURFACE s, const GYrect* a);
GYOBJ PhoneUI_app_text(GYOBJ parent, int x, int y, int w, int h, const char* value);
GYOBJ PhoneUI_app_input(GYOBJ view, int x, int y, int w, const char* value, size_t cap);
void PhoneUI_animate_bar(GYOBJ bar, int value, uint32 duration);

#endif
