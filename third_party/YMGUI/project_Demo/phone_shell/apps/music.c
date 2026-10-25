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
	GYOBJ album, btn_prev, btn_next;      /* 存进 state 才能在歌单页整页隐藏 */
	GYOBJ playlist_button;                /* 播放页 → 歌单页的入口 */
	GYOBJ list_panel, list_title, list_back;   /* 歌单页（补丁31）*/
	int   music_playing;   /* UI 侧的播放意图（真正在不在出声看 BoardMusic_Playing）*/
	int   index;           /* 当前曲目在扫描表里的下标 */
	int   count;           /* 上次扫描到的条目数 */
	/* ---- 补丁29：外部文件模式（文件管理器「播放」把录音交过来）----
	 * ext_mode = 1 时**不认扫描表的 index**，只认 ext_path：
	 *   录音在 /YMGUI/RECORD，与 /YMGUI/MUSIC 的扫描表是两回事，
	 *   拿音乐下标去播录音必然播错文件。
	 * ⚠ 补丁31 起：/YMGUI/MUSIC 里的文件**不再**走这里（见 ext_play），
	 *   ext_mode 只剩"录音等真正外部文件"一种语义。 */
	int   ext_mode;
	int   ext_kind;        /* 0 = 普通文件；1 = 录音（只影响状态栏文案）*/
	char  ext_path[56];    /* "/YMGUI/RECORD/" 14 + 最长文件名 42（重命名上限）*/
	char  title[40];
	char  status[48];
	char  time_text[32];   /* "m:ss / m:ss"；32 够装（-Wformat-truncation 不再报）*/
	uint32_t acc;          /* 进度刷新节流累加器（毫秒）*/

	/* ---- 歌单页手势/滚动状态（补丁31，照 files.c 的 pic_list 范式）----
	 * 为什么也是"按拍增量"：本板触摸是位拍 I2C，单帧丢读会注入假抬起，
	 * 靠 Pressed 基准的做法会 dy≈0 滑不动（补丁25e 已在本板踩过）。 */
	int   list_view;       /* 0 = 播放页；1 = 歌单页 */
	int   list_scroll;
	int   list_gesture;
	int   list_last_y;
	int   list_move_acc;
	int   list_drag_mode;  /* 0 = 拖列表体；1 = 拖右侧边条 */
	int   list_press_idx;  /* 按下时命中的卡片下标（-1 = 无）*/
} AppState;
static AppState state;

/* ===== 歌单页布局（view 内局部坐标；view 在屏幕 y=36，合法 0..411）===== */
#define ML_X          12
#define ML_Y          98
#define ML_W          296
#define ML_H          282         /* 98..380，底部留 32 给圆角/呼吸感 */
#define ML_CARD_H     46
#define ML_BAR_W      6
#define ML_BAR_MARGIN 2
#define ML_CARD_W     (ML_W - ML_BAR_W - ML_BAR_MARGIN - 6)   /* = 282，避开边条 */
#define ML_BAR_MIN    24
#define ML_DRAG_SLOP  6
#define ML_TAP_MAX    12
#define ML_DELTA_MAX  80

#define MUSIC_DIR_PREFIX "/YMGUI/MUSIC/"
#define REC_DIR_PREFIX   "/YMGUI/RECORD/"

static void app_tick(uint32 elapsed);
static void app_close(void);
static int app_command(const char* name, const char* argument);
static void album_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
static void update_music(void);
static void on_music(GYOBJ btn);
static void on_prev(GYOBJ btn);
static void on_next(GYOBJ btn);
static void on_seek(GYOBJ sld, int32 v);   /* 拖动进度条 → 跳播 */
static void ext_play(const char* path);    /* 补丁29：播外部文件（录音）*/
static void apply_play_ui(GYOBJ btn);      /* 播/停后统一刷图标与配色 */

/* 补丁31：歌单页（两态界面 + 自绘滑动列表 + 边条）*/
static void list_view_show(int on);
static void ml_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
static void ml_list_event(GYOBJ obj, GYEvent e);
static void ml_scroll_tick(void);
static void ml_reveal_current(void);
static void ml_select(int idx);
static void on_playlist(GYOBJ btn);
static void on_back(GYOBJ btn);
static int  music_locate(const char* path);

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
	/* 补丁29：外部文件（录音）模式 —— 扫表里没有它，名字只能问播放会话本身。
	 * ⚠ 播完/停掉后 CurrentName 返回 <0 ⇒ 保持上一首名字（不要刷成"（无曲目）"，
	 *   用户刚从文件管理器点进来，看到"无曲目"会以为白点了）。 */
	if (state.ext_mode)
	{
		char name[40];

		if (BoardMusic_CurrentName(name, (int)sizeof(name)) == 0)
		{
			snprintf(state.title, sizeof(state.title), "%s", name);
		}
		PhoneUI_text_set(state.music_title, state.title);
		/* 补丁31：文案跟**来源**走。旧实现不分来源一律写"录音回放" ⇒ 从文件管理器
		 * 点开一首音乐，标题下面却写着"录音回放"（用户看到的那个错）。
		 * ⚠ /YMGUI/MUSIC 的文件现在已在 ext_play 里转为正常列表模式，根本进不到这里；
		 *   这里只剩"录音"和"目录外的其它 wav"两种。 */
		snprintf(state.status, sizeof(state.status),
				 state.ext_kind == 1 ? "录音回放 · %s" : "文件播放 · %s",
				 BoardMusic_Playing() ? "播放中" : "已暂停");
		PhoneUI_text_set(state.music_status, state.status);
		return;
	}

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
	/* 歌单页：每拍推进增量滚动（与 files.c 同一纪律：不依赖 Pressing 事件，
	 * 触摸丢帧时手势仍然能用）。⚠ 必须放在下面 "未在播就 return" 之前。 */
	if (state.list_view)
	{
		ml_scroll_tick();
	}

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

		fmt_time(a, (int)sizeof(a), now);
		fmt_time(b, (int)sizeof(b), total);
		snprintf(state.time_text, sizeof(state.time_text), "%s / %s", a, b);
		PhoneUI_text_set(state.music_time, state.time_text);

		/* ⚠ 拖动进度条时不回写滑块值 —— 否则会把用户刚拖到的位置刷回去
		 *   （见 on_seek）。判断依据 = slider 是否处于 Pressed 态（官方 music_player 同款做法）。 */
		if (!(state.music_seek->state & GY_STATE_Pressed))
		{
			if (total > 0u)
			{
				YMGUI_Slider_SetValue(state.music_seek,
									  (int32)((uint64_t)now * 100u / (uint64_t)total));
			}
		}
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
	/* 歌单页的"当前曲音柱"跟着播放状态变色（播放中橙 / 暂停灰）*/
	if (state.list_view)
		YMGUI_Obj_Invalidate(state.list_panel);
}

/* 播/停之后的统一收尾（图标 + 背景色）。原来这段只长在 on_music 尾部，
 * 补丁29 加了"外部文件"分支 ⇒ 抽出来复用，避免两条分支的配色各写一遍日后走样。 */
static void apply_play_ui(GYOBJ btn)
{
	update_music();
#if YMGUI_ANIM
	if (YMGUI_Anim_BgColorTo(btn, state.music_playing ? RGB(71, 190, 166) : RGB(123, 107, 194),
							 250, GY_ANIM_SMOOTH))
		return;
#endif
	YMGUI_Obj_SetBgColor(btn, state.music_playing ? RGB(71, 190, 166) : RGB(123, 107, 194));
}

/* 补丁31：判断一条绝对路径是不是 /YMGUI/MUSIC 里的曲目；是则返回它在扫描表里的下标。
 *
 * 【为什么必须做这一步】文件管理器「音乐」分类点卡片传的是**绝对路径**（与录音「播放」
 * 共用同一个入口）。如果一律当"外部文件"处理，会出现用户看到的那两个毛病：
 *   ① 状态栏写"录音回放"——明明是音乐；
 *   ② 前后曲失效、歌单页没有当前曲高亮（因为 ext_mode 下 index/count 都是 0）。
 * 所以 /YMGUI/MUSIC 的路径要先"翻译"回列表下标，回到正常音乐模式。 */
static int music_locate(const char* path)
{
	int n;

	if (path == NULL || strncmp(path, MUSIC_DIR_PREFIX, sizeof(MUSIC_DIR_PREFIX) - 1) != 0)
		return -1;

	/* ⚠ 扫描表与照片/笔记共用一张 ⇒ 必须现扫，不能信上次留下的下标。 */
	n = BoardMusic_Scan();
	if (n <= 0)
		return -1;
	state.count = n;

	for (int i = 0; i < n; ++i)
	{
		char p[64];

		if (BoardMusic_Path(i, p, (int)sizeof(p)) == 0 && strcmp(p, path) == 0)
			return i;
	}
	return -1;
}

/* 补丁29：播一个**外部文件**（文件管理器把选中的录音路径传进来）。
 * 为什么要复制路径：show(argument) 的参数"只在本次调用有效"，而续播/播完重按
 * 都要再用这条路径。
 * 补丁31：先看它是不是音乐目录里的曲目 —— 是就走正常列表模式（见 music_locate）。 */
static void ext_play(const char* path)
{
	int idx = music_locate(path);

	state.music_playing = 0;
	BoardMusic_Stop();                 /* 丢弃旧会话（可能是另一首音乐的暂停会话）*/

	if (idx >= 0)
	{
		state.ext_mode = 0;
		state.ext_kind = 0;
		state.index    = idx;
		(void)snprintf(state.ext_path, sizeof(state.ext_path), "%s", path);
		ml_reveal_current();           /* 歌单页滚到这首附近，进去就能看到高亮 */
		if (BoardMusic_Play(idx) == 0)
			state.music_playing = 1;
		update_music();
		apply_play_ui(state.music_button);
		if (!state.music_playing)
		{
			/* 失败文案必须在 update_music() **之后**写（refresh_track 会覆盖状态栏）*/
			snprintf(state.status, sizeof(state.status), "起播失败 rc=%d", BoardMusic_LastRc());
			PhoneUI_text_set(state.music_status, state.status);
		}
		return;
	}

	state.ext_mode = 1;
	/* 文案来源标记：录音目录 ⇒ "录音回放"；其它路径 ⇒ "文件播放"。
	 * （旧实现一律"录音回放"，所以音乐文件也被写成了录音 —— 本次修的正是这个。）*/
	state.ext_kind = (path != NULL &&
					  strncmp(path, REC_DIR_PREFIX, sizeof(REC_DIR_PREFIX) - 1) == 0) ? 1 : 0;
	(void)snprintf(state.ext_path, sizeof(state.ext_path), "%s", path);
	state.count   = 0;                 /* 外部文件不属于 /YMGUI/MUSIC 列表 */
	state.index   = 0;
	if (BoardMusic_PlayFile(state.ext_path) == 0)
	{
		state.music_playing = 1;
	}
	else
	{
		/* 起播失败如实显示（卡不在 / 文件坏 / codec 没起）—— 不假装播起来了。
		 * 顺序讲究：update_music() 会用 refresh_track() 覆盖状态栏，
		 * 所以失败文案必须在它**之后**写。 */
		update_music();
		snprintf(state.status, sizeof(state.status), "起播失败 rc=%d", BoardMusic_LastRc());
		PhoneUI_text_set(state.music_status, state.status);
		return;
	}
	update_music();
}

static void on_music(GYOBJ btn)
{
	/* 补丁29：外部文件（录音）模式。起播用**路径**，不能用扫描表下标 ——
	 * 录音根本不在 /YMGUI/MUSIC 里，BoardMusic_Play(index) 会播成别的文件。 */
	if (state.ext_mode)
	{
		if (state.music_playing)
		{
			(void)BoardMusic_Pause();
			state.music_playing = 0;
		}
		else if (BoardMusic_Paused())
		{
			if (BoardMusic_Resume() != 0)
			{
				snprintf(state.status, sizeof(state.status), "续播失败 rc=%d", BoardMusic_LastRc());
				PhoneUI_text_set(state.music_status, state.status);
				return;
			}
			state.music_playing = 1;
		}
		else if (BoardMusic_PlayFile(state.ext_path) != 0)
		{
			snprintf(state.status, sizeof(state.status), "起播失败 rc=%d", BoardMusic_LastRc());
			PhoneUI_text_set(state.music_status, state.status);
			return;
		}
		else
		{
			state.music_playing = 1;
		}
		apply_play_ui(btn);
		return;
	}

	if (state.count <= 0)
	{
		snprintf(state.status, sizeof(state.status), "卡上 /YMGUI/MUSIC 没有 .wav");
		PhoneUI_text_set(state.music_status, state.status);
		return;
	}

	if (state.music_playing)
	{
		/* 暂停：停出音但保留文件句柄与位置 ⇒ 之后可续播。
		 * （旧版这里是 BoardMusic_Stop()，会关文件、位置归零 ⇒ 拖动后无法续播，正是本次要修的。）*/
		(void)BoardMusic_Pause();
		state.music_playing = 0;
	}
	else if (BoardMusic_Paused())
	{
		/* 续播：从暂停处 / 暂停后拖动到的位置继续。
		 * ⚠ 前提"暂停会话 == 当前 index"：切歌时 on_prev/on_next 会 Stop 掉旧会话，故不变式成立。 */
		if (BoardMusic_Resume() != 0)
		{
			snprintf(state.status, sizeof(state.status), "续播失败 rc=%d", BoardMusic_LastRc());
			PhoneUI_text_set(state.music_status, state.status);
			return;
		}
		state.music_playing = 1;
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
	apply_play_ui(btn);
}

static void on_prev(GYOBJ btn)
{
	(void)btn;
	/* 外部文件（录音）不在 /YMGUI/MUSIC 列表里，前后曲无意义 ⇒ 不动作 */
	if (state.ext_mode)
		return;
	if (state.count <= 0)
		return;
	state.index = (state.index - 1 + state.count) % state.count;
	/* 换歌一律先停掉当前（无论是否在播）—— 否则旧的暂停会话会留到新曲目上，
	 * 续播时对不上 index。在播则接着放新曲目，不在播则就此丢弃旧会话。 */
	BoardMusic_Stop();
	if (state.music_playing)
	{
		(void)BoardMusic_Play(state.index);
	}
	refresh_track();
	ml_reveal_current();                     /* 歌单页：把新曲滚进视口 */
	if (state.list_view)
		YMGUI_Obj_Invalidate(state.list_panel);
}

static void on_next(GYOBJ btn)
{
	(void)btn;
	if (state.ext_mode)   /* 同 on_prev：外部录音没有"下一首" */
		return;
	if (state.count <= 0)
		return;
	state.index = (state.index + 1) % state.count;
	BoardMusic_Stop();   /* 换歌先停当前，丢弃旧暂停会话（同 on_prev） */
	if (state.music_playing)
	{
		(void)BoardMusic_Play(state.index);
	}
	refresh_track();
	ml_reveal_current();                     /* 歌单页：把新曲滚进视口 */
	if (state.list_view)
		YMGUI_Obj_Invalidate(state.list_panel);
}

/* 拖动进度条：Slider 在按下/拖动中**持续**回调此函数（不是仅松手）。
 * v 是 0..100（slider 默认范围），换算成毫秒跳播；同时把时间标签实时刷成目标时间，
 * 这样拖动时"当前时间"跟着手走，而不是被 app_tick 的实时时钟刷回去。 */
static void on_seek(GYOBJ sld, int32 v)
{
	uint32 total = BoardMusic_TotalMs();
	uint32 now;
	char   a[12], b[12];

	(void)sld;
	/* 没有已加载的曲目（既不在播、也没有暂停会话）时拖动无意义；
	 * 顺带避开 stop 后残留的旧 total（wav_play_stop 不清 rate/total）。 */
	if (!state.music_playing && !BoardMusic_Paused())
	{
		return;
	}
	if (total == 0u)
	{
		return;
	}
	(void)BoardMusic_Seek((uint32)((uint64_t)total * (uint32)v / 100u));

	/* 跳播后 g_wav_pos 立即更新 ⇒ elapsed 反映新位置，标签同步。 */
	now = BoardMusic_ElapsedMs();
	fmt_time(a, (int)sizeof(a), now);
	fmt_time(b, (int)sizeof(b), total);
	snprintf(state.time_text, sizeof(state.time_text), "%s / %s", a, b);
	PhoneUI_text_set(state.music_time, state.time_text);
}

/* ===========================================================================
 * 补丁31：歌单页（"两态"界面的第二态）
 *
 * 【为什么整页替换而不是弹层】320×480 上弹层会压掉一半播放控件，且弹层一开就要
 *   处理遮罩/误触/关闭手势；整页替换只多一个返回键，交互面反而更小。
 *
 * 【为什么自己画列表】与文件管理器同一纪律：多行按钮 = 每行两次堆分配，本板
 *   phone_shell 开机就把所有 App 的 create 跑一遍（见 MEMORY 的 heap0 铁律），
 *   一个自绘容器只花一次分配。滚动/边条也照 files.c 的 pic_list 那套来。
 * =========================================================================== */

/* 列表的**屏幕绝对矩形**（事件里的指针坐标是屏幕坐标，view 在 y=36）*/
static void ml_area(GYOBJ obj, GYrect* out)
{
	if (obj != NULL)
	{
		YMGUI_Obj_GetAbsArea(obj, out);
	}
	else
	{
		out->x = ML_X; out->y = ML_Y; out->w = ML_W; out->h = ML_H;
	}
}

static int ml_content_h(void)
{
	int n = state.count > 0 ? state.count : 0;
	return n * ML_CARD_H;
}

static int ml_max_scroll(void)
{
	int m = ml_content_h() - ML_H;
	return (m > 0) ? m : 0;
}

static void ml_clamp(void)
{
	int m = ml_max_scroll();

	if (state.list_scroll < 0) state.list_scroll = 0;
	if (state.list_scroll > m) state.list_scroll = m;
}

/* 让当前曲大致居中可见（进歌单页 / 从文件管理器点某首进来时用）*/
static void ml_reveal_current(void)
{
	int y = state.index * ML_CARD_H;

	if (state.count <= 0) { state.list_scroll = 0; return; }
	state.list_scroll = y - (ML_H - ML_CARD_H) / 2;
	ml_clamp();
}

static void ml_draw_bar(GYSURFACE surface, const GYrect* area)
{
	int ch = ml_content_h();
	int vh = (int)area->h;
	int bx = (int)area->x + (int)area->w - ML_BAR_W - ML_BAR_MARGIN;
	int th, y;

	if (state.count <= 0) return;

	if (ch <= vh)   /* 全看完了：画满高浅色滑块（"边条没丢，只是没得滑"）*/
	{
		GYrect full = {(GYcoord)bx, (GYcoord)((int)area->y + 2), (GYcoord)ML_BAR_W,
					   (GYcoord)(vh - 4)};
		PhoneUI_rounded(surface, &full, RGB(203, 210, 220), ML_BAR_W / 2, GY_OPA_COVER);
		return;
	}

	th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(ML_BAR_MIN, th, vh);
	y  = (int)area->y + (int)(((int64_t)(vh - th) * state.list_scroll) / (ch - vh));
	y  = GYLimitMaxMin((int)area->y, y, (int)area->y + vh - th);

	{
		GYrect track = {(GYcoord)bx, (GYcoord)((int)area->y + 2), (GYcoord)ML_BAR_W,
						(GYcoord)(vh - 4)};
		GYrect thumb = {(GYcoord)bx, (GYcoord)y, (GYcoord)ML_BAR_W, (GYcoord)th};

		PhoneUI_rounded(surface, &track, RGB(219, 224, 233), ML_BAR_W / 2, GY_OPA_COVER);
		PhoneUI_rounded(surface, &thumb, RGB(120, 132, 150), ML_BAR_W / 2, GY_OPA_COVER);
	}
}

static void ml_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	GYrect saved_clip = surface->clip;
	GYrect clip;

	(void)obj;
	/* 自绘容器引擎不自动裁到自身矩形 ⇒ 手动收窄 clip，否则卡片会画到页脚/返回键上 */
	if (GY_Rect_Intersect(&clip, area, &saved_clip) == 0)
		return;
	surface->clip = clip;

	{
		GYrect tray = {area->x, area->y, area->w, area->h};
		PhoneUI_rounded(surface, &tray, RGB(237, 239, 244), 14, GY_OPA_COVER);
	}

	if (state.count <= 0)
	{
		PhoneUI_draw_text(surface, "（/YMGUI/MUSIC 里没有 .wav）", RGB(140, 148, 156), 0,
						  (GYcoord)(area->x + area->w / 2), (GYcoord)(area->y + 20));
	}
	else
	{
		for (int i = 0; i < state.count; ++i)
		{
			int cy = (int)area->y + i * ML_CARD_H - state.list_scroll;

			if (cy + ML_CARD_H <= (int)area->y) continue;   /* 视口上方：不画 */
			if (cy >= (int)area->y + (int)area->h) break;    /* 越出下方：停 */

			{
				GYrect card = {(GYcoord)area->x, (GYcoord)cy,
							   (GYcoord)ML_CARD_W, (GYcoord)(ML_CARD_H - 4)};
				int    cur = (!state.ext_mode && state.index == i);
				GYcolor bg = cur ? RGB(252, 233, 186) : RGB(255, 255, 255);
				char   name[40];
				char   no[8];
				int    ty = cy + (ML_CARD_H - 4 - 16) / 2;

				PhoneUI_rounded(surface, &card, bg, 8, GY_OPA_COVER);

				/* 序号（两位）：比图标更能说明"这是第几首"，也省一次图片绘制 */
				snprintf(no, sizeof(no), "%02d", i + 1);
				PhoneUI_draw_text(surface, no,
								  cur ? RGB(150, 104, 20) : RGB(152, 160, 170), 0,
								  (GYcoord)(area->x + 22), (GYcoord)ty);

				if (BoardMusic_Name(i, name, (int)sizeof(name)) != 0)
					snprintf(name, sizeof(name), "（空）");
				if (strlen(name) > 18u)   /* 截断：右端要留给音柱，且不许溢出卡片 */
				{
					name[15] = '.'; name[16] = '.'; name[17] = '.'; name[18] = 0;
				}

				{
					int tx  = (int)area->x + 42;              /* 文本区左边界 */
					int twz = ML_CARD_W - 42 - 30;            /* 右侧留 30 给音柱 */
					int twt = PhoneUI_text_width(name, 0);    /* 左对齐：cx = 左 + 宽/2 */

					if (twt > twz) twt = twz;
					PhoneUI_draw_text(surface, name,
									  cur ? RGB(58, 42, 20) : RGB(70, 78, 86), 0,
									  (GYcoord)(tx + twt / 2), (GYcoord)ty);
				}

				/* 当前曲：右端三根音柱（播放中橙、暂停时灰）—— 一眼看出"正在放的是哪首" */
				if (cur)
				{
					static const int hs[3] = {8, 15, 6};
					GYcolor c = state.music_playing ? RGB(224, 140, 40) : RGB(188, 178, 152);
					int bx = (int)card.x + ML_CARD_W - 20;
					int by = cy + (ML_CARD_H - 4) / 2;

					for (int k = 0; k < 3; ++k)
					{
						GYrect b = {(GYcoord)(bx + k * 5), (GYcoord)(by - hs[k] / 2),
									(GYcoord)3, (GYcoord)hs[k]};
						PhoneUI_rounded(surface, &b, c, 1, GY_OPA_COVER);
					}
				}
			}
		}
	}

	ml_draw_bar(surface, area);      /* 边条：在裁剪区内、按绝对坐标画 */
	surface->clip = saved_clip;      /* 恢复：不影响其它控件 */
}

/* 边条拖动：指针 y → 滚动位置（与 ml_draw_bar 同一套滑块高度算法）*/
static void ml_bar_set_from_pointer(int py, const GYrect* area)
{
	int ch = ml_content_h();
	int vh = (int)area->h;
	int th, y0, y;

	if (ch <= vh) { state.list_scroll = 0; return; }

	th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(ML_BAR_MIN, th, vh);

	y0 = (int)area->y;
	y  = py - th / 2;
	if (y < y0) y = y0;
	if (y > y0 + vh - th) y = y0 + vh - th;

	state.list_scroll = (int)(((int64_t)(y - y0) * (ch - vh)) / (vh - th));
	ml_clamp();
}

/* 每拍增量滚动（与 files.c 的 pic_scroll_tick 同源：只用"上一拍指针位置"做基准，
 * 不依赖 Pressing 事件是否被派发 ⇒ 触摸丢帧时手势不会断）。
 * ⚠ 必须在 app_tick 的 "if (!music_playing) return;" **之前**调用。 */
static void ml_scroll_tick(void)
{
	GYOBJ  o = state.list_panel;
	GYCTX  c;
	GYrect a;
	int    py, dy;

	if (o == NULL || state.list_gesture == 0)
		return;
	c = o->ctx;
	if (c == NULL) { state.list_gesture = 0; return; }
	if (c->point_pressed == 0u) { state.list_gesture = 0; return; }   /* 抬起（含漏帧收尾）*/

	py = (int)c->point_y;
	dy = py - state.list_last_y;
	if (dy == 0)
		return;
	if (dy > ML_DELTA_MAX || dy < -ML_DELTA_MAX)
	{ state.list_last_y = py; return; }        /* 坐标跳变：只更新基准，不滚 */

	state.list_last_y   = py;
	state.list_move_acc += (dy > 0) ? dy : -dy;

	ml_area(o, &a);
	if (state.list_drag_mode == 1)
	{
		ml_bar_set_from_pointer(py, &a);
	}
	else
	{
		if (state.list_move_acc <= ML_DRAG_SLOP)
			return;                            /* 抖动阈值内先不滚 */
		state.list_scroll -= dy;               /* 手指往上拖(dy<0)看后面的曲目 */
		ml_clamp();
	}
	YMGUI_Obj_Invalidate(o);
}

static void ml_list_event(GYOBJ obj, GYEvent e)
{
	GYCTX ctx = obj->ctx;
	GYrect a;
	int    px, py, bar_x;

	if (ctx == NULL) return;
	px = (int)ctx->point_x;
	py = (int)ctx->point_y;
	ml_area(obj, &a);
	bar_x = (int)a.x + (int)a.w - ML_BAR_W - ML_BAR_MARGIN;

	if (e == GY_EVENT_Pressed)
	{
		state.list_gesture  = 1;
		state.list_last_y   = py;
		state.list_move_acc = 0;
		state.list_drag_mode = 0;

		if (px >= bar_x - 6 && px <= bar_x + ML_BAR_W + 6)
		{
			state.list_drag_mode = 1;
			ml_bar_set_from_pointer(py, &a);
			YMGUI_Obj_Invalidate(obj);
			return;
		}
		{
			int rel = py - (int)a.y;
			int idx = (state.list_scroll + rel) / ML_CARD_H;

			state.list_press_idx = (idx >= 0 && idx < state.count) ? idx : -1;
		}
		return;
	}

	if (e == GY_EVENT_Pressing)
	{
		ml_scroll_tick();
		return;
	}

	if (e == GY_EVENT_Released)
	{
		int mode  = state.list_drag_mode;
		int moved = state.list_move_acc;
		int idx   = state.list_press_idx;

		state.list_gesture   = 0;
		state.list_drag_mode = 0;
		state.list_press_idx = -1;

		if (mode == 1) return;             /* 拖边条：不算点击 */
		if (moved > ML_TAP_MAX) return;    /* 累计位移超阈值 ⇒ 是滚动，不是点选 */
		ml_select(idx);
		return;
	}

	if (e == GY_EVENT_ReleasedOff)
	{
		state.list_gesture   = 0;
		state.list_drag_mode = 0;
		state.list_move_acc  = ML_TAP_MAX + 1;   /* 拖出容器 ⇒ 绝不当点击 */
		return;
	}
}

/* 点某一首：起播（真起播才改 UI；失败回播放页把 rc 显示出来）*/
static void ml_select(int idx)
{
	if (idx < 0 || idx >= state.count)
		return;

	state.ext_mode = 0;
	state.ext_kind = 0;
	state.index    = idx;
	state.music_playing = 0;
	BoardMusic_Stop();                     /* 丢弃旧会话（可能是别的曲目的暂停态）*/
	if (BoardMusic_Play(idx) == 0)
		state.music_playing = 1;

	update_music();
	apply_play_ui(state.music_button);
	YMGUI_Obj_Invalidate(state.list_panel);

	if (!state.music_playing)
	{
		/* 失败就回播放页：列表页没有状态栏，静默失败等于"点了没反应" */
		list_view_show(0);
		snprintf(state.status, sizeof(state.status), "起播失败 rc=%d", BoardMusic_LastRc());
		PhoneUI_text_set(state.music_status, state.status);
	}
}

/* 播放页 → 歌单页 */
static void on_playlist(GYOBJ btn)
{
	(void)btn;
	if (state.ext_mode)                    /* 外部录音没有歌单 */
		return;
	list_view_show(1);
}

static void on_back(GYOBJ btn)
{
	(void)btn;
	list_view_show(0);
}

/* 两态切换：统一在这里改可见性，别的函数不直接碰 SetHidden */
static void list_view_show(int on)
{
	char t[40];

	state.list_view = on ? 1 : 0;

	YMGUI_Obj_SetHidden(state.album,          on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.music_title,    on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.music_status,   on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.music_seek,     on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.music_time,     on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.music_button,   on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.btn_prev,       on ? 1 : 0);
	YMGUI_Obj_SetHidden(state.btn_next,       on ? 1 : 0);

	YMGUI_Obj_SetHidden(state.list_panel,     on ? 0 : 1);
	YMGUI_Obj_SetHidden(state.list_title,     on ? 0 : 1);
	YMGUI_Obj_SetHidden(state.list_back,      on ? 0 : 1);

	/* 入口键只在播放页出现；录音（外部文件）模式没有歌单 */
	YMGUI_Obj_SetHidden(state.playlist_button,
						(on || (state.ext_mode && state.ext_kind == 1)) ? 1 : 0);

	if (on)
	{
		snprintf(t, sizeof(t), "播放列表 · %d 首", state.count);
		PhoneUI_text_set(state.list_title, t);
		ml_reveal_current();
		YMGUI_Obj_Invalidate(state.list_panel);
	}
}

static void app_show(const char* argument)
{
	/* 每次进入都从播放页开始：上次停在歌单页、这次从桌面进来还停在那里会让人以为
	 * "播放控件丢了"。 */
	list_view_show(0);

	/* 补丁29：文件管理器「播放」按钮把选中录音的**绝对路径**传进来（以 '/' 开头）。
	 * 语义是"播这个文件"，不是"播 /YMGUI/MUSIC 里的第 i 首" ⇒ 走外部文件模式，
	 * 且**不必扫 /YMGUI/MUSIC**（录音不在那张表里，扫了也没用）。 */
	if (argument != NULL && argument[0] == '/')
	{
		ext_play(argument);
		return;
	}

	state.ext_mode = 0;   /* 从桌面/其他入口进来 ⇒ 回到正常音乐列表模式 */
	state.ext_kind = 0;
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
	state.album = YMGUI_Creat_Obj_Creat(view, 68, 62, 184, 160);
	state.album->draw_cb = album_draw;
	/* 曲名：真实文件名（扫描表里最长 47 字节，这里 40 够用）*/
	state.music_title = PhoneUI_label_ex(view, 20, 225, 280, "—", INK, 3);
	/* 状态行收窄到 200：右侧要给「列表」入口让位（补丁31）*/
	state.music_status = PhoneUI_label_ex(view, 20, 264, 200, "0/0 · 已暂停", MUTED, 2);
	state.music_seek = YMGUI_Creat_Slider_Creat(view, 30, 289, 260, 22);
	YMGUI_Slider_SetValue(state.music_seek, 0);
	state.music_seek->draw_cb = PhoneUI_slider_draw;
	YMGUI_Slider_SetChanged(state.music_seek, on_seek);   /* 拖动进度条 → 跳播 */
	state.music_time = PhoneUI_left_label(view, 30, 313, 140, "0:00 / 0:00", MUTED, 2);
	state.music_button = PhoneUI_button(view, 134, 344, 52, 52, "", on_music, PhoneApps_Get(MUSIC)->color);
	PhoneUI_button_icon(state.music_button, ICON_PLAY);
	/* 前后曲：原来是两层控件叠出来的"视觉标记"，现在直接是按钮（自带图标）。 */
	state.btn_prev = PhoneUI_button(view, 68, 351, 38, 38, "", on_prev, RGB(156, 161, 181));
	PhoneUI_button_icon(state.btn_prev, ICON_PREVIOUS);
	state.btn_next = PhoneUI_button(view, 214, 351, 38, 38, "", on_next, RGB(156, 161, 181));
	PhoneUI_button_icon(state.btn_next, ICON_NEXT);

	/* ---- 播放列表入口（只在播放页出现）----
	 * 放在状态行右侧：不占播放控件的位置，也与"曲名/进度"这一列对齐。 */
	state.playlist_button = PhoneUI_button(view, 240, 258, 62, 28, "列表", on_playlist,
										   RGB(228, 91, 130));

	/* ---- 歌单页（默认隐藏，见 list_view_show）----
	 * 构造顺序在播放页控件之后 ⇒ 层级在上；但两态互斥显示，不会互相遮挡。 */
	state.list_title = PhoneUI_left_label(view, 22, 66, 200, "播放列表", INK, 2);
	state.list_back  = PhoneUI_button(view, 246, 60, 60, 30, "返回", on_back, RGB(156, 161, 181));
	state.list_panel = YMGUI_Creat_Obj_Creat(view, ML_X, ML_Y, ML_W, ML_H);
	{
		GYOBJ lp = state.list_panel;

		lp->draw_cb  = ml_list_draw;
		lp->event_cb = ml_list_event;
	}
	list_view_show(0);
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
	if (!strcmp(name, "music_list_view"))
		return (intptr_t)state.list_view;
	if (!strcmp(name, "music_ext_mode"))
		return (intptr_t)state.ext_mode;
	if (!strcmp(name, "music_ext_kind"))
		return (intptr_t)state.ext_kind;
	if (!strcmp(name, "music_list_scroll"))
		return (intptr_t)state.list_scroll;
	return 0;
}

const PhoneApp PhoneApp_music = {
	.key = "music", .title = "音乐", .color = RGB(228, 91, 130), .icon = ICON_MUSIC, .create = app_create, .inspect = app_inspect, .tick = app_tick, .close = app_close, .command = app_command, .destroy = app_destroy, .show = app_show};
