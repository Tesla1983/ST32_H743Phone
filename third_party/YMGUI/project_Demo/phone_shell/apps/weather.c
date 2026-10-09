#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardNet_City* / BoardNet_ClockHM（同 gallery.c 的做法） */

/* ===========================================================================
 * 天气 app —— 真数据版（2026-10-10）
 *
 * 之前的版本是**假的**：城市/温度/天气三张表是写死的字符串，副标题干脆写着
 * "离线示例天气"。数据层其实早就有了（$WD 每 30 min 一帧），但那只有
 * **默认城市**一城，撑不起这里的多城列表。
 *
 * 所以这一版把链路补完整了：
 *   本板 --$?WEA,<城市码>--> ESP32 --HTTP--> sojson 天气接口
 *   本板 <--$WX,<城市码>,...-- ESP32
 * 三个城市**串行**问（对端邮箱只有一个槽位，连发会被拒），结果落在
 * src/uart_link.c 的城市表里，UI 只经 BoardNet_City* 这一组接口读。
 *
 * ⚠ 界面上**不允许出现假数据**：没取到就显示占位串（"--" / "等待数据"），
 *   副标题报真实状态（"等待网络数据" / "更新中 2/3" / "实时 12:34 更新"），
 *   绝不写"示例"也不写会误导的常量。
 * ========================================================================= */

typedef struct
{
	GYOBJ subtitle;
	GYOBJ city, temp, condition, sub;
	GYOBJ row_cond[3];
	GYOBJ row_temp[3];
	int   city_index;
} AppState;
static AppState state;

static void weather_refresh(void);

static void weather_next(GYOBJ obj)
{
	(void)obj;
	int n = BoardNet_CityCount();
	if (n <= 0)
		return;
	state.city_index = (state.city_index + 1) % n;
	weather_refresh();
}

/* 把当前数据写进控件。
 * ⚠ 一律用 PhoneUI_text_if_changed（判据 = label 当前内容），**不要**用
 *   调用方自缓存的 last 值比较 —— 切语言等路径会直接改写 label，缓存一脱钩
 *   就永远不再刷新（2026-10-09 桌面日期卡死的正是这个坑）。 */
static void weather_refresh(void)
{
	char buf[96];
	int  i = state.city_index;

	PhoneUI_text_if_changed(state.city, BoardNet_CityName(i));

	BoardNet_CityTempText(i, buf, (int)sizeof(buf));
	PhoneUI_text_if_changed(state.temp, buf);

	BoardNet_CityCondText(i, buf, (int)sizeof(buf));
	PhoneUI_text_if_changed(state.condition, buf);

	BoardNet_CitySubText(i, buf, (int)sizeof(buf));
	PhoneUI_text_if_changed(state.sub, buf);

	/* ---- 副标题：报**真实**的取数状态 ----
	 * 三态：一个都没拿到 / 拿到了一些（还在扫）/ 全拿到了（给更新时间）。
	 * 不再有"离线示例天气"这种字样 —— 数据是真的，写上"示例"就是骗人。 */
	int n    = BoardNet_CityCount();
	int got  = BoardNet_CityUpdatedCount();
	if (got <= 0)
	{
		PhoneUI_text_if_changed(state.subtitle, "等待网络数据");
	}
	else if (BoardNet_CityBusy() || got < n)
	{
		snprintf(buf, sizeof(buf), "更新中 %d/%d", got, n);
		PhoneUI_text_if_changed(state.subtitle, buf);
	}
	else
	{
		char hm[16];
		BoardNet_ClockHM(hm, (int)sizeof(hm));
		snprintf(buf, sizeof(buf), "实时 %s 更新", hm);
		PhoneUI_text_if_changed(state.subtitle, buf);
	}

	/* ---- 三行：每个城市一行，真实天气 ---- */
	for (int k = 0; k < 3 && k < n; ++k)
	{
		BoardNet_CityCondText(k, buf, (int)sizeof(buf));
		PhoneUI_text_if_changed(state.row_cond[k], buf);

		BoardNet_CityTempText(k, buf, (int)sizeof(buf));
		PhoneUI_text_if_changed(state.row_temp[k], buf);
	}
}

static void app_create(GYOBJ view)
{
	state = (AppState){0};

	/* 不复用 PhoneUI_app_header：它不返回控件句柄，而副标题要按真实状态刷新。
	 * 这里照它的版式自己建两个 label（坐标/字体与它一致）。 */
	PhoneUI_left_label(view, 22, 0, 276, "天气", INK, 3);
	state.subtitle = PhoneUI_left_label(view, 23, 37, 275, "等待网络数据", MUTED, 2);

	state.city      = PhoneUI_label_ex(view, 18, 72, 284, BoardNet_CityName(0), INK, 3);
	state.temp      = PhoneUI_label_ex(view, 18, 123, 284, "--", ACCENT, 1);
	state.condition = PhoneUI_label(view, 18, 195, 284, "等待数据", MUTED);
	state.sub       = PhoneUI_label_ex(view, 18, 219, 284, "", MUTED, 2);

	int n = BoardNet_CityCount();
	for (int i = 0; i < 3 && i < n; ++i)
	{
		GYOBJ row = PhoneUI_panel(view, 18, 241 + i * 40, 284, 36, WHITE);
		PhoneUI_left_label(row, 14, 9, 60, BoardNet_CityName(i), INK, 0);
		state.row_cond[i] = PhoneUI_left_label(row, 78, 9, 100, "等待数据", MUTED, 0);
		state.row_temp[i] = PhoneUI_left_label(row, 180, 9, 90, "--", ACCENT, 0);
	}

	PhoneUI_button(view, 18, 369, 284, 37, "切换城市", weather_next, PhoneApps_Get(WEATHER)->color);

	weather_refresh();
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
}

/* 每拍调一次（宿主只派发给运行中的应用）。
 * refresh 内部用 text_if_changed，数据没变就不置脏 —— 不会白白重绘。 */
static void app_tick(uint32 elapsed)
{
	(void)elapsed;
	weather_refresh();
}

static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "weather_city"))
		return (intptr_t)state.city;
	if (!strcmp(name, "weather_temp"))
		return (intptr_t)state.temp;
	if (!strcmp(name, "weather_condition"))
		return (intptr_t)state.condition;
	if (!strcmp(name, "city_index"))
		return (intptr_t)state.city_index;
	if (!strcmp(name, "subtitle"))
		return (intptr_t)state.subtitle;
	if (!strcmp(name, "sub"))
		return (intptr_t)state.sub;
	if (!strcmp(name, "row_cond"))
		return (intptr_t)state.row_cond[index >= 0 && index < 3 ? index : 0];
	if (!strcmp(name, "row_temp"))
		return (intptr_t)state.row_temp[index >= 0 && index < 3 ? index : 0];
	return 0;
}

const PhoneApp PhoneApp_weather = {
	.key = "weather", .title = "天气", .color = RGB(69, 161, 218), .icon = ICON_WEATHER,
	.create = app_create, .tick = app_tick, .inspect = app_inspect, .destroy = app_destroy};
