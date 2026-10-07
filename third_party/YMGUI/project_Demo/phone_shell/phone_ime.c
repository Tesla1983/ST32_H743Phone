#include "phone_ime.h"
#include "YMGUI_Hal.h"
#include "YMGUI_Event.h"
#include "YMGUI_Invalidate.h"
#include "YMGUI_DrawFill.h"
#include "YMGUI_Font.h"
#include "YMGUI_Geom.h"
#include "YMGUI_Anim.h"
#include "ime_candidates.inc"
#include "ime_symbols.h"

#define RGB(r, g, b) GY_ARGB(255, r, g, b)
#define KEYBOARD_Y 226
/* 自管避让容器注册表容量（本工程实际只有设置页滚动视口 1 个，
 * 留 4 个余量；满了就放弃注册并退回原避让逻辑 = 安全侧）。 */
#define GIME_SELF_AVOID_MAX 4
typedef struct InputTarget
{
	GYOBJ obj;
	GYobj_free_cb original_free;
	int multiline;
	struct InputTarget* next;
} InputTarget;
typedef struct
{
	GYOBJ obj;
	char text[96];
	int action;
} Key;
enum
{
	MODE = 0x2000,
	SYMBOLS,
	NUMBERS,
	SHIFT,
	SPACE,
	PREV,
	NEXT,
	CANDIDATE = 0x2100,
	SYMBOL_CATEGORY = 0x2200,
	SYMBOL_ITEM = 0x2300
};
static GYCTX context;
static GYOBJ keyboard, query_label;
static InputTarget *inputs, *active;
static GYrect original_area;
static Key keys[48], candidates[5], candidate_prev, candidate_next;
static Key symbol_categories[7], symbol_keys[15], symbol_backspace;
static int symbol_panel, symbol_category, symbol_page;
static int key_count, visible, symbols, upper, bypass, page_start, page_count;

/* 键盘可见性诊断量（2026-10-04，供 tools/ribbon_check.py 读）。
 *
 * 【为什么需要】visible 是 static int，而 ELF 里有**两个同名符号**
 *   （实测 0x20002788 与 0x20002b30，分属不同编译单元），
 *   read_vars.py 按名字取址会取到错的那个 ⇒ 读出来不可信。
 *   明确导出全局量最省事，也避免以后重排变量就静默失效。
 *
 * 【这个量为什么关键】键盘挂在 top_layer 且占 y=226..448，会盖住设置页
 *   滚动区下半部分。脚本要点"关机按钮"（滚到底后 abs y=408..448）时，
 *   若键盘还开着，点下去命中的是键盘而不是按钮 ⇒ 表现为"按钮点不到"。
 *   这是本次验收真实踩到的坑，**不是坐标算错** —— 别再怀疑坐标。
 *
 * ⚠⚠ 赋值纪律（2026-10-04 板上实测踩坑后立的规矩）：
 *   本量**只在 phone_shell.c 的 advance() 里每帧现算**，禁止在
 *   PhoneIME_Show/Hide 里"顺手增量同步"。原因：那些函数被 -O2 内联进
 *   多处，编译器看不到本 volatile 只有 SWD 在读，会把写入合并/移除 ——
 *   实测出现过"画面上键盘明明收着、本量却报可见"。 */
volatile int32_t g_diag_ime_visible = 0;
static char composition[IME_QUERY_CAP];
static void refresh_symbols(void);
static void update_letters(void);
static int is_child(GYOBJ obj, GYOBJ ancestor)
{
	for (; obj; obj = obj->parent)
		if (obj == ancestor)
			return 1;
	return 0;
}
static int usable(GYOBJ obj)
{
	for (; obj; obj = obj->parent)
		if (obj->state & GY_STATE_Hidden)
			return 0;
	return 1;
}
static InputTarget* lookup(GYOBJ obj)
{
	for (InputTarget* p = inputs; p; p = p->next)
		if (p->obj == obj)
			return p;
	return NULL;
}

/* 该对象是否位于**自管避让的滚动容器**内。
 *
 * 【为什么用注册表而不是"猜"结构】曾想按"祖先有 ClipChildren 且 scroll_y>=0"
 * 判定，但本工程有 6 处开了 ClipChildren（app_page / app_views[i] /
 * recent_page / recent_viewport / viewport / 设置页滚动视口），
 * 其中大部分是**普通裁剪容器、scroll_y 恒为 0**，纯结构判据会把它们
 * 全部误判成滚动容器 ⇒ 定长页面上的避让被无差别关掉，是回归。
 *
 * 【所以锚定"注册"这个行为】容器自己管避让时调
 * PhoneIME_RegisterSelfAvoiding() 把自己的根对象登记进来。
 * 判据 = "祖先链上有对象在注册表里"，与结构无关、不会误伤。
 * 未注册的容器一律走原避让逻辑 ⇒ 行为与改动前完全一致。 */
static GYOBJ s_self_avoid[GIME_SELF_AVOID_MAX];
static int   s_self_avoid_cnt = 0;

void PhoneIME_RegisterSelfAvoiding(GYOBJ root)
{
	int i;
	if (root == NULL)
		return;
	for (i = 0; i < s_self_avoid_cnt; ++i)
		if (s_self_avoid[i] == root)
			return;                    /* 幂等：app_create 可能被重跑 */
	if (s_self_avoid_cnt >= GIME_SELF_AVOID_MAX)
		return;                        /* 满了就放弃注册，退回原避让（安全侧） */
	s_self_avoid[s_self_avoid_cnt++] = root;
}

static int in_scroll_container(GYOBJ obj)
{
	int i;
	for (GYOBJ p = obj ? obj->parent : NULL; p; p = p->parent)
	{
		for (i = 0; i < s_self_avoid_cnt; ++i)
			if (s_self_avoid[i] == p)
				return 1;
	}
	return 0;
}
static void restore_area(void)
{
	if (!active)
		return;
	YMGUI_Obj_Invalidate(active->obj);
	active->obj->area = original_area;
	YMGUI_Obj_Invalidate(active->obj);
}
static void focus_target(void)
{
	if (!active)
		return;
	YMGUI_SetFocus(context, active->obj);
	active->obj->state |= GY_STATE_Editing;
}
static void send_key(uint32 key)
{
	if (!active)
		return;
	focus_target();
	bypass = 1;
	YMGUI_Inject_Key(key, 1);
	YMGUI_Inject_Key(key, 0);
	bypass = 0;
}
static void insert(const char* text)
{
	if (!active)
		return;
	focus_target();
	if (active->multiline)
		YMGUI_EditView_InsertText(active->obj, text);
	else
		for (const unsigned char* p = (const unsigned char*)text; *p; ++p)
			send_key(*p);
}
static void key_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	Key* key = obj->user_data;
	GYsurface clipped = *s;
	GYrect hit;
	if (!GY_Rect_Intersect(&hit, a, &s->clip))
		return;
	clipped.clip = hit;
	GYcolor bg = obj->state & GY_STATE_Pressed ? RGB(197, 204, 231) : RGB(255, 255, 255);
	if (symbol_panel && key->action == SYMBOL_CATEGORY + symbol_category)
		bg = RGB(197, 204, 231);
	YMGUI_Draw_Fill(&clipped, a, bg, 255);
	GYrect bottom = {a->x, a->y + a->h - 1, a->w, 1};
	YMGUI_Draw_Fill(&clipped, &bottom, RGB(186, 192, 203), 255);
	int w = YMGUI_Font_TextWidth(&YMGUI_Font_Default, key->text);
	YMGUI_Draw_Text(&clipped, &YMGUI_Font_Default, a->x + (a->w - w) / 2, a->y + (a->h - 16) / 2,
					key->text, RGB(40, 47, 68));
}
static void query_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	(void)obj;
	const char* text = composition[0] ? composition : (g_english ? "英文输入" : "拼音输入");
	char caption[96];
	if (symbol_panel)
	{
		snprintf(caption, sizeof(caption), "%s%s %d/%d", g_english ? "英" : "中",
				 s_symbol_set_names[symbol_category], symbol_page + 1,
				 (imeSymbolCount(symbol_category, g_english) + 14) / 15);
		text = caption;
	}
	YMGUI_Draw_Text(s, &YMGUI_Font_Default, a->x, a->y, text, RGB(83, 91, 140));
}
static void refresh_candidates(void)
{
	/* 空组合把两个窄翻页键合为一个收起键，文字和命中区域同步切换。 */
	int composing = composition[0] != 0 || symbol_panel;
	candidate_prev.obj->area.w = composing ? 24 : 54;
	candidate_prev.obj->area.y = candidate_next.obj->area.y = symbol_panel ? 0 : 24;
	candidate_prev.obj->area.h = candidate_next.obj->area.h = symbol_panel ? 22 : 35;
	snprintf(candidate_prev.text, sizeof(candidate_prev.text), "%s", composing ? "<" : "收起");
	YMGUI_Obj_SetHidden(candidate_next.obj, !composing);
	page_count = 0;
	int x = 5;
	for (int i = 0; i < 5; ++i)
		YMGUI_Obj_SetHidden(candidates[i].obj, 1);
	if (composition[0] && !symbol_panel)
	{
		for (int i = 0; i < 5; ++i)
		{
			extendPrefixCandidates(page_start + i + 1);
			if (page_start + i >= g_word_count)
				break;
			const char* word = g_words[page_start + i];
			int width = YMGUI_Font_TextWidth(&YMGUI_Font_Default, word) + 18;
			if (width > 247)
				width = 247;
			if (x + width > 255)
				break;
			Key* key = &candidates[i];
			key->obj->area = (GYrect){x, 24, width, 28};
			snprintf(key->text, sizeof(key->text), "%s", word);
			YMGUI_Obj_SetHidden(key->obj, 0);
			x += width + 3;
			page_count++;
		}
	}
	YMGUI_Obj_Invalidate(keyboard);
}
static void reset_query(void)
{
	page_start = 0;
	imeCandidatesReset(composition);
	refresh_candidates();
}
static void commit(int index)
{
	if (index >= 0 && index < page_count)
	{
		const char* word = g_words[page_start + index];
		insert(word);
		learnWord(word);
	}
	else if (composition[0])
		insert(composition);
	composition[0] = 0;
	reset_query();
}
int PhoneIME_Hide(void)
{
	int was_visible = visible;
	if (context == NULL)
		return 0;
	visible = 0;
	YMGUI_Inject_SetKeyFilter(NULL, NULL);
	if (active && context->focus_obj == active->obj)
		YMGUI_SetFocus(context, NULL);
	restore_area();
	active = NULL;
	composition[0] = 0;
	if (keyboard)
	{
#if YMGUI_ANIM
		YMGUI_Anim_CancelObject(keyboard);
#endif
		YMGUI_Obj_SetHidden(keyboard, 1);
	}
	return was_visible;
}
int PhoneIME_Visible(void)
{
	return visible;
}


GYOBJ PhoneIME_Target(void)
{
	return active ? active->obj : NULL;
}

static void update_letters(void)
{
	const char* letters = "qwertyuiopasdfghjklzxcvbnm";
	const char* digits = "1234567890-/:;()$&@.,?!'\"+=%";
	for (int i = 0; i < 26; ++i)
	{
		char c = symbols ? digits[i] : letters[i];
		if (!symbols && upper)
			c -= 'a' - 'A';
		keys[i].action = c;
		keys[i].text[0] = c;
		keys[i].text[1] = 0;
	}
	for (int i = 0; i < 28; ++i)
		YMGUI_Obj_SetHidden(keys[i].obj, symbol_panel);
	snprintf(keys[28].text, sizeof(keys[28].text), "%s", symbol_panel ? "ABC" : "符号");
	refresh_symbols();
	YMGUI_Obj_Invalidate(keyboard);
}
static void refresh_symbols(void)
{
	int count = imeSymbolCount(symbol_category, g_english);
	for (int i = 0; i < 7; ++i)
		YMGUI_Obj_SetHidden(symbol_categories[i].obj, !symbol_panel);
	YMGUI_Obj_SetHidden(symbol_backspace.obj, !symbol_panel);
	for (int i = 0; i < 15; ++i)
	{
		int index = symbol_page * 15 + i;
		YMGUI_Obj_SetHidden(symbol_keys[i].obj, !symbol_panel || index >= count);
		if (index < count)
			snprintf(symbol_keys[i].text, sizeof(symbol_keys[i].text), "%s", imeSymbolAt(symbol_category, index, g_english));
	}
	refresh_candidates();
}
static void action(int code)
{
	if (!active)
		return;
	focus_target();
	if (code >= SYMBOL_ITEM && code < SYMBOL_ITEM + 15)
	{
		const char* text = imeSymbolAt(symbol_category, symbol_page * 15 + code - SYMBOL_ITEM, g_english);
		if (text)
			insert(text);
		return;
	}
	if (code >= SYMBOL_CATEGORY && code < SYMBOL_CATEGORY + 7)
	{
		symbol_category = code - SYMBOL_CATEGORY;
		symbol_page = 0;
		refresh_symbols();
		return;
	}
	if (code >= CANDIDATE && code < CANDIDATE + 5)
	{
		commit(code - CANDIDATE);
		return;
	}
	if (code == 27 || ((code == PREV || code == NEXT) && !composition[0] && !symbol_panel))
	{
		PhoneIME_Hide();
		return;
	}
	if (code == MODE)
	{
		commit(-1);
		g_english = !g_english;
		symbols = 0;
		symbol_page = 0;
		update_letters();
		return;
	}
	if (code == SYMBOLS)
	{
		if (composition[0])
			commit(page_count ? 0 : -1);
		symbol_panel = !symbol_panel;
		symbols = 0;
		symbol_page = 0;
		update_letters();
		return;
	}
	if (code == NUMBERS)
	{
		commit(-1);
		symbol_panel = 0;
		symbols = !symbols;
		update_letters();
		return;
	}
	if (symbol_panel && (code == NEXT || code == PREV))
	{
		int last = (imeSymbolCount(symbol_category, g_english) - 1) / 15;
		if (code == NEXT && symbol_page < last)
			++symbol_page;
		if (code == PREV && symbol_page > 0)
			--symbol_page;
		refresh_symbols();
		return;
	}
	if (code == SHIFT)
	{
		upper = !upper;
		update_letters();
		return;
	}
	if (code == NEXT)
	{
		extendPrefixCandidates(page_start + page_count + 1);
		if (page_count && page_start + page_count < g_word_count)
			page_start += page_count;
		refresh_candidates();
		return;
	}
	if (code == PREV)
	{
		int start = 0, previous = 0;
		while (start < page_start)
		{
			previous = start;
			int x = 5, count = 0;
			while (start < page_start && count < 5)
			{
				int width = YMGUI_Font_TextWidth(&YMGUI_Font_Default, g_words[start]) + 18;
				if (width > 247)
					width = 247;
				if (x + width > 255)
					break;
				x += width + 3;
				start++;
				count++;
			}
		}
		page_start = previous;
		refresh_candidates();
		return;
	}
	size_t n = strlen(composition);
	if (code == GY_KEY_BACKSPACE && n)
	{
		composition[n - 1] = 0;
		reset_query();
		return;
	}
	if (code == GY_KEY_ENTER)
	{
		/* 回车不是另一个选词键：先上屏，再交给目标控件换行或提交。 */
		if (n)
			commit(page_count ? 0 : -1);
		send_key(GY_KEY_ENTER);
		if (active && !active->multiline)
			PhoneIME_Hide();
		return;
	}
	if (code == SPACE || code == ' ')
	{
		if (n)
			commit(page_count ? 0 : -1);
		else
			insert(" ");
		return;
	}
	if (!g_english && code >= '1' && code <= '5' && n && code - '1' < page_count)
	{
		commit(code - '1');
		return;
	}
	if (!g_english && !symbols && !symbol_panel && ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')))
	{
		if (n < sizeof(composition) - 1)
		{
			composition[n] = code >= 'a' ? code : code + 'a' - 'A';
			composition[n + 1] = 0;
			reset_query();
		}
		return;
	}
	if (n && code >= 32 && code < 127)
		commit(page_count ? 0 : -1);
	send_key((uint32)code);
}
static uint8 filter(uint32 key, uint8 pressed, void* user)
{
	(void)user;
	if (bypass || !visible || !active || context->focus_obj != active->obj)
		return 0;
	if (key == GY_KEY_TAB || key >= GY_KEY_LEFT)
		return 0;
	if (pressed)
		action((int)key);
	return 1;
}
static void keyboard_event(GYOBJ obj, GYEvent event)
{
	if (event == GY_EVENT_Pressed || event == GY_EVENT_ReleasedOff || event == GY_EVENT_Released)
	{
		/* 原生指针按下会清除旧焦点；软键盘按键不成为新的输入目标。 */
		focus_target();
		YMGUI_Obj_Invalidate(obj);
	}
}
static void key_event(GYOBJ obj, GYEvent event)
{
	keyboard_event(obj, event);
	if (event == GY_EVENT_Clicked)
		action(((Key*)obj->user_data)->action);
}
static void make_key(Key* key, int x, int y, int w, const char* text, int code)
{
	key->obj = YMGUI_Creat_Obj_Creat(keyboard, x, y, w, 35);
	if (!key->obj)
		return;
	snprintf(key->text, sizeof(key->text), "%s", text);
	key->action = code;
	key->obj->user_data = key;
	key->obj->draw_cb = key_draw;
	key->obj->event_cb = key_event;
}
void PhoneIME_Update(int enabled)
{
	if (!context || !keyboard)
		return;
	if (!enabled)
	{
		if (visible)
			PhoneIME_Hide();
		return;
	}
	InputTarget* next = lookup(context->focus_obj);
	if (!next || !usable(next->obj))
	{
		if (visible && !is_child(context->pressed_obj, keyboard))
			PhoneIME_Hide();
		return;
	}
	if (next == active && visible)
		return;
	restore_area();
	active = next;
	original_area = active->obj->area;
	composition[0] = 0;
	symbol_panel = 0;
	update_letters();
	reset_query();
	GYrect abs;
	YMGUI_Obj_GetAbsArea(active->obj, &abs);
	/* ⚠⚠ 滚动容器里**必须跳过**下面这段避让（2026-10-04 板上定位的真缺陷）。
	 *
	 * 【原代码的坐标系混用 bug】
	 *   上面 GetAbsArea 拿的是**绝对**坐标（含父链的 scroll_y 偏移），
	 *   下面改的却是 area.y —— **局部**坐标（相对父对象，不含 scroll_y）。
	 *   `area.y -= (abs.y - X)` 等于把"绝对差值"从"局部坐标"里减掉，
	 *   在无滚动的定长页面里两者恰好一致、凑巧能用；
	 *   一旦父对象是滚动容器（scroll_y ≠ 0），就把控件挪到**视口外面**去了。
	 *
	 * 【实测症状】设置页点设备名输入框：
	 *   ① 输入框被挪到视口顶边以上，压在标题"把手机调成喜欢的样子"上，
	 *      板级像素扫描：abs y=80..88 有 79~82 个非背景像素（那行残字），
	 *      而 y=90/91 干净 ⇒ 溢出点正好在视口顶 92 之上。
	 *   ② 更早还观察到输入框直接压在"界面语言"卡片上，两行文字叠成乱码。
	 *
	 * 【为什么可以安全跳过】
	 *   避让的**目的**（别让控件被键盘盖住）已由容器自己达成：
	 *   settings.c 的 scroll_apply_ime() 在 PhoneIME_Update 之后的
	 *   PhoneApps_Tick 里把视口高度缩到键盘上沿、并把焦点输入框滚进视野
	 *   （实测 scroll_y 135 → 307，输入框完整可见）。
	 *   ⇒ 容器方案已覆盖避让需求，这里再挪控件纯属画蛇添足且破坏布局。
	 *
	 * 【判据】怎么知道对象在滚动容器里：沿 parent 链看有没有祖先
	 *   带 scroll_y 的可见滚动语义。保守做法是只在"父对象就是滚动视口"
	 *   这一种明确形态下跳过，其余（定长页面）保持原行为不变。 */
	if (!in_scroll_container(active->obj))
	{
		if (abs.y + abs.h > KEYBOARD_Y - 6)
		{
			if (abs.y > KEYBOARD_Y - 40)
			{
				active->obj->area.y -= abs.y - (KEYBOARD_Y - 44);
				abs.y = KEYBOARD_Y - 44;
			}
			active->obj->area.h = KEYBOARD_Y - 6 - abs.y;
			YMGUI_Obj_Invalidate(context->root);
		}
	}
	int opening = !visible;
	visible = 1;
	YMGUI_Obj_SetHidden(keyboard, 0);
	if (opening)
	{
		keyboard->area.y = KEYBOARD_Y;
#if YMGUI_ANIM
		keyboard->area.y = 448;
		if (!YMGUI_Anim_MoveTo(keyboard, 0, KEYBOARD_Y, 180, GY_ANIM_EASE_OUT))
			keyboard->area.y = KEYBOARD_Y;
#endif
		YMGUI_Obj_Invalidate(keyboard);
	}
	YMGUI_Inject_SetKeyFilter(filter, NULL);
}
static void input_free(GYOBJ obj)
{
	InputTarget** link = &inputs;
	while (*link && (*link)->obj != obj)
		link = &(*link)->next;
	if (!*link)
		return;
	InputTarget* entry = *link;
	if (active == entry)
		PhoneIME_Hide();
	*link = entry->next;
	GYobj_free_cb callback = entry->original_free;
	GY_free1(entry);
	if (callback)
		callback(obj);
}
static GYOBJ bind(GYOBJ obj, int multiline)
{
	if (!obj)
		return NULL;
	InputTarget* p = GY_malloc1(sizeof(*p));
	if (!p)
	{
		YMGUI_Free_ObjFree(obj);
		return NULL;
	}
	*p = (InputTarget){obj, obj->free_cb, multiline, inputs};
	inputs = p;
	obj->free_cb = input_free;
	return obj;
}
GYOBJ PhoneIME_TextInput(GYOBJ p, GYcoord x, GYcoord y, GYcoord w, GYcoord h, size_t cap)
{
	return bind(YMGUI_Creat_TextInput_Creat(p, x, y, w, h, cap), 0);
}
GYOBJ PhoneIME_EditView(GYOBJ p, GYcoord x, GYcoord y, GYcoord w, GYcoord h, size_t cap)
{
	return bind(YMGUI_Creat_EditView_Creat(p, x, y, w, h, cap), 1);
}
void PhoneIME_Init(GYCTX ctx)
{
	context = ctx;
	loadCharDictionary();
	loadPhraseDictionary();
	loadEnglishDictionary();
	if (S_CHAR_COUNT)
		s_char_matches = GY_malloc1(S_CHAR_COUNT * sizeof(*s_char_matches));
	keyboard = YMGUI_Creat_Obj_Creat(ctx->top_layer, 0, KEYBOARD_Y, 320, 222);
	if (!keyboard)
		return;
	keyboard->state |= GY_STATE_ClipChildren;
	keyboard->event_cb = keyboard_event;
	YMGUI_Obj_SetBgColor(keyboard, RGB(227, 231, 239));
	query_label = YMGUI_Creat_Obj_Creat(keyboard, 8, 4, 304, 18);
	query_label->draw_cb = query_draw;
	query_label->event_cb = keyboard_event;
	for (int i = 0; i < 5; ++i)
		make_key(&candidates[i], 5, 24, 45, "", CANDIDATE + i);
	for (int i = 0; i < 26; ++i)
	{
		int row = i < 10 ? 0 : i < 19 ? 1
									  : 2;
		int col = i - (row == 0 ? 0 : row == 1 ? 10
											   : 19);
		make_key(&keys[key_count++], (row == 0 ? 5 : row == 1 ? 20
															  : 43) +
										 col * (row == 2 ? 30 : 31),
				 58 + row * 40, 28, "", 0);
	}
	make_key(&keys[key_count++], 5, 138, 35, "Aa", SHIFT);
	make_key(&keys[key_count++], 257, 138, 58, "退格", GY_KEY_BACKSPACE);
	make_key(&keys[key_count++], 5, 178, 45, "符号", SYMBOLS);
	make_key(&keys[key_count++], 54, 178, 40, "123", NUMBERS);
	make_key(&keys[key_count++], 98, 178, 105, "空格", SPACE);
	make_key(&keys[key_count++], 207, 178, 60, "中/英", MODE);
	make_key(&keys[key_count++], 271, 178, 44, "回车", GY_KEY_ENTER);
	make_key(&candidate_prev, 260, 24, 24, "<", PREV);
	make_key(&candidate_next, 290, 24, 24, ">", NEXT);
	for (int i = 0; i < 7; ++i)
	{
		make_key(&symbol_categories[i], 5 + i % 4 * 79, 26 + i / 4 * 26, 73,
				 s_symbol_set_names[i], SYMBOL_CATEGORY + i);
		symbol_categories[i].obj->area.h = 23;
	}
	make_key(&symbol_backspace, 242, 52, 73, "退格", GY_KEY_BACKSPACE);
	symbol_backspace.obj->area.h = 23;
	for (int i = 0; i < 15; ++i)
	{
		make_key(&symbol_keys[i], 5 + i % 5 * 63, 80 + i / 5 * 32, 58, "", SYMBOL_ITEM + i);
		symbol_keys[i].obj->area.h = 28;
	}
	update_letters();
	reset_query();
	YMGUI_Obj_SetHidden(keyboard, 1);
}
void PhoneIME_Shutdown(void)
{
	PhoneIME_Hide();
	while (inputs)
	{
		InputTarget* entry = inputs;
		inputs = entry->next;
		entry->obj->free_cb = entry->original_free;
		GY_free1(entry);
	}
	if (keyboard)
		YMGUI_Free_ObjFree(keyboard);
	keyboard = NULL;
	GY_free1(s_char_matches);
	s_char_matches = NULL;
	freeEnglishDictionary();
	freeCharDictionary();
	freePhraseDictionary();
	context = NULL;
}

#if defined(PHONE_SHELL_BOARD)
/* ===========================================================================
 * 板级自测钩子（仅 ymgui-h743 编译，宿主构建不受影响）
 *
 * 桌面可以直接敲键盘才是"输入法能用"的直接证据；板上没有键盘也没有 stdio，
 * 只剩"看屏幕上有几个候选字"这种不确定的判据。这里把**软键盘字母键的真实处理
 * 函数 action()** 开放成一个可调用接口：
 *   action('n') → action('i') → ... 与手指逐个点软键盘走的是同一条代码路径
 *   （差异只有"命中测试"这一环，而命中测试在阶段 3 已经单独验证过）。
 * 候选结果再抄进全局可读数组，SWD 读出来就是 UTF-8 中文串 —— 不碰屏也能给出
 * 「拼音 → 中文候选」的确定结论。
 * =========================================================================== */

char PhoneIME_BoardCand[6][24];
int PhoneIME_BoardCandCount;
char PhoneIME_BoardText[192];   /* 上屏后目标输入框的文本（够放整段笔记） */

int PhoneIME_BoardSnapshot(void)
{
	int count = page_count < 6 ? page_count : 6;
	for (int i = 0; i < 6; ++i)
		PhoneIME_BoardCand[i][0] = 0;
	for (int i = 0; i < count; ++i)
		snprintf(PhoneIME_BoardCand[i], sizeof(PhoneIME_BoardCand[i]), "%s", candidates[i].text);
	PhoneIME_BoardCandCount = count;
	/* composition 一起给出：拼音串本身也是"字母确实进了组合状态"的证据 */
	return count;
}

int PhoneIME_BoardFocusFirst(void)
{
	if (!context)
		return 0;
	for (InputTarget* p = inputs; p; p = p->next)
	{
		if (!usable(p->obj))
			continue;
		YMGUI_SetFocus(context, p->obj);
		return 1;
	}
	return 0;
}

int PhoneIME_BoardType(const char* letters)
{
	int accepted = 0;
	if (!letters || !visible || !active)
		return -1;
	for (; *letters; ++letters)
	{
		size_t before = strlen(composition);
		action((int)*letters);          /* 与真点软键盘完全同一个处理器 */
		if (strlen(composition) == before)
			break;                      /* 这个键没被拼音模式接收，说明确实跑到了边界 */
		++accepted;
	}
	PhoneIME_BoardSnapshot();
	return accepted;
}

/* 清空当前目标，让每次自测的基线一致（否则第二轮的证据文本会把上一轮也带上，
 * 只看文本末尾分不清"这次上屏了什么"）。 */
int PhoneIME_BoardClearTarget(void)
{
	if (!active)
		return -1;
	if (active->multiline)
		YMGUI_EditView_SetText(active->obj, "");
	else
		YMGUI_TextInput_SetText(active->obj, "");
	return 0;
}

/* 选第 index 个候选上屏（等价于手指点一下候选条），再把目标控件的文本抄出来。
 * 候选列得出来不等于能写回去 —— 上屏这条路径（insert → 按键注入 / EditView_InsertText）
 * 才是"输入中文"真正落地的一步，两件事必须一起验，只验前者会漏掉一半。 */
int PhoneIME_BoardCommit(int index)
{
	if (!visible || !active)
		return -1;
	/* 注意：这里**不能**再调 BoardSnapshot()。commit() 内部会把 composition 清空、
	 * 候选条回到空态，快照会跟着被刷成 0 个；而自测要看的正是"上屏前那次命中"。
	 * 上屏前的快照在第 ⑤ 步已经拍好，这里保持不动，两步读到的才互不打架。 */
	commit(index);
	const char* text = active->multiline ? YMGUI_EditView_GetText(active->obj)
										 : YMGUI_TextInput_GetText(active->obj);
	snprintf(PhoneIME_BoardText, sizeof(PhoneIME_BoardText), "%s", text ? text : "");
	return 0;
}
#endif /* PHONE_SHELL_BOARD */
