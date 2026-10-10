#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shade.h"
#include "phone_quick.h"
#include "phone_quick_builtin.h"
#include "phone_shell_board.h"   /* BoardNet_*：WiFi/蓝牙无线电开关（2026-10-10） */
enum
{
	WIFI,
	DATA,
	FLIGHT,
	BLUETOOTH,
	HOTSPOT,
	SOUND,
	CAPTURE,
	QUIET
};
/* SOUND 表示静音已开启；默认正常响铃。
 *
 * ⚠ 2026-10-10：WIFI / BLUETOOTH 两项**不再用这里的本地假开关** ——
 *   WiFi 射频与蓝牙射频都在对端 ESP32 上，下拉抽屉拨开关必须真的下发命令
 *   （$?RADIO,<WIFI|BT>,<ON|OFF>）并以下发回来的 $RD 帧为准。
 *   于是这两项的"当前状态"改读 BoardNet_RadioWifiOn() / RadioBtOn()（见
 *   builtin_active），options[] 里对应两位只作历史残留、不再参与显示与逻辑。 */
static int options[8] = {1, 1, 0, 0, 0, 0, 0, 0};
static const char* keys[] = {"wifi", "data", "flight", "bluetooth", "hotspot", "sound", "capture", "quiet"};

/* 统一取"这一项当前是不是开的"。
 * ⚠ WIFI/BLUETOOTH 读**真实无线电状态**（$RD 回流，-1=还没同步到 ⇒ 当关处理）；
 *   其余项仍是本地联动状态。 */
static int builtin_active(int id)
{
	if (id == WIFI)
		return BoardNet_RadioWifiOn() > 0 ? 1 : 0;
	if (id == BLUETOOTH)
		return BoardNet_RadioBtOn() > 0 ? 1 : 0;
	return options[id];
}

int PhoneQuickBuiltin_State(const char* key)
{
	for (int i = 0; i < 8; ++i)
		if (!strcmp(key, keys[i]))
			return builtin_active(i);
	return 0;
}
void PhoneQuickBuiltin_SetWifi(int on)
{
	options[WIFI] = !!on;
}
static void toggle(int id)
{
	/* 真无线电开关：发命令即可，**不信本地缓存** ——
	 * 开关态由对端回的 $RD 刷新（设置页与抽屉的显示都走 BoardNet_Radio*On）。
	 * 发出后对端若因丢帧没收到，src/uart_link.c 的命令重试会补发。 */
	if (id == WIFI)
	{
		BoardNet_SetWifi(BoardNet_RadioWifiOn() > 0 ? 0 : 1);
		return;
	}
	if (id == BLUETOOTH)
	{
		BoardNet_SetBt(BoardNet_RadioBtOn() > 0 ? 0 : 1);
		return;
	}
	options[id] = !options[id];
	if (id == FLIGHT && options[id])
		options[DATA] = options[HOTSPOT] = 0;
	if ((id == DATA || id == HOTSPOT) && options[id])
		options[FLIGHT] = 0;
}
static void draw(int id, GYSURFACE s, int x, int y, GYcolor bg)
{
	if (id == WIFI || id == HOTSPOT)
	{
		YMGUI_Draw_ArcThick(s, x, y + 6, 10, 12, 220, 320, WHITE);
		YMGUI_Draw_ArcThick(s, x, y + 6, 5, 7, 220, 320, WHITE);
		YMGUI_Draw_CircleFill(s, x, y + 6, 2, WHITE);
		if (id == HOTSPOT)
			YMGUI_Draw_CircleFill(s, x, y - 9, 2, WHITE);
	}
	else if (id == CAPTURE)
		PhoneUI_icon_draw(s, ICON_CAMERA, x, y, bg);
	else if (id == SOUND)
	{
		GYrect speaker = {x - 10, y - 4, 6, 9};
		YMGUI_Draw_Fill(s, &speaker, WHITE, 255);
		YMGUI_Draw_Line(s, x - 4, y - 4, x + 1, y - 9, WHITE);
		YMGUI_Draw_Line(s, x + 1, y - 9, x + 1, y + 9, WHITE);
		YMGUI_Draw_Line(s, x + 1, y + 9, x - 4, y + 4, WHITE);
		if (options[id])
		{
			YMGUI_Draw_Line(s, x + 6, y - 4, x + 13, y + 4, WHITE);
			YMGUI_Draw_Line(s, x + 6, y + 4, x + 13, y - 4, WHITE);
		}
		else
			YMGUI_Draw_ArcThick(s, x + 1, y, 8, 10, 310, 410, WHITE);
	}
	else if (id == QUIET)
		PhoneUI_icon_draw(s, ICON_CLOCK, x, y, bg);
	else if (id == FLIGHT)
	{
		YMGUI_Draw_Line(s, x, y - 12, x, y + 11, WHITE);
		YMGUI_Draw_Line(s, x - 11, y + 3, x, y - 3, WHITE);
		YMGUI_Draw_Line(s, x, y - 3, x + 11, y + 3, WHITE);
		YMGUI_Draw_Line(s, x - 5, y + 11, x + 5, y + 11, WHITE);
	}
	else if (id == DATA)
	{
		YMGUI_Draw_Line(s, x - 5, y - 10, x - 5, y + 10, WHITE);
		YMGUI_Draw_Line(s, x - 10, y - 5, x - 5, y - 10, WHITE);
		YMGUI_Draw_Line(s, x + 5, y - 10, x + 5, y + 10, WHITE);
		YMGUI_Draw_Line(s, x + 5, y + 10, x + 10, y + 5, WHITE);
	}
	else
	{
		YMGUI_Draw_Line(s, x, y - 12, x, y + 12, WHITE);
		YMGUI_Draw_Line(s, x, y - 12, x + 8, y - 5, WHITE);
		YMGUI_Draw_Line(s, x + 8, y - 5, x - 7, y + 7, WHITE);
		YMGUI_Draw_Line(s, x - 7, y - 7, x + 8, y + 5, WHITE);
		YMGUI_Draw_Line(s, x + 8, y + 5, x, y + 12, WHITE);
	}
}
/* 每项是独立描述符：联动设备状态属于内置实现，不进入面板分发分支。
 * ⚠ active 必须走 builtin_active(id) 这个统一判据 —— 直接用 options[id]
 *   会让 WiFi/蓝牙瓦片显示本地假状态、拨了不动（2026-10-10 改真开关时的坑）。 */
#define FEATURE(name, title_, id)                                  \
	static int name##_active(void)                                 \
	{                                                              \
		return builtin_active(id);                                 \
	}                                                              \
	static void name##_activate(void)                              \
	{                                                              \
		toggle(id);                                                \
	}                                                              \
	static void name##_draw(GYSURFACE s, int x, int y, GYcolor bg) \
	{                                                              \
		draw(id, s, x, y, bg);                                     \
	}                                                              \
	const PhoneQuick PhoneQuick_##name = {#name, title_, name##_active, name##_activate, name##_draw};
FEATURE(wifi, "Wi-Fi", WIFI)
FEATURE(data, "数据", DATA)
FEATURE(flight, "飞行", FLIGHT)
FEATURE(bluetooth, "蓝牙", BLUETOOTH)
FEATURE(hotspot, "热点", HOTSPOT)
FEATURE(sound, "静音", SOUND)
FEATURE(quiet, "勿扰", QUIET)
static void capture_draw(GYSURFACE s, int x, int y, GYcolor bg)
{
	draw(CAPTURE, s, x, y, bg);
}
const PhoneQuick PhoneQuick_capture = {"capture", "截屏", NULL, PhoneShade_Capture, capture_draw};
