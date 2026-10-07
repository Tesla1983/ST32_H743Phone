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
