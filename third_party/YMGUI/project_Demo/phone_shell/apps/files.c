/* ===========================================================================
 * 文件管理器（2026-10-09 改造：四个分类里"照片""笔记"接 TF 卡真实文件）
 *
 * 【范围】用户定的：**只做分类存储**，不做通用文件浏览器（没有多级目录树、
 * 没有重命名/复制/移动/搜索）。每个分类 = 卡上一个固定目录：
 *   /YMGUI/PIC   照片（*.bmp，点一个就导入图库，相册里能看）
 *   /YMGUI/NOTE  笔记（*.txt，见 apps/notes.c 的持久化）
 *   录音 / 下载   **板上没有音频硬件**（厂商 43 个例程里无任何录音/麦克风实验）、
 *                 浏览器也没有联网下载能力 ⇒ 这两个分类保持占位说明，不造假数据。
 *
 * 【为什么只有 3 行能放文件】布局是 4 个按钮（y = 99 + i*49，高 41，到 y=286），
 * 下面 307 起是说明区，扩行会压到说明区。第 4 行留给"返回上一级"，
 * 所以一次最多列 3 个文件；总数与操作结果都写在下面的说明区里。
 *
 * 【导入为什么是异步的】见 src/img_store.h 顶部：灌 W25Q128 必须退出 memory-mapped，
 * 期间 **读 XIP 会精确 BusFault**。所以导入只能由主循环的 img_store_poll 执行
 * （它自己会置 g_img_busy 挡住渲染），本模块只负责"排队 + 用 tick 收结果"。
 * =========================================================================== */

#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardPic_*（不让 phone_shell 直接认识 FatFs） */

typedef struct
{
	GYOBJ file_path, file_rows[4], file_preview;
	int file_folder;
	int pic_n;           /* 上次 BoardPic_Scan() 的条目数（缓存，别每帧问 board） */
	int import_pending;  /* 1 = 已排队，等 tick 收结果 */
	int import_rc;       /* 上一次导入的返回码；-1 = 还没结果 */
} AppState;
static AppState state;

static int app_back(void);
static void file_refresh(void);
static void file_open(GYOBJ obj);
static void app_tick(uint32 elapsed);


static const char* folder_names[] = {"照片", "录音", "笔记", "下载"};
static int app_back(void)
{
	if (state.file_folder < 0)
		return 0;
	state.file_folder = -1;
	file_refresh();
	return 1;
}

/* 一行文件的文案："<名字>（<N> KB）"。名字过长就截断，避免超出按钮宽度被裁。 */
/* 行文案缓冲用的名字上限（不能引 src/img_store.h 里的宏 —— phone_shell 不依赖 src/） */
#define ROW_NAME_MAX 48

static void pic_row_text(int i, char* out, int n)
{
	char name[ROW_NAME_MAX];
	uint32_t size = 0;

	if (BoardPic_Name(i, name, (int)sizeof(name)) != 0)
	{
		snprintf(out, n, "（空）");
		return;
	}
	(void)BoardPic_Size(i, &size);

	/* 中文一个字 3 字节：按钮 272 px 宽放得下约 8~9 个中文字 + 尾巴。
	 * 超过 21 字节（7 个中文字）就截断，宁可短也不要被裁成半截字。 */
	if (strlen(name) > 21u)
	{
		name[18] = '.'; name[19] = '.'; name[20] = '.'; name[21] = 0;
	}
	snprintf(out, n, "%s（%u KB）", name, (unsigned)((size + 1023u) / 1024u));
}

static void file_refresh(void)
{
	char path[64];
	snprintf(path, sizeof(path), "演示存储 / %s", state.file_folder < 0 ? "" : folder_names[state.file_folder]);
	PhoneUI_text_set(state.file_path, path);

	if (state.file_folder < 0)
	{
		for (int i = 0; i < 4; ++i)
			PhoneUI_button_set(state.file_rows[i], folder_names[i]);
		YMGUI_TextView_SetText(state.file_preview,
			"照片与笔记读写 TF 卡的真实文件；\n录音无音频硬件、下载无网络来源，\n这两个分类只作占位说明。");
		return;
	}

	if (state.file_folder == 0)          /* ---- 照片：真实目录 ---- */
	{
		for (int i = 0; i < 3; ++i)
		{
			char row[64];
			if (i < state.pic_n)
				pic_row_text(i, row, (int)sizeof(row));
			else
				snprintf(row, sizeof(row), "（空）");
			PhoneUI_button_set(state.file_rows[i], row);
		}
		PhoneUI_button_set(state.file_rows[3], "返回上一级");

		char text[160];
		if (state.import_pending)
			snprintf(text, sizeof(text), "正在导入到图库…\n导入完成后到相册查看。");
		else if (state.import_rc >= 0)
			snprintf(text, sizeof(text), "/YMGUI/PIC 共 %d 个 .bmp\n上次导入返回码 %d（0 = 成功）",
					 state.pic_n, state.import_rc);
		else
			snprintf(text, sizeof(text), "/YMGUI/PIC 共 %d 个 .bmp\n点一个文件即导入图库。", state.pic_n);
		YMGUI_TextView_SetText(state.file_preview, text);
		return;
	}

	if (state.file_folder == 2)          /* ---- 笔记：真实目录 ---- */
	{
		for (int i = 0; i < 3; ++i)
		{
			char row[64];
			if (i < state.pic_n)
				pic_row_text(i, row, (int)sizeof(row));
			else
				snprintf(row, sizeof(row), "（空）");
			PhoneUI_button_set(state.file_rows[i], row);
		}
		PhoneUI_button_set(state.file_rows[3], "返回上一级");
		YMGUI_TextView_SetText(state.file_preview,
			"/YMGUI/NOTE 下的 .txt\n点一个文件即在笔记里打开，\n最后一行是返回上一级。");
		return;
	}

	/* ---- 录音 / 下载：保持占位，不造假数据 ---- */
	static const char* entries[2][3] = {
		{"（无音频硬件）", "（无音频硬件）", "（无音频硬件）"},
		{"（无网络来源）", "（无网络来源）", "（无网络来源）"}};
	for (int i = 0; i < 3; ++i)
		PhoneUI_button_set(state.file_rows[i], entries[state.file_folder == 1 ? 0 : 1][i]);
	PhoneUI_button_set(state.file_rows[3], "返回上一级");
	YMGUI_TextView_SetText(state.file_preview, state.file_folder == 1
		? "板上没有麦克风 / 音频编解码器：\n厂商 43 个例程里无任何录音实验\n（只有 ADC 18、DAC 20）。"
		: "浏览器是演示态，没有联网下载能力，\n因此没有真实的下载内容可列。");
}

static void file_open(GYOBJ obj)
{
	int index = 0;
	for (int i = 0; i < 4; ++i)
		if (obj == state.file_rows[i])
			index = i;
	if (state.file_folder < 0)
	{
		state.file_folder = index;
		/* 进分类时扫一次卡（毫秒级）。录音/下载不扫 —— 它们没有真实目录。 */
		if (index == 0)
			state.pic_n = BoardPic_Scan();
		else if (index == 2)
			state.pic_n = BoardNote_Scan();
		else
			state.pic_n = 0;
		file_refresh();
		return;
	}
	if (index == 3)
	{
		state.file_folder = -1;
		file_refresh();
		return;
	}
	if (state.file_folder == 0)          /* 照片：点一个就导入 */
	{
		if (index < state.pic_n && BoardPic_Busy() == 0)
		{
			if (BoardPic_Import(index) == 0)
			{
				state.import_pending = 1;
				state.import_rc = -1;
			}
		}
		file_refresh();
		return;
	}
	if (state.file_folder == 2)          /* 笔记：点一个就在笔记 app 里打开 */
	{
		if (index < state.pic_n)
		{
			char arg[4];
			snprintf(arg, sizeof(arg), "%d", index);
			PhoneHost_Open(NOTES, arg);
		}
		file_refresh();
		return;
	}
	file_refresh();
}

static void app_tick(uint32 elapsed)
{
	(void)elapsed;
	if (state.import_pending && BoardPic_Busy() == 0)
	{
		state.import_pending = 0;
		state.import_rc = BoardPic_LastRc();
		file_refresh();
	}
}
static void app_create(GYOBJ view)
{
	state = (AppState){.file_folder = -1, .pic_n = 0, .import_pending = 0, .import_rc = -1};
	PhoneUI_app_header(view, "文件管理器", "照片 / 笔记读写 TF 卡");
	state.file_path = PhoneUI_left_label(view, 24, 65, 272, "", MUTED, 2);
	for (int i = 0; i < 4; ++i)
		state.file_rows[i] = PhoneUI_button(view, 24, 99 + i * 49, 272, 41, "", file_open, RGB(160, 143, 105));
	state.file_preview = PhoneUI_app_text(view, 24, 307, 272, 92, "");
	file_refresh();
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "file_path"))
		return (intptr_t)state.file_path;
	if (!strcmp(name, "file_rows"))
		return index >= 0 && index < 4 ? (intptr_t)state.file_rows[index] : 0;
	if (!strcmp(name, "file_preview"))
		return (intptr_t)state.file_preview;
	if (!strcmp(name, "file_folder"))
		return (intptr_t)state.file_folder;
	if (!strcmp(name, "pic_n"))
		return (intptr_t)state.pic_n;
	return 0;
}

const PhoneApp PhoneApp_files = {
	.key = "files", .title = "文件管理", .color = RGB(231, 173, 65), .icon = ICON_FILES, .create = app_create, .tick = app_tick, .inspect = app_inspect, .back = app_back, .destroy = app_destroy};
