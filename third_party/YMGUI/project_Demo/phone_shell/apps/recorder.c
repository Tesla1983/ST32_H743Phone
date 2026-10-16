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
 *   本 app 把它显示成"准备中…"并**每拍重试**（不阻塞主循环，也就不会卡界面）。
 *
 * 【试听（2026-10-16 补丁18b）】录完不必拔卡拷到电脑：右下的"试听"按钮把**最近一段**
 *   录音（/YMGUI/RECORD 里编号最大的 REC_xxxx.WAV）用同一块板、同一只喇叭放出来。
 *   它**复用音乐播放链路**（wav_play）⇒ 起播安全栅 / 音量 ramp / DMA 路径与音乐 App
 *   完全一致，所以"试听有没有爆音"等价于"播放有没有爆音"，不是另一条待验证的路径。
 *   录音中点"试听"会先让引擎**正常收尾**（回填 WAV 头）再放，不会留下半个文件。
 *   ⚠ 退出 app 只停**本 app 发起**的那次试听，不碰音乐 App 正在放的歌。 */

/* ★ 补丁18c：ES8311 起不来时 App 侧"隔拍重试"的额度。
 *   为什么不靠引擎那 3 次：引擎是背靠背（间隔 2 ms），跨不过 es8311 的 **250 ms 熔断冷却**；
 *   App 每拍一次（≈17 ms/拍）能把冷却等过去，且不阻塞主循环。
 *   20 拍 × ~17 ms ≈ 340 ms > 250 ms ⇒ 足够跨过一次完整冷却（用户感觉不到，
 *   比"点了没反应"好得多）。 */
#define REC_UI_RETRY 20

typedef struct
{
	GYOBJ record_time, record_button, record_status, record_meter;
	GYOBJ play_button;      /* ★ 补丁18b：试听最近一段录音 */
	int   pending;          /* 1 = 正在等放音停完，还在重试 BoardRec_Start() */
	int   active;           /* 1 = 正在录音 */
	int   playing;          /* 1 = 本 app 发起的试听正在出声（由 BoardRec_Playing() 收尾）*/
	int   last_rc;          /* 最近一次动作码（<0 = 失败阶段）*/
	int   play_rc;          /* 最近一次试听的返回码 */
	int   elapsed_ms;       /* 已录毫秒（来自引擎，按帧数折算）*/
	int   level;            /* 当前电平 0..100（来自引擎的采样峰值）*/
	int   meter;            /* 电平条的显示值（做了下落平滑）*/
	int   warn_ms;          /* "未检测到信号"累计时长：超过阈值才提示，避免刚开录就报 */
	int   shown_warn;       /* 已经提示过"没信号"了 */
	int   adc_retry;        /* ★ 补丁18c：还剩几次"隔拍重试"（跨过 es8311 的 250 ms 熔断冷却）*/
	int   recording_count;  /* 本次进入 app 以来成功录过几段 */
	int   last_bytes;       /* 上一段写进卡的字节数 */
	char  last_name[20];    /* 上一段落地的文件名 */
} AppState;
static AppState state;

static void app_tick(uint32 elapsed);
static void app_close(void);
static int app_query(const char* name);
static void record_toggle(GYOBJ obj);
static void play_toggle(GYOBJ obj);
static void rec_show_fail(int rc);


/* 把状态行刷成"可直接画的一句中文"。用**静态字面量分支**而不是拼缓冲：
 * 本工程的 UI 文案要能被 phone_lang.c 的翻译表按字符串匹配到（中/英）。 */
static void show_status(const char* s)
{
	PhoneUI_text_set(state.record_status, s);
}

/* ★ 补丁18c：录音起不来时，按**细分码**告诉用户下一步做什么。
 * 旧文案只有一句"录音失败 · 检查 TF 卡与麦克风接线"，把五种完全不同的原因混成一句 ——
 * 用户报障时最需要的恰恰是"我该查线，还是再点一次"。 */
static void rec_show_fail(int rc)
{
	switch (rc)
	{
	case -1:
		show_status("起不来：I2S 收方向没起来（音频通道异常）");
		break;
	case -2:                                  /* ES8311 那一步：细分看 BoardRec_EsRc() */
		if (BoardRec_EsRc() == -3)
			show_status("起不来：找不到 ES8311 · 请查模块 I2C 接线（PB6/PB7）");
		else if (BoardRec_EsRc() == -4)
			show_status("起不来：I2C 总线本身没起来");
		else
			show_status("起不来：ES8311 丢写 · 请再点一次");
		break;
	case -3:
		show_status("起不来：TF 卡没挂载 · 插好卡再试");
		break;
	case -4:
		show_status("起不来：建不了录音文件（卡满或写保护）");
		break;
	case -5:
		show_status("起不来：写不进录音文件 · 检查 TF 卡");
		break;
	default:
		show_status("录音起不来 · 请再点一次");
		break;
	}
}

static void app_tick(uint32 elapsed)
{
	/* ---- 试听收尾：**由引擎状态判定**，不是 app 自己计时 ----
	 * 文件放完了 wav_play_busy() 会自己变 0；app 只是跟着把按钮复位。
	 * ⚠ 只做"1 → 0"的复位，不主动把 playing 置 1（否则音乐 App 在播时
	 *   进本 app 会误显示成"我在试听"）。 */
	if (state.playing && !BoardRec_Playing())
	{
		state.playing = 0;
		PhoneUI_button_set(state.play_button, "试听");
		show_status("试听结束 · 可再录一段或再试听");
	}

	/* ---- 准备中：等放音淡出结束，每拍重试（不阻塞）---- */
	if (state.pending)
	{
		int rc = BoardRec_Start();
		if (rc == 2)
			return;                            /* 还没停完，下一拍再来 */
		if (rc == 0)
		{
			state.pending = 0;
			state.adc_retry = 0;
			state.active = 1;
			state.warn_ms = 0;
			state.shown_warn = 0;
			PhoneUI_button_set(state.record_button, "停止录音");
			show_status("录音中 · 正在写入 TF 卡");
			return;
		}
		/* ★ 补丁18c：**失败也先重试几次再报错**。
		 *   为什么需要 App 侧再试：引擎内部是"背靠背 3 次、间隔 2 ms"，跨不过 es8311 的
		 *   250 ms 熔断冷却；而这里**每拍一次**（≈17 ms 一拍）能把冷却等过去。
		 *   实测（2026-10-16）本板 I2C 首试 NACK ≈30% 且成簇 ⇒ 一次失败并不代表坏了。 */
		if (state.adc_retry > 0)
		{
			state.adc_retry--;
			state.last_rc = rc;
			show_status("正在重试…（I2C 偶发丢写）");
			return;                            /* 保留 pending，下一拍再调 */
		}
		state.pending = 0;
		state.last_rc = rc;
		PhoneUI_button_set(state.record_button, "开始录音");
		rec_show_fail(rc);
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
	/* 试听也要收（只停**本 app 发起**的那次，别把音乐 App 正在放的歌停掉）*/
	if (state.playing)
	{
		BoardRec_StopPlay();
		state.playing = 0;
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
			state.adc_retry = 0;
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
		else if (rc == -2)
		{
			/* ★ 补丁18c：ES8311 那一步失败 ⇒ **不当场判死**，交给 tick 隔拍重试。
			 *   本板 I2C 首试 NACK ≈30% 且成簇（板上无外部上拉、仅 MCU 内部 ~40 kΩ），
			 *   "点了没反应"比"多等两拍"差得多。 */
			state.pending = 1;
			state.active = 0;
			state.adc_retry = REC_UI_RETRY;
			PhoneUI_button_set(state.record_button, "重试中…");
			show_status("起不来 · 正在重试（I2C 偶发丢写）");
		}
		else
		{
			state.pending = 0;
			state.active = 0;
			PhoneUI_button_set(state.record_button, "开始录音");
			rec_show_fail(rc);                 /* 卡/文件类失败：重试无意义，直接说实话 */
		}
	}
}

/* ★ 补丁18b：试听 —— 把**最近一段**录音（/YMGUI/RECORD 里编号最大的那条）放出来听。
 * 这样"录到的到底是不是声音、有多大、有没有爆音"当场就能用耳朵判，不必拔卡拷到电脑。
 * 复用音乐播放链路 ⇒ 起播安全栅 / 音量 ramp / DMA 路径与音乐 App 完全一致。 */
static void play_toggle(GYOBJ obj)
{
	(void)obj;

	/* ---- 正在试听 ⇒ 停 ---- */
	if (state.playing || BoardRec_Playing())
	{
		BoardRec_StopPlay();
		state.playing = 0;
		PhoneUI_button_set(state.play_button, "试听");
		show_status("已停止试听");
		return;
	}

	/* ---- 录音中让路：引擎会先把录音**正常收尾**（回填 WAV 头）再放 ---- */
	if (state.active || state.pending)
	{
		state.active = 0;
		state.pending = 0;
		PhoneUI_button_set(state.record_button, "开始录音");
		state.meter = 0;
		YMGUI_Bar_SetValue(state.record_meter, 0);
		++state.recording_count;
	}

	{
		int rc = BoardRec_PlayLast();
		state.play_rc = rc;

		if (rc == 0)
		{
			char name[24];
			char text[96];
			name[0] = 0;
			(void)BoardRec_PlayName(name, (int)sizeof(name));
			snprintf(text, sizeof(text), "正在试听 %s",
			         (name[0] != 0) ? name : "最近一段录音");
			show_status(text);
			state.playing = 1;
			PhoneUI_button_set(state.play_button, "停止试听");
		}
		else
		{
			state.playing = 0;
			PhoneUI_button_set(state.play_button, "试听");
			/* 失败要**分清**：没录过 vs 录了但放不出来 —— 两者的下一步完全不同 */
			show_status((rc == -1) ? "还没有录音可试听 · 先录一段"
			                       : "试听失败 · 检查 TF 卡与音频输出");
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
	/* 两个并排按钮：左 = 录音，右 = 试听（把最近一段放出来听，见 play_toggle）。
	 * 右侧用 ACCENT 色与录音键区分开。 */
	state.record_button = PhoneUI_button(view, 16, 351, 140, 43, "开始录音", record_toggle, PhoneApps_Get(RECORDER)->color);
	state.play_button   = PhoneUI_button(view, 164, 351, 140, 43, "试听", play_toggle, ACCENT);
}

static void app_destroy(void)
{
	if (state.active || state.pending)
		(void)BoardRec_Stop();
	if (state.playing)
		BoardRec_StopPlay();
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
	/* 试听（补丁18b）：这两个也是**引擎**的事实，不是 app 的记忆 */
	if (!strcmp(name, "rec_playing"))
		return (intptr_t)BoardRec_Playing();
	if (!strcmp(name, "rec_play_rc"))
		return (intptr_t)state.play_rc;
	if (!strcmp(name, "rec_play_hwrc"))
		return (intptr_t)BoardRec_PlayRc();
	/* 补丁18c：ES8311 那一步的细分码 + 重试额度（排障/验收脚本读这两个）*/
	if (!strcmp(name, "rec_es_rc"))
		return (intptr_t)BoardRec_EsRc();
	if (!strcmp(name, "rec_adc_retry"))
		return (intptr_t)BoardRec_AdcRetry();
	if (!strcmp(name, "rec_ui_retry"))
		return (intptr_t)state.adc_retry;
	return 0;
}

const PhoneApp PhoneApp_recorder = {
	.key = "recorder", .title = "录音机", .color = RGB(224, 94, 103), .icon = ICON_RECORDER, .create = app_create, .inspect = app_inspect, .tick = app_tick, .close = app_close, .query = app_query, .destroy = app_destroy};
