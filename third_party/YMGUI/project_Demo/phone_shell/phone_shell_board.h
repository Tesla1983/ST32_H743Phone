#ifndef PHONE_SHELL_BOARD_H
#define PHONE_SHELL_BOARD_H

#include "YMGUI_Hal.h"

/* ===========================================================================
 * phone_shell 的板级入口（阶段 5）
 *
 * 为什么需要单独一组入口：宿主版的 `main()` 依赖 SDL_LCD_*（窗口/事件/延时）
 * 与 stdio（getenv/atoi/fopen），板上都没有。做成独立入口后
 *   - 应用逻辑**一行都不改**（桌面验证过的行为板上一致）；
 *   - 板级主循环自己掌握时基与刷新节奏（DWT 计时 + 触摸轮询）。
 *
 * 定义在 phone_shell.c 里、由 `PHONE_SHELL_BOARD` 宏开启；宿主构建不定义该宏，
 * 于是 `main()` 照旧、这些入口不存在，两边互不干扰。
 * =========================================================================== */

/* 绑定板级 ctx 与 GYdisp，并完成应用层初始化（校验应用表、挂中文字体、
 * 注册快捷开关、build_ui()，其中 build_ui 末尾会 begin_boot() 进入开机动画）。
 * 返回 0 成功；-1 参数非法，-2 应用表校验失败，-3 快捷开关注册失败。 */
int PhoneShell_BoardInit(GYDISP disp, GYCTX board_ctx);

/* 每个主循环拍调一次：内部等价于 advance(elapsed_ms) + YMGUI_Refresh(ctx)。 */
void PhoneShell_BoardTick(uint32 elapsed_ms);

/* 取内部 ctx（板级做显示自检时需要）。 */
GYCTX PhoneShell_BoardCtx(void);

/* 挂"半透明开关"的持久化回调（2026-10-04 新增）。
 * phone_shell 在**开关真的变化**时回调一次，参数 1=半透明 / 0=不透明。
 * 由板级 main.c 接到 AppConfig_Save —— shell 层不认识 app_config，
 * 与 PhoneLang_SetPersistCallback 是同一套分层套路。
 * ⚠ 必须在 PhoneShell_BoardInit **之前**挂好，否则用户在设置页拨动开关无处落盘。 */
void PhoneShell_SetRibbonPersistCallback(void (*cb)(uint8_t translucent));

/* 挂"屏幕亮度"的持久化回调（2026-10-04 新增，与上一条同构）。
 * phone_shell 在 PhoneHost_SetBrightness 收到**与当前不同**的值时回调一次，
 * 参数是 0..100 的亮度值（不是布尔）。"值相同不回调"这条很关键：
 * 开机时 main.c 会用配置里的值调一次 SetBrightness 同步首帧，
 * 无条件回调会导致每次开机都擦一次 flash。
 * ⚠ 同样必须在 PhoneShell_BoardInit **之前**挂好。 */
void PhoneShell_SetBrightnessPersistCallback(void (*cb)(uint8_t brightness));

/* 设置屏幕亮度（0..100，越界自动钳位；**值相同则什么都不做**）。
 * 由 src/main.c 在 PhoneShell_BoardInit 之前用配置里的值调一次，好让首帧
 * 就用对亮度（晚一步会先画一帧错的再纠正，用户看得见闪一下）。
 * ⚠ 允许在 ctx 还没建立时调用 —— 内部有 NULL 守卫，此时只记值不重绘。 */
void PhoneHost_SetBrightness(int brightness);

/* ---- 图库（2026-10-08）：相册 app 经这一组接口取 W25Q128 上的图片 ----
 * 为什么要绕一层 board 接口，而不是让 gallery.c 直接 include src/img_store.h：
 *   phone_shell 是 third_party 里的官方 Demo，**不能依赖本工程的 src/**；
 *   已有的亮度/壁纸持久化也是同样的做法（回调 + board 层）。
 * 返回的数据指针指向 **QSPI 的 XIP 窗口**（0x9000_0000 起），是只读的，
 * 对应 YMGUI_DrawImg.h 里 `const GYpx* data` —— 图像不占任何 RAM。
 * ⚠ 退出 memory-mapped 期间（图库导入中）这些**都不能调用**，
 *   见 src/img_store.h 顶部关于 XIP 冲突的说明。 */
int         BoardGallery_Count(void);                       /* 有效图片数 */
int         BoardGallery_Info(int slot, unsigned int *off, unsigned int *len,
                              unsigned int *w, unsigned int *h);
const void *BoardGallery_Pixels(int slot);                  /* NULL = 该槽位无图 */

/* 删除第 slot 张（2026-10-11）。
 * 返回：0 = 成功；1 = 图库忙（导入中，拒绝）；2 = 槽位非法；3 = flash 写失败。
 * next_valid：成功时写入"下一张该显示的槽位"，图库删空了则写 -1（可传 NULL）。
 *
 * ⚠ UI 必须改用返回值里的 next_valid 当新下标，不能沿用旧下标：
 *   槽位索引是**物理槽号**不是"第几张"，删掉中间一张后会出现空洞，
 *   继续按旧下标取图会取到空槽（画面空白）。槽位总数是图库内部实现，
 *   这一层负责"找下一张"，UI 只管显示。 */
int         BoardGallery_Delete(int slot, int *next_valid);

/* 从 from 开始（含 from，回绕一圈）找下一个**有效**槽位；没有返回 -1。
 * ⚠ "下一张"必须用它，不能用 (index+1) % total：槽位号是物理槽号、不是"第几张"，
 *   一旦有人删过中间的图，索引就会出现空洞，取模算法会落在空槽上（画面空白）。 */
int         BoardGallery_Next(int from);

/* ---- 文件管理器：照片 / 笔记分类的真实文件（2026-10-09）----
 * 同一套分层（不让 phone_shell 认识 FatFs）。
 *
 * ⚠ **调用顺序有约束**：BoardPic_Scan() 会真正去读 TF 卡目录（毫秒级），
 *   所以**只在进入分类时调一次**；之后 Count / Name / Size 都读缓存表、不碰卡。
 *   表只有一份、照片与笔记共用 —— 切分类必须重新 Scan，索引不能跨分类混用。
 *
 * 目录固定为 /YMGUI/PIC（.bmp）与 /YMGUI/NOTE（.txt），Scan 内部会 f_mkdir
 * 保证目录存在，所以空卡第一次进分类也能得到 0 而不是失败。
 * 诊断量 g_scan_n / g_scan_cyc / g_scan_fs_rc 可直接 SWD 读。 */
int  BoardPic_Scan(void);                                   /* 扫一次，返回条目数；<0 = 目录打不开 */
int  BoardPic_Count(void);                                  /* 上次扫描的条目数 */
int  BoardPic_Name(int i, char* out, int n);                /* 文件名（不含目录），i 越界返回 -1 */
int  BoardPic_Size(int i, uint32_t* size);                  /* 字节数 */
int  BoardPic_Import(int i);                                /* 异步导入到图库：0=已排队，<0=没排上 */
int  BoardPic_Busy(void);                                   /* 1 = 正在导入（导入中不要重复点） */
int  BoardPic_LastRc(void);                                 /* 上一次导入的返回码（0 = 成功） */

/* ---- 笔记分类：/YMGUI/NOTE 下的 .txt ----
 * ⚠ **文件名一律用 ASCII**（note0.txt / note1.txt …），不拿标题当文件名：
 *   卡是 FAT32 + FF_CODE_PAGE=936，中文文件名要过 CP936 转码，写进去再读回来
 *   未必逐字节一致；而笔记正文是 UTF-8 原样读写，不受影响。
 *   ⇒ 标题存正文第一行，文件名只当槽位 id。 */
int  BoardNote_Scan(void);                                  /* 扫一次，返回条目数 */
int  BoardNote_Count(void);
int  BoardNote_Name(int i, char* out, int n);
int  BoardNote_Size(int i, uint32_t* size);
/* 读第 i 条正文到 out（NUL 结尾），返回读到的字节数；<0 = 失败 */
int  BoardNote_Load(int i, char* out, int n);
/* 写正文：i >= 0 覆盖第 i 条，i < 0 新建一个文件。返回写入后的索引；<0 = 失败 */
int  BoardNote_Save(int i, const char* text);

/* ---- 蓝牙收文件（P5，2026-10-10）----
 * 链路：手机 --(经典蓝牙 SPP)--> ESP32 --(SPI 从机)--> STM32 --(FatFs)--> TF 卡
 *                                                       --> 图库导入 --> 相册可见
 *
 * 这一层存在的理由与 BoardPic_* 完全一样：phone_shell 不许 include src/bt_recv.h。
 * 实现是纯转发（src/bt_recv.c 末尾），逻辑都在 src/bt_recv.c 上半部分。
 *
 * ⚠ **文件的落地与"进没进相册"是两件事**：收完先写 /YMGUI/PIC 下的 .bmp，
 *   然后由 img_store 异步导入图库；导入期间读 XIP 会触发 BusFault，
 *   所以导入是异步的（见 src/img_store.h）。UI 里"接收完成"只代表写盘成功。
 * ⚠ 收的是什么文件、多大，由**对端**在 OPEN 阶段告知（手机发的文件头），
 *   不是本板决定的 —— 所以 BoardBt_Total() 在拿到文件头之前是 0，
 *   这时进度条要画"不定长"态，别拿 0/0 算百分比。 */
int         BoardBt_Start(void);      /* 开始接收：开 BT 无线 + 起 SPP 服务端 + 开会话。
                                       * 0 = 已发起；-1 = 已有会话在跑；-2 = 链路无对端 */
int         BoardBt_Abort(void);      /* 中止并删掉半截文件；0 = 已中止，-1 = 本来没有会话 */
int         BoardBt_Busy(void);       /* 1 = 会话在跑（UI 该显示进度条而不是"开始"按钮）*/
int         BoardBt_State(void);      /* BT_RECV_IDLE/WAIT/RECV/DONE/ERROR = 0/1/2/3/4 */
int         BoardBt_Progress(void);   /* 0..100；**-1 = 总长还未知**（画不定长态）*/
uint32_t    BoardBt_Bytes(void);      /* 已写进 TF 的字节数 */
uint32_t    BoardBt_Total(void);      /* 对端声明的总字节数；0 = 未知 */
const char* BoardBt_Name(void);       /* 落地的文件名（不含目录）；无 = "" */
int         BoardBt_LastRc(void);     /* 最近一次会话结果码：0 = 成功，<0 = 失败阶段 */
const char* BoardBt_StatusText(void); /* 可直接画的一行中文状态 */

/* ---- ESP32 上行链路：NTP 时间 + 天气（2026-10-08）----
 * 声明实际在 src/uart_link.h（那里有完整的协议与踩坑说明），这里只是转一手，
 * 让 phone_shell 不用直接 include src 下的头。
 * 与图库同一套分层：UI 只拿"能直接画的字符串"，温度×10 → "20℃"、wday → "星期四"
 * 这类换算全部在 src/uart_link.c 里。
 *
 * ⚠ 只在板级构建里引：uart_link.h 在 src/ 下，宿主（SDL 桌面）构建**没有**这个头，
 *   无条件 include 会直接编不过。与 phone_ime.c 里那处同款条件编译。 */
#if defined(PHONE_SHELL_BOARD)
#include "uart_link.h"
#endif

/* 读回当前亮度（0..100）。本来就存在于 phone_host.h，但本工程的 board 侧
 * （src/main.c）只包含本头，而 main 需要**每拍**读它去换算背光 PWM 占空比
 * （拖动滑杆时走的是 Preview 路径、不触发落盘回调，所以不能靠回调拿值）。
 * ⚠ 不要为了这一个声明把 phone_host.h 整个引进来：那会把 APP 层依赖也带进
 *   board 侧，而本工程刻意让 board 只依赖 phone_shell_board.h 这一层。 */
int PhoneHost_GetBrightness(void);

/* 统计当前 UI 的**可见对象数**（不含 Hidden 的），并把最大树深写进 *out_max_depth。
 * 用途：每 band 的固定开销 ∝ 对象数 × 树深，数出来才知道裁剪/减 band 能省多少。 */
int PhoneShell_BoardObjStats(int* out_max_depth);

/* ---- 输入法自测（SWD 触发，无需碰屏）----
 * 板上没有物理键盘也没有 stdio，"看着屏幕上有没有候选"不算可靠判据。
 * 这里把「打开笔记 → 聚焦输入框 → 逐字母敲软键盘」做成可由 SWD 触发的状态机，
 * 敲的每一个字母都走手指点键那个真实的 action() 处理器。
 *
 *   g_ime_test = 1   启动；跑完（成功或失败）自动清 0
 *   g_ime_step       0 未启动 / 1 已发打开 / 2 等开场动画 / 3 已聚焦 / 4 已输入 / 5 已上屏 / 6 结束
 *   g_ime_rc         0 = 成功；-1 找不到 notes；-2 打开失败；
 *                    -3 树里没有可用输入框；-4 拼音字母没被全部接收；
 *                    -5 词典没给出候选；-6 上屏失败
 *   g_ime_pinyin[16] 待验证的拼音串（SWD 先写串再置 g_ime_test = 1，省得每次换词重烧 flash）
 * 结果读 phone_ime.h 里的 PhoneIME_BoardCand[] / PhoneIME_BoardCandCount（候选）
 * 与 PhoneIME_BoardText（上屏后目标输入框的完整文本）。 */
extern volatile int g_ime_test;
extern volatile int g_ime_step;
extern volatile int g_ime_rc;
extern volatile char g_ime_pinyin[16];

/* ---- 输入法按键耗时台架（2026-10-07，方案 B 验收用）----
 * 目的：把"每次按键要多久"从估算变成板上实测。方案 B 把词组词典搬去外部 flash
 * 之后，`imeCandidatesReset()` 里多了一段 XIP 顺序读，这笔钱必须量出来。
 *
 * 只在 YMGUI_XIP_BENCH=ON 的构建里存在（与 case 20~30 同一口径，不进出货构建）。
 *
 *   g_ime_bench_py[32]  待测拼音串（SWD 先写串，再置 g_ime_bench_cmd = 1）
 *   g_ime_bench_cmd     1 = 跑一次；跑完固件清 0
 *   g_ime_bench_cyc_r   imeCandidatesReset() 的周期数（建候选索引：扫词典）
 *   g_ime_bench_cyc_f   extendPrefixCandidates(6) 的周期数（惰性取满 6 个候选）
 *   g_ime_bench_words   这次一共产生了多少个候选
 * 候选串照旧落在 PhoneIME_BoardCand[] / PhoneIME_BoardCandCount。
 *
 * 【为什么拆成两段】reset 是"扫词典 + 组合 DP"，fill 是"惰性从游标里取词"。
 * 前者随拼音长度线性放大（每多一个字母多扫一次桶），后者基本恒定 —— 拆开
 * 才知道该优化哪一段。 */
#if defined(YMGUI_XIP_BENCH)
extern volatile char     g_ime_bench_py[32];
extern volatile uint32_t g_ime_bench_cmd;
extern volatile uint32_t g_ime_bench_cyc_r;
extern volatile uint32_t g_ime_bench_cyc_f;
extern volatile uint32_t g_ime_bench_words;
void PhoneIME_BoardBenchPoll(void);
#endif

/* DWT_CYCCNT 快照。phone_ime.c 不直接碰 CMSIS 寄存器，走板级这一层。
 * ⚠ 调用前必须已开 DWT 的 CYCCNT（main.c 的 board_init 里做的）。 */
uint32_t BoardCycNow(void);

#endif /* PHONE_SHELL_BOARD_H */
