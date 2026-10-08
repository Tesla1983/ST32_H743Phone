/* ===========================================================================
 * 笔记（2026-10-09 改造：正文持久化到 TF 卡 /YMGUI/NOTE/*.txt）
 *
 * 【存什么】文件内容 = 第一行标题 + 其余正文（UTF-8 原样）。
 * ⚠ **文件名不用标题**：卡是 FAT32 + FF_CODE_PAGE=936，中文文件名要过 CP936
 *   转码，写进去再读回来未必逐字节一致（项目里那两张 GBK 表就是为此存在的）。
 *   ⇒ 文件名固定 ASCII 的 note<N>.txt，只当槽位 id；标题放正文第一行。
 *
 * 【怎么打开卡上已有笔记】文件管理器 → 笔记分类 → 点某一行，走
 *   PhoneHost_Open(NOTES, "0"/"1"/"2")，本模块的 show() 按索引加载。
 *   不带参数打开（桌面图标）时，加载第 0 条；卡上还没有就保持默认示例内容。
 *
 * 【为什么用静态缓冲而不是栈上数组】标题 + 正文最多 2 KB，压进 UI 回调的栈
 *   风险太大（栈深了不好查），放 .bss 更稳。
 * =========================================================================== */

#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardNote_* */

#include <stdlib.h>

typedef struct
{
	GYOBJ note_status, note_title_input, note_editor;
	int note_pinned;
	int note_index;      /* 当前 loaded 的槽位；-1 = 还没存过（新建） */
} AppState;
static AppState state;

static int app_command(const char* name, const char* argument);
static void note_pin(GYOBJ obj);
static void note_save(GYOBJ obj);
static void app_show(const char* argument);

#define NOTE_BUF 2048u
static char s_buf[NOTE_BUF];


static int app_command(const char* name, const char* argument)
{
	if (strcmp(name, "set-body") || !argument)
		return 0;
	YMGUI_EditView_SetText(state.note_editor, argument);
	return 1;
}

/* 把卡上第 idx 条读进界面。返回 0 = 成功。 */
static int note_load(int idx)
{
	int n = BoardNote_Load(idx, s_buf, (int)sizeof(s_buf));
	if (n < 0)
		return -1;

	/* 第一行 = 标题，之后 = 正文 */
	char* nl = strchr(s_buf, '\n');
	if (nl != NULL)
	{
		*nl = 0;
		YMGUI_TextInput_SetText(state.note_title_input, s_buf);
		YMGUI_EditView_SetText(state.note_editor, nl + 1);
	}
	else
	{
		YMGUI_TextInput_SetText(state.note_title_input, s_buf);
		YMGUI_EditView_SetText(state.note_editor, "");
	}
	state.note_index = idx;

	char st[64];
	snprintf(st, sizeof(st), "已载入 note #%d", idx);
	PhoneUI_text_set(state.note_status, st);
	return 0;
}

static void app_show(const char* argument)
{
	int idx = 0;
	if (argument != NULL && argument[0] >= '0' && argument[0] <= '9')
		idx = atoi(argument);
	if (note_load(idx) != 0)
	{
		/* 卡上没有这条：保持界面上的默认内容，不弹报错 */
		state.note_index = -1;
		PhoneUI_text_set(state.note_status, "个人 / 今天");
	}
}

static void note_save(GYOBJ obj)
{
	const char* title = YMGUI_TextInput_GetText(state.note_title_input);
	const char* body  = YMGUI_EditView_GetText(state.note_editor);
	int k = 0;
	int idx;

	(void)obj;
	if (title == NULL) title = "";
	if (body == NULL)  body = "";

	/* 拼 "标题\n正文"，手工拷贝避免 snprintf 对 UTF-8 的截断问题 */
	for (const char* p = title; *p != 0 && k < (int)sizeof(s_buf) - 2; ++p)
		s_buf[k++] = *p;
	if (k < (int)sizeof(s_buf) - 2) s_buf[k++] = '\n';
	for (const char* p = body; *p != 0 && k < (int)sizeof(s_buf) - 1; ++p)
		s_buf[k++] = *p;
	s_buf[k] = 0;

	idx = BoardNote_Save(state.note_index, s_buf);
	if (idx < 0)
	{
		char st[64];
		snprintf(st, sizeof(st), "保存失败 rc=%d", idx);
		PhoneUI_text_set(state.note_status, st);
		return;
	}
	state.note_index = idx;
	{
		char st[64];
		snprintf(st, sizeof(st), "已存到卡 note #%d", idx);
		PhoneUI_text_set(state.note_status, st);
	}
}

static void note_pin(GYOBJ obj)
{
	state.note_pinned = !state.note_pinned;
	PhoneUI_text_set(state.note_status, state.note_pinned ? "已置顶 / 个人" : "个人 / 今天");
	PhoneUI_button_set(obj, state.note_pinned ? "取消置顶" : "置顶笔记");
}
static void app_create(GYOBJ view)
{
	state = (AppState){.note_index = -1};
	PhoneUI_app_header(view, "笔记", "记录生活里的小美好");
	GYOBJ note = PhoneUI_panel(view, 18, 62, 284, 263, RGB(255, 245, 212));
	state.note_status = PhoneUI_left_label(note, 18, 10, 248, "个人 / 今天", RGB(151, 121, 53), 2);
	state.note_title_input = YMGUI_Creat_TextInput_Creat(note, 14, 34, 256, 32, 127);
	state.note_title_input->draw_cb = PhoneUI_phone_input_draw;
	YMGUI_TextInput_SetText(state.note_title_input, "周末计划");
	state.note_editor = YMGUI_Creat_EditView_Creat(note, 14, 74, 256, 128, 4096);
	YMGUI_EditView_SetText(state.note_editor, "去湖边散步。\n一杯咖啡，一本书，慢慢来。");
	YMGUI_EditView_SetWrap(state.note_editor, 1);
	YMGUI_EditView_SetPadding(state.note_editor, 6, 5, 6, 5);
	YMGUI_EditView_SetTextColor(state.note_editor, INK);
	YMGUI_EditView_SetBgColor(state.note_editor, RGB(255, 245, 212));
	YMGUI_EditView_SetBorderColor(state.note_editor, RGB(225, 207, 158));
	PhoneUI_left_label(note, 18, 228, 248, "保存到 /YMGUI/NOTE（TF 卡）", MUTED, 2);
	/* 原来只有一个"置顶笔记"按钮（18,351,284,44），现在拆成"保存"+"置顶"两半 */
	PhoneUI_button(view, 18, 351, 138, 44, "保存到卡", note_save, RGB(196, 149, 52));
	PhoneUI_button(view, 164, 351, 138, 44, "置顶笔记", note_pin, RGB(196, 149, 52));

	/* 进笔记时若卡上已有内容，直接载入第 0 条（没有就保持示例内容） */
	app_show(NULL);
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "note_status"))
		return (intptr_t)state.note_status;
	if (!strcmp(name, "note_title_input"))
		return (intptr_t)state.note_title_input;
	if (!strcmp(name, "note_editor"))
		return (intptr_t)state.note_editor;
	if (!strcmp(name, "note_pinned"))
		return (intptr_t)state.note_pinned;
	if (!strcmp(name, "note_index"))
		return (intptr_t)state.note_index;
	return 0;
}

const PhoneApp PhoneApp_notes = {
	.key = "notes", .title = "笔记", .color = RGB(225, 173, 69), .icon = ICON_NOTES, .create = app_create, .show = app_show, .inspect = app_inspect, .command = app_command, .destroy = app_destroy};
