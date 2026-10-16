#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardRec_*：MIC → ES8311 ADC → I2S2 主收 → WAV 落卡 */

/* ====================== 录音机（2026-10-16 补丁18：从"演示壳"改成真录音）======================
 *
 * 【改造前】本 app 是一个**诚实的演示壳**：只有一个计时器和一个来回摆动的进度条，
 *   自己就写着"不采集麦克风声音"、"无音频文件"（见 apps/files.c 与
 *   docs/TF_CARD_APP_INTEGRATION_FEASIBILITY.md）。当时板上确实没有音频器件，
 *   项目里也没有任何录音/麦克风代码，所以它只能那样。
 *
 * 【现在】接真实链路（实现在 src/audio_rec.c + src/audio_out.c + src/es8311.c）：
 *   麦克风 → ES8311(模拟 PGA → ADC) → **模块 DOUT** → 排针 P2-9(PC2 = I2S2_SDI)
 *   → I2S2 主收 → DMA1_Stream3 → RAM 环 → FatFs → /YMGUI/RECORD/REC_xxxx.WAV
 *
 * 【三条"不许造假"的规则】（本项目对 UI 的硬要求）
 *   ① 计时用 `BoardRec_ElapsedMs()`（按**已写进文件的帧数**折算），不是 app 自己的计时器
 *      —— 两者会分叉：主循环被卡顿时 app 的计时器照走，但声音其实没录进去。
 *   ② 电平条用 `BoardRec_Level()`（真实采样峰值），不是自己的动画。
 *   ③ 开录 2 秒后若 `BoardRec_MicOk()` 仍为 0 ⇒ **如实报**"未检测到麦克风信号"
 *      并给出最可能原因（DOUT 未接 P2-9 / 模块没接麦克风），而不是继续显示"录音中"。
 *
 * 【与音乐播放互斥】I2S2 同一时刻只能主发或主收（I2SCFG 二选一）⇒ 录音前必须停音乐。
 *   BoardRec_Start() 会自己发起"停音乐"，并从**下一拍**才真正开始录；这期间返回 2，
 *   本 app 把它显示成"准备中…"并**每拍重试**（不阻塞主循环，也就不会卡界面）。 */

typedef struct
{
	GYOBJ record_time, record_button, record_status, record_meter;
	int   pending;          /* 1 = 正在等放音停完，还在重试 BoardRec_Start() */
	int   active;           /* 1 = 正在录音 */
	int   last_rc;          /* 最近一次动作码（<0 = 失败阶段）*/
	int   elapsed_ms;       /* 已录毫秒（来自引擎，按帧数折算）*/
	int   level;            /* 当前电平 0..100（来自引擎的采样峰值）*/
	int   meter;            /* 电平条的显示值（做了下落平滑）*/
	int   warn_ms;          /* "未检测到信号"累计时长：超过阈值才提示，避免刚开录就报 */
	int   shown_warn;       /* 已经提示过"没信号"了 */
	int   recording_count;  /* 本次进入 app 以来成功录过几段 */
	int   last_bytes;       /* 上一段写进卡的字节数 */
	char  last_name[20];    /* 上一段落地的文件名 */
} AppState;
static AppState state;

static void app_tick(uint32 elapsed);
static void app_close(void);
static int app_query(const char* name);
static void record_toggle(GYOBJ obj);

/* 把状态行刷成"可直接画的一句中文"。用**静态字面量分支**而不是拼缓冲：
 * 本工程的 UI 文案要能被 phone_lang.c 的翻译表按字符串匹配到（中/英）。 */
static void show_status(const char* s)
{
	PhoneUI_text_set(state.record_status, s);
}

static void app_tick(uint32 elapsed)
{
	/* ---- 准备中：等放音淡出结束，每拍重试（不阻塞）---- */
	if (state.pending)
	{
		int rc = BoardRec_Start();
		if (rc == 2)
			return;                            /* 还没停完，下一拍再来 */
		if (rc == 0)
		{
			state.pending = 0;
			state.active = 1;
			state.warn_ms = 0;
			state.shown_warn = 0;
			PhoneUI_button_set(state.record_button, "停止录音");
			show_status("录音中 · 正在写入 TF 卡");
			return;
		}
		/* 失败：如实退出准备态 */
		state.pending = 0;
		state.last_rc = rc;
		PhoneUI_button_set(state.record_button, "开始录音");
		show_status("录音失败 · 检查 TF 卡与麦克风接线");
		return;
	}
	if (!state.active)
		return;

	/* ---- 录音中：时间/电平全部来自引擎（真实数据）---- */
	state.elapsed_ms = (int)BoardRec_ElapsedMs();
	{
		char text[32];
		int  sec = state.elapsed_ms / 1000;
		snprintf(text, sizeof(text), "%02d:%02d", sec / 60, sec % 60);
		PhoneUI_text_set(state.record_time, text);
	}

	/* 电平条：上升立刻跟、下落慢一点（视觉上像真的音量表）。 */
	state.level = BoardRec_Level();
	if (state.level >= state.meter)
		state.meter = state.level;
	else
		state.meter = (state.meter * 3 + state.level) / 4;
	YMGUI_Bar_SetValue(state.record_meter, state.meter);

	/* ---- 诚实提示：录了 2 秒还没有任何信号 ----
	 * 这是**接线/硬件**问题最常见的表现，而它不会让链路报错（DMA 照搬、文件照生成），
	 * 所以必须由 UI 主动说出来，否则用户会以为录上了。 */
	if (!BoardRec_MicOk())
	{
		state.warn_ms += (int)elapsed;
		if (!state.shown_warn && state.warn_ms >= 2000)
		{
			state.shown_warn = 1;
			show_status("录音中，但未检测到麦克风信号：请查模块 DOUT → P2-9(PC2) 接线");
		}
	}
	else
	{
		state.warn_ms = 0;
		if (state.shown_warn)
		{
			state.shown_warn = 0;
			show_status("录音中 · 已检测到声音");
		}
	}
}

static void app_close(void)
{
	/* 退出 app 必须把录音收尾（回填 WAV 头并关文件），否则会留下一个头部长度不对的文件 */
	if (state.active || state.pending)
	{
		(void)BoardRec_Stop();
		state.active = 0;
		state.pending = 0;
		state.last_name[0] = 0;
		(void)BoardRec_Name();
		snprintf(state.last_name, sizeof(state.last_name), "%s", BoardRec_Name());
		state.last_bytes = (int)BoardRec_Bytes();
	}
}

static int app_query(const char* name)
{
	if (!strcmp(name, "record-count"))
		return state.recording_count;
	return 0;
}

static void record_toggle(GYOBJ obj)
{
	(void)obj;

	if (state.active)
	{
		/* ---- 停止 ---- */
		(void)BoardRec_Stop();
		state.active = 0;
		state.pending = 0;
		state.last_rc = BoardRec_LastRc();
		state.elapsed_ms = (int)BoardRec_ElapsedMs();
		state.last_bytes = (int)BoardRec_Bytes();
		snprintf(state.last_name, sizeof(state.last_name), "%s", BoardRec_Name());
		state.meter = 0;
		YMGUI_Bar_SetValue(state.record_meter, 0);
		PhoneUI_button_set(state.record_button, "开始录音");
		++state.recording_count;
		{
			/* 这一句里全是**真实**数据：文件名、秒数、KB 数。 */
			char text[96];
			int  sec = state.elapsed_ms / 1000;
			snprintf(text, sizeof(text), "已保存 %s · %d.%d 秒 · %d KB",
			         (state.last_name[0] != 0) ? state.last_name : "(无文件)",
			         sec, (state.elapsed_ms % 1000) / 100,
			         state.last_bytes / 1024);
			show_status(text);
		}
		return;
	}

	/* ---- 开始 ---- */
	state.elapsed_ms = 0;
	state.meter = 0;
	state.level = 0;
	state.warn_ms = 0;
	state.shown_warn = 0;
	state.last_name[0] = 0;
	state.last_bytes = 0;
	PhoneUI_text_set(state.record_time, "00:00");
	YMGUI_Bar_SetValue(state.record_meter, 0);

	{
		int rc = BoardRec_Start();
		state.last_rc = rc;
		if (rc == 0)
		{
			state.active = 1;
			state.pending = 0;
			PhoneUI_button_set(state.record_button, "停止录音");
			show_status("录音中 · 正在写入 TF 卡");
		}
		else if (rc == 2)
		{
			/* 音乐还在播：本引擎已发起"停音乐"，从下一拍起真正开始录 */
			state.pending = 1;
			state.active = 0;
			PhoneUI_button_set(state.record_button, "准备中…（先停止播放）");
			show_status("准备中 · 正在停止音乐播放，稍后自动开始录音");
		}
		else
		{
			state.pending = 0;
			state.active = 0;
			PhoneUI_button_set(state.record_button, "开始录音");
			show_status("录音起不来 · TF 卡或 ES8311 状态异常");
		}
	}
}

static void app_create(GYOBJ view)
{
	state = (AppState){0};
	PhoneUI_app_header(view, "录音机", "TF 卡 · 16 bit · 48.8 kHz");
	GYOBJ mark = PhoneUI_panel(view, 126, 76, 68, 68, PhoneApps_Get(RECORDER)->color);
	mark->draw_cb = PhoneUI_icon_tile_draw;
	mark->user_data = (void*)(intptr_t)ICON_RECORDER;
	state.record_time = PhoneUI_label_ex(view, 20, 166, 280, "00:00", INK, 1);
	state.record_meter = YMGUI_Creat_Bar_Creat(view, 28, 251, 264, 7);
	YMGUI_Bar_SetRange(state.record_meter, 0, 100);
	YMGUI_Bar_SetColors(state.record_meter, RGB(230, 219, 222), PhoneApps_Get(RECORDER)->color);
	state.record_status = PhoneUI_label_ex(view, 12, 286, 296, "点击开始录音", MUTED, 2);
	state.record_button = PhoneUI_button(view, 24, 351, 272, 43, "开始录音", record_toggle, PhoneApps_Get(RECORDER)->color);
}

static void app_destroy(void)
{
	if (state.active || state.pending)
		(void)BoardRec_Stop();
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "record_time"))
		return (intptr_t)state.record_time;
	if (!strcmp(name, "record_button"))
		return (intptr_t)state.record_button;
	if (!strcmp(name, "record_status"))
		return (intptr_t)state.record_status;
	if (!strcmp(name, "record_meter"))
		return (intptr_t)state.record_meter;
	if (!strcmp(name, "recording"))
		return (intptr_t)state.active;
	if (!strcmp(name, "record_ms"))
		return (intptr_t)state.elapsed_ms;
	if (!strcmp(name, "recording_count"))
		return (intptr_t)state.recording_count;
	if (!strcmp(name, "rec_pending"))
		return (intptr_t)state.pending;
	if (!strcmp(name, "rec_level"))
		return (intptr_t)state.level;
	if (!strcmp(name, "rec_meter"))
		return (intptr_t)state.meter;
	if (!strcmp(name, "rec_peak"))
		return (intptr_t)BoardRec_PeakRaw();
	if (!strcmp(name, "rec_mic_ok"))
		return (intptr_t)BoardRec_MicOk();
	if (!strcmp(name, "rec_last_rc"))
		return (intptr_t)state.last_rc;
	/* 引擎侧的权威状态（与 app 自己的状态可能不同步时，以这两个为准）*/
	if (!strcmp(name, "rec_active_hw"))
		return (intptr_t)BoardRec_Active();
	return 0;
}

const PhoneApp PhoneApp_recorder = {
	.key = "recorder", .title = "录音机", .color = RGB(224, 94, 103), .icon = ICON_RECORDER, .create = app_create, .inspect = app_inspect, .tick = app_tick, .close = app_close, .query = app_query, .destroy = app_destroy};
