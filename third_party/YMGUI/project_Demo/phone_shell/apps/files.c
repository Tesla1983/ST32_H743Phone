/* ===========================================================================
 * 文件管理器（2026-10-09 改造：照片 / 笔记 接 TF 卡真实文件）
 *
 * 【范围】用户定的：**只做分类存储**，不做通用文件浏览器（没有多级目录树、
 * 没有重命名/复制/移动/搜索 —— 除了录音卡片这里需要的那几个）。
 * 每个分类 = 卡上一个固定目录：
 *   /YMGUI/PIC   照片（*.bmp）
 *   /YMGUI/NOTE  笔记（*.txt）
 *   /YMGUI/RECORD 录音（*.WAV，见 recorder.c / audio_rec.c）—— **本文件把它做成卡片列表**
 *   蓝牙接收     第 4 个分类（2026-10-10 P5）：手机 → ESP32 → 本板 → 图库。
 *
 * 【录音卡片（2026-10-16 补丁19）】
 *   旧版「录音」分类是占位（板上当时没有音频硬件）。现在 ES8311 录音链路已通
 *   （补丁18），文件管理器把 /YMGUI/RECORD 里的录音当成**卡片列表**呈现：
 *     · 卡片展示：名称 / 时长 / 录制日期 / 大小；
 *     · 卡片内置播放/暂停（点左侧圆形键）与进度条；
 *     · 选中一段后可 删除（两段确认）/ 重命名（IME 输入）/ 播放（交给音乐播放器 App，补丁29）；
 *     · 列表加载（进入即扫卡）/ 下拉刷新（顶部下拉或「刷新」键）/ 空状态；
 *     · 播放失败、文件损坏等异常一律如实提示（假数据零容忍）。
 *   列表用**一个自定义容器**自绘所有卡片并做坐标命中（选择/播放/拖拽滚动/下拉刷新），
 *   避免多个按钮互相吞点击。底部操作栏与导航键是独立的普通按钮。
 *
 * 【为什么照片/笔记不是卡片】它们沿用「4 行按钮 + 说明区」的简单列表；录音因为要带
 *   播放/进度/操作，单独做成更丰富的卡片列表，风格（圆角白卡、强调色）与全 App 统一。
 * =========================================================================== */

#include "phone_ui.h"
#include "phone_host.h"
#include "phone_shell_board.h"   /* BoardPic_* / BoardNote_* / BoardBt_* / BoardRec_* */

#include <string.h>
#include <stdio.h>

/* 蓝牙会话状态码，与 src/bt_recv.h 的 BT_RECV_* **同值**。 */
#define BTUI_IDLE  0
#define BTUI_WAIT  1
#define BTUI_RECV  2
#define BTUI_DONE  3
#define BTUI_ERROR 4

/* ---- 录音卡片列表布局常量（与 rec_list_event 的坐标命中共用，必须一致）----
 * ⚠⚠ 2026-10-18 修：view 高 412（0..411），而旧的 REC_SLOTS=4 ⇒ 列表 88..352，
 *   下面 3 个操作键(y=378,H=38)底边到 416、2 个导航键(y=424)整排 424..462
 *   **完全在视口之外** ⇒ 录音页的「返回上一级/刷新」根本看不见、按不到。
 *   ⇒ 槽位 4→3（列表 88..286），下面依次排进 0..411（列表可滚动，少一槽无损失）。 */
#define REC_LIST_X 12
#define REC_LIST_Y 88
#define REC_LIST_W 296
#define REC_CARD_H 66
#define REC_SLOTS   3
#define REC_LIST_H (REC_SLOTS * REC_CARD_H)   /* 198 ⇒ 88..286 */

/* ---- 录音列表右侧下拉滑条（2026-10-18 补丁28，与照片列表同一套算法）----
 * 卡片宽度要**收进滑条左侧**，否则滑条会压在卡片右端的时长/日期文字上
 * （照片列表踩过一模一样的坑：卡片右边界 308、滑条 302 起 ⇒ 滑条压字）。
 * 滑条区 = REC_BAR_W + REC_BAR_MARGIN + 右侧留白(4)。 */
#define REC_BAR_W      6
#define REC_BAR_MARGIN 2
#define REC_CARD_W     (REC_LIST_W - REC_BAR_W - REC_BAR_MARGIN - 4)   /* = 284 */
#define REC_BAR_MIN    24     /* 滑块最小高度（与 PIC_BAR_MIN 同值）*/
#define REC_BAR_GRAB   4      /* 触点落在滑条左/右各这么多像素内即认为按在滑条上
                               * ★取 4 是算过的：卡片右边界 = x+284−1 = 295，
                               *   滑条左边界 = 300，"300−4 = 296" 正好在卡片之外
                               *   ⇒ 点卡片右缘不会误抓滑条，而滑条仍有 6+4 的可抓宽度。 */

typedef struct
{
	GYOBJ file_path, file_rows[5], file_preview;
	GYOBJ bt_bar;        /* 蓝牙接收页的进度条（只在那个分类可见） */
	int file_folder;
	int pic_n;           /* 上次 BoardPic_Scan() 的条目数（缓存，别每帧问 board） */
	/* ---- 照片/笔记列表：翻页 + 选中 + 删除（2026-10-18 新增）----
	 * 为什么需要翻页：列表只有 3 个文件行（第 4 行固定是"返回上一级"），
	 *   而 /YMGUI/PIC、/YMGUI/NOTE 里的文件数可以远超 3 ⇒ 不翻页就永远看不到第 4 个之后。
	 * sel_row 记**行号**（0..2），全局下标 = page*3 + sel_row。 */
	/* ---- 照片/笔记：可滑动的卡片列表 + 边条（补丁25，照 settings.c 的滚动范式）----
	 * 旧版是"3 行 + 翻页"，文件一多就得到处翻；改成**连续滑动** + 右侧边条指示位置。 */
	GYOBJ pic_list;      /* 自绘卡片列表（含边条）：**一个**对象，见 pic_list_draw 说明 */
	int   pic_scroll;    /* 纵向滚动偏移（**像素**，连续滚动，与 settings 的 scroll_y 同语义）*/
	int   pic_sel;       /* 选中的文件**全局下标**；-1 = 未选 */
	int   pic_press_entry; /* 按下时命中的文件下标；-1 = 没命中卡片 */
	int   pic_drag_mode;   /* 0 = 拖列表体滚动；1 = 拖右侧边条滚动（照设置 App 边条滑动）*/
	/* ---- 手势状态（补丁25e）：**按拍增量**滚动，见 pic_scroll_tick 的长注释 ----
	 * 旧实现把"按下点"当滚动基准（press_scroll - dy），一旦输入层在拖动中途漏掉一帧
	 * ⇒ 引擎判一次抬起、下一帧再判一次按下，基准被重置到手指当前位置 ⇒ dy 永远≈0
	 * ⇒ **完全滑不动**；同时那次假抬起还会被当成点击（选中错卡片/触发导入）。
	 * 现在基准是"上一拍的指针位置"，逐拍累加位移，单帧抖动最多丢一帧位移。 */
	int   pic_gesture;     /* 1 = 手势进行中（本 App 自己维护，不依赖按下/抬起事件成对）*/
	int   pic_last_y;      /* 上一拍指针 y（屏幕坐标）—— 增量滚动的唯一基准 */
	int   pic_move_acc;    /* 本次手势累计移动像素：>PIC_TAP_MAX 即判拖动，不当点击 */
	int   pic_import_idx;  /* >=0 = 下一拍要导入的照片下标（把耗时动作移出事件回调）*/
	int   del_arm;       /* 1 = 删除已按一次，等二次确认 */
	char  hint[80];      /* 操作结果/提示（写在说明区末尾；静态字面量拷进来）*/
	GYOBJ file_bar;      /* 底部操作栏：**一个**自绘对象 = 返回上一级/删除（见 file_bar_draw 说明）*/
	int import_pending;  /* 1 = 已排队，等 tick 收结果 */
	int import_rc;       /* 上一次导入的返回码；-1 = 还没结果 */
	int bt_last_p;       /* 上次写进进度条的值 */
	int bt_last_st;      /* 上次刷该页时的会话状态 */
	const char* bt_last_txt; /* 上次画上去的状态文字**指针** */

	/* ---- 录音卡片（补丁19）---- */
	GYOBJ rec_list;          /* 自定义滚动卡片容器（自绘 + 坐标命中）*/
	GYOBJ rec_hint;          /* 操作结果 / 提示条 */
	GYOBJ rec_del, rec_rename, rec_play;    /* 底部操作栏（补丁29：分享→播放）*/
	GYOBJ rec_back, rec_refresh;            /* 底部导航栏 */
	GYOBJ rec_rn_panel, rec_rn_input, rec_rn_ok, rec_rn_cancel, rec_rn_title; /* 重命名浮层 */
	int   rec_n;             /* 当前枚举到的录音条数 */
	int   rec_loading;       /* 1 = 正在加载（扫卡中）*/
	int   rec_need_scan;     /* 1 = 下一拍执行一次扫描（进入/刷新触发）*/
	int   rec_scroll;        /* 列表滚动基准（可见首条的下标）*/
	int   rec_selected;      /* 当前选中的卡片下标；-1 = 未选 */
	int   rec_del_arm;       /* 1 = 删除已按一次，等二次确认 */
	int   rec_pull;          /* 1 = 顶部下拉刷新已就绪（松手即刷）*/
	/* 拖拽/点击的临时量（一次按下周期内有效）*/
	int   rec_press_x, rec_press_y, rec_press_scroll;
	int   rec_press_entry, rec_press_play, rec_drag_moved;
	int   rec_bar_drag;      /* 1 = 本次手势按在右侧滑条上（拖动=绝对映射，不滚卡片、不算点击）*/
} AppState;
/* ⚠ AppState 放在 **SRAM2（.bss_sram2）**，绝不进 DTCM。
 * 【为什么】linker/stm32h743vit6.ld 里 `_heap0_start=_sbrk_end`、`_heap0_end=_estack−32KB`
 *   ⇒ **DTCM 的 .bss 与 heap0 抢同一块内存**：.bss 多一字节，heap0 就少一字节。
 *   而 phone_shell.c 开机就用 `PhoneApps_Create(i, app_views[i])` 把**所有** App 的
 *   create() 全部跑一遍 ⇒ 本 App 一开机就要建对象，直接顶到 heap0 上限。
 *   （2026-10-18 白屏事故：给 AppState 加了 hint[80]+3 个 GYOBJ+3 个 int（约 104 B），
 *   再加 3 个按钮对象（约 480 B）⇒ heap0 只剩 192 B ⇒ HardFault，主循环一次都没跑完。）
 * ⇒ 本文件的大状态一律搬出 DTCM，与 audio_rec.c 的 REC_BUF_ATTR 同做法。 */
#define FILES_STATE_ATTR __attribute__((section(".bss_sram2"), aligned(8)))
static AppState FILES_STATE_ATTR state;

/* 诊断量（P5）：当前分类，-1 = 在分类列表。用途见 files.c 旧注释。 */
volatile int g_files_folder = -1;
/* 照片/笔记列表诊断（2026-10-18 补丁25b 修复"显示错误/不能下滑"）：
 *   g_pic_evt        —— 列表自绘容器收到的事件计数（验证"事件有没有进到列表"）。
 *   g_pic_scroll_diag—— 当前滚动偏移（验证"能不能下滑"：拖完它应变化）。
 *   这两条只能由固件镜像出来，SWD 读全局变量即可，不依赖对象字段。 */
volatile int g_pic_evt = 0;
volatile int g_pic_scroll_diag = 0;
/* 补丁25e 新增：把"拖动到底走没走通"拆成**可直接计数**的分支量（铁律10：
 *   "哪条分支被走到"只能直接计数，不能拿别的量反推）。一次"按住往上滑"之后：
 *     g_pic_press_evt    ↑1    按下事件进了列表
 *     g_pic_hold_evt     ↑N    每拍一次（**按拍增量**驱动，不依赖 Pressing 事件）
 *     g_pic_move_px      ↑     累计位移（负数表示往上拖）
 *     g_pic_scroll_diag  ↑     滚动偏移变了 ⇒ 松手后画面就该停在新位置
 *     g_pic_tap          不变  拖动手势不会被误判成点击
 *   若 press_evt 不涨 ⇒ 事件根本没进列表（命中/层级问题）；
 *   若 press_evt 涨而 move_px 不涨 ⇒ 指针坐标没更新（输入层）。 */
volatile int g_pic_press_evt = 0;
volatile int g_pic_hold_evt  = 0;
volatile int g_pic_move_px   = 0;
volatile int g_pic_rel_evt   = 0;
volatile int g_pic_tap       = 0;

static int app_back(void);
static void file_del_reset(void);   /* 前置声明：app_back() 在它定义之前就要用 */
static void file_refresh(void);
static void file_open(GYOBJ obj);
static void app_tick(uint32 elapsed);
static void bt_view_refresh(void);
static void bt_buttons_set(void);

/* ---- 照片/笔记：可滑动卡片列表 + 边条（补丁25）----
 * 这些 static 函数在 file_list_refresh / file_delete / app_create 之前被引用，
 * 需要前向声明（C 不允许 static 函数定义前隐式调用）。 */
static void pic_clamp(void);
static int  pic_content_h(void);
static int  pic_max_scroll(void);
static void pic_draw_bar(GYSURFACE surface, const GYrect* area);
static void pic_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area);
static void pic_list_event(GYOBJ obj, GYEvent e);
static void pic_scroll_tick(void);          /* 每拍增量滚动（app_tick 调用）*/

/* ---- 录音卡片处理函数（补丁19）---- */
static void rec_enter(void);            /* 进入录音分类：挂卡列表、触发扫描 */
static void rec_leave(void);            /* 离开：隐藏录音控件、停播放 */
static void rec_apply_visibility(int rec); /* 按是否在录音分类切换两套控件显隐 */
static void rec_do_scan(void);          /* 真正执行 BoardRec_Scan 并夹紧下标 */
static void rec_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* abs);
static void rec_list_event(GYOBJ obj, GYEvent e);
static void rec_toggle_play(int entry); /* 播放/暂停/续播 同一入口 */
static void rec_on_del(GYOBJ obj);
static void rec_on_rename(GYOBJ obj);
static void rec_on_play(GYOBJ obj);
static void rec_on_back(GYOBJ obj);
static void rec_on_refresh(GYOBJ obj);
static void rec_rn_open(void);          /* 打开重命名浮层 */
static void rec_rn_ok(GYOBJ obj);
static void rec_rn_cancel(GYOBJ obj);
static void rec_set_hint(const char* s); /* 写 rec_hint（静态字面量或本文件缓冲）*/


/* 第 4 项「录音」—— 占位已去掉，改为真实卡片列表（见文件头）。 */
/* ---- 分类（补丁30：新增「音乐」）----
 * ⚠ 下标 = **分类 id**，是语义常量，别按显示顺序改它：
 *   0=照片 / 1=录音 / 2=笔记 / 3=蓝牙接收 / 4=音乐（TF 卡 /YMGUI/MUSIC）。
 *   文件里到处是 `folder == 0/1/2/3` 的判定，改 id 等于把这些判断全部改一遍。
 *   用户要的"照片下面"是**显示顺序**问题 ⇒ 用 root_order[] 做一层映射，id 保持不变。 */
#define FOLDER_MUSIC 4
static const char* folder_names[] = {"照片", "录音", "笔记", "蓝牙接收", "音乐"};
/* 根页 5 行的显示顺序：照片 → **音乐** → 录音 → 笔记 → 蓝牙接收 */
static const int root_order[5] = {0, FOLDER_MUSIC, 1, 2, 3};
#define FOLDER_ROWS 5

static int app_back(void)
{
	if (state.file_folder < 0)
		return 0;
	if (state.file_folder == 1)
		rec_leave();              /* 离开录音分类：停掉正在放的卡片 */
	file_del_reset();             /* 返回时撤销可能残留的"删除待确认" */
	state.file_folder = -1;
	g_files_folder = -1;
	state.hint[0] = 0;
	file_refresh();
	return 1;
}

/* 一行文件的文案（照片/笔记用）。名字过长就截断。 */
#define ROW_NAME_MAX 48

/* （照片/笔记卡片现在由 pic_list_draw 自绘，不再用 file_row_text 取文案）*/

/* ---- 文件行的自定义绘制（2026-10-18）：图标靠左 + 文本居中于剩余区域 ----
 * 为什么不用 PhoneUI_button 自带的 button_draw：它把图标画在**正中间、文字压在下面**，
 * 41px 的行高放不下"图标 + 文字"两行，且文件管理器应该是图标在左、名字在右。
 * ⇒ 这里自绘：图标固定在行内左侧同一 x（与文件/文件夹图标同尺寸同垂直中心），
 *   选中态用强调色底，按下态沿用 button_draw 的灰蓝 —— 三种状态视觉一致。 */
#define FILE_ROW_ICON_X 22          /* 图标中心相对行左边的偏移 */
#define FILE_ROW_TEXT_X 44          /* 文本区起点（图标右侧留白）*/

static void file_row_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	PhoneButton* b   = (PhoneButton*)obj->user_data;
	int          sel = 0;     /* file_rows 现在只在分类列表/蓝牙页出现，没有行选中概念 */

	GYcolor color = (obj->state & GY_STATE_Pressed) ? RGB(99, 135, 153)
	              : sel ? RGB(214, 168, 74)          /* 选中：暖黄强调色（与分类文件夹同色系）*/
	              : obj->bg_color;
	if (color)
		PhoneUI_rounded(surface, area, color, area->h >= 50 ? 15 : 12, GY_OPA_COVER);

	if (b->icon)
		PhoneUI_icon_draw(surface, b->icon, (GYcoord)(area->x + FILE_ROW_ICON_X),
						  (GYcoord)(area->y + area->h / 2), obj->bg_color);

	{
		int tx   = area->x + (b->icon ? FILE_ROW_TEXT_X : 0);
		int tw   = area->w - (b->icon ? FILE_ROW_TEXT_X : 0);
		PhoneUI_draw_text(surface, b->value, sel ? RGB(58, 42, 20) : RGB(250, 252, 253), 0,
						  (GYcoord)(tx + tw / 2), (GYcoord)(area->y + (area->h - 24) / 2));
	}
}

/* （照片/笔记列表已改为连续滑动，旧"翻页 + 行号→全局下标"模型废弃）*/

/* 照片 / 笔记 两个分类共用的列表刷新（2026-10-18）：
 *   3 行文件（带文件图标）+ 第 4 行"返回上一级"（带返回图标）+ 翻页/删除栏。
 * 之所以抽出来：原来两个分类各抄一遍，而"笔记用 BoardPic_* 取名"那个 bug 就藏在
 *   两份拷贝里 —— 合并成一份后这类分叉不可能再出现。 */
static void file_list_refresh(void)
{
	char preview[144];

	/* 夹紧：删文件后内容变短，滚动位置与选中项都可能失效 */
	pic_clamp();
	if (state.pic_sel >= state.pic_n) state.pic_sel = -1;

	/* 列表交给自绘容器（可滑动 + 边条），旧的 5 行按钮在照片/音乐/笔记里隐藏 */
	for (int i = 0; i < FOLDER_ROWS; ++i)
		YMGUI_Obj_SetHidden(state.file_rows[i], 1);
	YMGUI_Obj_SetHidden(state.pic_list, 0);
	if (state.pic_list) YMGUI_Obj_Invalidate(state.pic_list);

	/* 操作栏（返回上一级 / 删除）*/
	YMGUI_Obj_SetHidden(state.file_bar, 0);
	if (state.file_bar) YMGUI_Obj_Invalidate(state.file_bar);

	YMGUI_Obj_SetHidden(state.file_preview, 0);
	{
		/* 三个"文件列表"分类共用这一套（照片 / 音乐 / 笔记）*/
		const char* dir = "/YMGUI/PIC";
		const char* act = "点一个即导入图库";

		if (state.file_folder == 2)
		{
			dir = "/YMGUI/NOTE"; act = "点一个即在笔记打开";
		}
		else if (state.file_folder == FOLDER_MUSIC)
		{
			dir = "/YMGUI/MUSIC"; act = "点一个即用音乐播放器播放";
		}

		if (state.pic_n <= 0)
			snprintf(preview, sizeof(preview), "%s 下没有文件", dir);
		else
			snprintf(preview, sizeof(preview),
				"%s 共 %d 个文件\n上下滑动查看 · %s",
				dir, state.pic_n, act);

		/* 操作结果/提示接在后面（删除成功/失败、请先选中…）*/
		if (state.hint[0])
		{
			size_t used = strlen(preview);
			if (used + 1u < sizeof(preview))
				snprintf(preview + used, sizeof(preview) - used, "\n%s", state.hint);
		}
		YMGUI_TextView_SetText(state.file_preview, preview);
	}
}

static void file_refresh(void)
{
	/* 2026-10-18（补丁29）：顶部路径去掉"演示存储 / "前缀（用户要求）。
	 * 分类页只显示分类名；根页（分类列表）不显示 —— 顶部标题已经是"文件管理"，
	 * 这里再挂一条假的多级路径既没用也占一行。 */
	PhoneUI_text_set(state.file_path,
		state.file_folder < 0 ? "" : folder_names[state.file_folder]);

	/* 进度条只在"蓝牙接收"分类里可见。 */
	YMGUI_Obj_SetHidden(state.bt_bar, state.file_folder == 3 ? 0 : 1);

	/* 说明区默认位置（照片/笔记页：276..368，给底栏 372..406 让位）。
	 * 蓝牙页没有底栏、但多一根进度条(291..303) ⇒ 在下面单独把说明区下移到 310。 */
	YMGUI_Obj_SetPos(state.file_preview, 24, 276);

	/* 两套控件互斥显隐：录音分类用卡片列表，其余用 4 行按钮 + 说明区。 */
	rec_apply_visibility(state.file_folder == 1 ? 1 : 0);

	/* 照片/笔记的可滑动列表默认隐藏；只有 file_list_refresh()（照片/笔记分类）会显示它。
	 * 在这里先藏住，避免离开该分类后它还残留在别的页面底下。 */
	YMGUI_Obj_SetHidden(state.pic_list, 1);

	if (state.file_folder < 0)
	{
		/* 五个分类 = 五个文件夹 ⇒ 统一用（Windows 风格黄色）文件夹图标。
		 * ⚠ 文案按 root_order[] 的**显示顺序**取，按钮下标仍是物理行号（0..4）。 */
		for (int i = 0; i < FOLDER_ROWS; ++i)
		{
			PhoneUI_button_set(state.file_rows[i], folder_names[root_order[i]]);
			PhoneUI_button_icon(state.file_rows[i], ICON_FILES);
			YMGUI_Obj_SetHidden(state.file_rows[i], 0);
		}
		/* 操作栏只属于"文件列表页"，分类页不显示 */
		YMGUI_Obj_SetHidden(state.file_bar, 1);
		/* 5 行占 92..288 ⇒ 说明区从 294 起（照片/笔记页是 276，那两页只有列表）*/
		YMGUI_Obj_SetPos(state.file_preview, 24, 294);
		YMGUI_TextView_SetText(state.file_preview,
			"照片 / 音乐 / 笔记 / 录音 都是 TF 卡真实文件；\n"
			"音乐放 /YMGUI/MUSIC，点一个直接交给音乐播放器；\n"
			"蓝牙接收走手机--ESP32--本板。");
		YMGUI_Obj_SetHidden(state.file_preview, 0);
		return;
	}

	/* ---- 照片 / 音乐 / 笔记：共用同一个滑动列表（补丁30 把音乐并进来）---- */
	if (state.file_folder == 0 || state.file_folder == 2 ||
		state.file_folder == FOLDER_MUSIC)
	{
		file_list_refresh();
		return;
	}

	if (state.file_folder == 3)          /* ---- 蓝牙接收 ---- */
	{
		/* 这一页的 4 行是"开始接收/说明文字"，不是文件条目 ⇒ 不放图标、不显示翻页删除 */
		for (int i = 0; i < FOLDER_ROWS; ++i)
			PhoneUI_button_icon(state.file_rows[i], ICON_NONE);
		YMGUI_Obj_SetHidden(state.file_rows[4], 1);   /* 第 5 行（音乐）只属于根页 */
		YMGUI_Obj_SetHidden(state.file_bar, 1);
		/* 进度条占 291..303 ⇒ 说明区下移到 310..402（这页没有底栏，放得下 3 行）*/
		YMGUI_Obj_SetPos(state.file_preview, 24, 310);
		bt_view_refresh();
		return;
	}

	/* ---- 录音：卡片列表（控件在 rec_list 自绘，这里只保证显隐）---- */
	YMGUI_Obj_SetHidden(state.file_preview, 1);
	rec_set_hint("");                    /* 进入时清掉上次的提示 */
}

static void bt_buttons_set(void)
{
	PhoneUI_button_set(state.file_rows[0], BoardBt_Busy() ? "中止接收" : "开始接收");
	PhoneUI_button_set(state.file_rows[1], "（手机端用蓝牙发送文件）");
	PhoneUI_button_set(state.file_rows[2], "（收完自动导入，仅支持 BMP/JPG）");
	PhoneUI_button_set(state.file_rows[3], "返回上一级");
}

static void bt_view_refresh(void)
{
	char text[224];
	int  st    = BoardBt_State();
	uint32_t got = BoardBt_Bytes(), total = BoardBt_Total();
	const char* name = BoardBt_Name();
	const char* txt = BoardBt_StatusText();

	bt_buttons_set();

	if (st == BTUI_DONE)
		snprintf(text, sizeof(text),
			"状态：%s\n已收 %s（%u KB）",
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

	{
		int p = BoardBt_Progress();
		if (p < 0) p = 0;
		YMGUI_Bar_SetValue(state.bt_bar, p);
		state.bt_last_st  = st;
		state.bt_last_p   = p;
		state.bt_last_txt = txt;
	}
}

/* 把"删除待确认"复位（按钮文案/配色回到常态）。
 * 换页、换选中、返回、进入分类都要调 —— 否则上一次的"确认删除?"会残留到下一个文件上，
 * 变成"以为在删 A，其实删了 B"（危险且不诚实）。 */
static void file_del_reset(void)
{
	if (state.del_arm == 0) return;
	state.del_arm = 0;
	/* 删除键是自绘的（file_bar），复位配色只需要重绘 */
	if (state.file_bar) YMGUI_Obj_Invalidate(state.file_bar);
}

/* 回到分类列表（根）。照片/笔记改成滑动列表后，"返回上一级"由底栏按钮调用这里。 */
static void file_goto_root(void)
{
	file_del_reset();
	state.file_folder = -1;
	g_files_folder    = -1;
	state.pic_sel     = -1;
	state.pic_scroll  = 0;
	state.pic_gesture = 0;
	state.pic_move_acc = 0;
	state.pic_import_idx = -1;
	state.hint[0]     = 0;
	file_refresh();
}

static void file_open(GYOBJ obj)
{
	int row = -1;
	for (int i = 0; i < FOLDER_ROWS; ++i)
		if (obj == state.file_rows[i])
			row = i;
	/* ★ 修：没命中任何一行就直接返回。旧代码 index 初值是 0 ⇒ 任何"不是这四行"的
	 *   调用都会被当成点了第 0 行（静默做了别的事）。 */
	if (row < 0)
		return;

	if (state.file_folder < 0)
	{
		/* 根页：行号是**显示顺序**，分类 id 要走 root_order[] 映射
		 * （补丁30：音乐显示在照片下面，但它的 id 仍是 4，不是 1）。 */
		int folder = root_order[row];

		state.file_folder = folder;
		g_files_folder    = folder;
		file_del_reset();
		/* 进分类一律回到顶部、清选中（否则会带着上一次的滚动位置/选中下标进来）*/
		state.pic_scroll     = 0;
		state.pic_sel        = -1;
		state.pic_gesture    = 0;
		state.pic_move_acc   = 0;
		state.pic_drag_mode  = 0;
		state.pic_import_idx = -1;
		if (folder == 0)
		{
			int n = BoardPic_Scan();
			state.pic_n = (n > 0) ? n : 0;
		}
		else if (folder == FOLDER_MUSIC)
		{
			/* /YMGUI/MUSIC 下的 .wav。⚠ 扫描表与照片/笔记共用一张 ⇒ 每次进分类
			 *   都必须重扫，否则读到的还是上一个分类的条目（下标全错）。 */
			int n = BoardMusic_Scan();
			state.pic_n = (n > 0) ? n : 0;
		}
		else if (folder == 2)
		{
			int n = BoardNote_Scan();
			state.pic_n = (n > 0) ? n : 0;
		}
		else if (folder == 1)
			rec_enter();                 /* 录音：挂卡列表 + 触发扫描 */
		/* folder==3 蓝牙接收：不扫，进分类时由 bt_view_refresh 刷 */
		file_refresh();
		return;
	}
	if (row == 3 && state.file_folder == 3)
	{
		file_del_reset();
		state.file_folder = -1;
		g_files_folder    = -1;
		file_refresh();
		return;
	}
	if (state.file_folder == 3)          /* 蓝牙接收：第 0 行 = 开始/中止 */
	{
		if (row == 0)
		{
			if (BoardBt_Busy())
				(void)BoardBt_Abort();
			else
				(void)BoardBt_Start();
		}
		state.bt_last_st = -1;
		bt_view_refresh();
		return;
	}

	/* 照片/笔记分类已改用自绘滑动列表（pic_list）+ 点击卡片选中，
	 * 这里的 4 行按钮在那些分类里是隐藏的，不会走到这里。 */
}

/* 提示行：写在说明区末尾。要求与录音分类一致 —— **静态字面量或本文件缓冲**，
 * 调用方按内容比较，不接临时串。 */
static void file_set_hint(const char* s)
{
	snprintf(state.hint, sizeof(state.hint), "%s", s ? s : "");
}

/* （翻页已废弃，照片/笔记改为连续滑动 + 右侧边条）*/

/* 删除：两段确认（第一次按下 → 按钮变红改文案；第二次才真删）。
 * 与录音分类的 rec_on_del 同一套交互，用户不用学两套。 */
static void file_delete(void)
{
	int idx = state.pic_sel;                 /* 已经是全局下标 */

	if (idx < 0 || idx >= state.pic_n)
	{
		file_set_hint("先点一个文件，再删除");
		file_refresh();
		return;
	}
	if (state.del_arm == 0)
	{
		state.del_arm = 1;                     /* file_bar_draw 会把它画成红色"确认删除?" */
		file_set_hint("再点一次删除选中的文件");
		file_refresh();
		return;
	}
	{
		int rc;
		int n;

		/* 三个文件列表分类各用各的删除接口（补丁30 加音乐）。
		 * ⚠ 不能写成一个三元表达式了 —— 三种来源、三套返回码。 */
		if (state.file_folder == 0)
			rc = BoardPic_Delete(idx);
		else if (state.file_folder == FOLDER_MUSIC)
			rc = BoardMusic_Delete(idx);
		else
			rc = BoardNote_Delete(idx);

		file_del_reset();
		if (rc == 0)
		{
			/* 删完重建计数并夹紧滚动/选中：内容变短时滚动位置可能越界 */
			if (state.file_folder == 0)            n = BoardPic_Count();
			else if (state.file_folder == FOLDER_MUSIC) n = BoardMusic_Count();
			else                                   n = BoardNote_Count();
			state.pic_n  = (n > 0) ? n : 0;
			state.pic_sel = -1;
			pic_clamp();
			file_set_hint("已删除");
		}
		else if (rc == 2)
			file_set_hint("删除失败：下标无效（可能已被删）");
		else if (rc == -1)
			file_set_hint("删除失败：TF 卡未挂载");
		else if (rc == -3)
			file_set_hint("删除失败：路径拼接失败");
		else
			file_set_hint("删除失败：卡可能已卸或文件被占用");
		file_refresh();
	}
}

/* ---------- 底部操作栏：**一个**自绘对象顶替 3 个按钮（2026-10-18）----------
 * ⚠ 为什么不用 3 个 PhoneUI_button：phone_shell 开机就把所有 App 的 create() 跑一遍
 *   （phone_shell.c 的 PhoneApps_Create 循环）⇒ 每个按钮开机就要堆分配两次
 *   （对象 + PhoneButton≈104 B），3 个 ≈ 480 B；而 heap0 与 DTCM 的 .bss 抢同一块内存、
 *   余量只剩几百字节 ⇒ 直接把 heap0 挤爆（2026-10-18 白屏事故）。
 * ⇒ 改成 1 个自绘容器 + 坐标命中，只多 1 次分配 —— 与录音卡片列表 rec_list 同一套做法。 */
/* ⚠⚠ 2026-10-18 修（"返回上一级/删除 看不见"）：app view 是
 *   `YMGUI_Creat_Obj_Creat(app_page, 0, 36, W, 412)`（phone_shell.c:1788）
 *   ⇒ **本文件所有坐标都是 view 局部坐标，合法范围只有 0..411**。
 *   旧值 FILE_BAR_Y=402 配 H=36 ⇒ 底边 438，超出视口 26px，只有 10px 露在屏幕最底边，
 *   看起来就是"两个按钮不见了"。现在把整页纵向重排进 0..411（见下面的布局表）。 */
#define FILE_BAR_X 18
#define FILE_BAR_Y 372
#define FILE_BAR_W 284
#define FILE_BAR_H 34
/* 照片/笔记改成滑动列表后，翻页键没有意义 ⇒ 底栏只留「返回上一级 / 删除」两段。 */
static const char* file_bar_labels[2] = { "返回上一级", "删除" };

static void file_bar_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	int seg = area->w / 2;

	for (int i = 0; i < 2; ++i)
	{
		GYrect  r = {(GYcoord)(area->x + i * seg + 2), (GYcoord)area->y,
					 (GYcoord)(seg - 4), (GYcoord)area->h};
		GYcolor c = (i == 0) ? RGB(160, 143, 105)
		                     : (state.del_arm ? RGB(200, 76, 76) : RGB(147, 112, 136));

		PhoneUI_rounded(surface, &r, c, 10, GY_OPA_COVER);
		/* 待确认态把"删除"换成"确认删除?"（配色已在上面转红）*/
		PhoneUI_draw_text(surface, (i == 1 && state.del_arm) ? "确认删除?" : file_bar_labels[i],
						  RGB(250, 252, 253), 0,
						  (GYcoord)(r.x + r.w / 2), (GYcoord)(r.y + (r.h - 24) / 2));
	}
}

static void file_bar_event(GYOBJ obj, GYEvent e)
{
	GYCTX  ctx = obj->ctx;
	GYrect a;
	int    px, py, rel, i;

	if (e != GY_EVENT_Released) return;          /* 与 rec_list 一致：松手才算点击 */

	/* ★必须用**屏幕绝对矩形**：ctx->point_x/y 是屏幕坐标，而 FILE_BAR_X/Y 是 view
	 *   局部坐标（view 在屏幕 y=36 起）⇒ 拿常量直接比会整整偏 36px，命中区间整体下移
	 *   一个按钮高，按在按钮上反而"没反应"。 */
	YMGUI_Obj_GetAbsArea(obj, &a);
	px = (int)ctx->point_x;
	py = (int)ctx->point_y;

	if (py < (int)a.y || py > (int)(a.y + a.h)) return;

	rel = px - (int)a.x;
	if (rel < 0 || rel >= (int)a.w) return;

	i = rel / ((int)a.w / 2);
	if (i < 0) i = 0;
	if (i > 1) i = 1;

	if (i == 0)      { file_goto_root(); }       /* 返回分类列表 */
	else if (i == 1) { file_delete(); }

	YMGUI_Obj_Invalidate(obj);
}

/* ---------- 照片/笔记：可滑动的卡片列表 + 边条（补丁25）----------
 * 【为什么照 settings.c 的范式】那边是"视口 + scroll_y + 右侧滑块"，
 *   这里把同样的滚动语义搬到**自绘容器**上（与录音卡片列表 rec_list 一样是自绘，
 *   避免给每张卡片都建对象 —— 见 2026-10-18 白屏事故：heap0 与 DTCM 抢内存，
 *   多一个对象就多一次开机分配）。
 * 【边条公式与 settings 完全一致】滑块高 = H*H/内容高（下限 PIC_BAR_MIN），
 *   位置 = 顶 + (H−滑块高)*scroll/(内容高−H)；内容没超出视口时不画。 */
/* ---------- 照片页纵向布局表（view 高 412，合法 0..411）----------
 *   65..83    file_path 标签
 *   88..272   pic_list（4 张卡 × 46）
 *  276..368   file_preview 说明（3 行 × 24 = 72）
 *  372..406   file_bar（返回上一级 / 删除）
 * 旧布局 card=52、bar=402 起 ⇒ bar 底边 438 超出视口 26px（"按钮看不见"）。 */
#define PIC_LIST_X     12
#define PIC_LIST_Y     88
#define PIC_LIST_W     296
#define PIC_CARD_H     46
#define PIC_SLOTS      4
#define PIC_LIST_H     (PIC_CARD_H * PIC_SLOTS)   /* 184 ⇒ 88..272 */
/* 卡片宽度：**收进右边条左侧**，否则卡片会盖住边条。
 * 边条区 = PIC_BAR_W + PIC_BAR_MARGIN + 右侧留白(6)，卡片到此为止 ⇒ 边条落在留白里。 */
#define PIC_BAR_W      6        /* 边条宽：4 → 6，太细时在浅色托盘上几乎看不见 */
#define PIC_BAR_MARGIN 2
#define PIC_CARD_W     (PIC_LIST_W - PIC_BAR_W - PIC_BAR_MARGIN - 6)  /* = 282 */
#define PIC_BAR_MIN    24
#define PIC_DRAG_SLOP  6      /* 超过这么多像素才算"拖动"（从此开始滚）*/
#define PIC_TAP_MAX   12      /* 松手时累计位移 ≤ 此值才算"点击"（防拖动被误判成点击）*/
#define PIC_DELTA_MAX  80     /* 单拍位移上限：超过视为坐标跳变，丢这一帧不甩列表 */

/* 列表的**屏幕绝对矩形**。
 * ⚠ 本文件的 PIC_LIST_X/Y 是 **view 内局部坐标**（app view 在 y=36），而事件里的
 *   ctx->point_x/y 是**屏幕坐标** ⇒ 旧的 `py - PIC_LIST_Y` 整整偏了 36px
 *   （= 0.7 个卡片高）：按在卡片上半部会命中**上一张**，边条也画到视口外看不见。
 *   凡是要跟指针坐标做运算的地方，一律用本函数取绝对矩形。 */
static void pic_area(GYOBJ obj, GYrect* out)
{
	if (obj != NULL)
	{
		YMGUI_Obj_GetAbsArea(obj, out);
	}
	else
	{
		out->x = PIC_LIST_X; out->y = PIC_LIST_Y;
		out->w = PIC_LIST_W; out->h = PIC_LIST_H;
	}
}

static int pic_content_h(void)
{
	int n = state.pic_n > 0 ? state.pic_n : 0;
	return n * PIC_CARD_H;
}

static int pic_max_scroll(void)
{
	int m = pic_content_h() - PIC_LIST_H;
	return (m > 0) ? m : 0;
}

static void pic_clamp(void)
{
	int m = pic_max_scroll();
	if (state.pic_scroll < 0) state.pic_scroll = 0;
	if (state.pic_scroll > m) state.pic_scroll = m;
	g_pic_scroll_diag = state.pic_scroll;   /* 唯一写入口，顺手镜像给 SWD 诊断 */
}

/* 边条：与 settings 的 scroll_place_bar 同一套算法，只是直接在自绘里画。
 * ★area 必须是**屏幕绝对矩形**：旧代码把 PIC_LIST_Y(88，view 局部) 当绝对坐标用，
 *   而列表实际在屏幕 y=124 起 ⇒ 滑块被画在视口上边界之外，被 clip 裁掉 ⇒ "边条不见了"。 */
static void pic_draw_bar(GYSURFACE surface, const GYrect* area)
{
	int   ch = pic_content_h();
	int   vh = (int)area->h;
	int   bx = (int)area->x + (int)area->w - PIC_BAR_W - PIC_BAR_MARGIN;
	int   th, y;

	if (state.pic_n <= 0) return;               /* 空分类：不画 */

	/* 内容没超出视口 ⇒ 画一根"满高浅色"滑块：告诉用户右侧确实是滚动条、
	 *   而且已经全部看完了。旧实现直接不画，看起来像"边条丢了"。 */
	if (ch <= vh)
	{
		GYrect full = {(GYcoord)bx, (GYcoord)((int)area->y + 2), (GYcoord)PIC_BAR_W,
					   (GYcoord)(vh - 4)};
		PhoneUI_rounded(surface, &full, RGB(203, 210, 220), PIC_BAR_W / 2, GY_OPA_COVER);
		return;
	}

	th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(PIC_BAR_MIN, th, vh);
	y  = (int)area->y + (int)(((int64_t)(vh - th) * state.pic_scroll) / (ch - vh));
	y  = GYLimitMaxMin((int)area->y, y, (int)area->y + vh - th);

	{
		GYrect track = {(GYcoord)bx, (GYcoord)((int)area->y + 2), (GYcoord)PIC_BAR_W,
						(GYcoord)(vh - 4)};
		GYrect thumb = {(GYcoord)bx, (GYcoord)y, (GYcoord)PIC_BAR_W, (GYcoord)th};

		/* 轨道 + 滑块两段：只有滑块时会和浅色托盘糊在一起，加浅轨道才有"滚动条"观感 */
		PhoneUI_rounded(surface, &track, RGB(219, 224, 233), PIC_BAR_W / 2, GY_OPA_COVER);
		PhoneUI_rounded(surface, &thumb, RGB(120, 132, 150), PIC_BAR_W / 2, GY_OPA_COVER);
	}
}

static void pic_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* area)
{
	GYrect saved_clip = surface->clip;
	GYrect clip;

	(void)obj;
	/* ★裁剪到列表视口：自绘容器引擎不会自动裁到自身区域（见 YMGUI_Invalidate.c：
	 *   只有开 ClipChildren 的子对象才裁），所以卡片会越界画到下方说明区 ⇒ 显示错乱。
	 *   这里手动把 surface.clip 收窄到 area（对象绝对矩形），绘制完再恢复。 */
	if (GY_Rect_Intersect(&clip, area, &saved_clip) == 0)
		return;                     /* 整块在裁剪带之外：不画 */
	surface->clip = clip;

	{
		int n = state.pic_n > 0 ? state.pic_n : 0;

		/* 列表托盘背景：与录音卡片列表(rec_list)同色，给白卡一个容器层次感。 */
		GYrect tray = {area->x, area->y, area->w, area->h};
		PhoneUI_rounded(surface, &tray, RGB(237, 239, 244), 14, GY_OPA_COVER);

		if (n <= 0)
		{
			PhoneUI_draw_text(surface, "（这个分类下没有文件）", RGB(140, 148, 156), 0,
							  (GYcoord)(area->x + area->w / 2), (GYcoord)(area->y + 20));
		}
		else
		{
			for (int i = 0; i < n; ++i)
			{
				int cy = (int)area->y + i * PIC_CARD_H - state.pic_scroll;

				if (cy + PIC_CARD_H <= area->y) continue;   /* 在视口上方：不画 */
				if (cy >= area->y + area->h) break;          /* 已越过视口下方：停止 */

				{
					GYrect card = {(GYcoord)area->x, (GYcoord)cy,
								   (GYcoord)PIC_CARD_W, (GYcoord)(PIC_CARD_H - 4)};
					int    sel  = (state.pic_sel == i);
					GYcolor card_bg = sel ? RGB(214, 168, 74) : RGB(255, 255, 255);

					PhoneUI_rounded(surface, &card, card_bg, 8, GY_OPA_COVER);

					/* 文件图标（与文件夹图标同画布、同对齐）；图标 28 高，
					 * 垂直中心 = 卡片内区中心 ⇒ 卡片高度变化时自动居中。 */
					PhoneUI_icon_draw(surface, ICON_FILE, (GYcoord)(area->x + 26),
									  (GYcoord)(cy + (PIC_CARD_H - 4) / 2),
									  card_bg);

					/* 文件名（过长截断）+ 大小 */
					{
						char name[ROW_NAME_MAX];
						char line[72];
						uint32_t size = 0;

						if (state.file_folder == 2)
						{
							if (BoardNote_Name(i, name, (int)sizeof(name)) == 0)
								(void)BoardNote_Size(i, &size);
							else
								snprintf(name, sizeof(name), "（空）");
						}
						else if (state.file_folder == FOLDER_MUSIC)
						{
							/* 音乐分类（补丁30）：/YMGUI/MUSIC 的 .wav。
							 * ⚠ 这里读的是**同一张** img_scan 表，进分类时已由
							 *   BoardMusic_Scan() 填好；不能在绘制里再扫（绘制回调里做 I/O）。 */
							if (BoardMusic_Name(i, name, (int)sizeof(name)) == 0)
								(void)BoardMusic_Size(i, &size);
							else
								snprintf(name, sizeof(name), "（空）");
						}
						else
						{
							if (BoardPic_Name(i, name, (int)sizeof(name)) == 0)
								(void)BoardPic_Size(i, &size);
							else
								snprintf(name, sizeof(name), "（空）");
						}

						if (strlen(name) > 15u)
						{
							name[12] = '.'; name[13] = '.'; name[14] = '.'; name[15] = 0;
						}
						snprintf(line, sizeof(line), "%s  %u KB", name, (unsigned)((size + 1023u) / 1024u));
						/* ⚠ PhoneUI_draw_text 的 cx 是**文本中心 X**（内部 x = cx - 宽/2），
						 * 不是左边界！旧代码传 area->x + 48（左边界）⇒ 文字以 x=60 为中心向左右
						 * 各散一半，长名直接盖住左侧 ICON_FILE 并冲出屏幕左缘 —— 这正是
						 * "照片卡片全部渲染错乱"的根因。改为传**文本区中心**（与 file_row_draw 同范式）。 */
						{
							int tx  = (int)area->x + 48;         /* 文本区左边界（图标右侧）*/
							int twz = PIC_CARD_W - 48 - 10;      /* 文本区宽度，右侧留 10px */
							int twt = PhoneUI_text_width(line, 0);  /* 文本实际宽 ⇒ 左对齐 */
							if (twt > twz) twt = twz;
							PhoneUI_draw_text(surface, line, sel ? RGB(58, 42, 20) : RGB(70, 78, 86), 0,
											  (GYcoord)(tx + twt / 2),
											  (GYcoord)(cy + (PIC_CARD_H - 4 - 24) / 2));
						}
					}
				}
			}
		}

		pic_draw_bar(surface, area);       /* 边条（在裁剪区内、按绝对坐标绘制）*/
	}

	surface->clip = saved_clip;      /* 恢复：不影响别的控件 */
}

/* 边条拖动：把指针 y 映射到滚动位置，让滑块跟着手指走（照设置 App 的边条滑动）。 */
static void pic_bar_set_from_pointer(int py, const GYrect* area)
{
	int ch = pic_content_h();
	int vh = (int)area->h;
	if (ch <= vh) { state.pic_scroll = 0; return; }

	/* 滑块高与 pic_draw_bar 同算法（下限 PIC_BAR_MIN）。 */
	int th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(PIC_BAR_MIN, th, vh);

	/* 期望滑块顶 = 指针 − 半滑块高，再 clamp 到 [视口顶, 视口底−滑块高]。 */
	int y0 = (int)area->y;
	int y  = py - th / 2;
	if (y < y0) y = y0;
	if (y > y0 + vh - th) y = y0 + vh - th;

	state.pic_scroll = (int)(((int64_t)(y - y0) * (ch - vh)) / (vh - th));
	pic_clamp();
}

/* ===========================================================================
 * 每拍增量滚动（补丁25e）—— **这是"不能下滑"的真正修法**
 *
 * 【为什么不能只靠 GY_EVENT_Pressing 的按下点做基准】
 *   旧实现：Pressed 记 press_y / press_scroll，Pressing 里 scroll = press_scroll - dy。
 *   这要求"按下 → 一连串 Pressing → 抬起"这条事件序列**一帧都不能断**。
 *   而本板触摸读是**位拍 I2C**（一次 GSTID+TP1 约 0.5 ms，且板上无外部上拉，
 *   自然丢读基线偏高）：只要中途有一帧读失败，touch_port 就会注入一次"抬起"；
 *   下一帧读到触点又注入"按下" ⇒ 引擎侧 pressed_obj 被清掉又重建，
 *   我们这边 press_y 被重置到**手指当前所在位置** ⇒ dy 恒 ≈ 0 ⇒ 怎么拖都不动。
 *   同一次假抬起还会走 Released 分支：pic_drag_moved 也是 0 ⇒ 被当成"点了这张卡片"
 *   ⇒ 随机触发导入 —— 这就是"点击选择照片响应不及时/点错"的来源。
 *
 * 【现在的做法】基准改为"上一拍指针位置"，逐拍累加位移：
 *   · 单帧丢点最多丢一帧位移，手势不会被打断；
 *   · 完全不依赖 Pressing 是否被派发（app_tick 每拍无条件调用本函数）；
 *   · 累计位移 > PIC_TAP_MAX 即判为拖动 ⇒ 松手绝不触发导入（防误点）。
 * =========================================================================== */
static void pic_scroll_tick(void)
{
	GYOBJ  o = state.pic_list;
	GYCTX  c;
	GYrect a;
	int    py, dy;

	if (o == NULL || state.pic_gesture == 0)
		return;
	c = o->ctx;
	if (c == NULL)
	{ state.pic_gesture = 0; return; }

	/* 手指已抬起（Released 事件漏了也能在这里收尾）⇒ 结束手势 */
	if (c->point_pressed == 0u)
	{ state.pic_gesture = 0; return; }

	py  = (int)c->point_y;
	dy  = py - state.pic_last_y;
	g_pic_hold_evt++;

	if (dy == 0)
		return;
	if (dy > PIC_DELTA_MAX || dy < -PIC_DELTA_MAX)
	{                       /* 坐标跳变（不是人手）：只更新基准，不滚 */
		state.pic_last_y = py;
		return;
	}

	state.pic_last_y  = py;
	state.pic_move_acc += (dy > 0) ? dy : -dy;

	pic_area(o, &a);

	if (state.pic_drag_mode == 1)
	{
		pic_bar_set_from_pointer(py, &a);      /* 拖边条：绝对映射，滑块跟着手指 */
	}
	else
	{
		if (state.pic_move_acc <= PIC_DRAG_SLOP)
			return;                            /* 还在抖动阈值内，先不滚 */
		/* 与 settings 同号：手指往上拖(dy<0)看后面的内容 */
		state.pic_scroll -= dy;
		pic_clamp();
	}
	g_pic_move_px -= dy;                       /* 诊断：累计位移（往上拖为负）*/
	YMGUI_Obj_Invalidate(o);
}

static void pic_list_event(GYOBJ obj, GYEvent e)
{
	GYCTX  ctx = obj->ctx;
	GYrect a;
	int    px = (int)ctx->point_x;
	int    py = (int)ctx->point_y;
	int    bar_x;

	g_pic_evt += 1;                 /* 诊断：事件确实进到了列表（SWD 可读）*/
	pic_area(obj, &a);
	bar_x = (int)a.x + (int)a.w - PIC_BAR_W - PIC_BAR_MARGIN;

	if (e == GY_EVENT_Pressed)
	{
		g_pic_press_evt++;

		/* ★新手势：基准 = 当前指针位置（增量滚动的起点）*/
		state.pic_gesture  = 1;
		state.pic_last_y   = py;
		state.pic_move_acc = 0;
		state.pic_drag_mode = 0;      /* 0 = 拖列表体；1 = 拖边条 */

		/* 按在右侧边条区域 ⇒ 进入"拖边条"模式（照设置 App 的边条滑动 UI）。 */
		if (px >= bar_x - 6 && px <= bar_x + PIC_BAR_W + 6)
		{
			state.pic_drag_mode = 1;
			pic_bar_set_from_pointer(py, &a);
			YMGUI_Obj_Invalidate(obj);
			return;
		}

		/* 命中哪张卡片：**必须用屏幕绝对坐标**（旧的 py - PIC_LIST_Y 偏了 view 的 36px）*/
		{
			int rel = py - (int)a.y;
			int idx = (state.pic_scroll + rel) / PIC_CARD_H;
			state.pic_press_entry = (idx >= 0 && idx < state.pic_n) ? idx : -1;
		}
		return;
	}

	if (e == GY_EVENT_Pressing)
	{
		/* 真实拖动大多走这里；但即使这条事件不来，app_tick 里的
		 * pic_scroll_tick() 也会按拍推进（两者共用同一基准，重复调用只是 dy=0）。*/
		pic_scroll_tick();
		return;
	}

	if (e == GY_EVENT_Released)
	{
		int mode = state.pic_drag_mode;
		int moved = state.pic_move_acc;

		state.pic_gesture   = 0;
		state.pic_drag_mode = 0;
		g_pic_rel_evt++;               /* 诊断：抬起事件进到了列表 */

		if (mode == 1)                 /* 边条拖动：不算点击 */
			return;
		if (moved > PIC_TAP_MAX)       /* 累计位移超阈值 ⇒ 是滚动手势 */
			return;

		{
			int idx = state.pic_press_entry;
			if (idx < 0) return;

			state.pic_sel = idx;                   /* 选中，供底部「删除」使用 */
			file_del_reset();
			YMGUI_Obj_Invalidate(obj);             /* 立刻高亮，不等后面任何耗时动作 */
			g_pic_tap++;

			if (state.file_folder == 0)            /* 照片：**排队**导入，见 app_tick */
			{
				/* ⚠ 把导入挪到 app_tick：img_store_poll 是阻塞式一次性跑完（几百 ms~几秒），
				 *   放在事件回调里会让这次点击在"下一个可见反馈"之前就卡住 UI。 */
				state.pic_import_idx = idx;
				file_refresh();
			}
			else if (state.file_folder == FOLDER_MUSIC)
			{
				/* 音乐（补丁30）：点一个 .wav ⇒ 交给音乐播放器播（与录音「播放」同一条路）。
				 * 传绝对路径、'/' 开头 ⇒ music.c 判定为外部文件，用 BoardMusic_PlayFile 起播。 */
				char path[64];

				if (BoardMusic_Path(idx, path, (int)sizeof(path)) == 0)
				{
					if (!PhoneHost_Open(MUSIC, path))
						file_set_hint("打不开音乐播放器（等界面静止后再点）");
				}
				else
					file_set_hint("读取文件路径失败");
				file_refresh();
			}
			else                                   /* 笔记：点一个就在笔记 app 里打开 */
			{
				char arg[12];
				snprintf(arg, sizeof(arg), "%d", idx);
				PhoneHost_Open(NOTES, arg);
				file_refresh();
			}
		}
		return;
	}

	if (e == GY_EVENT_ReleasedOff)
	{
		state.pic_gesture   = 0;
		state.pic_drag_mode = 0;
		state.pic_move_acc  = PIC_TAP_MAX + 1;   /* 拖出容器 ⇒ 绝不当点击 */
		return;
	}
	/* GY_EVENT_Clicked：已在上面的 Released 处理，这里不再处理（避免重复触发）*/
}

static void app_tick(uint32 elapsed)
{
	(void)elapsed;

	/* ---- 照片 / 笔记：幻灯片列表（补丁25e）----
	 * ① pic_scroll_tick() 每拍推进一次增量滚动 —— **不依赖 Pressing 事件**
	 *    （输入层漏帧时仍能滑，见 pic_scroll_tick 的长注释）。
	 * ② 点击后的导入在这里排队执行：把阻塞几秒的 img_store_poll 从事件回调里搬出来，
	 *    否则"点一下"到"高亮/结果出现"之间会整段卡死（就是反馈里的"响应不及时"）。 */
	if (state.file_folder == 0 || state.file_folder == 2)
	{
		pic_scroll_tick();

		if (state.pic_import_idx >= 0)
		{
			int i = state.pic_import_idx;
			state.pic_import_idx = -1;
			if (i < state.pic_n && BoardPic_Busy() == 0 && BoardPic_Import(i) == 0)
			{
				state.import_pending = 1;
				state.import_rc = -1;
			}
		}
	}

	if (state.import_pending && BoardPic_Busy() == 0)
	{
		state.import_pending = 0;
		state.import_rc = BoardPic_LastRc();
		/* 导入结果必须让用户看见：以前只把返回码记进 state，界面上什么都没变
		 * ⇒ 点一张照片"没反应"（成功也没反应），用户只能去相册猜。 */
		if (state.import_rc == 0)
			file_set_hint("已导入图库，可到相册查看");
		else if (state.import_rc == BOARD_PIC_RC_DUP)
			file_set_hint("这张已经在图库里（未重复添加）");
		else
		{
			char msg[40];
			snprintf(msg, sizeof(msg), "导入失败 rc=%d", state.import_rc);
			file_set_hint(msg);
		}
		file_refresh();
	}

	/* 录音分类：① 延迟执行扫描（进入/刷新触发）② 播放中每拍刷新进度 */
	if (state.file_folder == 1)
	{
		if (state.rec_need_scan)
		{
			state.rec_need_scan = 0;
			rec_do_scan();
			if (state.rec_loading) state.rec_loading = 0;
			YMGUI_Obj_Invalidate(state.rec_list);
		}
		/* 正在放某张卡片 ⇒ 每拍重绘进度条（播放完会自动复位，这里跟着复位图标）*/
		if (BoardRec_PlayIndex() >= 0)
			YMGUI_Obj_Invalidate(state.rec_list);
	}

	if (state.file_folder == 3)
	{
		int st = BoardBt_State();
		int p  = BoardBt_Progress();
		if (p < 0) p = 0;
		if (st != state.bt_last_st || p != state.bt_last_p
		    || BoardBt_StatusText() != state.bt_last_txt)
			bt_view_refresh();
	}
}

/* ====================== 录音卡片（补丁19）====================== */

/* 进入录音分类：显示录音控件、隐藏标准列表、触发一次扫描。 */
static void rec_enter(void)
{
	state.rec_n = 0;
	state.rec_loading = 1;
	state.rec_need_scan = 1;
	state.rec_scroll = 0;
	state.rec_selected = -1;
	state.rec_del_arm = 0;
	state.rec_pull = 0;
	state.rec_drag_moved = 0;
	rec_apply_visibility(1);
}

/* 离开录音分类：隐藏录音控件、停掉正在放的卡片。 */
static void rec_leave(void)
{
	BoardRec_PlayStop();
	YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);   /* 顺手收起重命名浮层 */
	rec_apply_visibility(0);
}

/* 按是否在录音分类，切换两套控件显隐。 */
static void rec_apply_visibility(int rec)
{
	for (int i = 0; i < FOLDER_ROWS; ++i)
		YMGUI_Obj_SetHidden(state.file_rows[i], rec ? 1 : 0);
	YMGUI_Obj_SetHidden(state.bt_bar, (rec || state.file_folder != 3) ? 1 : 0);
	YMGUI_Obj_SetHidden(state.file_preview, rec ? 1 : 0);
	/* 操作栏属于"照片/笔记列表"，录音分类不用（录音有自己的底部按钮）*/
	if (rec)
		YMGUI_Obj_SetHidden(state.file_bar, 1);

	YMGUI_Obj_SetHidden(state.rec_list, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_hint, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_del, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_rename, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_play, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_back, rec ? 0 : 1);
	YMGUI_Obj_SetHidden(state.rec_refresh, rec ? 0 : 1);
	if (rec == 0)
		YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);
}

/* 真正执行扫描并夹紧下标（被进入/刷新/删除/重命名调用）。 */
static void rec_do_scan(void)
{
	state.rec_n = BoardRec_Scan();
	if (state.rec_n < 0) state.rec_n = 0;
	if (state.rec_scroll > state.rec_n - REC_SLOTS)
		state.rec_scroll = (state.rec_n > REC_SLOTS) ? (state.rec_n - REC_SLOTS) : 0;
	if (state.rec_scroll < 0) state.rec_scroll = 0;
	if (state.rec_selected >= state.rec_n) state.rec_selected = -1;
	/* 删光后选中归位 */
	if (state.rec_selected >= 0 && state.rec_n == 0) state.rec_selected = -1;
}

static void rec_set_hint(const char* s)
{
	PhoneUI_text_set(state.rec_hint, s ? s : "");
}

/* 播放 / 暂停 / 续播 同一入口：点的是第 entry 张卡片的圆形键。 */
static void rec_toggle_play(int entry)
{
	if (entry < 0 || entry >= state.rec_n) return;
	int cur = BoardRec_PlayIndex();

	if (cur == entry)
	{
		if (BoardRec_PlayPaused())
			(void)BoardRec_PlayResume();
		else if (BoardRec_Playing())
			(void)BoardRec_PlayPause();
		else
			BoardRec_PlayStop();          /* 状态已陈旧 ⇒ 直接停 */
	}
	else
	{
		int rc = BoardRec_PlayList(entry);
		if (rc != 0)
		{
			/* 失败要分清：卡没挂载 / 文件打不开（损坏）/ 音频输出异常 / WAV 头坏 */
			if (rc == -2)       rec_set_hint("播放失败：TF 卡未挂载");
			else if (rc == -3)  rec_set_hint("播放失败：文件打不开（可能已损坏）");
			else if (rc == -22) rec_set_hint("播放失败：音频输出异常");
			else if (rc < -10)  rec_set_hint("播放失败：WAV 头损坏");
			else                rec_set_hint("播放失败（请重试）");
		}
		else
		{
			rec_set_hint("");
		}
	}
	YMGUI_Obj_Invalidate(state.rec_list);
}

static void rec_on_del(GYOBJ obj)
{
	(void)obj;
	if (state.rec_selected < 0) { rec_set_hint("先选择一段录音"); return; }
	if (state.rec_del_arm == 0)
	{
		state.rec_del_arm = 1;
		PhoneUI_button_set(state.rec_del, "确认删除?");
		YMGUI_Obj_SetBgColor(state.rec_del, RGB(200, 76, 76));
		rec_set_hint("再点一次删除选中的录音");
		return;
	}
	/* 若删的是正在放的，先停播放（否则 wav_play 还握着旧句柄）*/
	if (state.rec_selected == BoardRec_PlayIndex())
		BoardRec_PlayStop();
	{
		int next = -1;
		int rc = BoardRec_Delete(state.rec_selected, &next);
		state.rec_del_arm = 0;
		PhoneUI_button_set(state.rec_del, "删除");
		YMGUI_Obj_SetBgColor(state.rec_del, RGB(147, 112, 136));
		if (rc == 0)
		{
			state.rec_selected = next;
			rec_do_scan();                /* 重建后刷新 rec_n / 下标 */
			YMGUI_Obj_Invalidate(state.rec_list);
			rec_set_hint("已删除");
		}
		else if (rc == 2)
			rec_set_hint("删除失败：下标无效");
		else
			rec_set_hint("删除失败：卡可能已卸");
	}
}

static void rec_on_rename(GYOBJ obj)
{
	(void)obj;
	if (state.rec_selected < 0) { rec_set_hint("先选择一段录音"); return; }
	rec_rn_open();
}

/* 补丁29：原「分享」改为「播放」—— 把选中的录音交给**音乐播放器 App** 播。
 *
 * 【为什么开 App 而不是就地放】用户明确要求"调用音乐播放器播放"：音乐 App 有完整的
 *   进度条 / 暂停 / 续播 / 拖动跳播，卡片里的小圆键只够"听一下"。
 *   就地那条路（BoardRec_PlayList）仍然保留给卡片圆键，两条路都走同一个 wav_play 会话。
 *
 * 【参数约定】把**绝对路径**（以 '/' 开头）交给 music.c 的 show()：
 *   音乐 App 看到 '/' 开头即判定"外部文件"，用 BoardMusic_PlayFile 直接起播，
 *   不去查 /YMGUI/MUSIC 的扫描表（录音在 /YMGUI/RECORD，套音乐下标会播错文件）。
 * ⚠ 传路径而不是传"录音下标"：音乐 App 不认识录音列表，也没有理由认识。 */
static void rec_on_play(GYOBJ obj)
{
	char path[64];
	(void)obj;
	if (state.rec_selected < 0) { rec_set_hint("先选择一段录音"); return; }
	if (BoardRec_ListPath(state.rec_selected, path, (int)sizeof(path)) != 0)
	{
		rec_set_hint("读取文件名失败");
		return;
	}
	/* 卡片里的就地播放先停掉：音乐 App 一起来就接手同一个 wav_play 会话，
	 * 这里不停会留下"旧会话的手柄/进度"再被 wav_play_start 覆盖，状态提示容易自相矛盾。 */
	BoardRec_PlayStop();
	if (!PhoneHost_Open(MUSIC, path))
	{
		/* 打开失败（有过渡动画在跑/系统忙）⇒ 如实提示，绝不假装"已经在放了" */
		rec_set_hint("打不开音乐播放器（等界面静止后再点）");
	}
}

static void rec_on_back(GYOBJ obj)
{
	(void)obj;
	rec_leave();
	state.file_folder = -1;
	g_files_folder = -1;
	file_refresh();
}

static void rec_on_refresh(GYOBJ obj)
{
	(void)obj;
	state.rec_loading = 1;
	state.rec_need_scan = 1;
	rec_set_hint("刷新中…");
	YMGUI_Obj_Invalidate(state.rec_list);
}

/* 打开重命名浮层：把当前选中的文件名预填进去（去 .WAV 后缀方便改主体）。 */
static void rec_rn_open(void)
{
	char name[32];
	if (BoardRec_ListName(state.rec_selected, name, (int)sizeof(name)) != 0)
	{
		rec_set_hint("读取文件名失败");
		return;
	}
	/* 去掉末尾 .WAV 后缀，仅展示主体，让用户输入更易改 */
	{
		int len = (int)strlen(name);
		if (len > 4 && (name[len-4]=='.' || name[len-4]=='.'))
		{
			int ext = (name[len-3]=='W'||name[len-3]=='w') &&
			          (name[len-2]=='A'||name[len-2]=='a') &&
			          (name[len-1]=='V'||name[len-1]=='e');
			if (ext) name[len-4] = 0;
		}
	}
	YMGUI_TextInput_SetText(state.rec_rn_input, name);
	YMGUI_Obj_SetHidden(state.rec_rn_panel, 0);
}

static void rec_rn_ok(GYOBJ obj)
{
	const char* txt;
	(void)obj;
	txt = YMGUI_TextInput_GetText(state.rec_rn_input);
	if (txt == NULL || txt[0] == 0)
	{
		rec_set_hint("名字不能为空");
		return;
	}
	/* 若重命名的是正在放的，先停（wav_play 握着旧路径句柄）*/
	if (state.rec_selected == BoardRec_PlayIndex())
		BoardRec_PlayStop();
	{
		int rc = BoardRec_Rename(state.rec_selected, txt);
		if (rc >= 0)
		{
			state.rec_selected = rc;       /* 重命名后下标可能变 */
			rec_do_scan();
			YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);
			YMGUI_Obj_Invalidate(state.rec_list);
			rec_set_hint("已重命名");
		}
		else if (rc == 3)
			rec_set_hint("重命名失败：名字含非法字符");
		else
			rec_set_hint("重命名失败（卡满/已卸？）");
	}
}

static void rec_rn_cancel(GYOBJ obj)
{
	(void)obj;
	YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);
}

/* ---------- 自绘卡片列表：绘制 ---------- */

static void rec_draw_text_left(GYSURFACE s, const char* v, GYcolor c, uint8 large, int x_left, int top)
{
	if (!v || !v[0]) return;
	int w = PhoneUI_text_width(v, large);
	PhoneUI_draw_text(s, v, c, large, x_left + w / 2, top);
}
static void rec_draw_text_right(GYSURFACE s, const char* v, GYcolor c, uint8 large, int x_right, int top)
{
	if (!v || !v[0]) return;
	int w = PhoneUI_text_width(v, large);
	PhoneUI_draw_text(s, v, c, large, x_right - w / 2, top);
}
static void rec_draw_bar(GYSURFACE s, int x, int y, int w, int h, int ratio, GYcolor track, GYcolor fill)
{
	GYrect r = {x, y, w, h};
	PhoneUI_rounded(s, &r, track, h / 2, GY_OPA_COVER);
	if (ratio > 0)
	{
		int fw = w * ratio / 100;
		if (fw > w) fw = w;
		if (fw < 1) fw = 1;
		GYrect rf = {x, y, fw, h};
		PhoneUI_rounded(s, &rf, fill, h / 2, GY_OPA_COVER);
	}
}

/* ===========================================================================
 * 录音列表右侧下拉滑条（2026-10-18 补丁28，与照片列表 pic_draw_bar 同算法）
 *
 * 【与照片列表的唯一差别：滚动单位】
 *   照片列表 state.pic_scroll 是**像素**（连续滚动）；
 *   录音列表 state.rec_scroll 是**首条卡片下标**（整数张，历史实现如此）。
 *   ⇒ 滑条位置用 rec_scroll / 最大卡片位移 的比例换算，语义完全等价，
 *     不必改动已有的卡片滚动逻辑（改单位会连带动到下拉刷新那套判定）。
 * =========================================================================== */
static int rec_max_scroll_cards(void)
{
	int m = state.rec_n - REC_SLOTS;
	return (m > 0) ? m : 0;
}

/* 画滑条：轨道 + 滑块。
 * 内容全部可见时画"满高浅色滑块"（而不是整条不画）：告诉用户右侧确实是滚动条、
 * 而且已经看完了 —— 旧照片列表就因为"不画"被当成"边条丢了"（补丁26 的教训）。*/
static void rec_scrollbar_draw(GYSURFACE surface, const GYrect* area)
{
	int ch = state.rec_n * REC_CARD_H;
	int vh = (int)area->h;
	int mc = rec_max_scroll_cards();
	int bx = (int)area->x + (int)area->w - REC_BAR_W - REC_BAR_MARGIN;
	int th, y;

	if (state.rec_n <= 0) return;                 /* 空列表：不画 */

	if (ch <= vh || mc <= 0)
	{
		GYrect full = {(GYcoord)bx, (GYcoord)((int)area->y + 2),
		               (GYcoord)REC_BAR_W, (GYcoord)(vh - 4)};
		PhoneUI_rounded(surface, &full, RGB(203, 210, 220), REC_BAR_W / 2, GY_OPA_COVER);
		return;
	}

	th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(REC_BAR_MIN, th, vh);
	y  = (int)area->y + (int)(((int64_t)(vh - th) * state.rec_scroll) / mc);
	y  = GYLimitMaxMin((int)area->y, y, (int)area->y + vh - th);

	{
		GYrect track = {(GYcoord)bx, (GYcoord)((int)area->y + 2),
		                (GYcoord)REC_BAR_W, (GYcoord)(vh - 4)};
		GYrect thumb = {(GYcoord)bx, (GYcoord)y, (GYcoord)REC_BAR_W, (GYcoord)th};

		PhoneUI_rounded(surface, &track, RGB(219, 224, 233), REC_BAR_W / 2, GY_OPA_COVER);
		PhoneUI_rounded(surface, &thumb, RGB(120, 132, 150), REC_BAR_W / 2, GY_OPA_COVER);
	}
}

/* 拖滑条：把指针 y 绝对映射到"首条卡片下标"，让滑块跟着手指走（照设置 App 的边条滑动）。
 * 末尾 + (vh-th)/2 是**四舍五入**：否则拖到最底也差一张，永远看不到最后一条。 */
static void rec_scrollbar_set_from_pointer(int py, const GYrect* area)
{
	int ch = state.rec_n * REC_CARD_H;
	int vh = (int)area->h;
	int mc = rec_max_scroll_cards();
	int th, y, ns;

	if (ch <= vh || mc <= 0) { state.rec_scroll = 0; return; }

	th = (int)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(REC_BAR_MIN, th, vh);

	y = py - th / 2;
	y = GYLimitMaxMin((int)area->y, y, (int)area->y + vh - th);

	ns = (int)(((int64_t)(y - (int)area->y) * mc + (vh - th) / 2) / (vh - th));
	if (ns < 0) ns = 0;
	if (ns > mc) ns = mc;
	state.rec_scroll = ns;
}

static void rec_list_draw(GYOBJ obj, GYSURFACE surface, const GYrect* abs)
{
	/* ★ 补丁27：一律用**屏幕绝对矩形**。
	 *   draw_cb 收到的是屏幕坐标，而 REC_LIST_X/Y 是 view 内局部坐标（view 从 y=36 起）
	 *   ⇒ 用局部常量画，整块列表会比它的对象矩形高 36px，压在顶部"演示存储/录音"路径条上。
	 *   （与照片列表 pic_list_draw 同一纪律，那处已在补丁25b 改过。） */
	int x  = (int)abs->x;
	int y0 = (int)abs->y;
	int w  = (int)abs->w;
	int vh = (int)abs->h;
	(void)obj;

	/* 列表托盘背景：比 PAPER 略深的圆角底，让浮在其上的白卡有“容器”层次感，
	 * 与全 App 圆角白卡风格统一（照片/笔记分类是单按钮，录音分类是卡片列表）。 */
	{
		GYrect tray = {(GYcoord)x, (GYcoord)y0, (GYcoord)w, (GYcoord)vh};
		PhoneUI_rounded(surface, &tray, RGB(237, 239, 244), 14, GY_OPA_COVER);
	}

	/* 圆角白卡背景（与全 App 风格统一）；选中用浅蓝底 */
	if (state.rec_loading)
	{
		rec_draw_text_left(surface, "加载中…", MUTED, 3, x + 6, y0 + vh/2 - 10);
		return;
	}
	if (state.rec_n == 0)
	{
		rec_draw_text_left(surface, "暂无录音文件", INK, 3, x + 6, y0 + vh/2 - 26);
		rec_draw_text_left(surface, "去「录音机」录一段吧", MUTED, 2, x + 6, y0 + vh/2 + 4);
		return;
	}

	for (int k = 0; k < REC_SLOTS; ++k)
	{
		int entry = state.rec_scroll + k;
		if (entry < 0 || entry >= state.rec_n) break;     /* 没有更多卡片 */

		int card_y = y0 + k * REC_CARD_H;
		int ch = REC_CARD_H - 6;
		int sel = (entry == state.rec_selected) ? 1 : 0;
		/* ★卡片收进右侧滑条左边：用 w 会让卡片右边界(308)盖住滑条(302..308)，
		 *   滑条压在时长/大小文字上 —— 照片列表在补丁26 踩过同一坑。 */
		int cw = REC_CARD_W;
		GYrect card = {x, card_y, cw, ch};
		PhoneUI_rounded(surface, &card, sel ? RGB(223, 231, 255) : WHITE, 12, GY_OPA_COVER);

		/* 左侧圆形播放/暂停键 */
		int cx = x + 30, cy = card_y + ch / 2, r = 17;
		int playing_this = (entry == BoardRec_PlayIndex());
		YMGUI_Draw_CircleFill(surface, cx, cy, r, playing_this ? ACCENT : RGB(214, 219, 232));
		PhoneUI_icon_draw(surface, playing_this ? ICON_PAUSE : ICON_PLAY, cx, cy, WHITE);

		/* 名称（超长截断）*/
		{
			char name[32];
			if (BoardRec_ListName(entry, name, (int)sizeof(name)) == 0)
			{
				if (strlen(name) > 22u) { name[19] = '.'; name[20] = '.'; name[21] = '.'; name[22] = 0; }
				rec_draw_text_left(surface, name, INK, 2, x + 56, card_y + 9);
			}
		}

		/* 时长（右上）/ 损坏标记 */
		if (!BoardRec_IsValid(entry))
		{
			rec_draw_text_right(surface, "损坏", RGB(200, 76, 76), 2, x + cw - 6, card_y + 9);
		}
		else
		{
			uint32_t ms = BoardRec_DurationMs(entry);
			char d[16];
			uint32_t s = ms / 1000u;
			snprintf(d, sizeof(d), "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
			rec_draw_text_right(surface, d, MUTED, 2, x + cw - 6, card_y + 9);
		}

		/* 日期 · 大小（第二行）*/
		{
			char date[24], sz[24], line[64];
			uint32_t size = 0;
			BoardRec_DateText(entry, date, (int)sizeof(date));
			BoardRec_ListSize(entry, &size);
			if (size >= 1024u * 1024u)
			{
				uint32_t mb = size / 1048576u;
				uint32_t d1 = ((size % 1048576u) * 10u) / 1048576u;
				snprintf(sz, sizeof(sz), "%u.%u MB", (unsigned)mb, (unsigned)d1);
			}
			else
				snprintf(sz, sizeof(sz), "%u KB", (unsigned)((size + 1023u) / 1024u));
			snprintf(line, sizeof(line), "%s · %s", date, sz);
			rec_draw_text_left(surface, line, MUTED, 2, x + 56, card_y + 35);
		}

		/* 进度条 / 损坏说明（第三行）*/
		if (!BoardRec_IsValid(entry))
		{
			rec_draw_text_left(surface, "文件损坏 · 无法播放", RGB(200, 76, 76), 2, x + 56, card_y + 50);
		}
		else if (playing_this)
		{
			uint32_t tot = BoardRec_PlayTotalMs();
			uint32_t el  = BoardRec_PlayElapsedMs();
			int ratio = (tot > 0u) ? (int)(el * 100u / tot) : 0;
			if (ratio > 100) ratio = 100;
			rec_draw_bar(surface, x + 56, card_y + 51, cw - 64, 6, ratio,
			             RGB(214, 219, 232), BoardRec_PlayPaused() ? MUTED : RGB(91, 103, 222));
		}
		else
		{
			/* 未播放的卡片留一条空轨道，视觉一致 */
			rec_draw_bar(surface, x + 56, card_y + 51, cw - 64, 6, 0, RGB(214, 219, 232), RGB(214, 219, 232));
		}
	}

	/* 顶部下拉刷新提示（拖动到顶且向下拉时显示）*/
	if (state.rec_pull)
	{
		rec_draw_text_left(surface, "松开刷新…", ACCENT, 2, x + 6, y0 - 0 + 2);
	}

	/* 右侧下拉滑条（补丁28）：最后画，保证压在最上层 */
	rec_scrollbar_draw(surface, abs);
}

/* ---------- 自绘卡片列表：事件（坐标命中 + 拖拽滚动 + 下拉刷新）---------- */

static void rec_list_event(GYOBJ obj, GYEvent e)
{
	GYCTX ctx = obj->ctx;
	GYrect a;
	int px = (int)ctx->point_x;
	int py = (int)ctx->point_y;

	/* ★ 补丁27：ctx->point_x/y 是**屏幕坐标**，REC_LIST_X/Y 是 view 内局部坐标
	 *   （view 从屏幕 y=36 起）⇒ 用局部常量直接减会整体偏 36px：
	 *   按在卡片上半部却命中上一张、按圆形播放键经常没反应。
	 *   一律改用对象自己的绝对矩形（与 file_bar、pic_list 同一纪律）。 */
	YMGUI_Obj_GetAbsArea(obj, &a);
	{
		int ax = (int)a.x, ay = (int)a.y;
		int rel_x = px - ax;
		int rel_y = py - ay;

		if (e == GY_EVENT_Pressed)
		{
			state.rec_press_x = px;
			state.rec_press_y = py;
			state.rec_press_scroll = state.rec_scroll;
			state.rec_drag_moved = 0;
			state.rec_pull = 0;
			state.rec_bar_drag = 0;      /* 默认：拖列表体 */

			/* ---- 按在右侧滑条上 ⇒ 进入"拖滑条"模式（绝对映射，滑块跟手）----
			 * 必须先于卡片命中判断：滑条区落在卡片右侧留白里，若先判卡片
			 * 会得到一个"最右侧那张卡"的命中，拖动就会同时滚卡片 + 拉滑条。 */
			{
				int bar_x = ax + (int)a.w - REC_BAR_W - REC_BAR_MARGIN;
				if (px >= bar_x - REC_BAR_GRAB && px <= bar_x + REC_BAR_W + REC_BAR_GRAB)
				{
					state.rec_bar_drag    = 1;
					state.rec_press_entry = -1;
					state.rec_press_play  = 0;
					rec_scrollbar_set_from_pointer(py, &a);
					YMGUI_Obj_Invalidate(obj);
					return;
				}
			}

			/* 命中哪张卡片、是不是点在圆形键上 */
			{
				int k = rel_y / REC_CARD_H;
				if (rel_y >= 0 && rel_x >= 0 && rel_x < (int)a.w &&
				    k >= 0 && k < REC_SLOTS)
				{
					int entry = state.rec_scroll + k;
					if (entry >= 0 && entry < state.rec_n)
					{
						int cx = ax + 30;
						int cy = ay + k * REC_CARD_H + (REC_CARD_H - 6) / 2;
						int dx = px - cx, dyc = py - cy;
						state.rec_press_entry = entry;
						state.rec_press_play = (dx*dx + dyc*dyc <= 19*19) ? 1 : 0;
					}
					else { state.rec_press_entry = -1; state.rec_press_play = 0; }
				}
				else { state.rec_press_entry = -1; state.rec_press_play = 0; }
			}
			return;
		}
	}

	if (e == GY_EVENT_Pressing)
	{
		/* 拖滑条：绝对映射到首条卡片下标，不参与"卡片滚动/下拉刷新"那套判定 */
		if (state.rec_bar_drag)
		{
			rec_scrollbar_set_from_pointer(py, &a);
			state.rec_drag_moved = 1;      /* 拖过滑条 ⇒ 松手不算点击 */
			YMGUI_Obj_Invalidate(obj);
			return;
		}

		int dy = py - state.rec_press_y;
		if (!state.rec_drag_moved && (dy > 6 || dy < -6)) state.rec_drag_moved = 1;
		if (!state.rec_drag_moved) return;

		int max_scroll = (state.rec_n > REC_SLOTS) ? (state.rec_n - REC_SLOTS) : 0;
		if (state.rec_scroll > 0 || dy < 0)
		{
			/* 正常滚动：向上拖（dy<0）看后面、向下拖（dy>0 且非顶部）看前面 */
			int ns = state.rec_press_scroll - (dy + (dy >= 0 ? REC_CARD_H/2 : -REC_CARD_H/2)) / REC_CARD_H;
			if (ns < 0) ns = 0;
			if (ns > max_scroll) ns = max_scroll;
			if (ns != state.rec_scroll) { state.rec_scroll = ns; YMGUI_Obj_Invalidate(obj); }
			state.rec_pull = 0;
		}
		else if (dy > 0)
		{
			state.rec_pull = 1;          /* 顶部下拉 ⇒ 刷新就绪 */
			YMGUI_Obj_Invalidate(obj);
		}
		return;
	}

	if (e == GY_EVENT_Released)
	{
		if (state.rec_bar_drag)        /* 滑条拖动结束：不算点击、不触发刷新 */
		{
			state.rec_bar_drag = 0;
			state.rec_pull = 0;
			return;
		}
		if (state.rec_pull)
		{
			if (py - state.rec_press_y > 40)   /* 下拉足够距离才触发刷新 */
			{
				state.rec_loading = 1;
				state.rec_need_scan = 1;
				rec_set_hint("刷新中…");
				YMGUI_Obj_Invalidate(obj);
			}
			state.rec_pull = 0;
			return;
		}
		if (!state.rec_drag_moved)
		{
			/* 视为一次点击：圆形键 → 播放/暂停；卡片其余 → 选中 */
			if (state.rec_press_entry >= 0)
			{
				if (state.rec_press_play)
					rec_toggle_play(state.rec_press_entry);
				else
				{
					state.rec_selected = state.rec_press_entry;
					YMGUI_Obj_Invalidate(obj);
				}
			}
		}
		return;
	}

	if (e == GY_EVENT_ReleasedOff)
	{
		state.rec_pull = 0;
		state.rec_bar_drag = 0;
		state.rec_drag_moved = 1;     /* 拖出容器 ⇒ 不算点击 */
		return;
	}
	/* GY_EVENT_Clicked：这里不处理（已在 Released 处理点击，避免双击）*/
	(void)e;
}

static void app_create(GYOBJ view)
{
	state = (AppState){.file_folder = -1, .pic_n = 0, .import_pending = 0,
	                   .import_rc = -1, .bt_last_p = -1, .bt_last_st = -1,
	                   .bt_last_txt = NULL, .rec_n = 0, .rec_loading = 0,
	                   .rec_need_scan = 0, .rec_scroll = 0, .rec_selected = -1,
	                   .rec_del_arm = 0, .rec_pull = 0,
	                   .del_arm = 0, .pic_scroll = 0, .pic_sel = -1, .pic_drag_mode = 0,
	                   .pic_gesture = 0, .pic_move_acc = 0,
	                   .pic_press_entry = -1, .pic_import_idx = -1};
	g_files_folder = -1;
	PhoneUI_app_header(view, "文件管理", "照片 / 笔记 / 录音 读写 TF 卡");
	state.file_path = PhoneUI_left_label(view, 24, 65, 272, "", MUTED, 2);
	/* 分类/蓝牙页的行按钮（补丁30：4 行 → 5 行，多一行「音乐」）。
	 * ⚠ 行距 46 → 40、行高 41 → 36：5×46 会让最后一行落到 322..363，
	 *   直接压在说明区（原 276 起）上；改成 5×40 ⇒ 92..288，说明区挪到 294。 */
	for (int i = 0; i < FOLDER_ROWS; ++i)
	{
		state.file_rows[i] = PhoneUI_button(view, 24, 92 + i * 40, 272, 36, "", file_open, RGB(160, 143, 105));
		/* 自绘：图标靠左 + 名称居右 + 选中态（见 file_row_draw 的说明）*/
		if (state.file_rows[i])
			state.file_rows[i]->draw_cb = file_row_draw;
	}

	/* 蓝牙接收页进度条（仅那个分类可见）*/
	state.bt_bar = YMGUI_Creat_Bar_Creat(view, 24, 291, 272, 12);
	YMGUI_Bar_SetRange(state.bt_bar, 0, 100);
	YMGUI_Bar_SetValue(state.bt_bar, 0);
	YMGUI_Bar_SetColors(state.bt_bar, RGB(70, 62, 52), RGB(231, 173, 65));

	/* 说明区：照片页在列表(88..272)与底栏(372..406)之间 ⇒ 276..368（3 行 × 24）*/
	state.file_preview = PhoneUI_app_text(view, 24, 276, 272, 92, "");

	/* ---- 照片/笔记列表的底栏（2026-10-18 改造）：返回上一级 / 删除 ----
	 * **一个**自绘对象（不是 3 个按钮！见 file_bar_draw 的说明：3 个按钮会把 heap0 挤爆）。
	 * 放在说明区（307..399）下方的 402 行，只在照片/笔记分类显示。 */
	state.file_bar = YMGUI_Creat_Obj_Creat(view, FILE_BAR_X, FILE_BAR_Y, FILE_BAR_W, FILE_BAR_H);
	if (state.file_bar)
	{
		state.file_bar->draw_cb  = file_bar_draw;
		state.file_bar->event_cb = file_bar_event;
	}

	/* ---- 照片/笔记：可滑动卡片列表 + 右侧边条（照 settings.c 的滚动范式）----
	 * **一个**自绘对象：自己裁剪 + 坐标命中，避免给每张卡片都建对象
	 * （heap0 与 DTCM 抢内存，多一个对象就多一次开机分配，见 2026-10-18 白屏事故）。
	 * 默认隐藏，仅照片/笔记分类在 file_list_refresh() 里显示。 */
	state.pic_list = YMGUI_Creat_Obj_Creat(view, PIC_LIST_X, PIC_LIST_Y, PIC_LIST_W, PIC_LIST_H);
	if (state.pic_list)
	{
		state.pic_list->draw_cb  = pic_list_draw;
		state.pic_list->event_cb = pic_list_event;
		YMGUI_Obj_SetHidden(state.pic_list, 1);
	}

	/* ---- 录音卡片列表（补丁19）：自定义容器 + 操作/导航栏 + 重命名浮层 ---- */
	state.rec_list = YMGUI_Creat_Obj_Creat(view, REC_LIST_X, REC_LIST_Y, REC_LIST_W, REC_LIST_H);
	state.rec_list->draw_cb  = rec_list_draw;
	state.rec_list->event_cb = rec_list_event;
	YMGUI_Obj_SetHidden(state.rec_list, 1);

	/* 录音页：列表 88..286 ⇒ 提示 290..308 ⇒ 操作键 312..350 ⇒ 导航键 354..392
	 * （旧值 356/378/424 全部越出 412 高的视口，见 REC_SLOTS 处的说明）*/
	state.rec_hint = PhoneUI_left_label(view, 12, 290, 296, "", MUTED, 2);
	YMGUI_Obj_SetHidden(state.rec_hint, 1);

	state.rec_del = PhoneUI_button(view, 18, 312, 88, 38, "删除", rec_on_del, RGB(147, 112, 136));
	state.rec_rename = PhoneUI_button(view, 116, 312, 88, 38, "重命名", rec_on_rename, RGB(147, 112, 136));
	state.rec_play = PhoneUI_button(view, 214, 312, 88, 38, "播放", rec_on_play, RGB(91, 103, 222));
	state.rec_back = PhoneUI_button(view, 18, 354, 138, 38, "返回上一级", rec_on_back, RGB(160, 143, 105));
	state.rec_refresh = PhoneUI_button(view, 164, 354, 138, 38, "刷新", rec_on_refresh, RGB(160, 143, 105));
	YMGUI_Obj_SetHidden(state.rec_del, 1);
	YMGUI_Obj_SetHidden(state.rec_rename, 1);
	YMGUI_Obj_SetHidden(state.rec_play, 1);
	YMGUI_Obj_SetHidden(state.rec_back, 1);
	YMGUI_Obj_SetHidden(state.rec_refresh, 1);

	/* 重命名浮层（默认隐藏；打开时盖住列表与操作栏）。
	 * 用 PhoneUI_panel 而非裸 YMGUI_Creat_Obj_Creat + draw_cb=NULL：panel 自带
	 * panel_draw 实底绘制，否则裸对象背景可能不画出来（浮层“透明”压不住下面的列表）。 */
	state.rec_rn_panel = PhoneUI_panel(view, 16, 230, 288, 210, RGB(240, 241, 245));
	state.rec_rn_title = PhoneUI_left_label(state.rec_rn_panel, 14, 14, 260, "重命名为", INK, 3);
	state.rec_rn_input = YMGUI_Creat_TextInput_Creat(state.rec_rn_panel, 14, 48, 260, 36, 31);
	state.rec_rn_input->draw_cb = PhoneUI_phone_input_draw;
	state.rec_rn_ok = PhoneUI_button(state.rec_rn_panel, 14, 120, 124, 40, "确认", rec_rn_ok, RGB(91, 103, 222));
	state.rec_rn_cancel = PhoneUI_button(state.rec_rn_panel, 150, 120, 124, 40, "取消", rec_rn_cancel, RGB(147, 112, 136));
	YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);

	file_refresh();
}

static void app_destroy(void)
{
	/* 退出整个 App：停掉可能还在放的卡片播放，释放状态 */
	BoardRec_PlayStop();
	YMGUI_Obj_SetHidden(state.rec_rn_panel, 1);
	memset(&state, 0, sizeof(state));
	/* ⚠ memset 会把 pic_import_idx 变成 0；不清成 -1 就会被 app_tick 当成
	 *   "要导入第 0 张"，下次进 App 直接开始导入（假的用户动作）。 */
	state.pic_import_idx = -1;
	state.pic_press_entry = -1;
	state.pic_sel = -1;
	g_files_folder = -1;
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "file_path"))   return (intptr_t)state.file_path;
	if (!strcmp(name, "file_rows"))   return index >= 0 && index < FOLDER_ROWS ? (intptr_t)state.file_rows[index] : 0;
	if (!strcmp(name, "file_preview"))return (intptr_t)state.file_preview;
	if (!strcmp(name, "file_folder")) return (intptr_t)state.file_folder;
	if (!strcmp(name, "pic_n"))       return (intptr_t)state.pic_n;
	if (!strcmp(name, "pic_evt"))     return (intptr_t)g_pic_evt;
	if (!strcmp(name, "pic_scroll"))  return (intptr_t)g_pic_scroll_diag;
	/* 补丁25e：把"滑动到底走没走通"拆成可直接读的分支量（见 g_pic_* 注释）*/
	if (!strcmp(name, "pic_press"))   return (intptr_t)g_pic_press_evt;
	if (!strcmp(name, "pic_hold"))    return (intptr_t)g_pic_hold_evt;
	if (!strcmp(name, "pic_move"))    return (intptr_t)g_pic_move_px;
	if (!strcmp(name, "pic_rel"))     return (intptr_t)g_pic_rel_evt;
	if (!strcmp(name, "pic_tap"))     return (intptr_t)g_pic_tap;
	if (!strcmp(name, "pic_sel"))     return (intptr_t)state.pic_sel;
	if (!strcmp(name, "bt_bar"))      return (intptr_t)state.bt_bar;
	if (!strcmp(name, "bt_busy"))     return (intptr_t)BoardBt_Busy();
	if (!strcmp(name, "bt_state"))    return (intptr_t)BoardBt_State();
	if (!strcmp(name, "bt_progress")) return (intptr_t)BoardBt_Progress();
	/* ---- 录音卡片（补丁19）---- */
	if (!strcmp(name, "rec_list"))    return (intptr_t)state.rec_list;
	if (!strcmp(name, "rec_n"))       return (intptr_t)state.rec_n;
	if (!strcmp(name, "rec_loading")) return (intptr_t)state.rec_loading;
	if (!strcmp(name, "rec_scroll"))  return (intptr_t)state.rec_scroll;
	if (!strcmp(name, "rec_selected"))return (intptr_t)state.rec_selected;
	if (!strcmp(name, "rec_play_idx"))return (intptr_t)BoardRec_PlayIndex();
	return 0;
}

const PhoneApp PhoneApp_files = {
	.key = "files", .title = "文件管理", .color = RGB(231, 173, 65), .icon = ICON_FILES, .create = app_create, .tick = app_tick, .inspect = app_inspect, .back = app_back, .destroy = app_destroy};
