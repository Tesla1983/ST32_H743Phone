/* ===========================================================================
 * 文件管理器（2026-10-09 改造：四个分类里"照片""笔记"接 TF 卡真实文件）
 *
 * 【范围】用户定的：**只做分类存储**，不做通用文件浏览器（没有多级目录树、
 * 没有重命名/复制/移动/搜索）。每个分类 = 卡上一个固定目录：
 *   /YMGUI/PIC   照片（*.bmp，点一个就导入图库，相册里能看）
 *   /YMGUI/NOTE  笔记（*.txt，见 apps/notes.c 的持久化）
 *   蓝牙接收      第 4 个分类（2026-10-10 P5 新增）：手机 --(经典蓝牙 SPP)-->
 *                 ESP32 --(SPI 从机)--> 本板 --(FatFs)--> /YMGUI/PIC，收完自动导入图库。
 *                 这一页是本文件里唯一"带进度条"的页面（控件建在 y=291，见 app_create）。
 *   录音          **板上没有音频硬件**（厂商 43 个例程里无任何录音/麦克风实验）
 *                 ⇒ 保持占位说明，不造假数据。
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

/* 蓝牙会话状态码，与 src/bt_recv.h 的 BT_RECV_* **同值**（phone_shell 不引那个头）。
 * 同步点：src/bt_recv.h 顶部那五个宏；两边任一改动必须同时改。 */
#define BTUI_IDLE  0
#define BTUI_WAIT  1
#define BTUI_RECV  2
#define BTUI_DONE  3
#define BTUI_ERROR 4

typedef struct
{
	GYOBJ file_path, file_rows[4], file_preview;
	GYOBJ bt_bar;        /* 蓝牙接收页的进度条（只在那个分类可见） */
	int file_folder;
	int pic_n;           /* 上次 BoardPic_Scan() 的条目数（缓存，别每帧问 board） */
	int import_pending;  /* 1 = 已排队，等 tick 收结果 */
	int import_rc;       /* 上一次导入的返回码；-1 = 还没结果 */
	int bt_last_p;       /* 上次写进进度条的值（只为少做无谓 SetValue；判据仍是当前值） */
	int bt_last_st;      /* 上次刷该页时的会话状态（变状态才重设按钮文案） */
	const char* bt_last_txt; /* 上次画上去的状态文字**指针**（静态字面量，见 bt_view_refresh） */
} AppState;
static AppState state;

/* 诊断量（P5）：当前分类，-1 = 在分类列表。
 * 用途**不是**给 UI 自己看的（UI 直接用 state.file_folder），而是给抓屏脚本一个
 * **不用抓帧就能确认"点进去了没"**的判据 —— bdtap 是异步注入、偶发会丢一次，
 * 而抓一帧要停核好几秒，拿抓帧来判"还没进去"既慢又会干扰被测对象。
 * ⚠ 名字带 g_ 前缀 + volatile：本工程开了 --gc-sections，只写不读的会留下，
 *   但只声明不用的会被回收（那时脚本读符号会 KeyError）。 */
volatile int g_files_folder = -1;

static int app_back(void);
static void file_refresh(void);
static void file_open(GYOBJ obj);
static void app_tick(uint32 elapsed);
static void bt_view_refresh(void);
static void bt_buttons_set(void);


/* ⚠ 第 4 项原来叫「下载」，2026-10-10（P5）改成「蓝牙接收」：
 *   板上没有联网下载能力，而**蓝牙收到的文件本来就是一"下载"** ——
 *   与其留一个永远空的占位分类，不如把这条真实通路放进来。
 *   改动只在文案与这一个分类的行为，行数/坐标一个没动。 */
static const char* folder_names[] = {"照片", "录音", "笔记", "蓝牙接收"};
static int app_back(void)
{
	if (state.file_folder < 0)
		return 0;
	state.file_folder = -1;
	g_files_folder = -1;
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

	/* 进度条只在"蓝牙接收"分类里可见（别的分类没有可画进度的东西）。
	 * ⚠ 隐藏必须显式做 —— SetHidden 是"切换 Hidden 位"而不是"设成 0"，
	 *   这里按分类当前值写死，来回切分类才不会残留。 */
	YMGUI_Obj_SetHidden(state.bt_bar, state.file_folder == 3 ? 0 : 1);

	if (state.file_folder < 0)
	{
		for (int i = 0; i < 4; ++i)
			PhoneUI_button_set(state.file_rows[i], folder_names[i]);
		YMGUI_TextView_SetText(state.file_preview,
			"照片与笔记读写 TF 卡的真实文件；\n录音无音频硬件，蓝牙接收走\n"
			"手机--ESP32--本板，收完自动进相册。");
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

	if (state.file_folder == 3)          /* ---- 蓝牙接收（P5，真实通路）---- */
	{
		bt_view_refresh();
		return;
	}

	/* ---- 录音：保持占位，不造假数据 ---- */
	for (int i = 0; i < 3; ++i)
		PhoneUI_button_set(state.file_rows[i], "（无音频硬件）");
	PhoneUI_button_set(state.file_rows[3], "返回上一级");
	YMGUI_TextView_SetText(state.file_preview,
		"板上没有麦克风 / 音频编解码器：\n厂商 43 个例程里无任何录音实验\n（只有 ADC 18、DAC 20）。");
}

/* ---------------------------------------------------------------------------
 * 蓝牙接收页的刷新（P5）
 *
 * 【为什么单独一个函数】这个页面的内容**每拍都在变**（进度在走），
 * 而 file_refresh() 会连带重设 4 个按钮的文案 + 路径标签。每拍都重设文案
 * 有两个问题：① 白白跑一堆 snprintf；② PhoneUI_button_set 内部若走
 * "变了才标脏"的比较，字符串比较也不便宜。
 * ⇒ 分开：进分类/切状态时调 file_refresh()，进度只在本函数里刷。
 * ⚠ 判据一律用 BoardBt_* 的**当前值**做比较，不缓存"我上次写了什么" ——
 *   2026-10-09 那个"缓存与实际脱钩 ⇒ 永久失同步"的坑就是这么来的。
 * --------------------------------------------------------------------------- */
static void bt_buttons_set(void)
{
	PhoneUI_button_set(state.file_rows[0], BoardBt_Busy() ? "中止接收" : "开始接收");
	PhoneUI_button_set(state.file_rows[1], "（手机端用蓝牙发送文件）");
	PhoneUI_button_set(state.file_rows[2], "（收完自动进相册）");
	PhoneUI_button_set(state.file_rows[3], "返回上一级");
}

static void bt_view_refresh(void)
{
	char text[224];
	int  st    = BoardBt_State();
	uint32_t got = BoardBt_Bytes(), total = BoardBt_Total();
	const char* name = BoardBt_Name();
	/* ⚠ 取一次复用，并把**指针**存进缓存做变化检测（见 app_tick）。
	 *   src/bt_recv.c 的 bt_recv_status_text() 返回的全是静态字面量，
	 *   所以指针相等等价于内容相等；换成动态拼串就必须改成内容比较。 */
	const char* txt = BoardBt_StatusText();

	bt_buttons_set();

	/* 状态归类 —— 文案必须与实际能力一致（不拿 0℃ 冒充"没取到"那套纪律） */
	if (st == BTUI_DONE)
		snprintf(text, sizeof(text),
			"状态：%s\n已收 %s（%u KB）\n已排队导入图库，去相册看。",
			txt, name[0] ? name : "文件",
			(unsigned)((got + 511u) / 1024u));
	else if (st == BTUI_ERROR)
		snprintf(text, sizeof(text),
			"状态：%s（返回码 %d）\n半截文件已删除，没留给相册。\n可点「开始接收」重来。",
			txt, BoardBt_LastRc());
	else if (BoardBt_Busy())
	{
		if (total != 0u)
			snprintf(text, sizeof(text),
				"状态：%s\n%u / %u KB（%d%%）",
				txt, (unsigned)(got / 1024u),
				(unsigned)(total / 1024u), BoardBt_Progress());
		else
			snprintf(text, sizeof(text),
				"状态：%s\n大小未知（对端文件头还没到）",
				BoardBt_StatusText());
	}
	else
		snprintf(text, sizeof(text),
			"状态：%s\n点「开始接收」，再从手机蓝牙发文件。\n"
			"（手机侧用任意蓝牙串口 App 发送）",
			txt);

	YMGUI_TextView_SetText(state.file_preview, text);

	/* 进度条：-1（总长未知）画 0；其余按 0..100。
	 * ⚠ SetValue 内部会钳制并标脏，重复写同一个值不会引发重绘。 */
	{
		int p = BoardBt_Progress();
		if (p < 0) p = 0;
		YMGUI_Bar_SetValue(state.bt_bar, p);
		/* 缓存"刚画上去的值"，交给 app_tick 做变化检测。
		 * ⚠ 缓存放在**这个函数末尾**、由它自己维护 —— 否则从 file_refresh()
		 *   进来的那条路径不会更新缓存，app_tick 会以为"没变"而不刷。 */
		state.bt_last_st  = st;
		state.bt_last_p   = p;
		state.bt_last_txt = txt;
	}
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
		g_files_folder = index;
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
		g_files_folder = -1;
		file_refresh();
		return;
	}
	if (state.file_folder == 3)          /* 蓝牙接收：第 0 行 = 开始/中止 */
	{
		if (index == 0)
		{
			/* 返回值可以忽略（2026-10-10 真实路径复核）：
			 *   BoardBt_Start() 只会在"已有会话在跑"时返回 -1，而那种情况下
			 *   BoardBt_Busy() 为真 ⇒ 按钮显示的是「中止接收」，走的是另一条分支；
			 *   BoardBt_Abort() 只在"本来没有会话"时返回 -1，同理走不到。
			 *   链路不通**不是**返回值能表达的 —— 那次会话会进 WAIT，
			 *   4 s × 重试后落到 g_bt_err=BTE_OPEN_TO，UI 显示"接收失败"。
			 *   ⇒ 这里不需要额外提示，状态行已经会说清楚。 */
			if (BoardBt_Busy())
				(void)BoardBt_Abort();
			else
				(void)BoardBt_Start();
		}
		/* 第 1/2 行只是说明，点了不做任何事（不弹假提示） */
		state.bt_last_st = -1;           /* 强制下一拍重设按钮文案 */
		bt_view_refresh();
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

	/* 蓝牙接收页：每拍跟着会话走。
	 * ⚠ 只在**这个分类可见时**刷 —— 会话在别的分类里跑完了照样要正确，
	 *   所以重进分类时 file_refresh() 会再刷一次全量（见下面 file_open）。 */
	if (state.file_folder == 3)
	{
		int st = BoardBt_State();
		int p  = BoardBt_Progress();
		if (p < 0) p = 0;
		/* ⚠ 判据必须带上**状态文字**：WAIT 阶段里 s_opening 从 1→0 时
		 *   st(WAIT) 与 p(0) 都没变，只看这两个的话文字会一直卡在
		 *   "正在启动蓝牙…"，永远变不成"等待对端发送"。
		 *   （2026-10-10 真实路径实测：点「开始接收」→ 对端 560 ms 就开好
		 *    蓝牙并回了 $BT，但那之后文字本该从"正在启动蓝牙…"翻成
		 *    "等待对端发送"—— 只看 st/p 的话这一翻永远不会发生。） */
		if (st != state.bt_last_st || p != state.bt_last_p
		    || BoardBt_StatusText() != state.bt_last_txt)
			bt_view_refresh();   /* 缓存由 bt_view_refresh 自己更新 */
	}
}
static void app_create(GYOBJ view)
{
	state = (AppState){.file_folder = -1, .pic_n = 0, .import_pending = 0,
	                   .import_rc = -1, .bt_last_p = -1, .bt_last_st = -1,
	                   .bt_last_txt = NULL};
	g_files_folder = -1;
	PhoneUI_app_header(view, "文件管理器", "照片 / 笔记读写 TF 卡");
	state.file_path = PhoneUI_left_label(view, 24, 65, 272, "", MUTED, 2);
	for (int i = 0; i < 4; ++i)
		state.file_rows[i] = PhoneUI_button(view, 24, 99 + i * 49, 272, 41, "", file_open, RGB(160, 143, 105));

	/* 进度条插在"最后一行按钮"与说明区之间：按钮到 y=286，说明区从 307 起，
	 * 中间只剩 21 px ⇒ 条放 (24, 291, 272, 12)，上下各留 ~4 px。
	 * ⚠ 改按钮行数（4 行 ×49）就必须同步这里的 y。 */
	state.bt_bar = YMGUI_Creat_Bar_Creat(view, 24, 291, 272, 12);
	YMGUI_Bar_SetRange(state.bt_bar, 0, 100);
	YMGUI_Bar_SetValue(state.bt_bar, 0);
	YMGUI_Bar_SetColors(state.bt_bar, RGB(70, 62, 52), RGB(231, 173, 65));

	state.file_preview = PhoneUI_app_text(view, 24, 307, 272, 92, "");
	file_refresh();
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
	g_files_folder = -1;
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
	/* ---- 蓝牙接收页（P5）—— 给抓屏脚本用的自检取数口 ----
	 * ⚠ 与 settings.c 的 app_inspect 同一纪律：只暴露"外面判断不了的状态"，
	 *   不暴露可以直接读的控件指针（那些脚本能用 name 拿到）。 */
	if (!strcmp(name, "bt_bar"))
		return (intptr_t)state.bt_bar;
	if (!strcmp(name, "bt_busy"))
		return (intptr_t)BoardBt_Busy();
	if (!strcmp(name, "bt_state"))
		return (intptr_t)BoardBt_State();
	if (!strcmp(name, "bt_progress"))
		return (intptr_t)BoardBt_Progress();
	return 0;
}

const PhoneApp PhoneApp_files = {
	.key = "files", .title = "文件管理", .color = RGB(231, 173, 65), .icon = ICON_FILES, .create = app_create, .tick = app_tick, .inspect = app_inspect, .back = app_back, .destroy = app_destroy};
