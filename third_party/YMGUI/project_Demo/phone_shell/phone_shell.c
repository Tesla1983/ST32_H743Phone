#include "phone_ui.h"
#include "phone_app.h"
#include "phone_host.h"
#include "phone_locale.h"
#include "phone_lang.h"
#include "phone_launcher.h"
#include "phone_shade.h"
#include "phone_desktop.h"
#if defined(PHONE_SHELL_BOARD)
#include "phone_shell_board.h"   /* 板级入口（阶段 5）：不依赖 SDL/stdio */
#else
#include "SDL_LCD.h"             /* 宿主入口：SDL 假 LCD */
#endif

#define HOME_PAGES PHONE_LAUNCHER_PAGE_COUNT(APP_COUNT)
#define APP_OPEN_MS 330

/* ---- 后台应用快照的存储规格（计划书 §7.2 的降配项之一）----
 *
 * 原设计：每打开一个应用就存一张 320×448 的 native 快照 = 280 KB/项，
 *         16 个应用全开 = 4.48 MB。本板给 YMGUI 大对象池（heap1，AXI SRAM）
 *         只有 212 KB，**一个都放不下** —— 表现是最近任务里永远只有占位图。
 *
 * 现改为 **1/4 线性降采样**存储：80×112×2 B = 17.5 KB/项，16 项合计 280 KB。
 * 显示端（recent_draw）本来就把快照双线性缩到 120×168，
 * 80×112 → 120×168 仍是放大，所以观感损失很小。
 *
 * 采样方式说明：抓取时按**最近邻**取点（第 x/4、y/4 个像素）。
 * 不做 4×4 块平均是有意为之 —— 快照按 RGB565 打包存，直接对打包值求平均会串色
 * （R/B 的位会串到 G 上），要正确做块平均必须逐通道拆位累加，代码与状态都更多。
 * 而显示端那次缩放本身就是**逐通道**双线性的，会把 1/4 图平滑开。
 */
#define SNAP_SHIFT   2                       /* 1<<2 = 4 倍线性降采样 */
#define SNAP_DIV     (1 << SNAP_SHIFT)
#define SNAP_MASK    (SNAP_DIV - 1)
#define SNAP_W       (W >> SNAP_SHIFT)       /* 80  */
#define SNAP_H       (448 >> SNAP_SHIFT)     /* 112 */
#define SNAP_PX      ((size_t)SNAP_W * (size_t)SNAP_H)
/* 同时保留的快照数上限。即使降到 17.5 KB/项，16 项 × 17.5 KB = 280 KB
 * 仍超过 heap1 扣掉 draw buffer 后的约 150 KB，所以必须限项。
 * 超出时优先淘汰"已经关掉的、最老的那个"（见 snapshot_make_room）。 */
#define SNAP_MAX_LIVE 6

/* 快捷开关面板的背景截屏也按同样的比例降采样，定义在 phone_ui.h
 * （phone_shade.c 必须用同一个比例）。 */

enum
{
	BOOT,
	HOME,
	APP,
	RECENTS,
	SHUTDOWN,
	OFF
};

static GYCTX ctx;
static GYOBJ home_page, app_page, recent_page, boot_page, power_page;
static GYOBJ app_views[APP_COUNT], app_splash, app_title;
static GYOBJ boot_logo, boot_bar, power_bar, power_title, restart_button;
static GYOBJ recent_cards[APP_COUNT], home_buttons[APP_COUNT];
static GYOBJ recent_empty, recent_count, home_count;
static int scene = BOOT, current_app = -1, busy = 0, clearing = 0, splash_elapsed = 0;
/* 出厂默认 50（用户 2026-10-04 决策：全亮刺眼）。
 * ⚠ 这个初值只在"没读到有效配置"时起作用；正常上电路径由 main.c 在
 *   PhoneShell_BoardInit 之前用 AppConfig_GetBrightness() 覆盖它，
 *   所以改这里不会破坏持久化。 */
static int phase_elapsed = 0, display_brightness = 50;
static int theme_alt = 0, running[APP_COUNT] = {0}, failures = 0;
static GYcoord card_home_y[APP_COUNT];
static GYOBJ navigation, power_dialog, app_demo_dialog, dock_buttons[APP_COUNT];
static GYOBJ mini_music_button, home_music_status;
/* 语言切换时需要重设的常驻文案句柄（Label/Button 的文字是**创建时拷进对象**的副本，
 * 不重设就一直是旧语言 —— 见 PhoneHost_RefreshLanguage）。 */
static GYOBJ home_date, home_widget_title, home_music_title;   /* 桌面小组件 */
static GYOBJ recent_title, recent_clear_button;                /* 最近任务页 */

/* ---- ESP32 上行链路（NTP 时间 + 天气）在界面上的落点 ----
 * 原来这几处全是硬编码（"9:41" / "09:41" / "9月29日  星期二" / "多云 / 22 C"），
 * 现在由 net_refresh() 每 500 ms 从 board 层拉真实数据刷进去。
 * ⚠ 句柄必须存下来：PhoneUI_text_set 要 obj，光有字符串改不了界面。
 * status_clock[] 存两个是因为 status_bar() 在**首页与 app 页各调一次**。 */
static GYOBJ status_clock[2];
static int   status_clock_n;
/* 状态栏右侧图标（WiFi + 电池）的句柄，同样两个（首页 / app 页各一个）。
 * 为什么要句柄：图标是 draw_cb 画的，draw_cb 只在重绘时被调用，
 * 数据变了必须 YMGUI_Obj_Invalidate() 才会重画（见 net_refresh）。 */
static GYOBJ status_icon[2];
static int   status_icon_n;
/* 桌面大时间拆成三个 label —— 小时 / 冒号 / 分钟。
 * 为什么要拆：冒号要每秒闪一次（"冒号 ↔ 空"），单独拼在一个 label 里会因
 * ':' 与空格不等宽而让分钟数字左右跳位（见 src/uart_link.c 的 BoardNet_ClockHH）。 */
static GYOBJ home_clock_h, home_clock_c, home_clock_m;
/* dim_buffer 的定义挪到 dim_buffer_ensure() 旁边（那里有它为什么按需分配的长注释） */
static GYpx* capture_target;
static int capture_shift;   /* 抓屏降采样倍数：0=原尺寸(320×480)，2=1/4(80×120) */
static int recent_scroll = 0, recent_total = 0, recent_ids[APP_COUNT];
static GYOBJ recent_viewport, home_strip, home_dots, home_dock;
static int home_index = 0, home_start_x, home_start_y, home_origin, home_dragged, home_pressed_app = -1;
static int drag_scroll, drag_axis;
static int drag_x, drag_y, drag_dx, drag_dy, dragging, dismiss_id = -1;
static GYcoord drag_origins[APP_COUNT];
static GYcoord drag_origin_y;
static GYpx* snapshots[APP_COUNT];
static GYpx *test_frame, *test_reference;
static void (*display_flush)(struct GYdisp*, const GYrect*, const GYpx*);

/* 关机面板可见性诊断量（2026-10-04，供 tools/ribbon_check.py 读）。
 * ⚠ 为什么不用 scene 判"面板有没有打开"：scene 枚举里 4 是 SHUTDOWN
 *   （关机动画中）不是 OFF，而点"关机..."只是打开面板、scene 根本不变
 *   （仍是 APP=2）⇒ 按 scene 断言必然误判。
 * ⚠ 为什么不在脚本里直接读 power_dialog->state：那是对象字段，
 *   read_vars.py 只按全局符号取址。 */
volatile int32_t g_diag_power_dialog = 0;

static void show_power_dialog(void);
static void power_dialog_hide(int hidden);
static void recent_layout(int animate);
static void home_select(int index, int animate);



/* ---- 状态栏右侧图标组：WiFi（左 x=0..15）+ 4 格信号柱（中 x=18..35）+ 电池（右 x=45..72）----
 *
 * ⚠ 2026-10-10 第二轮：按用户要求把 4 格信号柱**回退**回来。
 *   ⚠ 那 4 根柱子是**装饰性**的：纯几何 Fill、恒满格、不读任何数据
 *     （与电池同一性质——板上既没有蜂窝也没有电量计）。
 *     真值只由**左边的 WiFi 图标**表达，两者别混为一谈。
 *
 * WiFi 图标（16×14）接真值，数据来自 ESP32 的 `$WF,<up>,<ip>,<rssi>`：
 *     已连( 1) → 按 BoardNet_WifiBars() 点亮「圆点 + N 条弧」（不透明）
 *     断开( 0) → 全部暗色 + 红色斜杠
 *     未知(-1) → 全部暗色、无斜杠（开机到首帧 $WF 之间，别被误读成掉线）
 *
 * ⚠⚠ 语义必须写清：**WiFi 射频在对端 ESP32 上，H743 本身没有无线**。
 *     本板是把 ESP32 当网络协处理器/网关用的，图标表达的是
 *     "经 ESP32 网关的联网状态"，不是本芯片的无线状态。
 *
 * ★ 形制照抄 E:\esp32S3_TFT2.8_project（其 main/gui.c 的「状态栏 WiFi 图标」）：
 *   16×14 位图 = 三层扇面弧（半径 4/7/10、扇区 ±45°）+ 底座圆点（半径 1.5），
 *   未点亮的弧保留暗色轮廓（"几格信号"一眼可读），未连接时叠左上→右下的红斜杠。
 *   位图表由该工程 `tools/wifi_icon_gen.py` 确定性生成（几何参数见该文件），
 *   本工程用同一张表 ⇒ 两边形状逐像素一致。
 *   ⚠ 改形状要改那边的脚本再把表同步过来，**不要手改下面两张表**
 *     （手改会与那边的预览图脱钩，审查过的形状就不再是上屏形状）。
 */

#define WIFI_ICON_W 16
#define WIFI_ICON_H 14
/* 4 格柱与电池整体右移的量 = WiFi 图标 16 + 间隙 2。
 * 容器宽度因此从 56 加到 74（右边缘 302 与改动前一致 ⇒ 电池没被挤）。 */
#define STATUS_SHIFT 18

/* tier 语义：0=空 1=圆点 2=内弧 3=中弧 4=外弧（参考工程脚本生成，勿手改） */
static const uint8_t s_wifi_tier[WIFI_ICON_H][WIFI_ICON_W] = {
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,4,4,4,4,4,4,4,0,0,0,0,0},
	{0,0,4,4,0,0,0,0,0,0,0,4,4,0,0,0},
	{0,4,0,0,0,0,0,0,0,0,0,0,0,4,0,0},
	{4,0,0,0,0,3,3,3,3,3,0,0,0,0,4,0},
	{0,0,0,3,3,0,0,0,0,0,3,3,0,0,0,0},
	{0,0,3,0,0,0,0,0,0,0,0,0,3,0,0,0},
	{0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0},
	{0,0,0,0,2,2,0,0,0,2,2,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* 未连接时的红色斜杠掩膜（左上 → 右下）。 */
static const uint8_t s_wifi_slash[WIFI_ICON_H][WIFI_ICON_W] = {
	{1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1},
};

/* draw_cb 上一次**实际画出来**的 up / bars。
 * ⚠ 判据必须是"画出来的"，不能是"我上次调用时传了什么"——
 *   后者会在别的路径改过数据后失同步，跟 2026-10-09 那个
 *   "自缓存 last 值导致日期永久停在切语言那一刻"是同一个坑。
 *   初值取 -99 保证首帧必画。 */
static int s_wifi_drawn_up   = -99;
static int s_wifi_drawn_bars = -99;

/* 画 16×14 的 WiFi 位图：(x0, y0) = 图标左上角。
 *
 * 点亮规则沿用参考工程：**tier <= level+1 点亮**（level = 信号格 0..3）
 *   level 3 全亮 / 2 点+内+中 / 1 点+内 / 0 只亮点；未确认的态一条都不点亮。
 *
 * 同色连续像素并成一次 Fill：224 个像素逐点 Fill 太碎，合并后每帧几十次。
 * 循环多跑一列（x == WIFI_ICON_W）当哨兵，把行尾那段 run 冲出来。 */
static void wifi_icon_draw(GYSURFACE s, int x0, int y0, int up, int bars, GYcolor ink)
{
	const int level   = (bars < 0) ? 0 : bars;          /* -1（还没收到过）按 0 处理 */
	const int lit     = (up == 1) ? (level + 1) : 0;    /* 未确认的态一条弧都不点亮 */
	const GYcolor c_slash = RGB(231, 76, 60);            /* 参考工程 C_ALERT #E74C3C */

	for (int y = 0; y < WIFI_ICON_H; ++y)
	{
		int run_x = 0, run_n = 0;
		GYcolor run_c = 0;
		GYopa run_o = GY_OPA_COVER;

		for (int x = 0; x <= WIFI_ICON_W; ++x)
		{
			int on = 0;
			GYcolor c = 0;
			GYopa o = GY_OPA_COVER;

			if (x < WIFI_ICON_W)
			{
				const uint8_t t = s_wifi_tier[y][x];
				if (up == 0 && s_wifi_slash[y][x] != 0u)
				{
					on = 1; c = c_slash; o = GY_OPA_COVER;      /* 已知断开：红斜杠 */
				}
				else if (t != 0u)
				{
					on = 1; c = ink;
					o = (t <= lit) ? GY_OPA_COVER : (GYopa)90;  /* 未点亮 = 暗色轮廓 */
				}
			}

			if (on && run_n != 0 && c == run_c && o == run_o)
			{
				run_n++;
				continue;
			}
			if (run_n != 0)
			{
				GYrect r = {(GYcoord)(x0 + run_x), (GYcoord)(y0 + y), (GYcoord)run_n, 1};
				YMGUI_Draw_Fill(s, &r, run_c, run_o);
				run_n = 0;
			}
			if (on)
			{
				run_x = x; run_n = 1; run_c = c; run_o = o;
			}
		}
	}
}

static void status_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	GYcolor ink = obj->bg_color;

	/* ---- WiFi 图标（左）：接真值 ----
	 * 18 高的框里放 14 高的图标 ⇒ y 下移 2 垂直居中。 */
	int up   = BoardNet_WifiUp();      /* 1 / 0 / -1（还没收到过） */
	int bars = BoardNet_WifiBars();    /* 0..3，-1 = 还没收到过 */
	wifi_icon_draw(surface, area->x, area->y + 2, up, bars, ink);

	s_wifi_drawn_up   = up;
	s_wifi_drawn_bars = bars;

	/* ---- 4 格信号柱（中）：回退的原实现（装饰性，恒满格，不读数据）---- */
	for (int i = 0; i < 4; ++i)
	{
		GYrect bar = {(GYcoord)(area->x + STATUS_SHIFT + i * 5),
					  (GYcoord)(area->y + 13 - i * 3), 3, (GYcoord)(4 + i * 3)};
		YMGUI_Draw_Fill(surface, &bar, ink, GY_OPA_COVER);
	}

	/* ---- 电池（右）：装饰性，画法原样、整体右移 STATUS_SHIFT ---- */
	int bx = area->x + STATUS_SHIFT;
	GYrect rim = {(GYcoord)(bx + 27), (GYcoord)(area->y + 3), 25, 13};
	PhoneUI_rounded(surface, &rim, ink, 4, GY_OPA_COVER);
	GYrect gap = {(GYcoord)(bx + 29), (GYcoord)(area->y + 5), 19, 9};
	PhoneUI_rounded(surface, &gap, obj->parent == app_page ? PAPER : RGB(55, 67, 105), 2, GY_OPA_COVER);
	GYrect fill = {(GYcoord)(bx + 30), (GYcoord)(area->y + 6), 16, 7};
	PhoneUI_rounded(surface, &fill, ink, 1, GY_OPA_COVER);
	GYrect tip = {(GYcoord)(bx + 53), (GYcoord)(area->y + 7), 2, 5};
	YMGUI_Draw_Fill(surface, &tip, ink, GY_OPA_COVER);
}




/* ---- ESP32 上行链路（NTP 时间 + 天气）的文本取用与刷新 ----
 *
 * 三个 net_xxx() 返回**静态缓冲**，给创建控件时用；周期性刷新的逻辑在 net_refresh()。
 * 为什么不直接在两处各调一次 BoardNet_*：创建和刷新必须共用同一套
 * "没数据时显示什么"的策略，收在一处才不会出现"创建时占位、刷新时真值"
 * 这种前后不一致。
 *
 * ⚠ 静态缓冲的代价：不能同时持有两次返回值（phone_shell 里不会这么用）。
 *
 * net_date()/net_weather() 一律返回 board 层的真实结果并过一遍 T()：
 * 已同步 → "10月9日 星期五" / "晴 / 15℃"（表里没有，原样输出）；
 * 未同步 → "未同步" / "等待天气"（表里有，英文模式下会翻）。 */

static char s_net_hms[12], s_net_date[40], s_net_wx[40];
static char s_net_hh[4], s_net_mm[4];

static const char* net_hms(void)
{
	BoardNet_ClockHMS(s_net_hms, (int)sizeof(s_net_hms));
	return s_net_hms;
}
static const char* net_hh(void)
{
	BoardNet_ClockHH(s_net_hh, (int)sizeof(s_net_hh));
	return s_net_hh;
}
static const char* net_mm(void)
{
	BoardNet_ClockMM(s_net_mm, (int)sizeof(s_net_mm));
	return s_net_mm;
}

/* 日期 / 天气文本：**总是**返回 board 层的真实结果（未同步时是占位串
 * "未同步" / "等待天气"），再交给 T() 走翻译 —— 表里查得到就翻，查不到原样。
 *
 * ⚠ 2026-10-09 改动：原来没同步时返回硬编码的 T("9月29日  星期二") /
 *   T("多云 / 22 C")。那是**假数据**：屏幕上看不出"没同步"和"真是这一天"的区别，
 *   而且一旦有别的路径把这个假值写进 label，周期刷新就再也纠正不回来
 *   （实测就是日期永久停在 "9月29日 星期二"）。占位串才是诚实的表达。 */
static const char* net_date(void)
{
	BoardNet_DateText(s_net_date, (int)sizeof(s_net_date));
	return T(s_net_date);
}

static const char* net_weather(void)
{
	BoardNet_WeatherText(s_net_wx, (int)sizeof(s_net_wx));
	return T(s_net_wx);
}

/* 每 250 ms 把上行链路的数据刷进界面。
 *
 * 为什么 250 ms（原为 500）：大时间的冒号按**秒的奇偶**闪（1 s 一个周期），
 * 采样间隔必须明显小于半个周期才看不出抖动；250 ms 下最坏相位误差 250 ms，
 * 肉眼看上去就是均匀的 500 ms 亮 / 500 ms 灭。
 *
 * 为什么不是每帧刷：时间帧 10 s 才来一次，中间靠**板载 tick 自己递推秒**
 * （见 src/uart_link.c 的 current_clock）。每帧都做字符串格式化毫无收益。
 *
 * ⚠ 值没变就不要调 PhoneUI_text_set —— 它内部会置脏，无条件调用等于
 *   每 250 ms 强制重绘这几个 label（对帧率没影响，但没必要）。 */
static void net_refresh(uint32 dt_ms)
{
	static uint32 acc = 0;
	acc += dt_ms;
	if (acc < 250u)
		return;
	acc = 0;

	/* ⚠ 下面一律用 PhoneUI_text_if_changed（与 label 现值比较），**不要**改回
	 *   "本函数自己缓存上一次写入的字符串"：
	 *   PhoneHost_RefreshLanguage() 会在切语言时整体重设桌面小组件（含日期/天气），
	 *   自带缓存的话，缓存里仍是刷新时写的真值、label 却已被换成别的 ⇒ 比较恒等
	 *   ⇒ 日期永久停在切语言那一刻的内容（2026-10-09 实测：数据层 10/9/5 正确，
	 *   屏幕一直显示硬编码 "9月29日 星期二"）。 */

	char hm[8];
	BoardNet_ClockHM(hm, (int)sizeof(hm));
	for (int i = 0; i < status_clock_n; ++i)
		PhoneUI_text_if_changed(status_clock[i], hm);

	/* 大时间：小时、分钟分开刷（冒号单独处理） */
	BoardNet_ClockHH(s_net_hh, (int)sizeof(s_net_hh));
	PhoneUI_text_if_changed(home_clock_h, s_net_hh);
	BoardNet_ClockMM(s_net_mm, (int)sizeof(s_net_mm));
	PhoneUI_text_if_changed(home_clock_m, s_net_mm);
	/* 冒号闪烁：亮时画 ":"，灭时画空串。
	 * ⚠ 空串而不是空格 —— 这一格是独立 label，写空串不影响左右两格位置；
	 *   写空格反而会留下一个看不见的宽度（无影响但没意义）。 */
	PhoneUI_text_if_changed(home_clock_c, BoardNet_ColonBlink() ? ":" : "");

	PhoneUI_text_if_changed(home_date, net_date());
	PhoneUI_text_if_changed(home_count, net_weather());

	/* WiFi 图标：draw_cb 不会自己察觉数据变了，必须显式置脏。
	 * ⚠ 判据是"draw_cb 上次**实际画出来**的值"（s_wifi_drawn_*），
	 *   不是本函数自己缓存的"上次刷新成了什么" —— 后者一旦被别的路径
	 *   （自检脚本直写内存、切语言重建控件）改过就永久失同步，
	 *   2026-10-09 的日期 bug 就是这么来的。 */
	int wup = BoardNet_WifiUp(), wbars = BoardNet_WifiBars();
	if (wup != s_wifi_drawn_up || wbars != s_wifi_drawn_bars)
	{
		for (int i = 0; i < status_icon_n; ++i)
			YMGUI_Obj_Invalidate(status_icon[i]);
	}
}

static void status_bar(GYOBJ parent)
{
	GYcolor ink = parent == app_page ? INK : WHITE;
	/* 状态栏时间：原来是硬编码 "9:41"，现在取 ESP32 的 NTP 时间。
	 * 句柄要留着 —— status_bar 在首页和 app 页各调一次，net_refresh() 两边一起刷。 */
	char clk[8];
	BoardNet_ClockHM(clk, (int)sizeof(clk));
	GYOBJ t = PhoneUI_left_label(parent, 19, 4, 54, clk, ink, 2);
	if (status_clock_n < 2)
		status_clock[status_clock_n++] = t;
	/* 图标组：56 → 74 宽、起点 248 → 230。
	 * 为什么改：左边要塞进 16px 的 WiFi 图标 + 2px 间隙，4 格柱与电池整体右移 18
	 * （STATUS_SHIFT）⇒ **右边缘仍是 302**，与改动前逐像素一致，电池没被挤；
	 * 左侧多出来的 18px 落在 x=230..247，首页/最近页这一带是空的，
	 * app 页的标题 label 是居中短文本（80..240），实际字形在 x≈140..180，不会撞。 */
	GYOBJ icon = YMGUI_Creat_Obj_Creat(parent, 230, 3, 74, 18);
	icon->bg_color = ink;
	icon->draw_cb = status_draw;
	/* 句柄同样要留着：WiFi 图标接的是真数据，变了得有人置它脏（见 net_refresh）。 */
	if (status_icon_n < 2)
		status_icon[status_icon_n++] = icon;
}



static GYrect splash_tile_area(const GYrect* window)
{
	int size = window->w < 66 ? window->w : 66;
	if (size > window->h)
		size = window->h;
	return (GYrect){window->x + (window->w - size) / 2,
					window->y + (window->h - size) / 2, size, size};
}

static void splash_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	(void)area;
	/* ResizeTo 不会缩放子坐标。按当前窗口排版，不在全屏子视图里写死图标偏移。 */
	GYrect window;
	YMGUI_Obj_GetAbsArea(obj->parent, &window);
	GYrect tile = splash_tile_area(&window);
	int id = current_app >= 0 && current_app < APP_COUNT ? current_app : TASKS;
	YMGUI_Draw_Fill(surface, &window, PAPER, GY_OPA_COVER);
	PhoneUI_rounded(surface, &tile, PhoneApps_Get(id)->color, tile.w / 3, GY_OPA_COVER);
	PhoneUI_icon_draw(surface, PhoneApps_Get(id)->icon, tile.x + tile.w / 2, tile.y + tile.h / 2, PhoneApps_Get(id)->color);
}



/* ---- 壁纸 ribbon 的一层：crest 相同的连续列合并成一次填充（2026-10-03 P0-1）----
 *
 * 【原实现的代价】对每一列 x 单独调一次 Draw_Fill，填 1 px 宽 × (H-crest) 高：
 *   · 每个 band 要 320 列 × 3 层 = **960 次**调用，整帧 15 个 band = **14400 次**；
 *   · 其中绝大多数是白跑的 —— crest 是整数抛物线，相邻列经常取同一个值
 *     （顶点附近能连着十几列完全相同）；而第 1 层的 crest ∈ [239,323]，
 *     本 band 的行区间在它之上时，Draw_Fill 会被裁成空矩形直接返回，
 *     每帧有 7 个 band 整层都不相交，却照样调满 320 次。
 *
 * 【改法】① crest 相同的相邻列合成一个矩形（宽 = 连续段长度）；
 *        ② 整段 [crest, H) 与本 band 的 clip 行区间不相交就整段跳过。
 *
 * 【为什么逐位等价】alpha 混合是**逐像素独立**的运算，且每个像素只被**一层**里的
 *  **一列**覆盖 —— 合并只是把"多次调用覆盖互不重叠的像素集合"改成"一次调用覆盖
 *  它们的并集"，每个像素被混合的**次数、顺序、颜色、opa 全部不变**；
 *  被跳过的那些原本也会被裁成空矩形，一个像素都不会写。 */
/* ★ 壁纸 ribbon 半透明开关（2026-10-04 由 UI 开关接管）
 *
 * 【语义】1 = 半透明（壁纸渐变透出来，**出厂默认**，满负载 ~41 FPS）
 *         0 = 不透明（ribbon 是实色，满负载 ~48 FPS）
 *
 * 【⚠ 历史上是反的】这个变量原先叫 `g_ribbon_opaque`、且 **1 表示不透明**，
 * 因为它诞生时只是 P1-3 的性能实验开关，名字按"不透明"取。
 * 2026-10-04 做成设置页开关后语义翻转成"1=半透明"，**是为了让变量名、UI 开关、
 * 配置项三者语义一致**，读代码不用心算取反。翻转时一并改了名，别再引用旧名。
 *
 * 【值从哪来】编译期默认 YMGUI_RIBBON_OPAQUE 已不再决定运行时取值 ——
 * 开机由 AppConfig_Load() 从 W25Q128 读出（出厂默认 1 = 半透明），
 * 之后由设置页的开关经 PhoneHost_SetRibbonTranslucent() 改并落盘。
 * 保留 YMGUI_RIBBON_OPAQUE 宏只作为"首次上电、flash 无有效配置"的兜底。
 *
 * 【为什么默认半透明】半透明是最初的设计意图（壁纸渐变要透出来）；
 * 默认关会让用户困惑"为什么这个功能默认是关的"。代价是 41 vs 48 FPS，
 * 是主动的视觉优先取舍。
 *
 * 【帧率代价实测】半透明路径每帧 176877 px 走 α 混合，17.33 cy/px ≈ 7.66 ms。
 * 改不透明即 24.29 → 20.77 ms（41.2 → 48.2 FPS）。
 * 对比图见 build/p1_ribbon_{semi,opaque}.png，差异 23.1% 集中在 y=239..447。 */
#ifndef YMGUI_RIBBON_OPAQUE
#define YMGUI_RIBBON_OPAQUE  1
#endif
volatile uint32_t g_ribbon_translucent = YMGUI_RIBBON_OPAQUE;

static void ribbon_layer(GYSURFACE surface, const GYrect* area, GYcolor color, GYopa opa,
						 int base, int xc, int div)
{
	/* 半透明关（g_ribbon_translucent = 0）⇒ 强制改成不透明。
	 * 注意取反：开关是"半透明"，这里判断的是"不要半透明"。 */
	if (g_ribbon_translucent == 0u)
		opa = GY_OPA_COVER;

	/* 把 clip 换算到 area 局部行坐标，便于判断"这段 ribbon 是否与本 band 相交" */
	int top = surface->clip.y - area->y;
	int bot = surface->clip.y + surface->clip.h - area->y;
	int x = 0;
	while (x < area->w)
	{
		int d = x - xc;
		int crest = base + d * d / div;
		int x2 = x + 1;
		while (x2 < area->w)          /* 向右扩到 crest 变化的那一列为止 */
		{
			int d2 = x2 - xc;
			if (base + d2 * d2 / div != crest)
				break;
			++x2;
		}
		if (crest < bot && H > top)   /* ribbon 占 [crest, H)，与 clip 的 [top, bot) 有交集才画 */
		{
			GYrect ribbon = {(GYcoord)(area->x + x), (GYcoord)(area->y + crest),
							 (GYcoord)(x2 - x), (GYcoord)(H - crest)};
			YMGUI_Draw_Fill(surface, &ribbon, color, opa);
		}
		x = x2;
	}
}

static void wallpaper(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	(void)obj;
	for (int yy = 0; yy < area->h; ++yy)
	{
		if (area->y + yy < surface->clip.y || area->y + yy >= surface->clip.y + surface->clip.h)
			continue;
		int r = theme_alt ? 47 + yy * 54 / H : 42 + yy * 150 / H;
		int g = theme_alt ? 79 + yy * 67 / H : 51 + yy * 61 / H;
		int b = theme_alt ? 90 + yy * 46 / H : 93 + yy * 67 / H;
#if YMGUI_COLOR_DEPTH == 16
		/* RGB565 的低位有序抖动：原生 1x 下避免大面积渐变出现色带。 */
		static const uint8 noise[4][4] = {{0, 4, 1, 5}, {6, 2, 7, 3}, {1, 5, 0, 4}, {7, 3, 6, 2}};
		/* ★2026-10-03 优化：行内改成指针直写，逐像素写的值与原实现逐位相同★
		 *
		 * 【原实现的代价】这一行原来是 320 次 GY_PutPx()。而 GY_PutPx 每写一个像素
		 * 要做 6 次边界比较 + 2 次减法 + 1 次乘法算索引，**再加一次 GY_ColorToPx()**
		 * （抖动值只有 4 种，却每像素都重算一遍打包）。壁纸是**全屏**背景，
		 * 每帧 153600 px 全部走这条路径 —— 板上实测光这一项就吃掉整帧约 50%
		 * （诊断计数 g_diag_putpx ≈ 155 k/帧，摊到 ~80 cycles/px）。
		 *
		 * 【这里的做法】同一行只有 4 个可能的抖动色，先算好放进 dith[]，
		 * 行内只做"查表 + store"。裁剪在循环外一次性算好 x0/x1 与行首指针，
		 * 因为 vis ⊆ clip ⊆ buf_area 是库保证的不变量，行内不必再逐像素判边界。
		 * 写入的像素集合与颜色与原实现**逐位相同**，不改变显示结果。 */
		{
			GYpx dith[4];
			int  p;
			int  x0 = 0, x1 = area->w;
			GYcoord by = (GYcoord)(area->y + yy) - surface->buf_area.y;

			for (p = 0; p < 4; ++p)
			{
				int d = noise[yy & 3][p];
				dith[p] = GY_ColorToPx(RGB(r + d, g + d / 2, b + d));
			}
			/* 裁剪：屏幕坐标 → 相对 area->x 的 [x0, x1) */
			if (area->x < surface->clip.x)
				x0 = surface->clip.x - area->x;
			if (area->x + x1 > surface->clip.x + surface->clip.w)
				x1 = surface->clip.x + surface->clip.w - area->x;
			if (area->x < surface->buf_area.x)
				x0 = surface->buf_area.x - area->x;
			if (area->x + x1 > surface->buf_area.x + surface->buf_area.w)
				x1 = surface->buf_area.x + surface->buf_area.w - area->x;
			if (x0 < 0)
				x0 = 0;
			if (x1 > area->w)
				x1 = area->w;
			if (by < 0 || by >= surface->buf_area.h || x1 <= x0)
				continue;
			{
				/* ★P1-5：行内 4 路展开 + 指针递增（2026-10-03）★
				 *
				 * 【为什么还要再改】P0 那轮已把逐像素 PutPx 改成"查表 + 索引 store"，
				 * 但 P1 轮的微基准发现**裸 store 的地板只有 0.88 cycles/px**
				 * （tools/micro.py case6），而反汇编显示这个循环每像素要付
				 * AND(x&3) + 变址取 dith + 变址算目标地址 + CMP 共 6 条指令，
				 * 板上实测摊到 ~9-11 cycles/px —— 是地板的 10 倍还多。
				 *
				 * 【这里的做法】抖动周期是 4：先把 x 对齐到 4 的倍数（头部 ≤3 px），
				 * 主体循环一次写 4 个像素（d0..d3 是 dith[0..3] 的寄存器副本，
				 * 无查表、无逐像素地址计算），尾部 ≤3 px 收尾。
				 *
				 * 【为什么逐位等价】每个屏幕像素 x 存的值仍是 dith[x & 3] ——
				 * d0..d3 就是 dith[0..3] 的拷贝，展开只是把"按 x&3 查表"换成
				 * "按位置固定取哪个副本"，写入的像素集合、值、顺序都不变。
				 * （微基准 case 10/11 就是这两种写法的 A/B 对照，见 tools/micro.py。） */
				GYpx*      dst = (GYpx*)surface->buf
				                 + (int32)by * surface->stride
				                 + (area->x + x0 - surface->buf_area.x);
				const GYpx d0 = dith[0], d1 = dith[1], d2 = dith[2], d3 = dith[3];
				int        x  = x0;
				while (x < x1 && (x & 3) != 0)     /* 头部：对齐到抖动周期（≤3 px） */
				{
					*dst++ = dith[x & 3];
					++x;
				}
				while (x + 4 <= x1)                /* 主体：一次 4 像素 */
				{
					*dst++ = d0; *dst++ = d1; *dst++ = d2; *dst++ = d3;
					x += 4;
				}
				while (x < x1)                     /* 尾部（≤3 px） */
				{
					*dst++ = dith[x & 3];
					++x;
				}
			}
		}
#else
		GYrect line = {area->x, (GYcoord)(area->y + yy), area->w, 1};
		YMGUI_Draw_Fill(surface, &line, RGB(r, g, b), GY_OPA_COVER);
#endif
	}
	/* 三层 ribbon，逐层的 crest(x) = base + (x - xc)^2 / div（与合并前逐列算的式子完全一致）。
	 * 见 ribbon_layer 上方注释：合并 + 跳空都是逐位等价的，只削掉碎调用与空调用。 */
	ribbon_layer(surface, area, theme_alt ? RGB(69, 135, 147) : RGB(241, 173, 164), 175, 239,  90,  620);
	ribbon_layer(surface, area, theme_alt ? RGB(32, 81, 106) : RGB(86, 76, 133), 240, 315, 340,  750);
	ribbon_layer(surface, area, theme_alt ? RGB(29, 65, 85) : RGB(44, 48, 88), 220, 379,   0, 1300);
}



static void logo_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	(void)obj;
	int cx = area->x + area->w / 2, cy = area->y + area->h / 2;
	YMGUI_Draw_CircleFill(surface, cx, cy, 37, RGB(66, 209, 180));
	YMGUI_Draw_CircleFill(surface, cx, cy, 27, RGB(14, 32, 55));
	YMGUI_Draw_CircleFill(surface, cx + 14, cy - 15, 8, RGB(66, 209, 180));
}

static void show_home(void)
{
	PhoneApps_Hide();
	scene = HOME;
	busy = 0;
	clearing = 0;
	YMGUI_Obj_SetHidden(boot_page, 1);
	YMGUI_Obj_SetHidden(power_page, 1);
	YMGUI_Obj_SetHidden(app_page, 1);
	YMGUI_Obj_SetHidden(recent_page, 1);
	YMGUI_Obj_SetHidden(home_page, 0);
	PhoneUI_set_pos(home_page, 0, 0);
	if (navigation)
		YMGUI_Obj_SetHidden(navigation, 0);
}

static void begin_boot(void)
{
#if YMGUI_ANIM
	YMGUI_Anim_CancelAll(ctx);
#endif
	home_select(0, 0);
	PhoneApps_CloseAll();
	for (int i = 0; i < APP_COUNT; ++i)
	{
		running[i] = 0;
		GY_free1(snapshots[i]);
		snapshots[i] = NULL;
	}
	if (navigation)
		YMGUI_Obj_SetHidden(navigation, 1);
	scene = BOOT;
	phase_elapsed = 0;
	busy = 0;
	current_app = -1;
	YMGUI_Obj_SetHidden(home_page, 1);
	YMGUI_Obj_SetHidden(app_page, 1);
	YMGUI_Obj_SetHidden(recent_page, 1);
	YMGUI_Obj_SetHidden(power_page, 1);
	YMGUI_Obj_SetHidden(boot_page, 0);
	PhoneUI_set_pos(boot_logo, 0, 106);
	YMGUI_Bar_SetValue(boot_bar, 0);
#if YMGUI_ANIM
	PhoneUI_move(boot_logo, 0, 94, 680);
	PhoneUI_animate_bar(boot_bar, 100, 740);
#else
	show_home();
#endif
}

static void begin_shutdown(void)
{
	if (scene == OFF || scene == SHUTDOWN)
		return;
	PhoneIME_Hide();
	PhoneDesktop_Close();
	PhoneApps_CloseAll();
	scene = SHUTDOWN;
	phase_elapsed = 0;
	busy = 0;
	power_dialog_hide(1);
	YMGUI_Obj_SetHidden(navigation, 1);
	YMGUI_Obj_SetHidden(app_page, 1);
	YMGUI_Obj_SetHidden(recent_page, 1);
	YMGUI_Obj_SetHidden(boot_page, 1);
	YMGUI_Obj_SetHidden(power_page, 0);
	YMGUI_Obj_SetHidden(restart_button, 1);
	YMGUI_Obj_SetHidden(power_bar, 0);
	PhoneUI_text_set(power_title, T("正在关机"));
	YMGUI_Bar_SetValue(power_bar, 0);
#if YMGUI_ANIM
	PhoneUI_animate_bar(power_bar, 100, 700);
#else
	scene = OFF;
	PhoneUI_text_set(power_title, T("已关机"));
	YMGUI_Obj_SetHidden(power_bar, 1);
	YMGUI_Obj_SetHidden(restart_button, 0);
#endif
}

static void update_running(void)
{
	PhoneApps_SyncRunning(running);
	char text[40];
	int n = 0;
	for (int i = 0; i < APP_COUNT; ++i)
		n += running[i];
	snprintf(text, sizeof(text), T("%d 个应用已打开"), n);
	PhoneUI_text_set(recent_count, text);
	YMGUI_Obj_SetHidden(recent_empty, n != 0);
	recent_total = 0;
	for (int i = 0; i < APP_COUNT; ++i)
	{
		YMGUI_Obj_SetHidden(recent_cards[i], !running[i]);
		if (running[i])
			recent_ids[recent_total++] = i;
	}
	recent_layout(scene == RECENTS && !clearing);
}


/* 快照数超上限时腾位：优先淘汰"已关闭且非本次要保留的"最老一项。
 * 一个都淘汰不掉（全在运行）时放弃本次分配，调用方会走占位图降级路径。 */
static void snapshot_make_room(int keep_id)
{
	int live = 0, i;
	for (i = 0; i < APP_COUNT; ++i)
		if (snapshots[i])
			++live;
	if (live < SNAP_MAX_LIVE)
		return;
	for (i = 0; i < APP_COUNT; ++i)
		if (snapshots[i] && i != keep_id && !running[i])
		{
			GY_free1(snapshots[i]);
			snapshots[i] = NULL;
			return;
		}
	for (i = 0; i < APP_COUNT; ++i)
		if (snapshots[i] && i != keep_id && i != current_app)
		{
			GY_free1(snapshots[i]);
			snapshots[i] = NULL;
			return;
		}
}

static GYpx* snapshot_alloc(int id)
{
	if (snapshots[id])
		return snapshots[id];
	snapshot_make_room(id);
	snapshots[id] = (GYpx*)GY_malloc1(SNAP_PX * sizeof(GYpx));
	if (snapshots[id])
		memset(snapshots[id], 0, SNAP_PX * sizeof(GYpx));
	return snapshots[id];
}

static void prepare_app(int id, int splash, const char* argument)
{
	current_app = id;
	running[id] = 1;
	(void)snapshot_alloc(id);   /* 失败也没关系：最近任务会画占位图 */
	update_running();
	PhoneApps_Show(id, argument);
	/* 应用页顶部标题：绘制/显示期取值，包一层 T() 即可跟随语言。
	 * 切换语言时若正停在某个应用里，由 PhoneHost_RefreshLanguage() 重设。 */
	PhoneUI_text_set(app_title, T(PhoneApps_Get(id)->title));
	for (int i = 0; i < APP_COUNT; ++i)
		YMGUI_Obj_SetHidden(app_views[i], i != id || splash);
	YMGUI_Obj_SetHidden(app_splash, !splash);
	splash_elapsed = 0;
}

static int open_app_from(int id, const char* argument, GYOBJ source)
{
	if (busy || id < 0 || id >= APP_COUNT || scene == BOOT || scene == OFF || scene == SHUTDOWN)
		return 0;
	int desktop_index = PhoneDesktop_Index(id);
	if (desktop_index < 0) return 0;
	PhoneDesktop_Close();
	PhoneShade_Close(0);
	PhoneIME_Hide();
	PhoneLauncherSlot slot;
	PhoneLauncher_Place(desktop_index, APP_COUNT, &slot);
	home_select(slot.page, 0);
	prepare_app(id, 1, argument);
	YMGUI_Obj_SetHidden(recent_page, 1);
	YMGUI_Obj_SetHidden(app_page, 0);
	GYrect origin;
	YMGUI_Obj_GetAbsArea(source ? source : home_buttons[id], &origin);
	PhoneUI_set_pos(app_page, origin.x, origin.y);
	app_page->area.w = origin.w;
	app_page->area.h = origin.h;
	scene = APP;
	busy = 1;
	PhoneUI_move(app_page, 0, 0, APP_OPEN_MS);
#if YMGUI_ANIM
	if (!YMGUI_Anim_ResizeTo(app_page, W, H, APP_OPEN_MS, GY_ANIM_EASE_OUT))
	{
		app_page->area.w = W;
		app_page->area.h = H;
	}
#else
	app_page->area.w = W;
	app_page->area.h = H;
#endif
#if !YMGUI_ANIM
	busy = 0;
	YMGUI_Obj_SetHidden(app_splash, 1);
	YMGUI_Obj_SetHidden(app_views[id], 0);
#endif
	return 1;
}

int PhoneHost_Open(int id, const char* argument)
{
	return open_app_from(id, argument, NULL);
}

static void open_app(int id)
{
	PhoneHost_Open(id, NULL);
}

static void open_recent(void)
{
	if (busy || scene == RECENTS || scene == BOOT || scene == OFF || scene == SHUTDOWN)
		return;
	PhoneIME_Hide();
	PhoneApps_Hide();
	if (scene == APP)
	{
		YMGUI_Obj_Invalidate(app_page);
		YMGUI_Refresh(ctx);
	}
	YMGUI_Obj_SetHidden(app_page, 1);
	PhoneUI_set_pos(app_page, 0, H);
	update_running();
	recent_scroll = 0;
	recent_layout(0);
	PhoneUI_set_pos(recent_page, 0, H);
	YMGUI_Obj_SetHidden(recent_page, 0);
	scene = RECENTS;
	busy = 1;
	PhoneUI_move(recent_page, 0, 0, 280);
#if !YMGUI_ANIM
	busy = 0;
#endif
}

static void go_home(void)
{
	if (busy || scene == HOME || scene == BOOT || scene == OFF || scene == SHUTDOWN)
		return;
	PhoneIME_Hide();
	PhoneApps_Hide();
	if (scene == APP)
	{
		YMGUI_Obj_Invalidate(app_page);
		YMGUI_Refresh(ctx);
	}
	GYOBJ leaving = scene == APP ? app_page : recent_page;
	scene = HOME;
	busy = 1;
	phase_elapsed = 0;
	if (leaving == app_page && current_app >= 0)
	{
		GYrect target;
		YMGUI_Obj_GetAbsArea(dock_buttons[current_app], &target);
		PhoneUI_move(leaving, target.x, target.y, 260);
#if YMGUI_ANIM
		YMGUI_Anim_ResizeTo(leaving, target.w, target.h, 260, GY_ANIM_EASE_OUT);
#endif
	}
	else
		PhoneUI_move(leaving, 0, H, 260);
#if !YMGUI_ANIM
	YMGUI_Obj_SetHidden(leaving, 1);
	busy = 0;
#endif
}

static void clear_recent(void)
{
	if (scene != RECENTS || busy)
		return;
	int any = 0;
	for (int slot = 0; slot < recent_total; ++slot)
	{
		int i = recent_ids[slot];
		if (running[i])
		{
			any = 1;
			PhoneUI_move(recent_cards[i], slot % 2 ? W + 8 : -144, recent_cards[i]->area.y, 280 + (slot / 2) * 65);
		}
	}
	if (!any)
		return;
#if YMGUI_ANIM
	busy = 1;
	clearing = 1;
#else
	for (int i = 0; i < APP_COUNT; ++i)
	{
		running[i] = 0;
		GY_free1(snapshots[i]);
		snapshots[i] = NULL;
	}
	current_app = -1;
	update_running();
#endif
}

static void advance(uint32 elapsed)
{
	PhoneDesktop_Tick(elapsed);
	PhoneShade_Update((scene == HOME || scene == APP || scene == RECENTS) && !busy && !PhoneDesktop_Busy() &&
		(power_dialog->state & GY_STATE_Hidden) && !YMGUI_MsgBox_IsShown(app_demo_dialog));
	PhoneIME_Update(scene == APP && !busy && !PhoneShade_Visible() && (power_dialog->state & GY_STATE_Hidden));
	/* 诊断量**每帧现算**，不做增量同步（2026-10-04 板上实测踩过的坑）。
	 * 增量写法（在被 -O2 内联的 setter 里赋值）会与真实状态脱节：
	 * 实测 state=0x04(Hidden) 而 g_diag_power_dialog=1，稳定复现。
	 * 编译器看不到这些 volatile 只有 SWD 在读，某些路径的写入被合并掉了。
	 * ⇒ 约定：诊断量只在本函数这一处赋值，读时不再缓存。
	 * 语义：1 = 已隐藏/不可见，0 = 显示中（与各自 GY_STATE_Hidden 同步）。 */
	g_diag_power_dialog = (power_dialog->state & GY_STATE_Hidden) ? 1 : 0;
	g_diag_ime_visible  = PhoneIME_Visible() ? 1 : 0;
	if (elapsed > 100)
		elapsed = 100;
	PhoneApps_Tick(elapsed);
	if (scene == BOOT)
	{
		phase_elapsed += (int)elapsed;
		if (phase_elapsed >= 800)
		{
			show_home();
			PhoneUI_set_pos(home_page, 0, 26);
			PhoneUI_move(home_page, 0, 0, 380);
		}
	}
	else if (scene == SHUTDOWN)
	{
		phase_elapsed += (int)elapsed;
		if (phase_elapsed >= 780)
		{
			scene = OFF;
			PhoneUI_text_set(power_title, T("已关机"));
			YMGUI_Obj_SetHidden(power_bar, 1);
			YMGUI_Obj_SetHidden(restart_button, 0);
		}
	}
	else if (scene == APP && !(app_splash->state & GY_STATE_Hidden))
	{
		splash_elapsed += (int)elapsed;
		if (splash_elapsed >= APP_OPEN_MS)
		{
			YMGUI_Obj_SetHidden(app_splash, 1);
			YMGUI_Obj_SetHidden(app_views[current_app], 0);
			busy = 0;
		}
	}
#if YMGUI_ANIM
	if (busy && scene == HOME)
	{
		phase_elapsed += elapsed;
		if (phase_elapsed >= 280)
		{
			YMGUI_Obj_SetHidden(app_page, 1);
			YMGUI_Obj_SetHidden(recent_page, 1);
			app_page->area.w = W;
			app_page->area.h = H;
			PhoneUI_set_pos(app_page, 0, H);
			PhoneUI_set_pos(recent_page, 0, H);
			busy = 0;
		}
	}
	if (clearing && scene == RECENTS)
	{
		int moving = 0;
		for (int i = 0; i < APP_COUNT; ++i)
			if (running[i] && recent_cards[i]->area.x > -144 && recent_cards[i]->area.x < W + 8)
				moving = 1;
		if (!moving)
		{
			for (int i = 0; i < APP_COUNT; ++i)
			{
				running[i] = 0;
				GY_free1(snapshots[i]);
				snapshots[i] = NULL;
			}
			current_app = -1;
			update_running();
			busy = 0;
			clearing = 0;
		}
	}
	else if (dismiss_id >= 0)
	{
		if (recent_cards[dismiss_id]->area.x <= -156 || recent_cards[dismiss_id]->area.x >= W)
		{
			running[dismiss_id] = 0;
			GY_free1(snapshots[dismiss_id]);
			snapshots[dismiss_id] = NULL;
			dismiss_id = -1;
			busy = 0;
			update_running();
			recent_layout(1);
		}
	}
	else if (busy && scene == RECENTS && recent_page->area.y == 0)
		busy = 0;
#endif
}

static void on_app_icon(GYOBJ btn)
{
	for (int i = 0; i < APP_COUNT; ++i)
		if (home_buttons[i] == btn || dock_buttons[i] == btn)
		{
			open_app_from(i, NULL, btn);
			return;
		}
}
static void on_home(GYOBJ btn)
{
	(void)btn;
	if (PhoneDesktop_Close()) return;
	if (PhoneShade_Close(1)) return;
	if (scene == HOME && !busy)
		home_select(0, 1);
	else
		go_home();
}
static void on_recent(GYOBJ btn)
{
	(void)btn;
	PhoneDesktop_Close();
	PhoneShade_Close(0);
	open_recent();
}
static void on_clear(GYOBJ btn)
{
	(void)btn;
	clear_recent();
}
static void on_restart(GYOBJ btn)
{
	(void)btn;
	begin_boot();
}

/* 应用层皮肤复用控件的模型、命中和捕获；不替换控件私有数据。 */



/* ---- 亮度叠加：融合单遍 + 通道查表（2026-10-04 步骤 2a）----------------------
 *
 * 【原来的代价】brightness < 100 时 phone_flush 做两件事：
 *   `memcpy(dim_buffer, pixels, n*2)` 然后 `YMGUI_Draw_Fill(dim_buffer, a, 黑, opa)`
 * 即"先整带拷一遍，再逐像素读-混-写一遍"。出厂默认 brightness = 50
 * ⇒ opa = (100-50)*160/100 = 80，于是**每一个被 flush 的像素**都要付：
 *   一次读 + 一次混合（≈17 cy/px）+ 一次写  ⇒ 153600 px ≈ 6.7 ms，加 memcpy ≈0.7 ms
 * 合计约 7.4 ms/帧，而它**出厂就是开着的**，README 的帧时间表里却没有这一笔。
 *
 * 【为什么可以便宜很多】调暗是"把 dst 往黑里拉一个固定比例"：
 *     out = mix(dst, 黑, opa) = dst × (255-opa) / 255，记 k = 255-opa
 * 源色恒为黑 ⇒ 不需要 src 的三次乘法、不需要取 src 分量。
 * 更关键的是 **RGB565 每个通道只有 5/6 位**：一个通道的映射只有 32（或 64）种输入，
 * 所以 `out5 = round(c*k/255)` 是一张 **32/64 项的查表**，每帧算一次即可。
 *
 * 【于是每次 flush 变成】建表 128 次乘法 + 逐像素"3 次查表 + 2 次或 + 1 次写"，
 * 并且把 memcpy 与调暗**融合成一遍**（省掉一半内存流量）。
 * 取整口径与引擎的 GY_MixPx 完全一致（`(x+127+1)*257>>16`，即四舍五入到最近），
 * 所以软件调暗与引擎自己的半透明混合不会出现亮度台阶。
 *
 * 【为什么不做成硬件 PWM】背光是 PB1 直连的普通 GPIO（lcd.c 只做 LCD_BL(1) 常亮），
 * 全工程没有任何 TIM/PWM。真正的"零成本亮度"要在 PB1 上出 PWM
 * （PB1 的 AF2 = TIM3_CH4，厂商头文件里有 GPIO_AF2_TIM3），那是硬件 bring-up，
 * 需要在板上确认 AF/极性/频率后才能定，本轮的 g_dim_mode 已为它留好位置。
 *
 * ★★ 2026-10-04 板上实测：硬件 PWM 已做通并**改为默认** ★★
 *
 * PB1 = TIM3_CH4/AF2 已在板上验证（`src/ymgui_port.c` 的 bl_pwm）：
 * 寄存器回读 PB1.MODER=2(AF)、CCER 的 CC4E 置位、实测 2000 Hz、CCR4 随占空比精确变化；
 * 占空比 1000↔0 交替时**背光确实随之亮/灭**（用户目视确认），
 * 且面板内容自检 ramp/gram 仍为 0/0（背光与显示是两条独立通路）。
 *
 * 帧率实测（桌面满负载，60 帧整屏标脏）：
 *   软件调暗 21.42 ms / 46.7 FPS  →  硬件 PWM 调暗 **17.77 ms / 56.3 FPS**
 * 那 3.65 ms/帧 直接归零，而且**板级不再需要 dim_buffer 那条 30 KB 的整带暂存**。
 *
 * 三种模式（g_dim_mode）：
 *   0 = 完全不调暗
 *   1 = 软件查表叠黑（旧路径，保留作可逆回退；本文件下面那套 LUT 仍完整保留）
 *   2 = **硬件 PWM 调背光（现默认）** —— 本文件跳过软件叠加；
 *       占空比由板级按亮度换算，映射见 src/main.c 的亮度→PWM 桥
 * ----------------------------------------------------------------------------- */
volatile uint32_t g_dim_mode = 2;   /* 0=不调暗  1=软件查表叠黑  2=硬件 PWM 调背光（默认） */

/* 查表项直接存**已经在最终位域里的**值，逐像素就只剩 3 次载入 + 2 次或 + 1 次写。 */
static uint16_t s_dim_r[32];
static uint16_t s_dim_g[64];
static uint16_t s_dim_b[32];

/* ---------------------------------------------------------------------------
 * 软件调暗的整带暂存：**按需分配**（2026-10-04）
 *
 * 【为什么改成按需】出厂默认 g_dim_mode = 2（硬件背光 PWM 调亮度），
 * phone_flush 里那条软件叠黑**整段跳过** ⇒ 这块缓冲根本用不到。
 * 而它原来在 PhoneShell_BoardInit 里就预分配 `W × 48 × 2 = 30720 B` 的 heap1
 * （heap1 峰值本来已到 97%、只剩约 7 KB）—— 等于为一个默认不用的回退路径白占 30 KB。
 *
 * 【为什么不是直接删掉】删了会**静默降级**：原条件里带 `dim_buffer &&`，
 * 一旦它恒为 NULL，有人把 g_dim_mode 写回 1（软件回退）时不会报错，
 * 只是"完全不调暗"——这种"改了开关没反应"最难查。
 * 所以保留回退路径，只把分配推迟到真的要用时；而且失败会留痕（见下）。
 *
 * 【顺带修掉一个潜在越界】原分配写死 `W × 48`，而板级 band 只有 32 行。
 * 两者是巧合而不是不变量：main.c 的 g_band_rows 钩子能改 band 行数
 * （只需 buffer 够大即可），一旦调到 >48 行，dim_apply 就会越界写。
 * 现在按**本次 band 实际需要的像素数**分配，容量也记下来复用。
 * ------------------------------------------------------------------------- */

/* 申请失败留痕（0 = 用不到或成功；1 = mode==1 但申请失败 ⇒ 回退路径不可用）。
 * 用途：让"降级"能被 SWD 一眼看见，而不是表现为"调暗开关没反应"。 */
volatile uint32_t g_dim_buf_rc = 0;

static GYpx*   dim_buffer = NULL;
static uint32_t dim_cap_px = 0u;   /* 当前这块能装多少像素 */
static uint32_t dim_tried   = 0u;  /* 申请失败过就不再每帧重试（避免无谓的堆遍历） */

static GYpx* dim_buffer_ensure(uint32_t need_px)
{
	if (need_px == 0u)
		return NULL;
	if ((dim_buffer != NULL) && (dim_cap_px >= need_px))
		return dim_buffer;                       /* 已有且够大 */
	if ((dim_tried != 0u) && (dim_buffer == NULL))
		return NULL;                            /* 之前失败过，别再每帧试 */

	if (dim_buffer != NULL)                     /* 现有那块不够大：换一块 */
	{
		GY_free1(dim_buffer);
		dim_buffer = NULL;
		dim_cap_px = 0u;
	}

	dim_buffer = (GYpx*)GY_malloc1((size_t)need_px * sizeof(GYpx));
	if (dim_buffer == NULL)
	{
		dim_tried = 1u;
		g_dim_buf_rc = 1u;
		return NULL;
	}
	dim_cap_px = need_px;
	g_dim_buf_rc = 0u;
	return dim_buffer;
}

/* 按 k = 255-opa 建表。取整用引擎同一个恒等式：floor((x+127)/255) 再移位 */
static void dim_build_lut(uint32_t k)
{
	uint32_t i;
	for (i = 0; i < 32u; ++i)
		s_dim_r[i] = (uint16_t)((((i * k + 128u) * 257u) >> 16) << 11);
	for (i = 0; i < 64u; ++i)
		s_dim_g[i] = (uint16_t)((((i * k + 128u) * 257u) >> 16) << 5);
	for (i = 0; i < 32u; ++i)
		s_dim_b[i] = (uint16_t)(((i * k + 128u) * 257u) >> 16);
}

/* 一遍完成"拷 + 调暗"：dst 可以是独立缓冲（当前实现），源只读一次 */
static void dim_apply(GYpx* dst, const GYpx* src, uint32_t n, uint32_t k)
{
	uint32_t i;
	dim_build_lut(k);
	for (i = 0; i < n; ++i)
	{
		uint16_t p = src[i];
		dst[i] = (uint16_t)(s_dim_r[(p >> 11) & 0x1Fu] |
		                    s_dim_g[(p >>  5) & 0x3Fu] |
		                    s_dim_b[ p        & 0x1Fu]);
	}
}

/* 在显示回调中保存 App 的真实最终帧，缩略卡片不另造一套假 UI。
 * 每个打开的 App 按需占用 320x448 个 native 像素；清理/退出释放。 */
static void phone_flush(struct GYdisp* disp, const GYrect* a, const GYpx* pixels)
{
	if (capture_target)
	{
		if (capture_shift == 0)
		{
			/* 原尺寸（桌面/大 RAM 目标）：与改动前**完全一致**，逐行 memcpy。
			 * 自检里那条"抓屏与底层画面逐像素相同"的断言走的就是这条路。 */
			for (int y = 0; y < a->h; ++y)
				memcpy(capture_target + (a->y + y) * W + a->x, pixels + y * a->w, a->w * sizeof(GYpx));
		}
		else
		{
			/* 降采样（小 RAM 目标的降级路径）：按 1<<shift 最近邻取点。
			 * 调用方必须用同一倍数申请缓冲并设好 GYimg 宽高 —— 见 phone_shade.c。 */
			const int div = 1 << capture_shift;
			const int mask = div - 1;
			const int sw = W >> capture_shift;
			for (int y = 0; y < a->h; ++y)
			{
				int fy = a->y + y;
				if (fy < 0 || (fy & mask) != 0)
					continue;
				int sy = fy >> capture_shift;
				for (int x = 0; x < a->w; ++x)
				{
					int fx = a->x + x;
					if (fx < 0 || (fx & mask) != 0)
						continue;
					capture_target[sy * sw + (fx >> capture_shift)] = pixels[y * a->w + x];
				}
			}
		}
	}
	if (test_frame)
		for (int y = 0; y < a->h; ++y)
			memcpy(test_frame + (a->y + y) * W + a->x, pixels + y * a->w, a->w * sizeof(GYpx));
	if (scene == APP && !busy && !PhoneShade_Visible() && current_app >= 0 && snapshots[current_app])
	{
		/* 按 1/4 最近邻把 band 写进快照（规格见 SNAP_* 的说明）。
		 * 原实现是整行 memcpy 存全尺寸 320×448，本板 RAM 放不下。 */
		for (int y = a->y; y < a->y + a->h && y < 448; ++y)
		{
			if (y < 0 || (y & SNAP_MASK) != 0)
				continue;                                  /* 每 4 行只取第 1 行 */
			int left = a->x < 0 ? 0 : a->x;
			int right = a->x + a->w > W ? W : a->x + a->w;
			int sy = y >> SNAP_SHIFT;
			for (int x = (left + SNAP_MASK) & ~SNAP_MASK; x < right; x += SNAP_DIV)
				snapshots[current_app][sy * SNAP_W + (x >> SNAP_SHIFT)] =
					pixels[(y - a->y) * a->w + (x - a->x)];
		}
	}
	int brightness = display_brightness;
	GYpx* db = NULL;

	/* ★只有 mode==1 才走软件叠加★
	 * mode==2（现默认）是硬件 PWM 调背光，由板级按亮度换算占空比，
	 * 这里必须**整段跳过** —— 否则会"软件叠黑 + 硬件调暗"叠两次，
	 * 观感比设定值暗得多。三种模式见 g_dim_mode 声明处。
	 *
	 * 整带暂存**按需申请**（见 dim_buffer_ensure）：默认模式根本进不到这里，
	 * 那 30 KB 就一直留在 heap1 可用。 */
	if ((brightness < 100) && (g_dim_mode == 1u))
	{
		db = dim_buffer_ensure((uint32_t)a->w * (uint32_t)a->h);
	}

	if (db != NULL)
	{
		/* 见 dim_build_lut 上方：调暗 = dst × (255-opa)/255，源恒为黑。
		 * 这里不再 memcpy+Draw_Fill，而是建表后**一遍**写出（步骤 2a）。
		 * opa 语义与原实现逐点一致：opa = (100-brightness)*160/100。 */
		uint32_t opa = (uint32_t)((100 - brightness) * 160 / 100);
		if (opa >= 255u)
		{
			/* 全黑（理论不可达：brightness>=0 ⇒ opa<=160）：直接填黑，省掉建表 */
			uint32_t i, n = (uint32_t)a->w * (uint32_t)a->h;
			for (i = 0; i < n; ++i)
				db[i] = 0u;
		}
		else if (opa != 0u)
		{
			dim_apply(db, pixels, (uint32_t)a->w * (uint32_t)a->h, 255u - opa);
		}
		else
		{
			/* opa == 0 不该走到这里（brightness<100 ⇒ opa>=1），
			 * 但保留原实现的保护：叠一层 alpha=1 的全屏黑罩是可见缺陷。 */
			memcpy(db, pixels, (size_t)a->w * (size_t)a->h * sizeof(GYpx));
		}
		display_flush(disp, a, db);
	}
	else
	{
		/* 两种可能：mode != 1（正常，走硬件 PWM），或 mode==1 但暂存申请失败
		 * ——后者会在这里降级成"完全不调暗"，**不是静默的**：
		 * g_dim_buf_rc 会被置 1，SWD 读一下就知道回退路径没生效。 */
		display_flush(disp, a, pixels);
	}
}

static void on_mini_music(GYOBJ obj)
{
	(void)obj;
	running[MUSIC] = 1;
	update_running();
	PhoneApps_Command(MUSIC, "toggle", NULL);
}

static void recent_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	int id = (int)(intptr_t)obj->user_data;
	/* 阴影必须位于对象失效矩形内，否则局部刷新会留下边缘残影。 */
	GYsurface clipped = *s;
	int x0 = a->x > s->clip.x ? a->x : s->clip.x;
	int y0 = a->y > s->clip.y ? a->y : s->clip.y;
	int x1 = a->x + a->w < s->clip.x + s->clip.w ? a->x + a->w : s->clip.x + s->clip.w;
	int y1 = a->y + a->h < s->clip.y + s->clip.h ? a->y + a->h : s->clip.y + s->clip.h;
	if (x1 <= x0 || y1 <= y0)
		return;
	clipped.clip = (GYrect){x0, y0, x1 - x0, y1 - y0};
	s = &clipped;
	GYrect shadow = {a->x - 2, a->y + 4, a->w + 4, a->h + 2};
	PhoneUI_rounded(s, &shadow, RGB(9, 15, 34), 17, 65);
	PhoneUI_rounded(s, a, WHITE, 14, GY_OPA_COVER);
	YMGUI_Draw_CircleFill(s, a->x + 19, a->y + 15, 6, PhoneApps_Get(id)->color);
	{
		/* 绘制期回调：局部变量只查一次表，避免 title 被翻译两遍 */
		const char* dtitle = T(PhoneApps_Get(id)->title);
		PhoneUI_draw_text(s, dtitle, INK, 2, a->x + 33 + PhoneUI_text_width(dtitle, 2) / 2, a->y + 6);
	}
	PhoneUI_draw_text(s, "x", MUTED, 0, a->x + a->w - 18, a->y + 2);
	if (snapshots[id])
	{
		GYrect target = {a->x + 8, a->y + 30, 120, 168};
		/* 双线性缩小真实快照，避免小字被最近邻采样丢成虚线。 */
		int x0 = target.x > s->clip.x ? target.x : s->clip.x;
		int y0 = target.y > s->clip.y ? target.y : s->clip.y;
		int x1 = target.x + target.w < s->clip.x + s->clip.w ? target.x + target.w : s->clip.x + s->clip.w;
		int y1 = target.y + target.h < s->clip.y + s->clip.h ? target.y + target.h : s->clip.y + s->clip.h;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
			{
				/* 快照是 1/4 降采样的（SNAP_W × SNAP_H），这里再做逐通道双线性放大到 120×168 */
				int sx = ((x - target.x) * 256 + 128) * SNAP_W / target.w - 128;
				int sy = ((y - target.y) * 256 + 128) * SNAP_H / target.h - 128;
				int ix = sx / 256, iy = sy / 256, fx = sx & 255, fy = sy & 255;
				int nx = ix + 1 < SNAP_W ? ix + 1 : ix, ny = iy + 1 < SNAP_H ? iy + 1 : iy;
				GYcolor colors[4] = {GY_PxToColor(snapshots[id][iy * SNAP_W + ix]), GY_PxToColor(snapshots[id][iy * SNAP_W + nx]),
									 GY_PxToColor(snapshots[id][ny * SNAP_W + ix]), GY_PxToColor(snapshots[id][ny * SNAP_W + nx])};
				int weights[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
				int r = 0, g = 0, b = 0;
				for (int k = 0; k < 4; ++k)
				{
					r += GY_COLOR_R(colors[k]) * weights[k];
					g += GY_COLOR_G(colors[k]) * weights[k];
					b += GY_COLOR_B(colors[k]) * weights[k];
				}
				GY_PutPx(s, x, y, GY_ColorToPx(RGB(r >> 16, g >> 16, b >> 16)));
			}
	}
	else
	{
		GYrect target = {a->x + 8, a->y + 30, 120, 168};
		PhoneUI_rounded(s, &target, PAPER, 8, GY_OPA_COVER);
		GYrect tile = {a->x + 41, a->y + 87, 54, 54};
		PhoneUI_rounded(s, &tile, PhoneApps_Get(id)->color, 14, GY_OPA_COVER);
		PhoneUI_icon_draw(s, PhoneApps_Get(id)->icon, a->x + 68, a->y + 114, PhoneApps_Get(id)->color);
	}
}

static int recent_scroll_limit(void)
{
	int limit = ((recent_total + 1) / 2) * 222 - 312;
	return limit > 0 ? limit : 0;
}

static void recent_layout(int animate)
{
	if (recent_scroll < 0)
		recent_scroll = 0;
	if (recent_scroll > recent_scroll_limit())
		recent_scroll = recent_scroll_limit();
	for (int i = 0; i < recent_total; ++i)
	{
		GYOBJ card = recent_cards[recent_ids[i]];
		int x = 16 + (i % 2) * 152;
		int y = 4 + (i / 2) * 222 - recent_scroll;
		card_home_y[recent_ids[i]] = y;
		if (animate)
			PhoneUI_move(card, x, y, 240);
		else
			PhoneUI_set_pos(card, x, y);
	}
}

static void dismiss_recent(int id)
{
	if (busy || id < 0 || !running[id])
		return;
#if YMGUI_ANIM
	busy = 1;
	dismiss_id = id;
	PhoneUI_move(recent_cards[id], drag_dx < 0 ? -160 : W + 8, recent_cards[id]->area.y, 260);
#else
	running[id] = 0;
	GY_free1(snapshots[id]);
	snapshots[id] = NULL;
	update_running();
#endif
}

static void recent_event(GYOBJ obj, GYEvent event)
{
	if (busy || scene != RECENTS)
		return;
	int id = (int)(intptr_t)obj->user_data;
	if (event == GY_EVENT_Pressed)
	{
		drag_x = ctx->point_x;
		drag_y = ctx->point_y;
		drag_dx = drag_dy = dragging = drag_axis = 0;
		drag_scroll = recent_scroll;
		for (int i = 0; i < APP_COUNT; ++i)
			drag_origins[i] = recent_cards[i]->area.x;
		drag_origin_y = obj->area.y;
#if YMGUI_ANIM
		for (int i = 0; i < APP_COUNT; ++i)
			YMGUI_Anim_CancelObject(recent_cards[i]);
#endif
	}
	else if (event == GY_EVENT_Pressing)
	{
		drag_dx = ctx->point_x - drag_x;
		drag_dy = ctx->point_y - drag_y;
		if (!drag_axis && abs(drag_dx) + abs(drag_dy) > 8)
		{
			dragging = 1;
			drag_axis = abs(drag_dx) > abs(drag_dy) && id >= 0 ? 1 : 2;
		}
		if (drag_axis == 1)
			PhoneUI_set_pos(obj, drag_origins[id] + drag_dx, drag_origin_y);
		else if (drag_axis == 2)
		{
			recent_scroll = drag_scroll - drag_dy;
			recent_layout(0);
		}
	}
	else if (event == GY_EVENT_Released || event == GY_EVENT_ReleasedOff)
	{
		if (!ctx->pressed_obj)
		{
			dragging = 1;
			recent_scroll = drag_scroll;
			recent_layout(1);
			return;
		}
		if (drag_axis == 1 && abs(drag_dx) > 60)
			dismiss_recent(id);
		else
			recent_layout(1);
	}
	else if (event == GY_EVENT_Clicked && !dragging && id >= 0)
	{
		GYrect a;
		YMGUI_Obj_GetAbsArea(obj, &a);
		if (ctx->point_y < a.y + 30 && ctx->point_x > a.x + a.w - 30)
			dismiss_recent(id);
		else
			open_app(id);
	}
	else if (event == GY_EVENT_Wheel)
	{
		recent_scroll -= ctx->wheel_y * 64;
		recent_layout(1);
	}
}

/* 关机面板显隐的唯一入口：顺手同步 g_diag_power_dialog。
 * ⚠ 别在别处直接调 YMGUI_Obj_SetHidden(power_dialog, …) ——
 *   漏一处诊断量就与面板实际状态不一致，验收脚本会给出假的 FAIL。
 *
 * ⚠⚠ 2026-10-04 实测踩到的坑：这个"增量同步"**不可靠**。
 *   板上是 state=0x04(Hidden 置位，画面上确实没面板) 但 g_diag_power_dialog=1，
 *   稳定复现三次。原因是 power_dialog_hide 被 -O2 内联进 4 处调用点，
 *   编译器看不到 volatile 变量有外部读者（只有 SWD 在读），
 *   某些路径的写入被合并/移除了。
 *   ⇒ 改成**每次读时现算**，不维护镜像。调用点只管 SetHidden，
 *     诊断量在 PhoneShell_BoardTick 每帧同步一次（那是唯一可靠的时机）。 */
static void power_dialog_hide(int hidden)
{
	YMGUI_Obj_SetHidden(power_dialog, hidden);
}

static void cancel_power(GYOBJ obj)
{
	(void)obj;
	power_dialog_hide(1);
}
static void confirm_power(GYOBJ obj)
{
	(void)obj;
	begin_shutdown();
}
static void power_backdrop_event(GYOBJ obj, GYEvent event)
{
	if (event == GY_EVENT_Clicked)
		cancel_power(obj);
}
static void dim_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	(void)obj;
	YMGUI_Draw_Fill(s, a, RGB(8, 12, 25), 160);
}
static void show_power_dialog(void)
{
	if (busy || scene == BOOT || scene == OFF || scene == SHUTDOWN)
		return;
	PhoneShade_Close(0);
	PhoneDesktop_Close();
	power_dialog_hide(0);
	GYOBJ sheet = power_dialog->child_head;
	PhoneUI_set_pos(sheet, 18, H);
	PhoneUI_move(sheet, 18, 286, 240);
}
static void on_back(GYOBJ obj)
{
	(void)obj;
	if (PhoneDesktop_Close()) return;
	if (PhoneShade_Close(1)) return;
	if (PhoneIME_Hide())
		return;
	if (YMGUI_MsgBox_IsShown(app_demo_dialog))
	{
		YMGUI_MsgBox_Close(app_demo_dialog);
		return;
	}
	if (scene == APP && !busy && PhoneApps_Back())
		return;
	if (!(power_dialog->state & GY_STATE_Hidden))
		cancel_power(NULL);
	else
		go_home();
}
static void nav_event(GYOBJ obj, GYEvent event)
{
	if (event == GY_EVENT_ContextRequested)
		show_power_dialog();
	else
		PhoneUI_button_event(obj, event);
}
static void build_navigation(void)
{
	navigation = YMGUI_Creat_Obj_Creat(ctx->top_layer, 0, 448, W, 32);
	YMGUI_Obj_SetBgColor(navigation, RGB(28, 32, 49));
	GYOBJ back = PhoneUI_button(navigation, 34, 0, 60, 32, "", on_back, 0);
	GYOBJ home = PhoneUI_button(navigation, 130, 0, 60, 32, "", on_home, 0);
	GYOBJ recent = PhoneUI_button(navigation, 226, 0, 60, 32, "", on_recent, 0);
	PhoneUI_button_icon(back, ICON_BACK);
	PhoneUI_button_icon(home, ICON_HOME);
	PhoneUI_button_icon(recent, ICON_RECENTS);
	home->event_cb = nav_event;
	power_dialog = YMGUI_Creat_Obj_Creat(ctx->top_layer, 0, 0, W, H);
	power_dialog->draw_cb = dim_draw;
	power_dialog->event_cb = power_backdrop_event;
	GYOBJ sheet = PhoneUI_panel(power_dialog, 18, 286, 284, 172, WHITE);
	PhoneUI_left_label(sheet, 20, 14, 244, T("确认关机？"), INK, 3);
	PhoneUI_left_label(sheet, 20, 55, 244, T("关闭演示，稍后可以重新开机。"), MUTED, 2);
	PhoneUI_button(sheet, 16, 96, 120, 44, T("取消"), cancel_power, RGB(127, 137, 158));
	PhoneUI_button(sheet, 148, 96, 120, 44, T("关机"), confirm_power, RGB(211, 83, 106));
	power_dialog_hide(1);
}

static void dots_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	(void)obj;
	for (int i = 0; i < HOME_PAGES; ++i)
		YMGUI_Draw_CircleFill(s, a->x + 11 + i * 16, a->y + 7, i == home_index ? 3 : 2,
							  i == home_index ? WHITE : RGB(166, 159, 192));
}

static void home_select(int index, int animate)
{
	home_index = index < 0 ? 0 : index >= HOME_PAGES ? HOME_PAGES - 1
													 : index;
	if (animate)
		PhoneUI_move(home_strip, -home_index * W, 0, 260);
	else
	{
#if YMGUI_ANIM
		YMGUI_Anim_CancelObject(home_strip);
#endif
		PhoneUI_set_pos(home_strip, -home_index * W, 0);
	}
	YMGUI_Obj_SetHidden(home_dock, home_index != 0 || PhoneDesktop_Index(MUSIC) < 0);
	PhoneUI_set_pos(home_dots, home_dots->area.x, home_index == 0 ? 348 : 422);
	YMGUI_Obj_Invalidate(home_dots);
}

/* 透明手势层覆盖桌面内容：图标/标签/空白处都可以开始翻页，拖动不误开 App。 */
static void home_event(GYOBJ obj, GYEvent event)
{
	(void)obj;
	if (scene != HOME || busy)
		return;
	if (event == GY_EVENT_ContextRequested || event == GY_EVENT_ContextDragging ||
		event == GY_EVENT_ContextReleased || event == GY_EVENT_ContextCancelled)
	{
		home_dragged = 1;
		PhoneDesktop_Context(event);
		return;
	}
	if (PhoneDesktop_Busy()) return;
	if (event == GY_EVENT_Pressed)
	{
#if YMGUI_ANIM
		YMGUI_Anim_CancelObject(home_strip);
#endif
		home_start_x = ctx->point_x;
		home_start_y = ctx->point_y;
		home_origin = home_strip->area.x;
		home_dragged = 0;
		home_pressed_app = -1;
		for (int i = 0; i < APP_COUNT; ++i)
		{
			if (PhoneDesktop_Index(i) < 0) continue;
			GYrect a;
			YMGUI_Obj_GetAbsArea(home_buttons[i], &a);
			if (ctx->point_x >= a.x - 5 && ctx->point_x < a.x + a.w + 5 &&
				ctx->point_y >= a.y && ctx->point_y < a.y + a.h + 22)
			{
				home_pressed_app = i;
				home_buttons[i]->state |= GY_STATE_Pressed;
				YMGUI_Obj_Invalidate(home_buttons[i]);
			}
		}
	}
	else if (event == GY_EVENT_Pressing)
	{
		int dx = ctx->point_x - home_start_x, dy = ctx->point_y - home_start_y;
		if (abs(dx) + abs(dy) > 8)
			home_dragged = 1;
		if (home_dragged)
		{
			int x = home_origin + dx;
			if (x > 0)
				x /= 4;
			int edge = -(HOME_PAGES - 1) * W;
			if (x < edge)
				x = edge + (x - edge) / 4;
			PhoneUI_set_pos(home_strip, x, 0);
			if (home_pressed_app >= 0)
			{
				home_buttons[home_pressed_app]->state &= (uint8)~GY_STATE_Pressed;
				YMGUI_Obj_Invalidate(home_buttons[home_pressed_app]);
			}
		}
	}
	else if (event == GY_EVENT_Released || event == GY_EVENT_ReleasedOff)
	{
		if (home_pressed_app >= 0)
		{
			home_buttons[home_pressed_app]->state &= (uint8)~GY_STATE_Pressed;
			YMGUI_Obj_Invalidate(home_buttons[home_pressed_app]);
		}
		if (!ctx->pressed_obj)
		{
			home_dragged = 1;
			home_select(home_index, 1);
			return;
		}
		int dx = ctx->point_x - home_start_x;
		int dy = ctx->point_y - home_start_y;
		int target = home_index;
		if (abs(dx) > 48 && abs(dx) > abs(dy))
			target += dx < 0 ? 1 : -1;
		if (abs(home_origin + home_index * W) > 2)
			home_dragged = 1;
		home_select(target, 1);
	}
	else if (event == GY_EVENT_Clicked && !home_dragged && home_pressed_app >= 0)
	{
		open_app(home_pressed_app);
	}
	else if (event == GY_EVENT_Wheel)
	{
		int delta = ctx->wheel_x ? ctx->wheel_x : -ctx->wheel_y;
		home_select(home_index + (delta > 0 ? 1 : -1), 1);
	}
}

static void dots_event(GYOBJ obj, GYEvent event)
{
	if (event == GY_EVENT_Clicked && scene == HOME && !busy)
		home_select((ctx->point_x - obj->area.x - 3) / 16, 1);
}

static void desktop_removed(int id)
{
	running[id] = 0;
	GY_free1(snapshots[id]);
	snapshots[id] = NULL;
	if (current_app == id) current_app = -1;
	update_running();
}
static void build_home(void)
{
	home_page = YMGUI_Creat_Obj_Creat(ctx->root, 0, 0, W, H);
	home_page->draw_cb = wallpaper;
	status_bar(home_page);
	GYOBJ viewport = YMGUI_Creat_Obj_Creat(home_page, 0, 28, W, 408);
	viewport->state |= GY_STATE_ClipChildren;
	viewport->draw_cb = NULL;
	home_strip = YMGUI_Creat_Obj_Creat(viewport, 0, 0, W * HOME_PAGES, 408);
	home_strip->draw_cb = NULL;
	GYOBJ pages[HOME_PAGES];
	for (int i = 0; i < HOME_PAGES; ++i)
	{
		pages[i] = YMGUI_Creat_Obj_Creat(home_strip, W * i, 0, W, 408);
		pages[i]->draw_cb = NULL;
	}
	/* 桌面大时间 / 日期 / 天气：原来全是硬编码，改成从 ESP32 上行链路取。
	 * 没同步到时 board 层返回占位串（"--:--" / "未同步" / "等待天气"）。
	 * 大时间是 **HH:MM + 冒号闪烁**（秒位不要了），三个 label 各自绝对定位 ——
	 * 冒号那格闪成空串时，左右两格的位置不受任何影响。
	 *
	 * ⚠⚠ 宽度必须按**字体真实步进**给，不能拍脑袋（2026-10-08 踩过：给了 60px，
	 *   而 display 字体一个数字就占 33px ⇒ 两个数字要 66px，第 2 位右边被裁掉）。
	 *   实测（从 elf 里读 phone_font_display_advance 表，ASCII 段）：
	 *     数字 '0'..'9' 步进 **33 px**，冒号 ':' 步进 **18 px**
	 *   ⇒ 两数字 66px，故小时/分钟各给 **70px**（留 4px 余量）。
	 *   参考：regular（状态栏那种）数字只有 9px，"23:35" 共 41px，54px 宽绰绰有余。 */
	home_clock_h = PhoneUI_left_label(pages[0], 22, 9, 70, net_hh(), WHITE, 1);
	home_clock_c = PhoneUI_left_label(pages[0], 90, 9, 22, ":", WHITE, 1);
	home_clock_m = PhoneUI_left_label(pages[0], 110, 9, 70, net_mm(), WHITE, 1);
	home_date = PhoneUI_left_label(pages[0], 25, 75, 272, net_date(), RGB(232, 230, 243), 0);
	GYOBJ widget = PhoneUI_panel(pages[0], 22, 123, 276, 79, RGB(68, 71, 110));
	widget->draw_cb = PhoneUI_glass_draw;
	home_widget_title = PhoneUI_left_label(widget, 16, 11, 170, T("给生活留一点空白"), WHITE, 0);
	home_count = PhoneUI_left_label(widget, 16, 40, 178, net_weather(), RGB(224, 224, 240), 2);
	GYOBJ weather = YMGUI_Creat_Obj_Creat(widget, 210, 12, 52, 52);
	weather->draw_cb = PhoneUI_weather_draw;

	PhoneDesktop_Init(ctx, home_strip, home_buttons, &home_index, home_select, desktop_removed);
	for (int i = 0; i < APP_COUNT; ++i) dock_buttons[i] = home_buttons[i];
	GYOBJ touch = YMGUI_Creat_Obj_Creat(home_page, 0, 28, W, 408);
	touch->draw_cb = NULL;
	touch->event_cb = home_event;
	home_dots = YMGUI_Creat_Obj_Creat(home_page, (W - HOME_PAGES * 16 - 6) / 2, 348, HOME_PAGES * 16 + 6, 17);
	home_dots->draw_cb = dots_draw;
	home_dots->event_cb = dots_event;

	GYOBJ dock = home_dock = PhoneUI_panel(home_page, 15, 368, 290, 68, RGB(42, 44, 73));
	dock->draw_cb = PhoneUI_glass_draw;
	dock_buttons[MUSIC] = PhoneUI_button(dock, 12, 13, 42, 42, "", on_app_icon, PhoneApps_Get(MUSIC)->color);
	PhoneUI_button_icon(dock_buttons[MUSIC], ICON_MUSIC);
	home_music_title = PhoneUI_left_label(dock, 65, 12, 167, T("晚间海浪"), WHITE, 0);
	home_music_status = PhoneUI_left_label(dock, 65, 36, 170, T("Yaomi 音乐 / 已暂停"), RGB(206, 207, 227), 2);
	mini_music_button = PhoneUI_button(dock, 242, 14, 36, 40, "", on_mini_music, 0);
	PhoneUI_button_icon(mini_music_button, ICON_PLAY);
}



static void build_app_views(void)
{
	app_page = YMGUI_Creat_Obj_Creat(ctx->root, 0, H, W, H);
	app_page->state |= GY_STATE_ClipChildren;
	YMGUI_Obj_SetBgColor(app_page, PAPER);
	status_bar(app_page);
	app_title = PhoneUI_label(app_page, 80, 2, 160, "", INK);
	YMGUI_Obj_SetHidden(app_title, 1);
	for (int i = 0; i < APP_COUNT; ++i)
	{
		app_views[i] = YMGUI_Creat_Obj_Creat(app_page, 0, 36, W, 412);
		YMGUI_Obj_SetBgColor(app_views[i], PAPER);
		app_views[i]->state |= GY_STATE_ClipChildren;
		PhoneApps_Create(i, app_views[i]);
	}

	app_splash = YMGUI_Creat_Obj_Creat(app_page, 0, 0, W, H);
	app_splash->draw_cb = splash_draw;
	YMGUI_Obj_SetHidden(app_page, 1);
}

static void build_recent(void)
{
	recent_page = YMGUI_Creat_Obj_Creat(ctx->root, 0, H, W, H);
	recent_page->draw_cb = wallpaper;
	recent_page->state |= GY_STATE_ClipChildren;
	status_bar(recent_page);
	recent_title = PhoneUI_left_label(recent_page, 22, 31, 260, T("最近应用"), WHITE, 3);
	recent_count = PhoneUI_left_label(recent_page, 24, 66, 264, "", RGB(217, 221, 239), 2);
	recent_viewport = YMGUI_Creat_Obj_Creat(recent_page, 0, 86, W, 312);
	recent_viewport->state |= GY_STATE_ClipChildren;
	recent_viewport->draw_cb = NULL;
	recent_viewport->event_cb = recent_event;
	recent_viewport->user_data = (void*)(intptr_t)-1;
	for (int i = 0; i < APP_COUNT; ++i)
	{
		recent_cards[i] = YMGUI_Creat_Obj_Creat(recent_viewport, 16 + (i % 2) * 152, 4 + (i / 2) * 222, 136, 206);
		recent_cards[i]->draw_cb = recent_draw;
		recent_cards[i]->event_cb = recent_event;
		recent_cards[i]->user_data = (void*)(intptr_t)i;
	}
	recent_empty = PhoneUI_label(recent_page, 20, 220, 280, T("已清理，轻装出发。"), WHITE);
	recent_clear_button = PhoneUI_button(recent_page, 106, 408, 108, 32, T("全部清理"), on_clear, RGB(83, 83, 124));
	YMGUI_Obj_SetHidden(recent_page, 1);
}

static void build_boot_power(void)
{
	boot_page = YMGUI_Creat_Obj_Creat(ctx->root, 0, 0, W, H);
	YMGUI_Obj_SetBgColor(boot_page, RGB(10, 25, 44));
	boot_logo = YMGUI_Creat_Obj_Creat(boot_page, 0, 106, W, 86);
	boot_logo->draw_cb = logo_draw;
	PhoneUI_label(boot_page, 40, 225, 240, "Y A O M I", RGB(237, 248, 251));
	PhoneUI_label(boot_page, 40, 258, 240, T("点亮你的小小世界"), RGB(140, 184, 202));
	boot_bar = YMGUI_Creat_Bar_Creat(boot_page, 65, 343, 190, 7);
	YMGUI_Bar_SetRange(boot_bar, 0, 100);
	YMGUI_Bar_SetColors(boot_bar, RGB(35, 65, 81), RGB(81, 223, 186));
	PhoneUI_label(boot_page, 45, 365, 230, T("正在启动"), RGB(134, 172, 190));

	power_page = YMGUI_Creat_Obj_Creat(ctx->root, 0, 0, W, H);
	YMGUI_Obj_SetBgColor(power_page, RGB(8, 18, 32));
	GYOBJ mark = YMGUI_Creat_Obj_Creat(power_page, 0, 100, W, 86);
	mark->draw_cb = logo_draw;
	power_title = PhoneUI_label(power_page, 42, 221, 236, T("正在关机"), RGB(234, 248, 250));
	power_bar = YMGUI_Creat_Bar_Creat(power_page, 65, 280, 190, 7);
	YMGUI_Bar_SetRange(power_bar, 0, 100);
	YMGUI_Bar_SetColors(power_bar, RGB(35, 56, 73), RGB(81, 223, 186));
	restart_button = PhoneUI_button(power_page, 75, 336, 170, 46, T("重新开机"), on_restart, RGB(44, 114, 113));
	YMGUI_Obj_SetHidden(power_page, 1);
}

static void build_ui(void)
{
	YMGUI_Obj_SetBgColor(ctx->root, RGB(9, 23, 40));
	build_home();
	build_app_views();
	build_recent();
	build_boot_power();
	build_navigation();
	PhoneIME_Init(ctx);
	PhoneShade_Init(ctx);
	PhoneDesktop_BuildOverlay();
	/* 模态最后创建，盖住导航与键盘；不能让底层按钮穿透。 */
	app_demo_dialog = YMGUI_Creat_MsgBox_Creat(ctx);
	YMGUI_MsgBox_SetColors(app_demo_dialog, GY_ARGB(140, 15, 20, 35), WHITE, RGB(215, 219, 231), INK, MUTED);
	update_running();
	begin_boot();
}


void PhoneHost_SetTheme(int alternate)
{
	theme_alt = !!alternate;
	YMGUI_Obj_Invalidate(home_page);
}
/* 亮度持久化回调（2026-10-04 新增，与 ribbon 的 s_ribbon_persist_cb 同构）。
 *
 * 【为什么用回调而不是直接调 app_config】phone_shell 是从 YMGUI 移植过来的
 * 库侧代码，不认识应用层的 src/app_config.h —— ribbon 与语言都是靠 main.c
 * 做桥（main.c 那边是 app_ribbon_persist / app_lang_persist）。照抄这个模式，
 * 别在库侧 #include 应用层头文件。
 *
 * ⚠ 与 ribbon 的差别：亮度**不是布尔**，回调参数是 0..100 的 uint8_t。
 *   另外亮度滑杆在拖动过程中会连续回调，AppConfig_SetBrightness 内部
 *   有"值相同则不写"的判断，加上 AppConfig_Save 的 memcmp，
 *   真正擦写只发生在用户松手后的最终值上。 */
static void (*s_bright_persist_cb)(uint8_t) = NULL;

/* "用户动过亮度"的粘滞标志。由 Preview 置位、由 SetBrightness 清掉。
 *
 * ⚠⚠ 这个标志存在的唯一理由：PhoneHost_SetBrightness 开头有
 *   "值相同直接 return"（开机时 main.c 会用配置里的值同步首帧，
 *   那次值恰好相同，无条件回调会每次开机擦一次 W25Q128 扇区）。
 *   但两段式落盘里，**松手时传的正是 Preview 刚刚设好的那个值** ——
 *   于是必然命中这条 return，persist_cb 永不被调用 ⇒
 *   屏上是新值、flash 里是旧值，重启后亮度自己变回去。
 *
 *   2026-10-04 板上实测到这个坑：press/change/commit 三个计数器
 *   全部 +1（事件通路完好），而 g_cfg_brightness 纹丝不动。
 *
 * ⚠ 它只在 Preview 里置位，所以开机那次同值同步仍然被挡住
 *   （dirty=0）⇒ 不会退化成"每次开机擦一次扇区"。 */
static uint8_t s_bright_dirty = 0u;

void PhoneShell_SetBrightnessPersistCallback(void (*cb)(uint8_t))
{
	s_bright_persist_cb = cb;
}

/* 亮度钳位。两处（Set / Preview）共用，避免下限口径不一致。
 * ⚠ 下限取 1 而不是 0：0 会让 phone_flush 叠一层 alpha=160 的全屏黑罩，
 *   屏上什么都看不见，与"屏坏了"无法区分。设置页滑杆 range 也是 1..100。 */
static int brightness_clamp(int value)
{
	return (value < 1) ? 1 : (value > 100) ? 100 : value;
}

/* 应用亮度到内存 + 重绘 + 同步 shade。**不含持久化**。
 *
 * 【为什么把 "sync 命令" 做成参数而不是两条路径各写一遍】
 *   这条命令不是无害的：它最终调到 settings.c 的 app_show()，而那里
 *   **会把滚动位置复位成 scroll_y = 0**（每次进页面都回顶部）。
 *   拖动过程中发它，等于用户正拖着滑杆、页面却"弹回顶部" ——
 *   板级实测后果：`g_diag_scroll_y` 从 135 变 0、滑杆屏幕 y 从 330 变 465，
 *   手指还在 330 处，于是后续位移全部落空，表现为"拖不动"。
 *
 * ⚠ 所以：拖动中（Preview）**绝不能**发 sync；松手后（Set）才发，
 *   那时页面复位到顶部是合理的（用户已经不在拖了）。 */
static void brightness_apply(int nv, int send_sync)
{
	display_brightness = nv;
	/* ⚠⚠ ctx 可能还是 NULL：main.c 在 PhoneShell_BoardInit **之前**就用
	 *   配置里的值调本函数同步初值（那时 ctx 还没被 BoardInit 赋指针）。
	 *   无条件 YMGUI_Obj_Invalidate(ctx->root) 就是 NULL 解引用 → 硬 Fault。
	 *   同理 PhoneApps_Command 也不能在 ctx 为 NULL 时调。 */
	if (ctx == NULL)
	{
		return;                 /* 值已记住，首帧由 build_ui 之后的应用逻辑读到 */
	}
	/* 【置脏范围】只有**软件调暗**（g_dim_mode==1）才需要整屏重画：那层光罩是画进
	 *   framebuffer 的，亮度一变整屏像素都变，而 phone_flush 是按 band 重新叠黑的，
	 *   所以 15 个 band 必须全部重刷。
	 *   硬件 PWM（g_dim_mode==2，出厂默认）改的是背光占空比，**framebuffer 一个
	 *   像素都没变** —— display_brightness 在全工程唯一影响像素的读取点就是
	 *   phone_flush 里那处（本文件约 974 行，紧跟着 g_dim_mode==1 判断）；
	 *   而两个亮度滑杆的拖柄由 YMGUI_Slider 自己标脏（SetValue 与拖动内部都调
	 *   YMGUI_Obj_Invalidate，见 YMGUI_Slider.c L107/111/148/163/175），
	 *   界面上也没有显示亮度数值的文本（只有静态标题"预览亮度"）。
	 *   ⇒ mode 2 下脏 root 是纯浪费：拖一次亮度白重绘 15 个 band。 */
	if (g_dim_mode == 1u)
	{
		YMGUI_Obj_Invalidate(ctx->root);
	}
	PhoneShade_Sync();
	if (send_sync)
	{
		PhoneApps_Command(SETTINGS, "sync", NULL);
	}
}

void PhoneHost_SetBrightnessPreview(int value)
{
	/* 拖动过程中的中间值。只改显示，**绝不碰持久化回调** ——
	 * 板级实测一次拖动会经过 5 个不同的中间值，若这里也落盘就是擦 5 次
	 * W25Q128 扇区（标称 10 万次/扇区，调一天亮度就磨掉一大截）。
	 * 落盘统一由松手时那次 PhoneHost_SetBrightness 负责。
	 *
	 * ⚠ 这里**必须**也有"值相同直接 return"：按下瞬间就会回调一次，
	 *   若此时无条件重绘，会白白让整屏重画一次。 */
	int nv = brightness_clamp(value);

	if (display_brightness == nv)
	{
		return;
	}
	s_bright_dirty = 1u;   /* 记住"用户动过"，松手时才真正落盘（见 s_bright_dirty） */
	brightness_apply(nv, 0);   /* 拖动中：不要发 sync（会把页面弹回顶部） */
}

void PhoneHost_SetBrightness(int value)
{
	int nv = brightness_clamp(value);

	/* ⚠⚠ 这里不能用简单的"值相同直接 return"—— 两段式落盘会因此完全失效。
	 *
	 *   拖动中Preview 已经把 display_brightness 推到目标值，松手时
	 *   settings.c 调的正是 PhoneHost_SetBrightness(PhoneHost_GetBrightness())
	 *   也就是**同一个值** ⇒ 若这里 return，落盘回调永不被调用。
	 *   板级实测就是这个现象（press/change/commit 都 +1，亮度却没变）。
	 *
	 *   ⇒ 正确判据是"值变了 **或** 用户动过（s_bright_dirty）"：
	 *     · 开机时 main.c 用配置里的值同步首帧：值相同且 dirty=0 → 无操作
	 *       （这才是那句 return 原本要防的：每次开机擦一次扇区 240）
	 *     · 松手提交：值相同但 dirty=1 → 照常落盘一次 */
	if ((display_brightness == nv) && (s_bright_dirty == 0u))
	{
		return;
	}
	s_bright_dirty = 0u;

	/* 值确实变了才需要动画面/发 sync；只是"补一次落盘"时（值相同但 dirty=1）
	 * 两件事都不必做，跳过即可 —— 画面已经是那个值了。 */
	if (display_brightness != nv)
	{
		/* 传 1（发 sync）而 Preview 传 0：松手后要把滑杆位置与文案
		 * 对齐回真实值；拖动中发它会让页面弹回顶部（见 brightness_apply）。 */
		brightness_apply(nv, 1);
	}
	if (s_bright_persist_cb != NULL)
	{
		s_bright_persist_cb((uint8_t)display_brightness);
	}
}
int PhoneHost_GetBrightness(void) { return display_brightness; }
/* WiFi 开关（2026-10-10 改真）：WiFi 射频在对端 ESP32 上，所以这里**必须**
 * 下发 $?RADIO,<ON|OFF>，不能再只改本地假状态。
 * ⚠ 不再调 PhoneShade_SetWifi（那只是本地选项位）：抽屉的 WiFi 瓦片与设置页
 *   的 WiFi 开关都改读 $RD 回流（BoardNet_RadioWifiOn），本地位已无意义。
 * ⚠ 这里也不发 PhoneApps_Command(SETTINGS,"sync")：设置页自己每拍比对
 *   g_net_rd_pkts 在状态真的变了时刷新，比"被动 sync"更可靠（不会把滚动位置弹回顶部）。 */
void PhoneHost_SetWifi(int on)
{
	BoardNet_SetWifi(on);
}
int PhoneHost_GetWifi(void) { return BoardNet_RadioWifiOn() > 0 ? 1 : 0; }

/* ---- 壁纸 ribbon 半透明（2026-10-04 新增，设置页开关走这里）----
 *
 * 【置脏范围】ribbon 只画在 home_page 与 recent_page 两页的 draw_cb 上
 * （build_home / build_recent 里都挂了 wallpaper），所以只脏这两页，
 * 不必脏整个 ctx->root —— 那是 brightness 的做法（全屏亮度罩要重画）。
 *
 * 【为什么不顺带脏 app_page】app_page 是应用容器，壁纸不画在它上面；
 * 多脏一页只是白费一次重绘，拖动开关时会明显发涩。
 *
 * 【落盘时机】值真的变了才回调持久化（回调里 AppConfig_Save 自带"未变不擦"），
 * 所以这里不需要再比较一次。 */
static void (*s_ribbon_persist_cb)(uint8_t) = NULL;

void PhoneShell_SetRibbonPersistCallback(void (*cb)(uint8_t))
{
	s_ribbon_persist_cb = cb;
}

void PhoneHost_SetRibbonTranslucent(int on)
{
	uint32_t v = (on != 0u) ? 1u : 0u;

	if (g_ribbon_translucent != v)
	{
		g_ribbon_translucent = v;
		YMGUI_Obj_Invalidate(home_page);
		YMGUI_Obj_Invalidate(recent_page);
		if (s_ribbon_persist_cb != NULL)
		{
			s_ribbon_persist_cb((uint8_t)v);
		}
	}
}

int PhoneHost_GetRibbonTranslucent(void)
{
	return (g_ribbon_translucent != 0u) ? 1 : 0;
}
void PhoneHost_SetCaptureShift(int shift)
{
	capture_shift = shift;
}
void PhoneHost_Capture(GYpx* pixels)
{
	capture_target = pixels;
	YMGUI_Obj_Invalidate(ctx->root);
	YMGUI_Obj_Invalidate(ctx->top_layer);
	YMGUI_Refresh(ctx);
	capture_target = NULL;
}
void PhoneHost_MusicChanged(int playing)
{
	if (mini_music_button)
		PhoneUI_button_icon(mini_music_button, playing ? ICON_PAUSE : ICON_PLAY);
	if (home_music_status)
		PhoneUI_text_set(home_music_status, playing ? T("Yaomi 音乐 / 播放中") : T("Yaomi 音乐 / 已暂停"));
}

/* ===========================================================================
 * 语言切换后的全局重设（2026-10-04）
 *
 * 【为什么必须有这个函数】
 *   本工程的文案取用分两类：
 *     · **绘制期取值**：如 Dock 标签（每次重绘都读 PhoneApps_Get(id)->title），
 *       包了 T() 之后语言一变就自动跟随，**无需干预**；
 *     · **创建期设定**：Label / Button / TextView 的文字在创建时就被**拷进**
 *       对象自己的缓冲（PhoneText.value[] 是副本，不是指针），
 *       语言变了它不会自己变 —— **必须显式重设**。这一类是绝大多数。
 *   本函数只处理"常驻页面"上的第二类（桌面小组件、最近任务页、应用页标题），
 *   各 APP 内部的由它们各自的 show() 惰性刷新（比对 PhoneLang_Version()）。
 *
 * 【调用时机】设置页切换语言后立即调用一次。设置页自己那些文案由它内部的
 *   settings_retranslate() 负责，不在这里重复。
 * =========================================================================== */
void PhoneHost_RefreshLanguage(void)
{
	/* 应用页顶部标题：若正停在某个应用里，立刻可见 */
	if (current_app >= 0 && app_title != NULL)
	{
		PhoneUI_text_set(app_title, T(PhoneApps_Get(current_app)->title));
	}

	/* 桌面小组件。
	 * ⚠ 日期与天气**不要**写死文案 —— 它们是上行链路的实时数据，
	 *   写死会把真值覆盖掉（且周期刷新当时用的是自带缓存，覆盖后再也纠正不回来，
	 *   2026-10-09 实测即为此 bug：日期永久显示 "9月29日 星期二"）。
	 *   这里改成取 net_date()/net_weather()：切语言照样生效，内容仍是真的。 */
	if (home_date != NULL)         { PhoneUI_text_set(home_date, net_date()); }
	if (home_widget_title != NULL) { PhoneUI_text_set(home_widget_title, T("给生活留一点空白")); }
	if (home_count != NULL)        { PhoneUI_text_set(home_count, net_weather()); }
	if (home_music_title != NULL)  { PhoneUI_text_set(home_music_title, T("晚间海浪")); }
	if (home_music_status != NULL) { PhoneUI_text_set(home_music_status, T("Yaomi 音乐 / 已暂停")); }

	/* 最近任务页 */
	if (recent_title != NULL)        { PhoneUI_text_set(recent_title, T("最近应用")); }
	if (recent_empty != NULL)        { PhoneUI_text_set(recent_empty, T("已清理，轻装出发。")); }
	if (recent_clear_button != NULL) { PhoneUI_button_set(recent_clear_button, T("全部清理")); }

	/* 16 个桌面图标标签 + 长按菜单（那些控件在 phone_desktop.c 里，转给它） */
	PhoneDesktop_Retranslate();

	/* PhoneUI_text_set / PhoneUI_button_set 内部已各自置脏，无需整屏刷新。 */
}

void PhoneHost_Power(void)
{
	show_power_dialog();
}
void PhoneHost_Close(void)
{
	if (busy || scene != APP)
		return;
	int id = current_app;
	go_home();
	running[id] = 0;
	GY_free1(snapshots[id]);
	snapshots[id] = NULL;
	current_app = -1;
	update_running();
}
static void notice_result(GYOBJ dialog, int index)
{
	YMGUI_MsgBox_Close(dialog);
	if (index == 1)
		PhoneHost_Close();
}
void PhoneHost_Notice(const char* title, const char* message)
{
	if (busy || scene != APP || !title || !message)
		return;
	PhoneIME_Hide();
	YMGUI_MsgBox_SetTitle(app_demo_dialog, title);
	YMGUI_MsgBox_SetText(app_demo_dialog, message);
	YMGUI_MsgBox_ClearButtons(app_demo_dialog);
	YMGUI_MsgBox_AddButton(app_demo_dialog, T("关闭弹窗"), notice_result);
	YMGUI_MsgBox_AddButton(app_demo_dialog, T("返回桌面"), notice_result);
	YMGUI_MsgBox_Show(app_demo_dialog);
}

#include "phone_selftest.inc"

static void select_preview(const char* arg)
{
	if (!strcmp(arg, "--home-menu") || !strcmp(arg, "--home-drag"))
	{
		show_home();
		step_time(400);
		YMGUI_Inject_ContextBegin(48,289);
		if (!strcmp(arg, "--home-drag")) YMGUI_Inject_ContextMove(121,292);
		return;
	}
	if (!strcmp(arg, "--shade") || !strcmp(arg, "--screenshot") || !strncmp(arg, "--shade-clear-", 14))
	{
		show_home();
		step_time(400);
		PhoneShade_Open();
		step_time(260);
		if (!strcmp(arg, "--screenshot")) { click_at(200, 185); step_time(260); }
		if (!strncmp(arg, "--shade-clear-", 14))
		{
			click_at(270, 282);
			step_time(!strcmp(arg, "--shade-clear-mid") ? 160 : 400);
#if YMGUI_ANIM
			if (!strcmp(arg, "--shade-clear-mid"))
			{
				YMGUI_Anim_CancelObject((GYOBJ)PhoneShade_Inspect("card0"));
				YMGUI_Anim_CancelObject((GYOBJ)PhoneShade_Inspect("card1"));
			}
#endif
		}
		return;
	}
	if (!strcmp(arg, "--boot"))
		return;
	/* 冻结启动过渡，供首帧/中途截图检查，不修改动画采样算法。 */
	if (!strncmp(arg, "--launch-start-", 15) || !strncmp(arg, "--launch-mid-", 13))
	{
		int first = !strncmp(arg, "--launch-start-", 15);
		int id = PhoneApps_Find(arg + (first ? 15 : 13));
		if (id < 0)
			return;
		show_home();
		open_app(id);
		if (!first)
			step_time(120);
#if YMGUI_ANIM
		YMGUI_Anim_CancelObject(app_page);
#endif
		return;
	}
	if (!strcmp(arg, "--camera-dialog") || !strcmp(arg, "--browser-dialog"))
	{
		select_preview(!strcmp(arg, "--camera-dialog") ? "--camera" : "--browser");
		PhoneApps_Command(current_app, "notice", NULL);
		return;
	}
	if (!strcmp(arg, "--ime") || !strcmp(arg, "--ime-idle") || !strcmp(arg, "--symbols"))
	{
		select_preview("--notes");
		PhoneApps_Command(NOTES, "set-body", "你好，Yaomi。\n记录今天的小想法。");
		click_at(75, 188);
		step_time(220);
		if (!strcmp(arg, "--symbols")) { click_at(25, 421); return; }
		if (!strcmp(arg, "--ime-idle"))
			return;
		const char* py = "nihao";
		for (const char* p = py; *p; ++p)
			YMGUI_Inject_Key(*p, 1);
		return;
	}
	if (!strcmp(arg, "--clear-mid") || !strcmp(arg, "--clear-done"))
	{
		select_preview("--recents");
		clear_recent();
		step_time(!strcmp(arg, "--clear-mid") ? 100 : 1000);
#if YMGUI_ANIM
		if (!strcmp(arg, "--clear-mid"))
			for (int i = 0; i < APP_COUNT; ++i)
				YMGUI_Anim_CancelObject(recent_cards[i]);
#endif
		return;
	}
	show_home();
	int app_id = -1;
	for (int i = 0; i < APP_COUNT; ++i)
		if (!strncmp(arg, "--", 2) && !strcmp(arg + 2, PhoneApps_Get(i)->key))
			app_id = i;
	if (!strncmp(arg, "--home", 6) && arg[6] >= '2' && arg[6] <= '0' + HOME_PAGES)
	{
		home_select(arg[6] - '1', 0);
		return;
	}
	if (app_id >= 0)
	{
		prepare_app(app_id, 0, NULL);
		YMGUI_Obj_SetHidden(app_page, 0);
		PhoneUI_set_pos(app_page, 0, 0);
		scene = APP;
	}
	else if (!strcmp(arg, "--recents"))
	{
		for (int i = 0; i < APP_COUNT; ++i)
		{
			prepare_app(i, 0, NULL);
			app_page->area.w = W;
			app_page->area.h = H;
			YMGUI_Obj_SetHidden(app_page, 0);
			PhoneUI_set_pos(app_page, 0, 0);
			scene = APP;
			YMGUI_Obj_Invalidate(ctx->root);
			YMGUI_Refresh(ctx);
		}
		PhoneApps_Hide();
		YMGUI_Obj_SetHidden(app_page, 1);
		recent_scroll = 0;
		update_running();
		YMGUI_Obj_SetHidden(recent_page, 0);
		PhoneUI_set_pos(recent_page, 0, 0);
		scene = RECENTS;
	}
	else if (!strcmp(arg, "--dialog"))
	{
		show_power_dialog();
	}
	else if (!strcmp(arg, "--power"))
	{
		begin_shutdown();
		scene = OFF;
		PhoneUI_text_set(power_title, T("已关机"));
		YMGUI_Obj_SetHidden(power_bar, 1);
		YMGUI_Obj_SetHidden(restart_button, 0);
	}
}

#if defined(PHONE_SHELL_BOARD)

/* ===========================================================================
 * 板级入口（阶段 5）
 *
 * 宿主版 main() 依赖 SDL_LCD_*（窗口/事件/延时）与 stdio（getenv/atoi/fopen），
 * 板上都没有。这里把 main 里**除 SDL/stdio 之外**的部分原样复用：
 * 校验应用表 → 挂中文字体 → 注入 ctx → 注册快捷开关 → build_ui()
 * （build_ui 末尾自带 begin_boot()，进入开机动画）。
 *
 * 应用逻辑一行都没改 —— 桌面（宿主）验证过的行为，板上走的是同一条代码路径，
 * 差别只在"谁提供时基与 flush_cb"。
 * =========================================================================== */

static void board_ime_step(void);   /* 前向声明：输入法自测状态机，挂在 BoardTick 头 */

int PhoneShell_BoardInit(GYDISP disp, GYCTX board_ctx)
{
	if (!disp || !board_ctx)
		return -1;
	ctx = board_ctx;

	/* 调暗用的整带暂存**不再在这里预分配**（2026-10-04）：
	 * 出厂默认走硬件背光 PWM（g_dim_mode=2），软件叠黑整段跳过，
	 * 预分配等于白占 heap1 约 30 KB。改成真的切到 mode=1 时按需申请，
	 * 见 dim_buffer_ensure 上方的长注释。 */

	if (!PhoneApps_Validate())
		return -2;
	PhoneLocale_Init();
	YMGUI_Inject_SetCtx(ctx);
	if (!PhoneQuick_RegisterDefaults())
		return -3;

	/* 与宿主 main 同一手法：保存原 flush（板级是 ymgui_port 的 lcd_flush_cb，
	 * 它负责推 LCD + 累积全帧镜像），换成 phone_flush（先做快照累积，再转下去）。 */
	display_flush = disp->flush_cb;
	disp->flush_cb = phone_flush;

	build_ui();
	return 0;
}

/* ---- SWD 注入点击/滑动（2026-10-03，AI 免触摸测试用）----
 * 用法（host 侧，例如 tools/bench.py 的 rd/wr）：
 *   1) 先写 g_bdtap_x / g_bdtap_y（滑动再写 g_bdtap_dx / g_bdtap_dy，纯点击则留 0）；
 *   2) 把 g_bdtap_seq 加 1 触发；下一次 BoardTick 消费掉这次注入。
 * 行为与 selftest 的 click_at()/drag_pointer() 完全一致：
 *   dx=dy=0 → 按下后立即抬起；否则按下 → 5 步插值移动 → 在终点抬起。
 * 默认 seq=0 且无人写它 ⇒ 对正常运行零影响。 */
volatile int      g_bdtap_x  = 0, g_bdtap_y  = 0;
volatile int      g_bdtap_dx = 0, g_bdtap_dy = 0;
volatile uint32_t g_bdtap_seq = 0;

void PhoneShell_BoardTick(uint32 elapsed_ms)
{
	static uint32 bdtap_done = 0;
	if (g_bdtap_seq != bdtap_done)
	{
		bdtap_done = g_bdtap_seq;
		int x = g_bdtap_x, y = g_bdtap_y, dx = g_bdtap_dx, dy = g_bdtap_dy;
		YMGUI_Inject_Pointer((GYcoord)x, (GYcoord)y, 1);
		if (dx != 0 || dy != 0)
			for (int i = 1; i <= 5; ++i)
				YMGUI_Inject_Pointer((GYcoord)(x + dx * i / 5), (GYcoord)(y + dy * i / 5), 1);
		YMGUI_Inject_Pointer((GYcoord)(x + dx), (GYcoord)(y + dy), 0);
	}
	board_ime_step();
	PhoneCalc_BoardProbe();   /* 计算器格式化探针：见 apps/calculator.c 顶部的注释 */
	/* ESP32 上行链路：把 NTP 时间 / 天气刷进界面。内部自带 500 ms 节流，
	 * 每帧调也没关系。放在 advance 之前，好让本拍的置脏由 advance 里的刷新完成。 */
	net_refresh(elapsed_ms);
	advance(elapsed_ms);
	YMGUI_Refresh(ctx);
}

GYCTX PhoneShell_BoardCtx(void)
{
	return ctx;
}

/* 对象树规模统计（2026-10-03，渲染瓶颈定位用）。
 *
 * 【为什么要数它】库的重绘是"每 band 从 root 递归一遍整棵树"：
 * 每个对象都要 `YMGUI_Obj_GetAbsArea()`（沿 parent 链一路加到 root，O(树深)）
 * 再做一次矩形相交测试。也就是说**每 band 的固定开销 ∝ 对象数 × 平均树深**。
 * 不知道 N 就无法判断"减少 band 数"或"裁剪不可见子树"能换来多少。
 *
 * 只统计**非隐藏**对象 —— 隐藏对象在 drawObjRec 里第一行就 return，
 * 除了那次 GetAbsArea 不产生后续开销，计入会高估。 */
static int count_rec(GYOBJ o, int depth_total, int* max_depth, int depth)
{
	int n = 0;
	while (o != NULL)
	{
		if (!(o->state & GY_STATE_Hidden))
		{
			n += 1;
			if (depth > *max_depth)
				*max_depth = depth;
			n += count_rec(o->child_head, depth_total, max_depth, depth + 1);
		}
		o = o->sibling;
	}
	return n;
}

int PhoneShell_BoardObjStats(int* out_max_depth)
{
	int n = 0;
	int maxd = 0;
	if (ctx != NULL)
	{
		n += count_rec(ctx->root, 0, &maxd, 1);
		if (ctx->top_layer != NULL)
			n += count_rec(ctx->top_layer, 0, &maxd, 1);
	}
	if (out_max_depth != NULL)
		*out_max_depth = maxd;
	return n;
}

/* ===========================================================================
 * 输入法自测状态机（g_ime_test = 1 触发）
 *
 * 为什么做成状态机而不是一个函数从头跑到尾：打开应用有 330 ms 的开场动画，
 * 键盘也要等 PhoneIME_Update() 在下一拍发现"焦点落在一个已注册的输入框上"
 * 才会升起来。这两步都得让主循环真的转起来若干拍才能到下一状态，
 * 一步到位会停在"应用还没开完"的中间态上。
 *
 * 每一步的出口都写进 g_ime_step / g_ime_rc，SWD 读出来就能定位卡在哪一步。
 * =========================================================================== */
volatile int g_ime_test = 0;        /* 写 1 启动；跑完自动清 0 */
volatile int g_ime_step = 0;        /* 0 未启动 / 1 已发打开 / 2 等动画 / 3 已聚焦 / 4 已输入 / 5 已上屏 / 6 完成 */
volatile int g_ime_rc   = 99;       /* 0 = 成功；<0 见下面的错误码 */

/* 待验证的拼音串。做成可写缓冲而不是编译期常量：每次改这几个字节都要重新烧一遍
 * flash（约 3 分钟），而 SWD 写 RAM 是秒级的 —— 想换词就别付这个冤枉钱。 */
volatile char g_ime_pinyin[16] = "nihao";

static void board_ime_step(void)
{
	/* 上升沿识别：跑完一轮后 g_ime_step 停在 6 不再动，所以第二次探针写 1 时
	 * 必须先把状态机归零，否则之后的每一次自测都只是把上一轮的结果又打印一遍
	 * （都 rc=0，看着全绿实则一遍都没跑）。 */
	static int prev_test;
	int rising = (g_ime_test != 0) && (prev_test == 0);
	prev_test = g_ime_test;
	if (rising)
	{
		g_ime_step = 0;
		g_ime_rc   = 99;
		for (int i = 0; i < 6; ++i)
			PhoneIME_BoardCand[i][0] = 0;
		PhoneIME_BoardCandCount = 0;
		PhoneIME_BoardText[0] = 0;
	}
	if (!g_ime_test || g_ime_step >= 6)
		return;
	switch (g_ime_step)
	{
	case 0:                                   /* ① 打开笔记（里面有两个输入框）*/
	{
		int id = PhoneApps_Find("notes");
		if (id < 0)
		{
			g_ime_rc = -1;                    /* 应用表里没有 notes */
			g_ime_step = 6;
			break;
		}
		if (!PhoneHost_Open(id, NULL))
		{
			g_ime_rc = -2;                    /* open 失败（多数是还在别的状态）*/
			g_ime_step = 6;
			break;
		}
		g_ime_step = 1;
		break;
	}
	case 1:                                   /* ② 等开场动画结束（busy 落 0）*/
		if (!busy)
			g_ime_step = 2;
		break;
	case 2:                                   /* ③ 把焦点交给笔记里第一个输入框 */
		if (!PhoneIME_BoardFocusFirst())
		{
			g_ime_rc = -3;                    /* 树里没有可用的输入框 */
			g_ime_step = 6;
			break;
		}
		(void)PhoneIME_BoardClearTarget();   /* 基线清空：每轮自测的文本证据互不叠加 */
		g_ime_step = 3;
		break;
	case 3:                                   /* ④ 等 PhoneIME_Update 把软键盘拉起 */
		if (PhoneIME_Visible())
		{
			/* 清空要放在这里而不是刚聚焦时：active 是 PhoneIME_Update 认到
			 * "焦点落在一个已注册的输入框上"之后才赋值的，早于此 Ptr 还是 NULL。 */
			(void)PhoneIME_BoardClearTarget();  /* 基线清空：每轮自测的文本证据互不叠加 */
			g_ime_step = 4;
		}
		break;
	case 4:                                   /* ⑤ 逐字母敲拼音（走真实 action()）*/
	{
		size_t want = strlen((const char*)g_ime_pinyin);
		int got = PhoneIME_BoardType((const char*)g_ime_pinyin);
		if (got != (int)want)
		{
			g_ime_rc = -4;                    /* 拼音字母没被全部接收 */
			g_ime_step = 6;
			break;
		}
		if (PhoneIME_BoardSnapshot() <= 0)
		{
			g_ime_rc = -5;                    /* 词典没找到任何候选 */
			g_ime_step = 6;
			break;
		}
		g_ime_step = 5;
		break;
	}
	case 5:                                   /* ⑥ 第一个候选上屏，抄回目标文本 */
		g_ime_rc = PhoneIME_BoardCommit(0) < 0 ? -6 : 0;
		g_ime_step = 6;
		g_ime_test = 0;
		break;
	default:
		break;
	}
}

#else  /* ---------------- 以下为宿主入口（未改动）---------------- */

int main(int argc, char** argv)
{
	int max_frames = -1, frame = 0, test = 0;
	const char* preview = NULL;
	if (argc > 1)
	{
		if (!strcmp(argv[1], "--selftest"))
		{
			test = 1;
			max_frames = 4;
		}
		else if (argv[1][0] == '-')
		{
			preview = argv[1];
			max_frames = 40;
		}
		else
			max_frames = atoi(argv[1]);
	}
	GYdisp disp = {0};
	disp.hor_res = W;
	disp.ver_res = H;
	disp.buf_px_cnt = W * 48;
	disp.buf1 = (GYpx*)GY_malloc1(disp.buf_px_cnt * sizeof(GYpx));
	/* dim_buffer 不再预分配（按需，见 dim_buffer_ensure）——桌面路径同样受益 */
	if (!disp.buf1 || SDL_LCD_Init(&disp, 1) != 0)
	{
		failures = 1;
		goto done;
	}
	display_flush = disp.flush_cb;
	disp.flush_cb = phone_flush;
	SDL_LCD_SetTitle("Yaomi Phone - YMGUI");
	ctx = YMGUI_Creat_Ctx_Creat(&disp, W, H);
	if (!ctx)
	{
		failures = 1;
		goto done;
	}
	if (!PhoneApps_Validate())
	{
		failures = 1;
		goto done;
	}
	PhoneLocale_Init();
	YMGUI_Inject_SetCtx(ctx);
	if (!PhoneQuick_RegisterDefaults()) { failures = 1; goto done; }
	if (test)
	{
		if (!PhoneQuick_Register(&quick_probe)) ++failures;
	}
	build_ui();
	if (test)
		selftest();
	else if (preview)
		select_preview(preview);
	else if (getenv("YMGUI_SHOT"))
		select_preview("--home");
	while (SDL_LCD_PumpEvents())
	{
		if (!test && !preview && !getenv("YMGUI_SHOT"))
			advance(ctx->tick_elapsed);
		YMGUI_Refresh(ctx);
		SDL_LCD_Delay(16);
		if (max_frames > 0 && ++frame >= max_frames)
			break;
	}
done:
	PhoneShade_Shutdown();
	PhoneApps_Destroy();
	PhoneIME_Shutdown();
	PhoneLocale_Shutdown();
	YMGUI_Inject_SetCtx(NULL);
	if (ctx)
		YMGUI_Free_CtxFree(ctx);
	for (int i = 0; i < APP_COUNT; ++i)
		GY_free1(snapshots[i]);
	SDL_LCD_Destroy();
	GY_free1(disp.buf1);
	/* dim_buffer 按需分配，可能从未申请（默认走硬件 PWM）——GY_free1(NULL) 安全;
	 * 这里一并把状态复位，便于同一进程内重复跑（SDL 端的 main 可被再次调用）。 */
	GY_free1(dim_buffer);
	dim_buffer = NULL;
	dim_cap_px = 0u;
	dim_tried  = 0u;
	return failures ? 1 : 0;
}

#endif /* PHONE_SHELL_BOARD */
