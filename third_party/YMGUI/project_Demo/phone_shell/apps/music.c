#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardMusic_*：TF 卡 /YMGUI/MUSIC 下的 .wav */

/* ===========================================================================
 * 音乐 app（2026-10-12 从"假播放器"改成真播放）
 *
 * 【改了什么】
 *   原来只有一组静态文案 + 一个会自己走进度条的假状态；现在：
 *     打开 → 扫 TF 卡 /YMGUI/MUSIC 下的 .wav → 显示真实文件名；
 *     点播放 → BoardMusic_Play(i) → src/wav_play.c 流式读卡 → I2S2 → ES8311 → 喇叭；
 *     进度按"已读字节 / 总字节"走，前后曲切歌，退出 app 自动停播。
 *
 * 【为什么不在 UI 层直接碰 FatFs】
 *   phone_shell 是 third_party 里的官方 Demo，不许 include src/ 下的头；
 *   与 BoardPic_*（照片）/ BoardBt_*（蓝牙）同一套分层，全部走 board 接口。
 *
 * ⚠ 扫描表与照片/笔记共用一张（img_scan_*），所以**每次进页面都要重新 Scan** ——
 *   离开相册/文件管理器再进来，表里的索引已经不是音乐的了。
 * =========================================================================== */
typedef struct
{
	GYOBJ music_button, music_status, music_seek, music_time, music_title;
	int   music_playing;   /* UI 侧的播放意图（真正在不在出声看 BoardMusic_Playing）*/
	int   index;           /* 当前曲目在扫描表里的下标 */
	int   count;           /* 上次扫描到的条目数 */
	char  title[40];
	char  status[48];
	char  time_text[24];
	uint32_t acc;          /* 进度刷新节流累加器（毫秒）*/
} AppState;
static AppState state;

static void app_tick(uint32 elapsed);
static void app_close(void);
static int app_command(const char* name, const char* argument);
static void album_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
static void update_music(void);
static void on_music(GYOBJ btn);
static void on_prev(GYOBJ btn);
static void on_next(GYOBJ btn);

static void fmt_time(char* out, int n, uint32 ms)
{
	uint32 s = ms / 1000u;

	if (n <= 0) return;
	snprintf(out, (size_t)n, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

/* 把"当前曲目 + 播放状态 + 进度"全部刷一遍。
 * ⚠ 文案一律用 PhoneUI_text_set：调用方（这里）负责把字符串留在静态缓冲里，
 *   label 只保存指针 —— 动态拼出来的局部串会在函数返回后失效。 */
static void refresh_track(void)
{
	if (BoardMusic_Name(state.index, state.title, (int)sizeof(state.title)) != 0)
	{
		snprintf(state.title, sizeof(state.title), "（无曲目）");
	}
	PhoneUI_text_set(state.music_title, state.title);
	if (BoardMusic_Playing())
	{
		snprintf(state.status, sizeof(state.status), "%d/%d · 播放中",
				 state.count ? (state.index + 1) : 0, state.count);
	}
	else
	{
		snprintf(state.status, sizeof(state.status), "%d/%d · 已暂停",
				 state.count ? (state.index + 1) : 0, state.count);
	}
	PhoneUI_text_set(state.music_status, state.status);
}

static void app_tick(uint32 elapsed)
{
	if (!state.music_playing)
	{
		return;
	}

	/* 进度每 250 ms 刷一次就够了（42 ms 的半缓冲刷新一次纯属浪费）。 */
	state.acc += elapsed;
	if (state.acc < 250u)
	{
		return;
	}
	state.acc = 0u;

	/* ⚠ 播完会自动停（wav_play_tick 收尾）⇒ 这里要把 UI 状态跟回去，
	 *   否则会出现"喇叭停了、按钮还显示暂停图标"的假状态。 */
	if (!BoardMusic_Playing())
	{
		state.music_playing = 0;
		update_music();
		return;
	}

	{
		uint32 total = BoardMusic_TotalMs();
		uint32 now   = BoardMusic_ElapsedMs();
		char   a[12], b[12];

		if (total > 0u)
		{
			YMGUI_Slider_SetValue(state.music_seek,
								  (int32)((uint64_t)now * 100u / (uint64_t)total));
		}
		fmt_time(a, (int)sizeof(a), now);
		fmt_time(b, (int)sizeof(b), total);
		snprintf(state.time_text, sizeof(state.time_text), "%s / %s", a, b);
		PhoneUI_text_set(state.music_time, state.time_text);
	}
}
static void app_close(void)
{
	/* 退出 app 就停播：否则用户回到桌面还在响，且 I2S 还占着 PB9。 */
	BoardMusic_Stop();
	state.music_playing = 0;
	update_music();
}
static int app_command(const char* name, const char* argument)
{
	(void)argument;
	if (strcmp(name, "toggle"))
		return 0;
	on_music(state.music_button);
	return 1;
}

static void album_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	(void)obj;
	PhoneUI_rounded(surface, area, RGB(83, 70, 135), 22, GY_OPA_COVER);
	YMGUI_Draw_CircleFill(surface, area->x + 90, area->y + 80, 70, RGB(168, 117, 170));
	YMGUI_Draw_CircleFill(surface, area->x + 90, area->y + 80, 56, RGB(44, 50, 99));
	YMGUI_Draw_CircleFill(surface, area->x + 90, area->y + 80, 37, RGB(113, 96, 167));
	YMGUI_Draw_CircleFill(surface, area->x + 90, area->y + 80, 9, RGB(31, 42, 74));
}

static void update_music(void)
{
	PhoneUI_button_icon(state.music_button, state.music_playing ? ICON_PAUSE : ICON_PLAY);
	PhoneHost_MusicChanged(state.music_playing);
	refresh_track();
}

static void on_music(GYOBJ btn)
{
	if (state.count <= 0)
	{
		snprintf(state.status, sizeof(state.status), "卡上 /YMGUI/MUSIC 没有 .wav");
		PhoneUI_text_set(state.music_status, state.status);
		return;
	}

	if (state.music_playing)
	{
		BoardMusic_Stop();
		state.music_playing = 0;
	}
	else
	{
		/* 起播失败（卡不在 / 文件坏 / codec 没应答）时**不要**假装播起来了：
		 * 状态由真正的执行者（BoardMusic_Play 的返回码）决定，见 refresh_track。 */
		if (BoardMusic_Play(state.index) != 0)
		{
			snprintf(state.status, sizeof(state.status), "起播失败 rc=%d", BoardMusic_LastRc());
			PhoneUI_text_set(state.music_status, state.status);
			return;
		}
		state.music_playing = 1;
	}
	update_music();
#if YMGUI_ANIM
	if (YMGUI_Anim_BgColorTo(btn, state.music_playing ? RGB(71, 190, 166) : RGB(123, 107, 194),
							 250, GY_ANIM_SMOOTH))
		return;
#endif
	YMGUI_Obj_SetBgColor(btn, state.music_playing ? RGB(71, 190, 166) : RGB(123, 107, 194));
}

static void on_prev(GYOBJ btn)
{
	(void)btn;
	if (state.count <= 0)
		return;
	state.index = (state.index - 1 + state.count) % state.count;
	if (state.music_playing)
	{
		BoardMusic_Stop();
		(void)BoardMusic_Play(state.index);
	}
	refresh_track();
}

static void on_next(GYOBJ btn)
{
	(void)btn;
	if (state.count <= 0)
		return;
	state.index = (state.index + 1) % state.count;
	if (state.music_playing)
	{
		BoardMusic_Stop();
		(void)BoardMusic_Play(state.index);
	}
	refresh_track();
}

static void app_show(const char* argument)
{
	(void)argument;
	/* ⚠ 每次进入都要重扫：扫描表与照片/笔记共用，索引不跨分类复用。 */
	state.count = BoardMusic_Scan();
	if (state.count < 0)
	{
		state.count = 0;
	}
	if (state.index >= state.count)
	{
		state.index = 0;
	}
	refresh_track();
}
static void app_create(GYOBJ view)
{
	state = (AppState){0};
	PhoneUI_app_header(view, "音乐", "TF 卡 /YMGUI/MUSIC");
	GYOBJ album = YMGUI_Creat_Obj_Creat(view, 68, 62, 184, 160);
	album->draw_cb = album_draw;
	/* 曲名：真实文件名（扫描表里最长 47 字节，这里 40 够用）*/
	state.music_title = PhoneUI_label_ex(view, 20, 225, 280, "—", INK, 3);
	state.music_status = PhoneUI_label_ex(view, 20, 264, 280, "0/0 · 已暂停", MUTED, 2);
	state.music_seek = YMGUI_Creat_Slider_Creat(view, 30, 289, 260, 22);
	YMGUI_Slider_SetValue(state.music_seek, 0);
	state.music_seek->draw_cb = PhoneUI_slider_draw;
	state.music_time = PhoneUI_left_label(view, 30, 313, 140, "0:00 / 0:00", MUTED, 2);
	state.music_button = PhoneUI_button(view, 134, 344, 52, 52, "", on_music, PhoneApps_Get(MUSIC)->color);
	PhoneUI_button_icon(state.music_button, ICON_PLAY);
	/* 前后曲：原来是两层控件叠出来的"视觉标记"，现在直接是按钮（自带图标）。 */
	GYOBJ prev = PhoneUI_button(view, 68, 351, 38, 38, "", on_prev, RGB(156, 161, 181));
	PhoneUI_button_icon(prev, ICON_PREVIOUS);
	GYOBJ next = PhoneUI_button(view, 214, 351, 38, 38, "", on_next, RGB(156, 161, 181));
	PhoneUI_button_icon(next, ICON_NEXT);
}

static void app_destroy(void)
{
	BoardMusic_Stop();
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "music_button"))
		return (intptr_t)state.music_button;
	if (!strcmp(name, "music_status"))
		return (intptr_t)state.music_status;
	if (!strcmp(name, "music_seek"))
		return (intptr_t)state.music_seek;
	if (!strcmp(name, "music_time"))
		return (intptr_t)state.music_time;
	if (!strcmp(name, "music_playing"))
		return (intptr_t)state.music_playing;
	if (!strcmp(name, "music_index"))
		return (intptr_t)state.index;
	if (!strcmp(name, "music_count"))
		return (intptr_t)state.count;
	return 0;
}

const PhoneApp PhoneApp_music = {
	.key = "music", .title = "音乐", .color = RGB(228, 91, 130), .icon = ICON_MUSIC, .create = app_create, .inspect = app_inspect, .tick = app_tick, .close = app_close, .command = app_command, .destroy = app_destroy, .show = app_show};
