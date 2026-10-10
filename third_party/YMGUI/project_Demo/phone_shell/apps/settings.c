#include "phone_ui.h"
#include "phone_host.h"
#include "phone_lang.h"
#include "phone_ime.h"   /* PhoneIME_Visible：键盘弹起时收缩滚动视口 */
#include "phone_shell_board.h"   /* BoardNet_*：WiFi/蓝牙无线电开关（2026-10-10） */

/* ===========================================================================
 * 设置页 · 七项 + 滚动（2026-10-04 新增"半透明效果"卡片 + 滚动容器）
 *
 * 【为什么改成滚动】第七张卡片塞不下：底部导航栏固定在 y=448，
 * 七项按 50 px 等分排到 y=491，超出可视区 60 px。
 * 用户 2026-10-04 明确要求**不压缩卡片高度**（50 px 是当前视觉密度，
 * 压到 44 会让整页显挤），因此改为可上下滚动。
 *
 *   项              滚动前(y,h)   滚动后(y,h)
 *   无线网络          65,  50     65,  50（不变）
 *   海蓝色壁纸       133,  50   133,  50（不变）
 *   界面语言         201,  50   201,  50（不变）
 *   半透明效果（新增）   —      →  269,  50
 *   预览亮度         269,  62   329,  62
 *   设备名输入       347,  28   407,  28
 *   关机按钮         391,  40   451,  40  └ 内容总高 491
 *
 * 【视口几何 —— 头部必须让出来】
 *   app 视图区 abs y=36..448（高 412）。头部两条 label 占 y=0..56
 *   （SCROLL_HEAD_H），滚动视口从 y=56 起、高 412-56=356。
 *   ⇒ clamp 上限 = content_h - 视口高 = 559-356 = **203**。
 *   ⚠ 视口不透明 + 开 ClipChildren，且**建在头部之后** ⇒ 会盖住头部。
 *     之前视口从 y=0 起，标题"设置/把手机调成喜欢的样子"被完全遮住
 *     （2026-10-04 板级抓屏实测：标题区一个文字像素都没有）。别再犯。
 *
 * ⚠ 改动布局必须同步 phone_shell/phone_selftest.inc 里的坐标断言
 *   （`device_name_input->area.y == 347` → 407 → 475），否则自检会失败。
 *
 * ⚠⚠ 底部三项（397 / 475 / 519）在 scroll_y=0 时**落在视口外**：
 *   视口 abs y = 36+56=92 .. 92+356=448，关机按钮 abs y = 92+519-203=408
 *   （滚到底后）—— 也就是说**滚到底它才进视口**。
 *   **任何脚本/自检要点它，必须先滚动到底。**
 *   这也是修掉 hitRec 忽略 ClipChildren 之后"点不到"变"明确不可点"的原因。
 *
 * 【滚动容器的实现要点 —— 三条都踩过坑，别改错】
 *
 * 1. **为什么不能直接用 YMGUI_Creat_List_Creat**
 *    库的 List（YMGUI_List.c:163 `YMGUI_List_AddItem`）只能加"文字条目"：
 *    内部 malloc 一个 GYitem_data 塞进 user_data 并挂上 itemDrawCb
 *    （画底色 + 默认字体 + 底部分隔线）。我们的卡片是 PhoneUI_panel + 一堆子控件，
 *    塞进去会被它自己的 draw_cb 覆盖掉。
 *    ⇒ 结论：自建一个"只有视口语义"的容器，复制库的两件事 ——
 *       开 ClipChildren（裁剪）、子对象绝对坐标减 scroll_y（引擎已在
 *       YMGUI_Obj.c:290 实现：子对象 abs 坐标 -= parent->scroll_y，绘制与命中都算）。
 *
 * 2. **为什么每个交互控件都要包一层转发**
 *    库的 sendEvent（YMGUI_Event.c:68）**不向父对象冒泡** ——
 *    只发给命中的那个对象。所以手指按在 switch 上时，Pressing 只进 switch，
 *    滚动容器永远收不到，滚不动。
 *    ⇒ 做法：给每个可交互子控件装一个 wrapper，把 Pressed/Pressing 转发给
 *       滚动容器（这正是库里 itemEventCb 的做法，见 YMGUI_List.c:120-124）。
 *       同时 wrapper 记住"本次手势是否移动过"，用来在 Clicked 时**吞掉滚动误触**。
 *
 * 2b. **必须递归包装整棵子树，不能只装交互控件**（2026-10-04 修正）
 *    命中测试取的是**最深的命中对象**，不是"最近的装了 wrapper 的祖先"
 *    （YMGUI_Event.c 的 hitRec）。而卡片是 PhoneUI_panel（只有 draw_cb，
 *    **没有 event_cb**）加几个 left_label 组成的 ⇒ 按在卡片中央的空白处时，
 *    命中最深的是 panel 或 label，它们的 event_cb 为 NULL ⇒ sendEvent 直接
 *    丢弃（YMGUI_Event.c:70 的 `if (... && obj->event_cb != NULL)`）
 *    ⇒ **手指按在卡片上怎么拖都不动**，只有按在开关/按钮上才能滚。
 *    ⇒ 现在用 scroll_wrap_tree 递归给子树里每个对象都装一层。
 *       对 event_cb 本就为 NULL 的对象，wrapper 转发时无事可做，纯增不减。
 *
 * 3. **为什么 Clicked 要防误触**
 *    库在"按下和抬起都在同一对象"时发 Clicked（YMGUI_Event.c:345-351），
 *    且 Switch 的 swEventCb（YMGUI_Switch.c:46）收到 Clicked 就**立刻翻转状态**。
 *    若用户是"按下开关后想滚动、手指仍在开关上抬起" ⇒ 开关被误改。
 *    ⇒ wrapper 记录 Pressed 时的指针 y 与当时 scroll_y；Pressing 里一旦位移
 *       超过 SCROLL_SLOP 就置 moved 标志；Clicked 时若 moved 则只把开关拨回去，
 *       **不改业务状态**。
 *
 * 【为什么保留 YMGUI_List 作为参考而不用】它验证了 scroll_y 语义可用
 *   （tests/test_scroll.c 是官方单测），但它的 item 模型与我们不兼容（见 1）。
 *   保留这条结论，避免以后有人再踩"为什么不能用现成 List"这一遍。
 *
 * 【语言卡片 = 样式 B：分段选择器】
 *   左边一段"中文"、右边一段"English"，当前语言那段填成蓝色胶囊（白字），
 *   另一段是蓝字无底。用**两个并排按钮**实现：
 *     事件走现成的 PhoneUI_button（Clicked 回调），无需自己解析点击坐标
 *     （库的 GY_EVENT_Clicked 不带坐标，自己判断落点反而更绕）；
 *     绘制换成下面的 lang_seg_draw —— 高亮状态**实时读 PhoneLang_Get()**，
 *     因此切换后只要把两个按钮置脏就会重画成正确状态。
 * =========================================================================== */

#include "YMGUI_Obj.h"
#include "YMGUI_Invalidate.h"

/* ---- 滚动容器 ---- */
#define SCROLL_SLOP        4      /* 位移超过这么多 px 就判定为"在滚动"，吞掉点击 */
#define SCROLL_BAR_W       3      /* 滚动条宽 */
#define SCROLL_BAR_MARGIN  3      /* 距视口右边的留白 */
#define SCROLL_BAR_MIN     24     /* 滑块最小高度，太短会看不见 */

/* 头部高度（"设置" + 副标题两条 label）。
 * ⚠⚠ 视口必须从 y=SCROLL_HEAD_H 起 —— 它不透明且开了 ClipChildren，
 *   会盖住**建在它之前**又落在它范围内的兄弟对象（后建的画在上面）。
 *   之前视口从 y=0 起而标题也在 y=0/37 ⇒ 标题被完全遮住
 *   （2026-10-04 板级抓屏实测：标题区一个文字像素都没有）。 */
#define SCROLL_HEAD_H      56
/* 视口高 = app 视图区高 412 减头部。 */
#define SCROLL_VIEW_H      (412 - SCROLL_HEAD_H)
/* 视口顶边的绝对 y：app 视图区 abs y=36，头部占 36..36+56。 */
#define SCROLL_VIEW_TOP    (36 + SCROLL_HEAD_H)
/* 软键盘顶边 y（phone_ime.c 的 KEYBOARD_Y，键盘对象建在 top_layer 上）。
 * 键盘弹起时视口要缩到这条线以上，否则底部控件被键盘盖住。 */
#define SCROLL_IME_TOP_Y   226
/* 键盘弹起时的视口高（= 键盘上沿 − 视口顶边，减去 6 px 留缝）。 */
#define SCROLL_VIEW_H_IME  (SCROLL_IME_TOP_Y - SCROLL_VIEW_TOP - 6)

typedef struct
{
	GYOBJ   view;           /* 视口：开 ClipChildren，scroll_y 生效 */
	int32_t content_h;      /* 内容总高（最后一个卡片的底边） */
	int32_t bar_h;          /* 滚动条滑块当前高度（画的时候要算，缓存省得每帧除法） */
	uint8_t dragging;       /* 1 = 本次手势正在拖动 */
	uint8_t moved;          /* 1 = 本次手势已移动超过 SLOP（用于吞 Clicked） */
	uint8_t ime_shrunk;     /* 1 = 视口已按键盘收缩过（避免每帧重复改几何） */
	int32_t press_ptr_y;    /* Pressed 时的指针 y（屏幕坐标） */
	int32_t press_scroll;   /* Pressed 时的 scroll_y */
	GYOBJ   bar;            /* 滚动条（view 的兄弟对象，画在卡片之上） */
	/* 供诊断量现算绝对坐标用的对象指针（放在这里就绕开了 AppState 的
	 * 声明顺序问题 —— scroll_place_bar 在 AppState 定义之前）。 */
	GYOBJ   sw_probe;       /* 半透明开关 */
	GYOBJ   btn_probe;      /* 关机按钮 */
	/* 键盘弹起时要滚进视野的输入框。同样是因为声明顺序放在这里。
	 * input_bottom = 该输入框在**内容坐标**里的底边（布局期算好，
	 * 与 IME 的 area 改写无关）—— 见 scroll_apply_ime 里的警告。 */
	GYOBJ   input_probe;
	int32_t input_bottom;
} ScrollCtx;
static ScrollCtx s_scroll;

/* 滚动位置诊断量（2026-10-04，供 tools/ribbon_check.py 读）。
 * scroll_y/max 是**对象字段**，脚本无法直接取址，只能由固件镜像出来。
 * 内容总高也一并暴露：验收脚本要断言"滚到底 == max"，需要知道 max。 */
volatile int32_t g_diag_scroll_y = 0;
volatile int32_t g_diag_scroll_max = 0;
volatile int32_t g_diag_scroll_content = 0;

/* 半透明开关中心的**绝对屏幕坐标**（2026-10-04）。
 *
 * 【为什么必须由固件给，脚本不能自己算】开关位置 = 视口 abs y + 卡片内容 y
 *   + 卡片内偏移 − scroll_y。这几项里有 scroll_y（运行时状态），
 *   而每次改布局（本次就把视口下移了 56px 让出头部）算式就变 ——
 *   实测就因为按旧算式点 (278,330) 而点空，验收报"开关没翻转"。
 *
 * ⚠⚠ 由 Settings_UpdateDiag() **每帧**现算（phone_shell.c 的 advance 里调）。
 *   绝不能只在布局变化时算：app_page 自己会停在 y=480（on_recent 把它推到
 *   屏幕下方），而 open_app 的归位动画发生在 app_show 之后 ⇒ 算完就过期。
 *   详见 Settings_UpdateDiag 的注释。 */
volatile int32_t g_diag_ribbon_sw_x = 0;
volatile int32_t g_diag_ribbon_sw_y = 0;

/* 关机按钮中心的绝对屏幕坐标（同上理由：不许脚本自己算）。
 * ⚠ 它在 scroll_y=0 时落在视口外**且在屏幕之外**，所以脚本必须先滚到底再点。 */
volatile int32_t g_diag_power_btn_x = 0;
volatile int32_t g_diag_power_btn_y = 0;

/* 语言分段两段的绝对屏幕坐标（2026-10-04 新增）。
 *
 * 【为什么加这个】lang_check.py 原来用的是上一版布局实测下来的硬编码
 *   (195,255)/(259,255)。后来把设置页改成滚动版、并在语言卡下方插入了
 *   「半透明」卡片，卡片整体下移 ⇒ 脚本点空，报
 *   "点 English 段后 g_cfg_lang 仍 = 0（坐标不可点？）"。
 *   这类"布局一改坐标就失效"的坑已经踩过三次（设置图标、语言两段、
 *   半透明开关），所以统一收敛到固件现算，别再在脚本里存坐标。
 *
 * ⚠ 与上面几个诊断量同样**每帧**现算（见 Settings_UpdateDiag 的坑位说明）。 */
volatile int32_t g_diag_lang_zh_x = 0;
volatile int32_t g_diag_lang_zh_y = 0;
volatile int32_t g_diag_lang_en_x = 0;
volatile int32_t g_diag_lang_en_y = 0;

/* 亮度滑杆两端的绝对屏幕坐标（2026-10-04 新增）。
 * 同理：验收脚本要"从最左拖到最右"，起止点必须由固件给，
 * 否则滑杆范围一改（本次就是下限 0→1）脚本就拖偏。 */
volatile int32_t g_diag_bright_x0 = 0;
volatile int32_t g_diag_bright_x1 = 0;
volatile int32_t g_diag_bright_y  = 0;

/* 亮度滑杆的事件通路诊断（2026-10-04，排查"注入点不响应"）。
 *
 * 【背景】坐标现算与抓屏都对得上（x0=32 x1=288 y=330），但 SWD 注入点击
 *   扫 y=300..360 全范围无响应 ⇒ 不是坐标问题，是事件没进到滑杆。
 *   需要区分三种可能，用两个计数器分别打点：
 *     g_diag_sld_press —— wrapper 是否收到了滑杆的 Pressed（事件命中到了它）
 *     g_diag_sld_press 之外的 committed —— 松手时是否真的提交过
 *   ⚠ 这类"计数器打点"比推理靠谱：2026-10-04 这次坐标/抓屏/符号三路
 *     全对，最后靠它才定位到是命中问题。 */
volatile uint32_t g_diag_sld_press  = 0;   /* wrapper 转发给滑杆的事件数 */
volatile uint32_t g_diag_sld_change = 0;   /* brightness_changed 被调次数 */
volatile uint32_t g_diag_sld_commit = 0;   /* brightness_commit 实际提交次数 */

/* wrapper 最后一次命中到的对象指针（scroll_wrap_event 写）。
 * ⚠ 为什么不在 wrapper 里直接判"是不是滑杆"：wrapper 在 450 行，
 *   而 state 定义在 600 行之后，编译不过。记指针、在 Settings_UpdateDiag
 *   （state 可见处）比对，是绕开声明顺序的干净做法。 */
volatile uint32_t g_diag_last_event_obj = 0;

static void scroll_clamp(void)
{
	int32_t maxs = s_scroll.content_h - s_scroll.view->area.h;
	if (maxs < 0)
	{
		maxs = 0;
	}
	s_scroll.view->scroll_y = GYLimitMaxMin(0, s_scroll.view->scroll_y, maxs);
	/* 板级验收用的诊断量（tools/ribbon_check.py 读它）。
	 * ⚠ 为什么必须在这里同步：scroll_y 是**对象字段**而不是全局变量，
	 *   read_vars.py 只能按 ELF 符号取全局变量地址，取不到对象的成员。
	 *   而 scroll_clamp 是 scroll_y 唯一的写入口（scroll_drag / app_show /
	 *   app_create 都经它），所以放在这里就绝不会漏。
	 * ⚠ 别试图直接从脚本算 scroll_y —— 视口高度、内容总高、clamp 规则
	 *   都是实现细节，改布局就会漂。 */
	g_diag_scroll_y = s_scroll.view->scroll_y;
	g_diag_scroll_max = maxs;
}

static void scroll_place_bar(void)
{
	int32_t vh, ch, th, th_max, y;
	GYcoord vy, vx;

	if (s_scroll.bar == NULL)
	{
		return;
	}
	vh = s_scroll.view->area.h;
	ch = s_scroll.content_h;
	vx = s_scroll.view->area.x;
	vy = s_scroll.view->area.y;

	if (ch <= vh)
	{
		/* 内容没超出视口 ⇒ 不显示滚动条 */
		YMGUI_Obj_SetHidden(s_scroll.bar, 1);
		return;
	}
	YMGUI_Obj_SetHidden(s_scroll.bar, 0);

	/* 滑块高度按比例，且不小于 SCROLL_BAR_MIN */
	th = (int32_t)(((int64_t)vh * vh) / ch);
	th = GYLimitMaxMin(SCROLL_BAR_MIN, th, vh);
	th_max = vh - th;

	/* 滑块位置随 scroll_y 线性映射；分母是 (ch - vh)，
	 * 用 (int64) 是为防 320×480 级别下 vh*vh 溢出 32 位（392×392 不溢出，
	 * 但写习惯更稳，且内容高将来可能更大）。 */
	if (ch > vh)
	{
		y = vy + (int32_t)(((int64_t)(vh - th) * s_scroll.view->scroll_y) / (ch - vh));
	}
	else
	{
		y = vy;
	}
	y = GYLimitMaxMin(vy, y, vy + th_max);

	s_scroll.bar->area.x = (GYcoord)(vx + s_scroll.view->area.w
	                                - SCROLL_BAR_W - SCROLL_BAR_MARGIN);
	s_scroll.bar->area.y = (GYcoord)y;
	s_scroll.bar->area.h = (GYcoord)th;
	s_scroll.bar->area.w = (GYcoord)SCROLL_BAR_W;
}

static void scroll_moved(void)
{
	GYcoord old_scroll = s_scroll.view->scroll_y;
	GYrect  old_bar    = s_scroll.bar ? s_scroll.bar->area : (GYrect){0, 0, 0, 0};
	uint8   bar_moved;

	scroll_clamp();
	scroll_place_bar();

	/* ★只有**真的变了**才整视口标脏（2026-10-04 优化）。
	 *
	 * 【为什么非加这一道不可】滚动手势的 GY_EVENT_Pressing 分支是**无条件**
	 *   赋值 scroll_y + 调本函数的（见上面 event_cb：SLOP 只用来置 moved 标志，
	 *   并没拦住 394/395 两行），而那个处理器挂在**整个滚动视口**上、是页内所有
	 *   控件的祖先 ⇒ 设置页里任何拖动（**包括亮度滑杆**）冒泡上来都会执行到这里，
	 *   把 320×356 的视口整块标脏。
	 *
	 * 【板级实测】一次亮度拖动恒定刷 **113920 px**（= 11 条整宽 band + 一条
	 *   320×4，正好是整个视口），而横向拖动时 scroll_y 从头到尾没变过 ——
	 *   纯属白刷。加这道判断后，只有 scroll_y 或滚动条几何真的变了才重绘。
	 *
	 * 【为什么滚动条也要一起比】内容高度变化时 scroll_y 可能不变而条长/位置变，
	 *   漏比就会出现"滚动条画在原地"的残影。 */
	bar_moved = (uint8)((s_scroll.bar != NULL) &&
	                    ((s_scroll.bar->area.x != old_bar.x) || (s_scroll.bar->area.y != old_bar.y) ||
	                     (s_scroll.bar->area.w != old_bar.w) || (s_scroll.bar->area.h != old_bar.h)));
	if ((s_scroll.view->scroll_y == old_scroll) && (bar_moved == 0u))
	{
		return;                 /* 位置没变：不产生脏区 */
	}

	YMGUI_Obj_Invalidate(s_scroll.view);
	YMGUI_Obj_Invalidate(s_scroll.bar);
}

/* 键盘弹起/收起时调整视口高度（2026-10-04）。
 *
 * 【为什么必须在这里做，而不能让 IME 去挪控件】
 *   phone_ime.c 的 PhoneIME_Show() 里有一段"把被遮挡的输入框往上挪 / 压扁"
 *   的避让逻辑。它先 YMGUI_Obj_GetAbsArea() 拿**绝对**坐标（含父链的
 *   scroll_y 偏移），再改对象的 **area.y（局部坐标）**——
 *   两种坐标系混用，在普通定长页面里凑巧能用，在**滚动容器**里必然错位：
 *   板级抓屏实测（build/ime_after.png）键盘弹起后"Yaomi Phone"输入框
 *   直接压在"界面语言"卡片上，两行文字叠成乱码。
 *   ⇒ 正确做法是让**容器**自己让出键盘的高度（视口变矮），
 *   内容随之可以滚上去，控件一个都不用挪。
 *
 * 【为什么用 ime_shrunk 去重】
 *   本函数由 app_tick 每帧调用，而几何改动会触发重绘。
 *   不去重的话每帧都在改 area.h ⇒ 每帧全视口重绘，白烧 CPU。 */
static void scroll_apply_ime(int ime_on)
{
	int32_t want_h = ime_on ? SCROLL_VIEW_H_IME : SCROLL_VIEW_H;

	if ((uint8_t)(ime_on ? 1u : 0u) == s_scroll.ime_shrunk)
	{
		return;                 /* 状态没变，geometry 已对 */
	}
	s_scroll.ime_shrunk = (uint8_t)(ime_on ? 1u : 0u);
	s_scroll.view->area.h = (GYcoord)want_h;
	/* 键盘弹起时把焦点控件滚进视野。
	 * ⚠ 不能简单滚到底（板级抓屏实测：视口缩到 128 px，滚到底只能看到
	 *   亮度滑杆下半 + 关机按钮，**用户刚点开的输入框反而在视口外**）。
	 *   正确做法：保证焦点输入框的底边落在视口内即可。
	 *   设备名输入框在内容 y=475、高 28 ⇒ 需要 scroll_y ≥ 475+28-128 = 375。
	 *   取"刚好够"而不是 clamp 上限，用户还能往上翻看别的卡片。 */
	if (ime_on)
	{
		/* 找当前焦点对象（引擎在 Pressed 时置的 Focusable 焦点） */
		GYCTX  c  = s_scroll.view->ctx;
		GYOBJ  fo = (c != NULL) ? c->focus_obj : NULL;

		/* ⚠⚠ 这里**不能**用 fo->area 累加来定位它。
		 *   phone_ime.c 的 PhoneIME_Show() 已经把被遮挡输入框的 area.y 上移、
		 *   area.h 压扁了（见本函数上方的说明），而本函数由 app_tick 调用，
		 *   **跑在 PhoneIME_Update 之后**（phone_shell.c:648 先 IME_Update、
		 *   659 才 PhoneApps_Tick）⇒ 读到的已是被改坏的局部坐标。
		 *   实测正是这样：hit=1 逻辑执行了，但算出的 need=135（=不需滚），
		 *   因为 area.y 上移量恰好抵消了 area.h 的压缩量。
		 * ⇒ 改用 s_scroll.probe_h：布局期记下的、与 IME 无关的控件原始高度。
		 *   沿 parent 链只累加 y 也不安全（同理），所以整段改成
		 *   "拿探针指针直接比"：不是本页的输入框就不滚。 */
		if (fo == s_scroll.input_probe)
		{
			int32_t need = s_scroll.input_bottom - want_h;
			int32_t maxs = s_scroll.content_h - want_h;
			if (need < 0)
			{
				need = 0;
			}
			if (need > maxs)
			{
				need = maxs;
			}
			s_scroll.view->scroll_y = (GYcoord)need;
		}
	}
	/* 视口变矮 ⇒ clamp 上限变大（内容还能往上滚），
	 * 原来停在 135 的位置可能不再到底 ⇒ 必须重新 clamp。 */
	scroll_moved();
}

/* 拖动处理：由容器自身与所有 wrapper 转发进来 */
static void scroll_drag(GYEvent e)
{
	GYCTX c = s_scroll.view->ctx;
	int32_t py;

	if (c == NULL)
	{
		return;
	}
	py = c->point_y;

	if (e == GY_EVENT_Pressed)
	{
		s_scroll.press_ptr_y  = py;
		s_scroll.press_scroll = s_scroll.view->scroll_y;
		s_scroll.dragging     = 1u;
		s_scroll.moved        = 0u;   /* ★每次新手势都清，否则上次的滚动会误吞这次的点击 */
	}
	else if (e == GY_EVENT_Pressing)
	{
		int32_t d = s_scroll.press_ptr_y - py;
		if (s_scroll.dragging != 0u)
		{
			if ((d > SCROLL_SLOP) || (d < -SCROLL_SLOP))
			{
				if (s_scroll.moved == 0u)
				{
					/* 判定为滚动的那一刻，撤掉焦点。
					 *
					 * 【为什么必须做】引擎在 Pressed 时会给命中的可聚焦对象
					 * 置 Focusable 焦点（YMGUI_Event.c:338-343），而设置页的
					 * 设备名输入框就在滚动区里。手指按住它想上拖滚动时，
					 * 焦点已经落在输入框上 ⇒ 软键盘立刻弹出、盖住 y=226..448，
					 * 之后所有点击都被键盘吃掉。实测：drag 起点 (160,380)
					 * 正落在滚到底后的输入框上（abs 364..412），一次拖动就把
					 * 键盘拉起来，表现为"收好键盘又莫名其妙弹出来"。
					 *
					 * 【为什么在第一次 moved 时做一次就够】之后 moved 已是 1，
					 *  不必反复清 —— 反复清会把用户真正的编辑也打断。 */
					GYCTX c2 = s_scroll.view->ctx;
					if (c2 != NULL)
					{
						YMGUI_SetFocus(c2, NULL);
					}
				}
				s_scroll.moved = 1u;
			}
			s_scroll.view->scroll_y = s_scroll.press_scroll + d;
			scroll_moved();
		}
	}
	else if (e == GY_EVENT_Released || e == GY_EVENT_ReleasedOff)
	{
		s_scroll.dragging = 0u;
		/* moved 不清：Clicked 紧随 Released 之后发出（见 YMGUI_Event.c:349-350），
		 * 必须留到 Clicked 用完才能复位。复位放在点击回调里。 */
	}
}

/* 容器自身的事件（空白处按下也能拖） */
static void scroll_view_event(GYOBJ obj, GYEvent e)
{
	(void)obj;
	scroll_drag(e);
}

/* 滚动条的绘制：半透明圆角细条 */
static void scroll_bar_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	(void)obj;
	PhoneUI_rounded(s, a, RGB(120, 132, 150), SCROLL_BAR_W, GY_OPA_COVER);
}

/* 亮度滑杆的"松手落盘"（定义在下方亮度段落里）。
 * ⚠ 这条声明**必须**出现在 scroll_wrap_event 之前 —— wrapper 在 451 行就要调它，
 *   而 AppState 那组前向声明在 578 行之后。来早了 gcc 会按隐式声明处理成
 *   extern（非 static），与下面的 static 定义冲突，报
 *   "static declaration follows non-static declaration"。 */
static void brightness_commit(void);

/* ---- 子控件 wrapper：把拖动转发给滚动容器，并吞掉滚动时的误点击 ----
 *
 * 【为什么必须包一层】库的 sendEvent（YMGUI_Event.c:68-72）**不向父冒泡**，
 * 只发给命中的那个对象。手指按在 Switch 上时 Pressing 只进 Switch，
 * 滚动容器收不到 ⇒ 滚不动。
 *
 * 【为什么不能"外面再包一层对象"】命中测试取**最深的命中对象**
 * （YMGUI_Event.c 的 hit 查找），外面那层根本收不到事件。
 *
 * 【怎么旁挂】控件的 event_cb 已被库占用（Switch 是 swEventCb，
 * YMGUI_Switch.c:86），直接改会覆盖它的逻辑。所以备份原指针、
 * 换成自己的回调，按事件类型分派：拖动类转给滚动容器，其余转回原回调。
 * 每个控件在 s_wrap[] 里占一格，索引存进 obj->user_data 的**包装索引**。
 *
 * ⚠ 不能把索引塞进 obj->user_data —— 那是 Switch/Button 自己的数据指针
 *   （Switch 的 GYsw_data 在 YMGUI_Switch.c:24 是 .c 文件里的私有类型，
 *   头文件看不到）。覆盖它会把开关状态整个读成垃圾。
 *   ⇒ 改用"按对象指针反查索引"：控件数量是个位数，线性找完全够。 */
/* ⚠ 容量必须 ≥ 视口内全部对象数（scroll_wrap_tree 会给每个都装一层）。
 *   实测当前是 7 张卡 × (panel + 2 label + 控件) + 输入框 + 关机按钮 + 滚动条
 *   ≈ 30 个。这里给 48，留出"再加两张卡"的余量。
 *   ⚠ 装满后 scroll_wrap_ex 是**静默 return**（见上），不报错 ⇒ 溢出表现为
 *   "后面几个控件滚不动"，极难定位。改布局后若出现局部滚不动，先查这个数。 */
#define SCROLL_WRAP_MAX  48
static void (*s_wrap_cb[SCROLL_WRAP_MAX])(GYOBJ, GYEvent);
static GYOBJ   s_wrap_obj[SCROLL_WRAP_MAX];
static uint8_t s_wrap_is_sw[SCROLL_WRAP_MAX];   /* 该控件是不是 Switch */
static int     s_wrap_n = 0;

static int scroll_wrap_index(GYOBJ obj)
{
	int i;
	for (i = 0; i < s_wrap_n; ++i)
	{
		if (s_wrap_obj[i] == obj)
		{
			return i;
		}
	}
	return -1;
}

static void scroll_wrap_event(GYOBJ obj, GYEvent e)
{
	int idx = scroll_wrap_index(obj);

	/* 诊断：记下最后命中到的对象指针（见 g_diag_last_event_obj 的定义处注释）。
	 * ⚠ 不能在这里比state.bright_slider —— 本函数在 450 行，而 state 定义在
	 *   600 行之后，编译不过。改成记指针、由 Settings_UpdateDiag（state 可见处）
	 *   每帧比对并累加计数。 */
	g_diag_last_event_obj = (uint32_t)(uintptr_t)obj;

	/* 0) 亮度滑杆的"松手落盘"必须在**转发之前**。
	 *
	 * 【为什么可以放在 wrapper 而不是 Slider 自己的回调里】
	 *   Slider 的 sldEventCb（YMGUI_Slider.c:108-111）在 Released/ReleasedOff
	 *   只做"恢复拖柄色"，不碰值也不通知应用 —— 库没给"松手"留钩子，
	 *   所以只能在我们这层包住。包住是干净的：brightness_commit 内部
	 *   有 s_bright_dragging 守卫，不是亮度拖动的松手就直接 return。
	 *
	 * 【为什么 Released 和 ReleasedOff 都要收】
	 *   手指按下后滑出控件范围再抬起，引擎发的是 ReleasedOff
	 *   （Slider.c:109 是同款判断）。只认 Released 会在"拖出界外松手"时
	 *   丢掉最后一次落盘 ⇒ 屏上显示的是新值、闪存里存的是旧值，
	 *   重启后亮度"自己变回去"，最难排查。
	 *
	 * ⚠ 放在 scroll_drag 之后、转发之前：此刻 Pressing 阶段已经把最终值
	 *   预览进 display_brightness 了，commit 读到的就是用户的最终值。 */
	if (e == GY_EVENT_Released || e == GY_EVENT_ReleasedOff)
	{
		brightness_commit();
	}

	/* 1) 拖动类事件转给滚动容器（Pressed 必须也转，
	 *    它要记锚点 press_ptr_y/press_scroll，没有锚点就没法算位移）。 */
	if ((e == GY_EVENT_Pressed) || (e == GY_EVENT_Pressing) ||
	    (e == GY_EVENT_Released) || (e == GY_EVENT_ReleasedOff))
	{
		scroll_drag(e);
	}

	/* 2) 手势移动过 ⇒ 这次的 Clicked 是"想滚动"而不是"想点击"，吞掉。
	 *
	 * Switch 的 swEventCb（YMGUI_Switch.c:46-54）收到 Clicked 就翻转状态
	 * 并调 changed 回调 —— 也就是**真去改业务状态**。若放任不管，
	 * 用户"按住开关想滚动、手指仍在开关上抬起"就会误改设置。
	 * 回滚用公开的 YMGUI_Switch_SetOn：它只写状态、不触发 changed
	 *（YMGUI_Switch.c:95-100），正是这里需要的语义。
	 *    ⚠ 不能直接改 GYsw_data.on —— 那个类型是 Switch.c 的私有类型，
	 *      头文件里不可见，硬改要重新声明结构体，成员一改就静默错位。 */
	if ((e == GY_EVENT_Clicked) && (s_scroll.moved != 0u))
	{
		if ((idx >= 0) && (s_wrap_is_sw[idx] != 0u))
		{
			YMGUI_Switch_SetOn(obj, (uint8_t)(YMGUI_Switch_GetOn(obj) ? 0u : 1u));
		}
		/* Button/Slider 的 Clicked 语义是"点了才回调"，直接不转发即可，
		 * 它们的业务回调不会被触发，无需回滚任何状态。 */
		s_scroll.moved = 0u;   /* 这次手势的移动标记到此用完 */
		return;
	}

	/* 3) 其余事件交还控件原本的回调（Clicked / Focus* 等靠它正常工作）。 */
	if ((idx >= 0) && (s_wrap_cb[idx] != NULL))
	{
		s_wrap_cb[idx](obj, e);
	}
}

/* 装到交互控件上。必须在控件建好**之后**调用（要备份它已有的 event_cb）。
 * is_sw 传"这是不是 Switch"—— 库里没有 GY_OBJ_Switch 类型枚举可判
 *（YMGUI_Obj.h:17 只有 GY_OBJ_Button，Switch 复用通用 Obj），故由调用方指定。 */
static void scroll_wrap_ex(GYOBJ obj, int is_sw)
{
	if ((obj == NULL) || (s_wrap_n >= SCROLL_WRAP_MAX))
	{
		return;
	}
	s_wrap_cb[s_wrap_n]   = obj->event_cb;
	s_wrap_obj[s_wrap_n]  = obj;
	s_wrap_is_sw[s_wrap_n] = (uint8_t)(is_sw != 0);
	s_wrap_n += 1;
	obj->event_cb = scroll_wrap_event;
}

static void scroll_wrap(GYOBJ obj)
{
	scroll_wrap_ex(obj, 0);
}

/* 递归给一棵子树里**所有**对象装 wrapper（含 panel 和 label）。
 *
 * 【为什么必须递归、不能只装交互控件】
 *   库的事件分发只投给"命中最深的那一个对象"，而卡片是用
 *   PhoneUI_panel（纯背景 + draw_cb，**没有 event_cb**）加几个
 *   PhoneUI_left_label 组成的。按在卡片中央的空白处时命中最深的是
 *   panel 或 label，它们的 event_cb 是 NULL ⇒ sendEvent 直接丢弃
 *   （YMGUI_Event.c:70 `if (... && obj->event_cb != NULL)`）
 *   ⇒ 手指怎么拖都不动。
 *   命中"最深"而不是"最近装了 wrapper 的祖先"，所以在祖先上加回调
 *   也救不了 —— 必须在**可能被命中的每个对象**上都装。
 *
 * 【为什么安全】对 event_cb 本来就是 NULL 的对象，wrapper 在转发
 *   第 3 步时 `s_wrap_cb[idx]` 为 NULL，什么都不做 ⇒ 纯增不减。
 *   对本来就有回调的（Switch/Button/Slider/TextInput），wrapper
 *   备份原指针后照旧转发 ⇒ 行为与之前一致。 */
static void scroll_wrap_tree(GYOBJ obj)
{
	GYOBJ c;
	if (obj == NULL)
	{
		return;
	}
	scroll_wrap(obj);
	for (c = obj->child_head; c != NULL; c = c->sibling)
	{
		scroll_wrap_tree(c);
	}
}

typedef struct
{
	GYOBJ header_title, header_sub;
	GYOBJ net_title, net_status, net_switch;
	GYOBJ bt_title, bt_status, bt_switch;     /* 蓝牙无线电（2026-10-10 新增） */
	GYOBJ theme_title, theme_status, theme_switch;
	GYOBJ lang_title, lang_hint, lang_zh, lang_en;
	/* 半透明效果（2026-10-04 新增）：1 = 半透明（出厂默认） */
	GYOBJ ribbon_title, ribbon_status, ribbon_switch;
	GYOBJ bright_title, bright_slider;
	GYOBJ device_name_input;
	GYOBJ power_button;
	uint32_t lang_version;      /* 上次套用文案时的语言版本（惰性刷新用） */
} AppState;
static AppState state;
/* ---- 诊断量：每帧现算（必须在 state 与 s_scroll 都可见之后） ---- */
/* 现算两个交互控件中心的绝对屏幕坐标（诊断量，见其定义处注释）。
 *
 * ⚠⚠ 为什么挂在 scroll_place_bar 上、而不是 app_show 里算一次就完事
 *   （2026-10-04 板上定位到的真坑）：GetAbsArea 沿 parent 链累加，而
 *   **app_page 自己停在 y=480** —— phone_shell.c 的 on_recent() 每次进
 *   "最近任务"都会 PhoneUI_set_pos(app_page, 0, H) 把 app 页面推到屏幕
 *   下方，下次 open_app 才用 PhoneUI_move 动画移回 0。
 *   而 app_show（原先算诊断量的地方）跑在**动画之前** ⇒ 算出来的 y
 *   凭空多出 480，开关坐标报 (260,865)、点在屏幕外 ⇒ 验收误报"点不到"。
 *   ⚠ scroll_place_bar 也不是每帧跑的（只在布局变时），所以同样会过期。
 *
 * ⇒ 结论：**每帧在 advance() 里现算**，那才是唯一"一定是最���状态"的地方。 */
void Settings_UpdateDiag(void)
{
	if (s_scroll.sw_probe != NULL)
	{
		GYrect r;
		YMGUI_Obj_GetAbsArea(s_scroll.sw_probe, &r);
		g_diag_ribbon_sw_x = r.x + r.w / 2;
		g_diag_ribbon_sw_y = r.y + r.h / 2;
	}
	if (s_scroll.btn_probe != NULL)
	{
		GYrect r;
		YMGUI_Obj_GetAbsArea(s_scroll.btn_probe, &r);
		g_diag_power_btn_x = r.x + r.w / 2;
		g_diag_power_btn_y = r.y + r.h / 2;
	}
	/* 语言两段：脚本要分别点中文段与 English 段，两个都得给。 */
	if (state.lang_zh != NULL)
	{
		GYrect r;
		YMGUI_Obj_GetAbsArea(state.lang_zh, &r);
		g_diag_lang_zh_x = r.x + r.w / 2;
		g_diag_lang_zh_y = r.y + r.h / 2;
	}
	if (state.lang_en != NULL)
	{
		GYrect r;
		YMGUI_Obj_GetAbsArea(state.lang_en, &r);
		g_diag_lang_en_x = r.x + r.w / 2;
		g_diag_lang_en_y = r.y + r.h / 2;
	}
	/* 亮度滑杆：给两端（x0 最左、x1 最右）与纵向中心，脚本照此拖。
	 * ⚠ 滑杆的空白轨道也可点（updateFromPointer 按 rel 比例算），
	 *   所以两端取"区域最边缘"而非拖柄当前位置。 */
	if (state.bright_slider != NULL)
	{
		GYrect r;
		YMGUI_Obj_GetAbsArea(state.bright_slider, &r);
		g_diag_bright_x0 = r.x;
		g_diag_bright_x1 = r.x + r.w;
		g_diag_bright_y  = r.y + r.h / 2;
		/* 诊断：wrapper 命中的是不是滑杆（见 g_diag_last_event_obj）。 */
		if (g_diag_last_event_obj == (uint32_t)(uintptr_t)state.bright_slider)
		{
			g_diag_sld_press += 1u;
			g_diag_last_event_obj = 0u;   /* 只计一次，等下一次命中 */
		}
	}
}


static void on_power(GYOBJ obj);
static void on_settings(GYOBJ btn, uint8 on);
static void brightness_changed(GYOBJ obj, int32 value);
static void wifi_changed(GYOBJ obj, uint8 on);
static void ribbon_changed(GYOBJ obj, uint8 on);
static void settings_retranslate(void);
static void lang_pick_zh(GYOBJ obj);
static void lang_pick_en(GYOBJ obj);

/* ---- 分段选择器的绘制 ----
 * 哪个按钮代表哪种语言，靠 obj 指针比对（两个句柄在 app_create 里建好）。
 * 文字直接用常量："中文" / "English" 在两种语言下写法相同，不必进翻译表。 */
static void lang_seg_draw(GYOBJ obj, GYSURFACE s, const GYrect* a)
{
	uint8 cur = PhoneLang_Get();
	int   on  = (obj == state.lang_zh) ? (cur == PHONE_LANG_ZH)
	                                   : (cur == PHONE_LANG_EN);
	const char* label = (obj == state.lang_zh) ? "中文" : "English";

	if (on)
	{
		/* 选中段：蓝色圆角胶囊。半径取 6 —— 比全高 26 小得多，
		 * 看起来是"圆角块"而不是"药丸"，与卡片内其它元素风格一致。 */
		PhoneUI_rounded(s, a, ACCENT, 6, GY_OPA_COVER);
	}

	/* 居中画字：large=2 是 small 档（与卡片副标题同档）。
	 * 垂直位置 (a->h - 16)/2 让 16 px 字高落在段中央。 */
	PhoneUI_draw_text(s, label, on ? WHITE : ACCENT, 2,
					  a->x + a->w / 2, a->y + (a->h - 16) / 2);
}

/* 本页**创建期设定**的全部文案都集中在这里重设。
 * 语言切换后调用一次即可；绘制期取值的地方（分段高亮、开关状态）会自己读最新值。 */
static void settings_retranslate(void)
{
	PhoneUI_text_set(state.header_title, T("设置"));
	PhoneUI_text_set(state.header_sub,   T("把手机调成喜欢的样子"));
	PhoneUI_text_set(state.net_title,    T("无线网络"));
	PhoneUI_text_set(state.theme_title,  T("海蓝色壁纸"));
	PhoneUI_text_set(state.lang_title,   T("界面语言"));
	PhoneUI_text_set(state.lang_hint,    T("切换后立即生效"));
	PhoneUI_text_set(state.ribbon_title, T("半透明"));
	PhoneUI_text_set(state.ribbon_status,
					 T(PhoneHost_GetRibbonTranslucent() ? "透出" : "实色"));
	PhoneUI_text_set(state.bright_title, T("预览亮度"));
	PhoneUI_button_set(state.power_button, T("关机..."));

	/* 这两个是"跟状态走"的文案，不能写死：按当前开关状态选词后再翻译 */
	PhoneUI_text_set(state.net_status, T(PhoneHost_GetWifi() ? "Yaomi 工作室" : "未连接"));
	PhoneUI_text_set(state.theme_status,
					 YMGUI_Switch_GetOn(state.theme_switch) ? T("海蓝") : T("暮色"));

	state.lang_version = PhoneLang_Version();
}

static void lang_pick_zh(GYOBJ obj)
{
	(void)obj;
	PhoneLang_Set(PHONE_LANG_ZH);     /* 值没变时内部直接返回，不会重复落盘 */
	settings_retranslate();
	PhoneHost_RefreshLanguage();      /* 桌面/最近任务/应用标题等常驻文案 */
	YMGUI_Obj_Invalidate(state.lang_zh);
	YMGUI_Obj_Invalidate(state.lang_en);
}

static void lang_pick_en(GYOBJ obj)
{
	(void)obj;
	PhoneLang_Set(PHONE_LANG_EN);
	settings_retranslate();
	PhoneHost_RefreshLanguage();
	YMGUI_Obj_Invalidate(state.lang_zh);
	YMGUI_Obj_Invalidate(state.lang_en);
}

static void on_power(GYOBJ obj)
{
	(void)obj;
	PhoneHost_Power();
}
static void on_settings(GYOBJ btn, uint8 on)
{
	(void)btn;
	PhoneUI_text_set(state.theme_status, on ? T("海蓝") : T("暮色"));
	PhoneHost_SetTheme(on);
}

/* 亮度滑杆：拖动中只改显示，松手才落盘。
 *
 * 【为什么必须延迟落盘 —— 这是真实的闪存磨损问题，不是洁癖】
 *   YMGUI_Creat_Slider_Creat 的回调在 **Pressed 和每一次 Pressing** 都触发
 *   （YMGUI_Slider.c:101 `if (e == GY_EVENT_Pressed || e == GY_EVENT_Pressing)`），
 *   而 AppConfig_Save 是"值变就擦一个扇区"。板级实测：一次拖动
 *   ⇒ g_cfg_save_cnt +6、g_cfg_erase_last=1 ⇒ **擦了 6 次扇区**。
 *   用户正常调亮度一天几十次 ⇒ 每天几百次擦除；W25Q128 标称
 *   10 万次/扇区 ⇒ 用不到一年就磨光该扇区。
 *
 * 【为什么不能靠库层的"值相同不写"】那只挡住**重复**的值；
 *   拖动经过 5 个不同的中间值就是 5 次不同 ⇒ 照样擦 5 次。
 *
 * 【为什么不能 debounce 定时器】本工程主循环是 tick 驱动、无 RTOS，
 *   加"松手后 500 ms 再写"要多一个状态 + 一个时间戳；而松手事件
 *   （GY_EVENT_Released / ReleasedOff）本来就在 scroll_wrap 里看得见，直接用。
 *
 * 【两段式的关键：拖动中绝不能碰 PhoneHost_SetBrightness】
 *   它"值相同直接 return"，拖动中每步都不同 ⇒ 每步都会走到
 *   s_bright_persist_cb ⇒ 每步都落盘，正是要避免的。
 *   所以拖动中走 PhoneHost_SetBrightnessPreview()：只改内存 + 重绘，
 *   **不碰持久化回调**（库侧新增，与 SetBrightness 语义分离）。
 *   松手时才调 PhoneHost_SetBrightness() 走完整链路，落盘一次。 */
static uint8_t s_bright_dragging = 0u;   /* 1 = 正在拖动（还没松手） */

/* 记录上次见到的 $RD 帧数（2026-10-10）。app_tick 里发现它变了就重刷两个无线电
 * 开关 —— 这样开关只在"对端回报了新状态"时才动，不会和用户的手动拨动打架
 * （拨动后到 $RD 回来的几十毫秒里，开关保持用户意图，再由 $RD 确认/回滚）。 */
static uint32_t s_last_rd_pkts = 0u;

/* 滑杆的 changed 回调 = 拖动中（含按下瞬间）。只预览，不落盘。 */
static void brightness_changed(GYOBJ obj, int32 value)
{
	(void)obj;
	g_diag_sld_change += 1u;
	s_bright_dragging = 1u;
	PhoneHost_SetBrightnessPreview(value);
}

/* 松手时提交：这时才真正生效并落盘。
 * 由 scroll_wrap_event 在转发之前调用。 */
static void brightness_commit(void)
{
	if (s_bright_dragging == 0u)
	{
		return;                 /* 不是这次拖动的收尾，别多写一次 flash */
	}
	s_bright_dragging = 0u;
	g_diag_sld_commit += 1u;
	PhoneHost_SetBrightness(PhoneHost_GetBrightness());
}

static void wifi_changed(GYOBJ obj, uint8 on)
{
	(void)obj;
	/* ★2026-10-10：从"假 WiFi（PhoneHost_SetWifi）"改成真命令 ——
	 * 经 ESP32 上行链路发 $?RADIO,WIFI,ON/OFF，由对端真正开关 WiFi STA。
	 * 开关位置随后由 $RD 回流刷新（见 app_tick 的 s_last_rd_pkts 逻辑），
	 * 不靠本地的"我点了什么"自缓存，避免与实际状态脱钩。 */
	BoardNet_SetWifi(on);
}

static void bt_changed(GYOBJ obj, uint8 on)
{
	(void)obj;
	/* 蓝牙无线电开关：发 $?RADIO,BT,ON/OFF（2026-10-10）。 */
	BoardNet_SetBt(on);
}

/* 半透明开关（2026-10-04 新增）
 *
 * 状态文案跟开关走，不能写死 —— 拨完要立刻反映"现在是透光还是实色"。
 * 关 = 半透明（壁纸渐变透出来，慢 7 ms 左右）；开 = 不透明（快）。
 * 命名上"半透明效果 = 关"看起来反直觉，但这是 Switch 的常规语义：
 * 开关名描述**功能是否开启**，而该功能就是"半透明"本身。
 * 副标题固定写"半透明"、状态词写"透出/实色"就是为消除这种歧义。 */
static void ribbon_changed(GYOBJ obj, uint8 on)
{
	(void)obj;
	PhoneHost_SetRibbonTranslucent(on ? 0 : 1);   /* ★开关开 = 不要半透明 */
	PhoneUI_text_set(state.ribbon_status,
					 T(PhoneHost_GetRibbonTranslucent() ? "透出" : "实色"));
}

/* 每帧刷新诊断量（PhoneApps_Tick 会派发给运行中的应用）。
 * ⚠ 这是**唯一**能让坐标保持正确的时机 —— 见 Settings_UpdateDiag 的注释：
 *   app_page 自己会停在 y=480，任何"布局变化时算一次"的写法都会过期。 */
static void app_tick(uint32 elapsed)
{
	(void)elapsed;
	/* 键盘弹起时把视口缩到键盘上沿，内容才能滚上去不被盖住。
	 * 必须每帧判断：IME 的显隐由 phone_shell 的 advance 驱动，
	 * 本函数是设置页唯一每帧跑的钩子（见 scroll_apply_ime 的注释）。 */
	scroll_apply_ime(PhoneIME_Visible());
	Settings_UpdateDiag();

	/* 对端回报了新的 $RD（无线电状态帧）：重刷两个开关，确认或回滚用户的拨动。
	 * 只在帧数变化时动，避免和手动拨动打架（见 s_last_rd_pkts 注释）。 */
	if (g_net_rd_pkts != s_last_rd_pkts)
	{
		s_last_rd_pkts = g_net_rd_pkts;
		int w = BoardNet_RadioWifiOn();
		YMGUI_Switch_SetOn(state.net_switch, (uint8)(w > 0 ? 1u : 0u));
		int wf = BoardNet_WifiUp();
		PhoneUI_text_set(state.net_status,
						 T(wf > 0 ? "已连接" : (wf < 0 ? "等待同步" : "未连接")));
		int b = BoardNet_RadioBtOn();
		YMGUI_Switch_SetOn(state.bt_switch, (uint8)(b > 0 ? 1u : 0u));
		PhoneUI_text_set(state.bt_status,
						 T(b > 0 ? "已开启" : (b < 0 ? "等待同步" : "已关闭")));
	}
}

/* 把控件状态对齐到宿主的真实值（开关/文案/滑杆）。
 *
 * 【为什么要与 app_show 分开】app_show 兼着"真正进入页面"的职责，
 *   其中包含"每次进入都回到顶部"。而 sync 命令是**页面已经在屏上**时
 *   由宿主发来的（改亮度、改 wifi、改半透明），此时把 scroll_y 复位就是
 *   把用户正在看的页面弹回顶部。
 *   板级实测踩过：拖动亮度滑杆时 Preview → sync → app_show →
 *   scroll_y 135→0、滑杆屏幕 y 330→465，手指还在 330 处 ⇒ "拖不动"。
 *   ⇒ 拆成两个函数：app_show 走 reset_scroll=1，sync 走 reset_scroll=0。 */
static void settings_sync_controls(int reset_scroll)
{
	/* ---- 无线：开关=无线电开/关（来自 $RD），副标题=连接/开启状态 ----
	 * ⚠ 不再用假的 PhoneHost_GetWifi()：WiFi 无线电在对端 ESP32，$RD 才是权威。
	 *   -1（还没收到过 $RD）= 等待同步：开关置关、副标题提示，等 $RD 回来再对齐。 */
	int wifi_on = BoardNet_RadioWifiOn();
	YMGUI_Switch_SetOn(state.net_switch, (uint8)(wifi_on > 0 ? 1u : 0u));
	int wf = BoardNet_WifiUp();
	PhoneUI_text_set(state.net_status, T(wf > 0 ? "已连接" : (wf < 0 ? "等待同步" : "未连接")));

	int bt_on = BoardNet_RadioBtOn();
	YMGUI_Switch_SetOn(state.bt_switch, (uint8)(bt_on > 0 ? 1u : 0u));
	PhoneUI_text_set(state.bt_status, T(bt_on > 0 ? "已开启" : (bt_on < 0 ? "等待同步" : "已关闭")));

	/* 记一下当前 $RD 计数，避免 app_tick 在下一帧立刻又重刷一遍（刚同步过）。 */
	s_last_rd_pkts = g_net_rd_pkts;

	YMGUI_Slider_SetValue(state.bright_slider, PhoneHost_GetBrightness());

	/* 半透明：宿主侧可能被别的路径改过（如 SWD 直接写 g_ribbon_translucent 做实验），
	 * 回到本页时以实际值为准同步一次开关与文案。 */
	YMGUI_Switch_SetOn(state.ribbon_switch,
					   (uint8_t)(PhoneHost_GetRibbonTranslucent() ? 0 : 1));
	PhoneUI_text_set(state.ribbon_status,
					 T(PhoneHost_GetRibbonTranslucent() ? "透出" : "实色"));

	/* 每次进入都回到顶部：否则从底部半截位置切走再切回来，
	 * 用户会看到"设置自己动了一下"。
	 * ⚠ sync 路径必须跳过 —— 见本函数开头的说明。 */
	if (reset_scroll && (s_scroll.view != NULL))
	{
		/* 视口高度复位：上次若在键盘弹起时收缩过（ime_shrunk=1），
		 * 带着缩过的视口进页面会看到"可视区突然少了一截"。
		 * 置 0 后 app_tick 会在下一帧按当前 IME 实际状态重新决定。 */
		s_scroll.ime_shrunk = 0u;
		s_scroll.view->area.h = (GYcoord)SCROLL_VIEW_H;
		s_scroll.view->scroll_y = 0;
		scroll_clamp();
		/* ⚠ scroll_place_bar 顺带把 g_diag_ribbon_sw_* / g_diag_power_btn_*
		 *   现算好（见它的注释）—— 不要在这里另写一份，会和它打架。 */
		scroll_place_bar();
		YMGUI_Obj_Invalidate(s_scroll.view);
	}

	/* 惰性刷新：别的应用里切了语言的话，回到本页时才补刷一次。
	 * 比"每次 show 都全量重设"省事，也避免无谓的文字重排与置脏。 */
	if (state.lang_version != PhoneLang_Version())
	{
		settings_retranslate();
		YMGUI_Obj_Invalidate(state.lang_zh);
		YMGUI_Obj_Invalidate(state.lang_en);
	}
}

static void app_show(const char* argument)
{
	(void)argument;
	settings_sync_controls(1);      /* 真正进页面：复位滚动位置 */
}

static int app_command(const char* name, const char* argument)
{
	if (strcmp(name, "sync"))
		return 0;
	/* ⚠ reset_scroll=0：页面已在屏上，不能动用户的滚动位置。 */
	settings_sync_controls(0);
	return 1;
}

static void app_create(GYOBJ view)
{
	state = (AppState){0};
	s_scroll = (ScrollCtx){0};
	s_wrap_n = 0;

	/* 头部：不用 PhoneUI_app_header —— 它会自己建两个 label 但不返回句柄，
	 * 而语言切换时需要重设这两段文案，所以这里手动建同样位置/字号的 label。
	 *
	 * ⚠ 头部的**高度必须与滚动视口的 y 偏移一致**（SCROLL_HEAD_H）。
	 *   视口是后建的、bg_color 不透明（PAPER）且开了 ClipChildren ——
	 *   它会**盖住**所有建在它之前、又落在它范围内的东西。
	 *   之前视口从 y=0 起、标题也在 y=0/37 ⇒ 标题被完全遮住（板级抓屏实测：
	 *   y=36..99 区域全是卡片的白/淡紫底，一个文字像素都没有）。 */
	state.header_title = PhoneUI_left_label(view, 22, 0, 276, T("设置"), INK, 3);
	state.header_sub   = PhoneUI_left_label(view, 23, 37, 275, T("把手机调成喜欢的样子"), MUTED, 2);

	/* ---- 滚动视口（2026-10-04 新增）----
	 *
	 * 【几何】app 视图区是 (0,36,320,412)（phone_shell.c build_app_views），
	 *   所以可视区在这个局部坐标里就是 (0,0,320,412)。
	 *   头部两个 label 留在 view 上（不跟着滚），卡片全建在 s_scroll.view 里。
	 *   ⇒ 视口从 SCROLL_HEAD_H 起、高 412-SCROLL_HEAD_H，把头部让出来。
	 *
	 * 【为什么自己建而不用 YMGUI_Creat_List_Creat】见文件头"为什么不能直接用 List"。
	 *   这里只借用它的两件核心语义：开 ClipChildren + 用 scroll_y 偏移子对象
	 *   （偏移是引擎行为，YMGUI_Obj.c:290 `ay -= p->parent->scroll_y`，不是我们算的）。
	 *
	 * ⚠ bg_color 必须设成 PAPER：卡片之间若透明，下层壁纸会透出来形成漏底条纹。 */
	s_scroll.view = YMGUI_Creat_Obj_Creat(view, 0, SCROLL_HEAD_H, 320, SCROLL_VIEW_H);
	s_scroll.view->state |= GY_STATE_ClipChildren;
	s_scroll.view->bg_color = PAPER;
	s_scroll.view->event_cb = scroll_view_event;

	/* 滚动条：建在 view 上（与 s_scroll.view 平级，卡片之后创建 ⇒ 画在最上层）。
	 * ⚠ 绝不能建成 s_scroll.view 的子对象 —— 它会跟着 scroll_y 一起滑走，
	 *   就从"指示位置"退化成"跟着内容跑的色条"，失去意义。 */
	s_scroll.bar = YMGUI_Creat_Obj_Creat(view, 0, 0, SCROLL_BAR_W, 40);
	s_scroll.bar->draw_cb = scroll_bar_draw;

	/* ---- 七张卡片：坐标相对 s_scroll.view ----
	 * 内容总高 491 > 视口 412-56=356 ⇒ 可滚动，clamp 上限 = 491-356 = 135。 */

	/* ---- WiFi 无线电（2026-10-10：从假开关改成真命令）----
	 * 开关经 ESP32 上行链路发 $?RADIO,WIFI,ON/OFF，副标题显示连接状态。
	 * 初始 SetOn(0)：进页面时 settings_sync_controls 会用 $RD 对齐真实状态。 */
	GYOBJ net = PhoneUI_panel(s_scroll.view, 18, 65, 284, 50, WHITE);
	state.net_title = PhoneUI_left_label(net, 14, 4, 180, T("WiFi"), INK, 0);
	state.net_status = PhoneUI_left_label(net, 14, 28, 180, T("等待同步"), MUTED, 2);
	state.net_switch = YMGUI_Creat_Switch_Creat(net, 218, 11, 48, 27);
	YMGUI_Switch_SetOn(state.net_switch, 0);
	state.net_switch->draw_cb = PhoneUI_switch_draw;
	YMGUI_Switch_SetChanged(state.net_switch, wifi_changed);

	/* ---- 蓝牙无线电（2026-10-10 新增）----
	 * 与 WiFi 同一套：开关发 $?RADIO,BT,ON/OFF，副标题显示已开启/已关闭。 */
	GYOBJ bt = PhoneUI_panel(s_scroll.view, 18, 133, 284, 50, WHITE);
	state.bt_title = PhoneUI_left_label(bt, 14, 4, 180, T("蓝牙"), INK, 0);
	state.bt_status = PhoneUI_left_label(bt, 14, 28, 180, T("等待同步"), MUTED, 2);
	state.bt_switch = YMGUI_Creat_Switch_Creat(bt, 218, 11, 48, 27);
	YMGUI_Switch_SetOn(state.bt_switch, 0);
	state.bt_switch->draw_cb = PhoneUI_switch_draw;
	YMGUI_Switch_SetChanged(state.bt_switch, bt_changed);

	GYOBJ theme = PhoneUI_panel(s_scroll.view, 18, 201, 284, 50, WHITE);
	state.theme_title = PhoneUI_left_label(theme, 14, 4, 180, T("海蓝色壁纸"), INK, 0);
	state.theme_status = PhoneUI_left_label(theme, 14, 28, 180, T("暮色"), MUTED, 2);
	state.theme_switch = YMGUI_Creat_Switch_Creat(theme, 218, 11, 48, 27);
	state.theme_switch->draw_cb = PhoneUI_switch_draw;
	YMGUI_Switch_SetChanged(state.theme_switch, on_settings);

	/* ---- 界面语言：分段控件靠右 ----
	 * 左段"中文"40 px、右段"English"58 px、间隔 2 px，合计 100 px，
	 * 右对齐到卡片内边距 14 px ⇒ 起点 x = 284 - 14 - 100 = 170。 */
	GYOBJ lang = PhoneUI_panel(s_scroll.view, 18, 269, 284, 50, WHITE);
	state.lang_title = PhoneUI_left_label(lang, 14, 4, 120, T("界面语言"), INK, 0);
	state.lang_hint  = PhoneUI_left_label(lang, 14, 28, 120, T("切换后立即生效"), MUTED, 2);
	state.lang_zh = PhoneUI_button(lang, 170, 12, 40, 26, "", lang_pick_zh, 0);
	state.lang_en = PhoneUI_button(lang, 212, 12, 58, 26, "", lang_pick_en, 0);
	state.lang_zh->draw_cb = lang_seg_draw;
	state.lang_en->draw_cb = lang_seg_draw;

	/* ---- 半透明效果（2026-10-04 新增，用户定：默认开 = 半透明）----
	 * 标题写功能名"半透明"、副标题写当前效果"透出/实色"，
	 * 消除"开关叫半透明、打开却是不透明"的歧义。 */
	GYOBJ rib = PhoneUI_panel(s_scroll.view, 18, 337, 284, 50, WHITE);
	state.ribbon_title  = PhoneUI_left_label(rib, 14, 4, 120, T("半透明"), INK, 0);
	state.ribbon_status = PhoneUI_left_label(rib, 14, 28, 120, T("透出"), MUTED, 2);
	state.ribbon_switch = YMGUI_Creat_Switch_Creat(rib, 218, 11, 48, 27);
	/* ★开关语义取反：开关"关"才要半透明（因为开关名就是"半透明"这个功能）。
	 * 这里传的是"不要半透明"，ribbon_changed 里再做一次同样的取反。 */
	YMGUI_Switch_SetOn(state.ribbon_switch,
					   (uint8_t)(PhoneHost_GetRibbonTranslucent() ? 0 : 1));
	state.ribbon_switch->draw_cb = PhoneUI_switch_draw;
	YMGUI_Switch_SetChanged(state.ribbon_switch, ribbon_changed);

	GYOBJ bright = PhoneUI_panel(s_scroll.view, 18, 397, 284, 62, WHITE);
	state.bright_title = PhoneUI_left_label(bright, 14, 4, 244, T("预览亮度"), INK, 0);
	state.bright_slider = YMGUI_Creat_Slider_Creat(bright, 14, 32, 256, 24);
	/* 范围与"出厂默认 50"配套：下限取 1 而不是 0 —— brightness=0 会让
	 * phone_flush 叠一层 alpha=160 的全屏黑罩，屏上什么都看不见，
	 * 与"屏坏了"无法区分。config 侧的钳制也用同一条下界。 */
	YMGUI_Slider_SetRange(state.bright_slider, 1, 100);
	/* 初值取**实际**亮度而不是写死 100：app_create 可能先于
	 * main.c 的配置应用跑（顺序反了也不该画出错的一帧），
	 * 且 app_show 每次进页面会再同步一次真正的值。 */
	YMGUI_Slider_SetValue(state.bright_slider, PhoneHost_GetBrightness());
	state.bright_slider->draw_cb = PhoneUI_slider_draw;
	YMGUI_Slider_SetChanged(state.bright_slider, brightness_changed);

	state.device_name_input = YMGUI_Creat_TextInput_Creat(s_scroll.view, 24, 475, 272, 28, 95);
	state.device_name_input->draw_cb = PhoneUI_phone_input_draw;
	YMGUI_TextInput_SetText(state.device_name_input, "Yaomi Phone");

	state.power_button = PhoneUI_button(s_scroll.view, 18, 519, 284, 40, T("关机..."),
										 on_power, RGB(198, 91, 114));

	/* 内容总高 = 最后一个元素底边（关机按钮 519+40 = 559）。
	 * ⚠ 以后再加卡片必须同步改这里，否则最后一项被裁掉且看不出原因。
	 *   2026-10-10 新增"蓝牙"卡片并把后续卡片下移 68px（蓝牙卡 50 + 间隔 18）。 */
	s_scroll.content_h = 519 + 40;
	g_diag_scroll_content = s_scroll.content_h;
	/* 诊断量用的探针指针必须在 scroll_place_bar 之前设好 ——
	 * 它在函数里就现算这两个控件的绝对坐标。 */
	s_scroll.sw_probe  = state.ribbon_switch;
	s_scroll.btn_probe = state.power_button;
	/* 输入框探针 + 它在内容坐标里的底边（IME 弹起时要滚进视野用）。
	 * ⚠ 底边在**布局期**算：那时 IME 还没改过 area，读到的是原始几何。 */
	s_scroll.input_probe  = state.device_name_input;
	s_scroll.input_bottom = state.device_name_input->area.y
	                      + state.device_name_input->area.h;
	/* 告诉 IME：本页的键盘避让由容器自己管（视口收缩 + 滚焦点输入框），
	 * 别再用那段会挪错位置的避让逻辑 —— 详见 phone_ime.c 的说明。 */
	PhoneIME_RegisterSelfAvoiding(s_scroll.view);
	scroll_clamp();
	scroll_place_bar();
	/* ⚠ 用 SCROLL_VIEW_H 而不是魔数 412 —— 视口高度是"视图区高减头部"，
	 *   改头部高度时这里必须跟着变，写死会在头变矮/变高时判断错。 */
	YMGUI_Obj_SetHidden(s_scroll.bar, (s_scroll.content_h <= SCROLL_VIEW_H) ? 1 : 0);

	/* ---- 给所有可交互控件包一层拖动转发 ----
	 * 【2026-10-04 改为递归】原先只手工列了 8 个交互控件，结果"按在卡片
	 * 空白处拖不动"—— 命中最深的是 panel/label，它们的 event_cb 是 NULL，
	 * sendEvent 直接丢弃（详见 scroll_wrap_tree 的注释）。
	 * 现在递归给整棵子树装满，Switch 的 is_sw 标记改由下面三行单独补。 */
	scroll_wrap_tree(s_scroll.view);
	/* Switch 需要"回滚"标记，而库里没有 GY_OBJ_Switch 类型可判
	 * （YMGUI_Obj.h:17 只有 GY_OBJ_Button，Switch 复用通用 Obj），
	 * 故按对象指针反查索引补标。 */
	{
		static GYOBJ sw_mark[4];
		int i, k;
		sw_mark[0] = state.net_switch;
		sw_mark[1] = state.theme_switch;
		sw_mark[2] = state.ribbon_switch;
		sw_mark[3] = state.bt_switch;
		for (i = 0; i < 4; ++i)
		{
			for (k = 0; k < s_wrap_n; ++k)
			{
				if (s_wrap_obj[k] == sw_mark[i])
				{
					s_wrap_is_sw[k] = 1u;
					break;
				}
			}
		}
	}

	state.lang_version = PhoneLang_Version();
}

static void app_destroy(void)
{
	memset(&state, 0, sizeof(state));
	/* 滚动上下文与 wrapper 表也要清：app 会被销毁重建（切走再切回），
	 * s_wrap_n 不清的话，第二次进来时 scroll_wrap 会把新控件登记到旧行号上，
	 * 事件被转发给上一个 app 的控件 —— 表现为"点设置页没反应"。 */
	memset(&s_scroll, 0, sizeof(s_scroll));
	s_wrap_n = 0;
}
static intptr_t app_inspect(const char* name, int index)
{
	(void)index;
	if (!strcmp(name, "theme_switch"))
		return (intptr_t)state.theme_switch;
	if (!strcmp(name, "wifi_switch"))
		return (intptr_t)state.net_switch;
	if (!strcmp(name, "brightness_slider"))
		return (intptr_t)state.bright_slider;
	if (!strcmp(name, "device_name_input"))
		return (intptr_t)state.device_name_input;
	/* 关机按钮：自检要用它点开关机面板。
	 * ⚠ 2026-10-04 补记：这一条以前**漏了**，而自检里已经在按名字取它
	 *   ⇒ PhoneApps_Inspect 返回 0 ⇒ 调用方拿 NULL 去 GetAbsArea 直接崩。
	 *   教训：自检里出现的每个 inspect 名字都必须在这里有对应条目，
	 *   改布局时把两边一起对。 */
	if (!strcmp(name, "power_button"))
		return (intptr_t)state.power_button;
	if (!strcmp(name, "wifi_status"))
		return (intptr_t)state.net_status;
	/* 蓝牙无线电卡（2026-10-10 新增）：供回归测试点击开关、
	 * 断言副标题状态。inspect 名字必须和自检脚本里取的名字一致。 */
	if (!strcmp(name, "bt_switch"))
		return (intptr_t)state.bt_switch;
	if (!strcmp(name, "bt_status"))
		return (intptr_t)state.bt_status;
	if (!strcmp(name, "settings_status"))
		return (intptr_t)state.theme_status;
	/* 新增：语言卡片，供回归测试点击与断言 */
	if (!strcmp(name, "lang_zh"))
		return (intptr_t)state.lang_zh;
	if (!strcmp(name, "lang_en"))
		return (intptr_t)state.lang_en;
	if (!strcmp(name, "lang_title"))
		return (intptr_t)state.lang_title;
	/* 半透明卡片（2026-10-04）*/
	if (!strcmp(name, "ribbon_switch"))
		return (intptr_t)state.ribbon_switch;
	if (!strcmp(name, "ribbon_title"))
		return (intptr_t)state.ribbon_title;
	/* 滚动容器（供回归脚本驱动滚动、断言可滚范围）*/
	if (!strcmp(name, "scroll_view"))
		return (intptr_t)s_scroll.view;
	return 0;
}

const PhoneApp PhoneApp_settings = {
	.key = "settings", .title = "设置", .color = RGB(102, 125, 158), .icon = ICON_SETTINGS, .create = app_create, .inspect = app_inspect, .show = app_show, .tick = app_tick, .command = app_command, .destroy = app_destroy};
