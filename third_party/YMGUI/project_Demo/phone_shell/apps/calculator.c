#include "phone_ui.h"
#include "phone_host.h"

typedef struct
{
	GYOBJ calculator_display;
	int64_t calc_value, calc_accumulator;
	char calc_operator;
	int calc_fresh;
} AppState;
static AppState state;

static void calculator_key(GYOBJ obj);

/* ---- 板上探针（2026-10-03）-------------------------------------------------
 * SWD 写 g_calc_probe_v（想格式化的值）→ 写 1 到 g_calc_probe，
 * 固件就用**与按键完全相同的 calc_format()** 跑一遍，结果留在 g_calc_probe_text。
 *
 * 【为什么要有它】这个 bug（newlib-nano 下 "%lld" 把 1 打印成 "1d"）只在真机 libc
 * 上出现：host（glibc）跑一遍 selftest 是抓不到的，而板上要走到计算器还要靠触摸。
 * 有了它，AI 不用碰屏、不用重编译就能验证"数字 → 显示串"这一步。 */
volatile int  g_calc_probe   = 0;
volatile long g_calc_probe_v = 0;
char          g_calc_probe_text[40] = "";

/* 数字 → 显示串。⚠ 2026-10-03：原来这里写的是 "%lld"，在 **newlib-nano**
 * （CMakeLists.txt 链接 --specs=nano.specs）下不被支持 —— nano 的 snprintf 把
 * "%l" 当长整型吃掉后，剩下的 'd' 被当成普通字符原样输出，于是 1 被打印成
 * "1d"、清零键之后显示 "0d"。
 *
 * 改回 32 位 "%ld" 是安全的：calc_value 虽声明为 int64_t，但入口上限 99999999
 * （见下方 key 分支）与结果钳位 ±999999999（越界提示分支）都远小于 2^31，
 * 而 ARM32 上 long 就是 32 位 —— 值域完全覆盖，不会截断。
 * 顺带不再依赖 libc 的 64 位格式化支持，换工具链也不会复发。 */
static void calc_format(char* out, size_t n, int64_t v)
{
	snprintf(out, n, "%ld", (long)v);
}

void PhoneCalc_BoardProbe(void)
{
	if (g_calc_probe == 0)
		return;
	calc_format(g_calc_probe_text, sizeof(g_calc_probe_text), (int64_t)g_calc_probe_v);
	g_calc_probe = 0;
}


static void calculator_key(GYOBJ obj)
{
	char key = ((PhoneButton*)obj->user_data)->value[0];
	if (key == 'C')
	{
		state.calc_value = state.calc_accumulator = state.calc_operator = 0;
		state.calc_fresh = 1;
	}
	else if (key >= '0' && key <= '9')
	{
		if (state.calc_fresh)
			state.calc_value = 0;
		state.calc_fresh = 0;
		if (state.calc_value <= 99999999)
			state.calc_value = state.calc_value * 10 + key - '0';
	}
	else
	{
		if (state.calc_operator && !state.calc_fresh)
		{
			if (state.calc_operator == '/' && state.calc_value == 0)
			{
				PhoneUI_text_set(state.calculator_display, "不能除以零");
				state.calc_value = state.calc_accumulator = state.calc_operator = 0;
				state.calc_fresh = 1;
				return;
			}
			if (state.calc_operator == '+')
				state.calc_value = state.calc_accumulator + state.calc_value;
			else if (state.calc_operator == '-')
				state.calc_value = state.calc_accumulator - state.calc_value;
			else if (state.calc_operator == '*')
				state.calc_value = state.calc_accumulator * state.calc_value;
			else if (state.calc_operator == '/')
				state.calc_value = state.calc_accumulator / state.calc_value;
			if (state.calc_value > 999999999 || state.calc_value < -999999999)
			{
				PhoneUI_text_set(state.calculator_display, "超出计算范围");
				state.calc_value = state.calc_accumulator = state.calc_operator = 0;
				state.calc_fresh = 1;
				return;
			}
		}
		state.calc_accumulator = state.calc_value;
		state.calc_operator = key == '=' ? 0 : key;
		state.calc_fresh = 1;
	}
	char value[40];
	calc_format(value, sizeof(value), state.calc_value);
	PhoneUI_text_set(state.calculator_display, value);
}
static void app_create(GYOBJ view)
{
	state = (AppState){.calc_fresh = 1};
	PhoneUI_app_header(view, "计算器", "简简单单，算得明白");
	GYOBJ display = PhoneUI_panel(view, 18, 67, 284, 70, WHITE);
	state.calculator_display = PhoneUI_label_ex(display, 8, 17, 268, "0", INK, 3);
	static const char* keys[] = {"7", "8", "9", "+", "4", "5", "6", "-", "1", "2", "3", "*", "C", "0", "=", "/"};
	for (int i = 0; i < 16; ++i)
		PhoneUI_button(view, 18 + (i % 4) * 73, 158 + (i / 4) * 60, 65, 52, keys[i], calculator_key,
					   i % 4 == 3 || i == 14 ? ACCENT : RGB(129, 139, 160));
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "calculator_display"))
		return (intptr_t)state.calculator_display;
	if (!strcmp(name, "calc_value"))
		return (intptr_t)state.calc_value;
	if (!strcmp(name, "calc_accumulator"))
		return (intptr_t)state.calc_accumulator;
	if (!strcmp(name, "calc_operator"))
		return (intptr_t)state.calc_operator;
	if (!strcmp(name, "calc_fresh"))
		return (intptr_t)state.calc_fresh;
	return 0;
}

const PhoneApp PhoneApp_calculator = {
	.key = "calculator", .title = "计算器", .color = RGB(124, 102, 196), .icon = ICON_CALCULATOR, .create = app_create, .inspect = app_inspect, .destroy = app_destroy};
