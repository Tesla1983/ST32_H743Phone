/* ===========================================================================
 * YMGUI 移植工程 —— 主程序（阶段 2：显示 flush_cb 打通）
 *
 * 本版做三件事：
 *   1. 板级初始化（时钟 / MPU / LCD，同阶段 1）；
 *   2. 初始化 YMGUI 显示端口（src/ymgui_port.c 挂 flush_cb），建一个最小界面；
 *   3. 主循环里 YMGUI_Refresh，把 YMGUI 渲染结果推到 ST7796。
 *
 * 验收看三个 SWD 可读量：
 *   g_port_rc   = 0    YMGUI 显示端口初始化成功（draw buffer 申请到了）
 *   g_ctx_rc    = 1    上下文与控件创建成功
 *   g_flush_calls / g_frame_seq 在涨   ⇒ flush_cb 真的在被调用、整帧真的在刷
 * 最终再抓全帧镜像缓冲还原 PNG 目视核对（tools/grab_ymgui.py）。
 *
 * 顺序上的硬约束：
 *   1. MPU 必须在**任何 FMC 访问之前**配置（计划书 §3.5）；
 *   2. FMC 区的 MPU 属性必须是强序 TEX=0/C=0/B=0（计划书 §0.4 红线 1）。
 * =========================================================================== */

#include "stm32h7xx_hal.h"

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/delay/delay.h"
#include "./SYSTEM/usart/usart.h"
#include "./BSP/LED/led.h"
#include "./BSP/MPU/mpu.h"
#include "./BSP/LCD/lcd.h"

#include "YMGUI_Hal.h"
#include "YMGUI_Obj.h"
#include "YMGUI_Invalidate.h"
#include "YMGUI_Label.h"
#include "YMGUI_Button.h"
#include "YMGUI_DrawFill.h"
#include "YMGUI_DrawImg.h"
#include "YMGUI_Font.h"
#include "YMGUI_DrawArc.h"
#include "phone_font.h"

#include "ymgui_port.h"
#include "touch_port.h"

/* 32 位"可能别名"类型 gy_u32_alias_t：用来往 GYpx(uint16) 缓冲里做 32 位双像素写，
 * 避开严格别名规则（strict aliasing）。
 * ⚠ 2026-10-04：该 typedef 已**归口到 YMGUI_DrawPx.h**（引擎侧 hspan 也要用），
 *   这里改成直接引那个头，不要再在本文件里重复定义一遍 ——
 *   同一 TU 里 typedef 重复声明在 C99 是错误，两处各写一份迟早会撞上。 */
#include "YMGUI_DrawPx.h"
#include "qspi_port.h"
#include "qspi_provision.h"
#include "font_provision.h"
#include "phone_locale.h"
#include "phone_lang.h"
#include "phone_shell_board.h"
#include "dma_bench.h"
#include "sd_card.h"
#include "fatfs_port.h"
#include "img_store.h"
#include "uart_link.h"
#include "rtc_clock.h"
#include "spi_link.h"
#include "bt_recv.h"
#include "app_config.h"

/* ---- 供 SWD 直接读的指示量（地址用 arm-none-eabi-nm 查）----
 * ⚠ g_boot_magic 必须在 main 里**写一次**（见 main 开头），不能用初始化器赋常量：
 *   本工程开了 `-fdata-sections` + `--gc-sections`，而它只在初始化器里出现、
 *   运行时代码从不引用 → 被判定为不可达段直接删掉（实测 nm 查不到符号、
 *   读回来是空；加 __attribute__((retain)) 在本机 ld 上也无效）。
 *   运行时写一次即让 main 引用它，段自然保留；语义也更准（= main 已进入）。 */
volatile uint32_t g_boot_magic = 0;
volatile uint32_t g_loop_count = 0;
volatile uint32_t g_lcd_id     = 0;
volatile int32_t  g_port_rc    = 99;          /* ymgui_port_init 的返回值，0 = 成功 */
volatile int32_t  g_ctx_rc     = 0;           /* 1 = 上下文创建成功 */
volatile int32_t  g_shell_rc   = 99;          /* PhoneShell_BoardInit 的返回值，0 = 成功 */
volatile uint32_t g_tick_ms    = 0;           /* 上一拍实测经过的毫秒数 */
volatile uint32_t g_dwt_hz     = 0;           /* DWT 计数频率（= CPU 主频），用于核对时基 */

/* CYCCNT 快照：给不直接包含 CMSIS 头的模块（如 phone_ime.c 的按键耗时台架）用。 */
uint32_t BoardCycNow(void)
{
	return DWT->CYCCNT;
}
/* 循环内打点：用于定位主循环卡在哪一步（0=没到，>0=已到） */
volatile uint32_t g_mark_tick  = 0;           /* 已注入 Tick */
volatile uint32_t g_mark_poll  = 0;           /* 已跑完 touch_port_poll */
volatile uint32_t g_mark_refr  = 0;           /* 已跑完 YMGUI_Refresh */
volatile uint32_t g_mark_qspi  = 0;           /* 已跑完 QSPI 初始化与阶段 4 验收 */
/* 时钟初始化结果（2026-10-04 新增，配合 sys.c 里的 g_clock_* 使用）。
 * 实测教训：VOSRDY 不就绪时，厂商代码在 sys.c 里**无超时死等**，固件静默挂死，
 * 外部只看得到 g_loop_count 停在 0，与"屏坏了"完全无法区分。改为超时后，
 * 至少 g_clock_rc 一定非 0，直接指出是时钟这一步。 */
volatile int32_t  g_clock_rc   = -1;          /* sys_stm32_clock_init 返回值，0 = 成功 */
volatile uint32_t g_clock_stage = 0xFFFFFFFFu;/* 复制一份 sys.c 的阶段号，便于在 main 侧读 */
volatile uint32_t g_clock_vos   = 0xFFFFFFFFu;/* PWR_D3CR 快照：bit15:14=目标档位, bit13=VOSRDY */
/* SWD 置 1 → 下一拍重跑 ramp + gram 双自检（阶段 6 的 A2："跑了很多帧之后复测"）。
 * 完成后自动清 0；结果看 g_ramp_mismatch / g_gram_mismatch。 */
volatile uint32_t g_gram_recheck = 0;
/* SWD 置 1 → 下一拍测一次整屏 flush 净耗时（阶段 6 的 A1 判法）。
 * 完成后自动清 0；结果看 g_flush_cyc / g_flush_nspx。 */
volatile uint32_t g_flush_measure = 0;
volatile uint32_t g_refr_cyc      = 0;   /* 上一拍 PhoneShell_BoardTick 的 DWT 周期数（应用层帧成本） */

/* ---- FMC 写时序扫描钩子（2026-10-04 步骤 3）----------------------------------
 *
 * 【要解决的问题】A1 记的"整屏 flush = 7.68 ms ≈ 50.0 ns/px，已打到总线地板"
 * 只对**当前寄存器设置**成立。一次 16 位写的成本是
 *     (ADDSET + DATAST + 1) / fmc_ker_ck      （本板 fmc_ker_ck = PLL2_R = 220MHz）
 * 5/5 ⇒ 11 周期 ⇒ 50.0 ns，与实测 50.0 ns/px 完全吻合。但面板规格书只约束
 * **写脉宽 tWRL ≥ 19ns**，那是 DATAST 的事（≥4 周期 = 22.7ns 即可）；
 * ADDSET 只决定 CS/RS 建立时间，从来没人单独扫过它。
 * 也就是说 50 ns/px 是"当初那一轮七档整体扫描选中的点"，不是硬件极限。
 *
 * 【怎么用】SWD 写 g_lcd_fmc_addset / g_lcd_fmc_datast（定义在 BSP/LCD/lcd.c），
 * 再写 1 到本变量：它会 apply 一次时序**并立刻重测整屏 flush**，
 * 结果就在 g_flush_cyc / g_flush_nspx。一次写入拿到一个档位的数字，
 * 所以扫描脚本就是"写档位 → 写 1 → 读 g_flush_cyc"三步的循环，
 * **不必重新编译、不必复位**。
 *
 * 【安全】换档后画面可能错乱（太快就丢写）—— 那是**预期**的实验现象。
 * 定档后必须复跑 A2 双自检：g_ramp_mismatch 与 g_gram_mismatch 都要为 0，
 * 且要跑够几十帧再复测一次。
 * 【参考值】ADDSET=1/DATAST=4 ⇒ 6 周期 ⇒ 27.3 ns/px ⇒ 整屏约 4.19 ms（省 45%）。 */
volatile uint32_t g_fmc_timing_sweep = 0;
/* 上一次真正写进寄存器的档位（回读用，防止脚本看花了眼） */
volatile uint32_t g_fmc_applied_addset = 0;
volatile uint32_t g_fmc_applied_datast = 0;

/* 满负载端到端帧基准（2026-10-03 新增）。
 * 【为什么需要它】上面几个量各有盲区：
 *   g_flush_cyc      —— 只量裸写循环，不含渲染、窗口命令、镜像；
 *   g_flush_cb_cyc   —— 含窗口命令与镜像，但只覆盖 flush 那一半，不含渲染；
 *   g_refr_cyc       —— 静止时脏区为空，Refresh 直接返回，量出来的 ~400 cycles 没有意义。
 * 只看任何一个都会得出偏差结论（静止时看着上百帧、一动就掉到十几帧）。
 * 这里**强制每帧把整屏标脏**，让「渲染 + 切 band + 重绘 + flush」全部走满，
 * 得到的才是 GUI 在这个平台上的真实上限帧率。
 * 用法：写 N 到 g_bench_frames ⇒ 跑 N 帧后自动停，结果在 g_bench_cyc / g_bench_done。 */
volatile uint32_t g_bench_frames = 0;
volatile uint32_t g_bench_cyc    = 0;
volatile uint32_t g_bench_done   = 0;

/* 抓屏用：强制重绘一拍（2026-10-04 新增）。
 *
 * 【为什么需要它 —— 抓屏工具最大的坑】
 *   g_frame_mirror 只在 phone_flush 被调时把整帧拷进 _frame_buf，而 phone_flush
 *   只在 YMGUI_Refresh 真的渲染并 flush 时才被调。**画面静止时脏区为空，
 *   YMGUI_Refresh 直接 return** ⇒ 镜像里留的是上一次动过时的老帧。
 *   后果：tools/pwr_shot.py 连续抓两张，画面已经变了但两张 PNG 逐字节相同
 *   （md5 一致），我曾据此误判"面板没画出来"。变量断言说状态对、图却不变，
 *   两边打架时**不能相信图**。
 *
 * 【用法】SWD 写 1 → 下一拍强制整屏标脏并重绘，跑完自动清 0。
 *   抓屏前必做：write 1 → 等约 0.3 s（≥2 拍）→ 再读 _frame_buf。
 *   和 g_bench_frames 的区别：本变量只刷 1 拍，不累加计时、不受 band 行数扫描影响。 */
volatile uint32_t g_force_redraw = 0;

/* 渲染瓶颈定位实验（2026-10-03）：**运行时**改 band 行数，改一个数就能扫描。
 *
 * 【要分离的两个量】满负载一帧的总时间可拆成
 *     T = n_bands × A  +  P × b
 *   A = 每 band 的**固定开销**（从 root 遍历整棵对象树 + 每个对象算绝对区域 + 相交测试），
 *       与 band 内有多少像素无关；
 *   P = 本帧实际填充的像素数（整屏标脏时恒为 153600）；
 *   b = 每像素**填充开销**（光栅化、alpha 混合、缩放采样）。
 * 每个 band 都要从 root 重画一遍 ⇒ n_bands 越大，A 付得越多，而 P 不变。
 * 于是量两个不同 n_bands 的 T，就是二元一次方程组，可直接解出 A 和 b。
 *
 * 【为什么能运行时改】库的 refreshOneRect() 每帧现读 disp->buf_px_cnt 算 band_h，
 * 不是 init 时算死的。buffer 已按 DRAW_BAND_ROWS(32) 分配 20 KB，
 * **往下调**行数只是用得更少，不会越界；往上调会越界，故此钩子限制 ≤ 32。
 * 用法：先写 g_band_rows，再启动 g_bench_frames。 */
volatile uint32_t g_band_rows = 0;   /* 0 = 用编译期值；否则运行时覆盖（上限 32） */

/* 当前 UI 的对象树规模：可见对象数、最大树深。由基准跑起来时顺手统计。
 * 每 band 的固定开销 ∝ 对象数 × 树深，这两个数决定了"减 band / 裁剪子树"值不值。 */
volatile uint32_t g_obj_count = 0;
volatile uint32_t g_obj_depth = 0;

/* 渲染原语微基准（2026-10-03）：单独量每种图元的每像素成本。
 *
 * 【为什么需要它】满负载基准只给得出"一帧总共 61 ms"，看不出这 61 ms 是
 * "少数几层很贵的绘制"还是"很多层便宜的绘制叠起来"。而这个区别直接决定
 * 优化方向：前者要改算法，后者要**削 overdraw**（少画几层）。
 * 做法：直接构造一个指向 draw buffer 的 surface，单独调一次图元，DWT 计时。
 * 面积固定为一个 band（320×32），与真实渲染的写入目标完全一致。
 *
 *   g_micro_test = 1  不透明直写填充（opa=255，走 row[bx] = px）
 *                 2  半透明混合填充（opa=160，走读-混合-写回）
 *                 3  缩放 blit（源 320×480 → 目标 320×32，走定点采样 + PutPx）
 *                 4  中文文字  5  圆弧×100
 *                 6  **裸 16 位 store 地板**（直接往 band buffer 写，不走任何图元）
 *                 7  裸 32 位 store 地板（一次写两个像素，看总线事务减半值多少）
 *                 8  裸 16 位 store，但**临时关掉 D-Cache 强制透写**（局部原子操作，
 *                    测完立刻 SCB_CleanInvalidateDCache + 恢复 FORCEWT）
 *                 9  Draw_Fill 不透明填充，同样临时关强制透写
 * 结果在 g_micro_cyc / g_micro_px。 */

/* ★★ 运行期 cache 策略开关已删除（2026-10-04，L1 加固）★★
 *
 * 【曾经有过什么】2026-10-03 加过两个运行期开关：
 *   `g_cache_wb`    写 1 ⇒ 清掉 `SCB->CACR.FORCEWT`，让缓存区（AXI SRAM 等）走**写回**；
 *   `g_cache_clean` 写 1 ⇒ 手动 clean+invalidate 一次 D-Cache。
 * 当时的动机是怀疑 FORCEWT（强制透写）拖慢了逐像素写（"地板 11–12 cy/px"看着像
 * 每写一次付一次总线事务）。
 *
 * 【为什么删掉】
 *   1. **实测无收益**：微基准 case 8/9（临时关 FORCEWT）结果 1.00–1.10×，属噪声。
 *      写缓冲已经把连续 store 合并了，关透写买不到性能。详见 README §渲染优化。
 *   2. **留着只有风险**：只要有"能关掉安全阀"的开关，就有可能在某个时刻被打开；
 *      而写回模式下 **SWD（probe-rs 读内存）绕过 D-Cache**，于是
 *      `grab_ymgui.py` 抓屏、`read_vars.py` 读 AXI 区变量都会读到**陈旧值** ——
 *      这是本工程历史上反复吃亏的静默数据不一致（见阶段 3 总线路障那两节）。
 *   3. **实例清单**：删除前已全库确认，`tools/` 下没有任何脚本依赖这两个变量。
 *
 * 【现在的策略 = 厂商策略，且不可运行期更改】
 *   D-Cache 常开 + **FORCEWT 恒为 1**（`sys.c` 的 `sys_cache_enable()`）。
 *   由此得到一条结构性保证：**CPU 写一定直达内存** ⇒ 将来任何 DMA/SWD 读到的
 *   都是最新值，不依赖"谁记得先 clean"。
 *
 *   ⚠ 但 FORCEWT **只覆盖"写"这一个方向**：DMA 写内存后，CPU 的 D-Cache 里可能
 *     仍是最新的**旧副本**，此后 CPU 读到陈旧数据。这条风险**不可能靠 FORCEWT 消除**，
 *     必须靠"专用非缓存 DMA 区"或手动 invalidate 解决。
 *     **DMA 双向台架（src/dma_bench.c）就是把这条论断变成板上实测证据的地方。 */
/* 微基准 case 8/9 仍会**临时**关 FORCEWT 做对照 —— 那是局部、原子、测完立刻恢复的
 * 动作（见 run_render_micro），不依赖任何全局开关，不受本次删除影响。 */

volatile uint32_t g_micro_test = 0;
volatile uint32_t g_micro_cyc  = 0;
volatile uint32_t g_micro_px   = 0;
/* case 8/9（关强制透写后）测到的周期数：单独存一份，便于和透写模式直接对照 */
volatile uint32_t g_micro_wb_cyc = 0;

/* Draw_Fill 的像素/调用计数（由 YMGUI_DIAG_FILLPX 打开，定义在 YMGUI_DrawFill.c）。
 * 用它们算 overdraw：一帧填的像素数 ÷ 153600 = 平均每个屏幕像素被写了多少遍。
 *
 * ⚠ 2026-10-04 步骤 2b：本宏**默认已关**（见 CMakeLists 的 YMGUI_DIAG 选项）。
 *   关掉后这几个全局量根本不存在，所以本文件里所有引用都必须包在同一个 #if 里，
 *   否则链接期会报 undefined reference。 */
#if defined(YMGUI_DIAG_FILLPX)
extern uint32_t g_diag_fill_px;
extern uint32_t g_diag_fill_calls;
extern uint32_t g_diag_fill_opa_px;
extern uint32_t g_diag_putpx;
extern uint32_t g_diag_blendpx;
#endif
/* ribbon 半透明开关的运行时值（定义在 phone_shell.c）。1=半透明 0=不透明。
 * 开机时由 main 从 AppConfig 读出写入 —— 那里是唯一的初始赋值点。 */
extern volatile uint32_t g_ribbon_translucent;

/* 亮度实现方式（定义在 phone_shell.c）：
 *   0 = 完全不调暗   1 = 软件查表叠黑（旧的、要 3.65 ms/帧）   2 = 硬件 PWM 调背光（现默认）
 * 取 2 时 phone_flush 整段跳过软件叠加，亮度改由下面 main 循环里的
 * "亮度 → 背光 PWM" 桥实现，那 3.65 ms/帧 就归零了。
 * 与 g_ribbon_translucent 同样是为了让 main 能读写库侧的运行时开关。 */
extern volatile uint32_t g_dim_mode;
volatile uint32_t g_diag_px_per_frame     = 0;
volatile uint32_t g_diag_calls_per_frame  = 0;
volatile uint32_t g_diag_opa_px_per_frame = 0;
volatile uint32_t g_diag_put_per_frame    = 0;
volatile uint32_t g_diag_blend_per_frame  = 0;

/* per-object 绘制耗时（P0-0，由 YMGUI_DIAG_OBJTIME 打开，表定义在 YMGUI_Invalidate.c）。
 *
 * 【为什么必须有这张表】整帧基准只给得出"一帧 41 ms"。之前靠"像素数 × 图元单价"
 * 反推占比，只能估出 Draw_Fill 一共花了多少 —— 剩下那 25% 是谁完全看不见。
 * 现在每个对象的 draw_cb 单独计时，top-N 一眼就能看到是谁在吃时间。
 *
 * 【对齐帧基准】每次启动 g_bench_frames 时自动清零（见下面基准块），
 * 这样读到的就是"刚才那 N 帧"的分布，不会混进开机动画、切页等历史。
 * 也可以手动写 1 到 g_objtime_reset 清一次。 */
volatile uint32_t g_objtime_reset = 0;

static void run_render_micro(void)
{
    GYdisp  *d = ymgui_port_disp();
    GYsurface sf;
    GYrect   area;
    GYimg    im;
    uint32_t c0;

    if (g_micro_test == 0u || d == NULL || d->buf1 == NULL)
    {
        return;
    }

    area.x = 0;
    area.y = 0;
    area.w = (GYcoord)YMGUI_PORT_W;
    area.h = (GYcoord)DRAW_BAND_ROWS;      /* 一个 band，正好是 buffer 容量 */
    sf.buf      = d->buf1;
    sf.buf_area = area;
    sf.clip     = area;
    sf.stride   = (GYcoord)YMGUI_PORT_W;

    switch (g_micro_test)
    {
    case 1u:
        c0 = DWT->CYCCNT;
        YMGUI_Draw_Fill(&sf, &area, GY_ARGB(0xFF, 0x20, 0x40, 0x60), GY_OPA_COVER);
        g_micro_cyc = DWT->CYCCNT - c0;
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 2u:
        c0 = DWT->CYCCNT;
        YMGUI_Draw_Fill(&sf, &area, GY_ARGB(0xFF, 0x20, 0x40, 0x60), 160);
        g_micro_cyc = DWT->CYCCNT - c0;
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 3u:
        im.data    = ymgui_port_frame();   /* 全帧镜像当源，320×480 */
        im.w       = (GYcoord)YMGUI_PORT_W;
        im.h       = (GYcoord)YMGUI_PORT_H;
        im.use_key = 0;
        im.key     = 0;
        c0 = DWT->CYCCNT;
        YMGUI_Draw_ImgScaled(&sf, &im, area);
        g_micro_cyc = DWT->CYCCNT - c0;
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 4u:      /* 中文文字：4 个 24px 汉字，字模走 XIP（PhoneLocale 的 glyph 回调） */
        c0 = DWT->CYCCNT;
        YMGUI_Draw_Text(&sf, &phone_font_regular, 4, 4,
                        "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C",
                        GY_ARGB(0xFF, 0xFF, 0xFF, 0xFF));
        g_micro_cyc = DWT->CYCCNT - c0;
        g_micro_px  = 4u * 24u * 24u;      /* 4 字 × 24×24 */
        break;
    case 5u:      /* 圆弧：phone_ui.c 的典型用法（r_in=12, r_out=14, 整圈），跑 100 次 */
        c0 = DWT->CYCCNT;
        {
            int i;
            for (i = 0; i < 100; i++)
            {
                YMGUI_Draw_ArcThick(&sf, 60, 16, 12, 14, 0, 360,
                                    GY_ARGB(0xFF, 0xFF, 0xFF, 0xFF));
            }
        }
        g_micro_cyc = DWT->CYCCNT - c0;
        g_micro_px  = 100u * 160u;         /* 周长约 2πr≈80 px × 厚度 2 × 100 次 */
        break;
    case 6u:      /* 裸 16 位 store：纯内存写地板，不含任何图元逻辑 */
        {
            GYpx*  p = (GYpx*)d->buf1;
            uint32 n = (uint32_t)area.w * (uint32_t)area.h;
            c0 = DWT->CYCCNT;
            do { *p++ = (GYpx)0x1234u; } while (--n != 0u);
            g_micro_cyc = DWT->CYCCNT - c0;
        }
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 7u:      /* 裸 32 位 store：一次两个像素，看"事务减半"能买回多少 */
        {
            gy_u32_alias_t* p = (gy_u32_alias_t*)d->buf1;
            uint32 n = ((uint32_t)area.w * (uint32_t)area.h) >> 1;
            c0 = DWT->CYCCNT;
            do { *p++ = 0x12341234u; } while (--n != 0u);
            g_micro_cyc = DWT->CYCCNT - c0;
        }
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 8u:      /* 裸 16 位 store + 临时关强制透写 */
    case 9u:      /* Draw_Fill 不透明填充 + 临时关强制透写 */
        {
            uint32 n = (uint32_t)area.w * (uint32_t)area.h;
            SCB->CACR &= ~SCB_CACR_FORCEWT_Msk;    /* 走写回 */
            c0 = DWT->CYCCNT;
            if (g_micro_test == 8u)
            {
                GYpx* p = (GYpx*)d->buf1;
                do { *p++ = (GYpx)0x1234u; } while (--n != 0u);
            }
            else
            {
                YMGUI_Draw_Fill(&sf, &area, GY_ARGB(0xFF, 0x20, 0x40, 0x60), GY_OPA_COVER);
            }
            g_micro_cyc     = DWT->CYCCNT - c0;
            g_micro_wb_cyc  = g_micro_cyc;
            SCB_CleanInvalidateDCache();           /* 脏行写回 */
            SCB->CACR |= SCB_CACR_FORCEWT_Msk;     /* 恢复厂商策略 */
        }
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 10u:     /* 渐变行内直写（原模式）：查 4 色抖动表 + 索引 store */
        {
            GYpx dith[4] = { 0x1234, 0x5678, 0x9abc, 0xdef0 };
            uint32 yy;
            c0 = DWT->CYCCNT;
            for (yy = 0; yy < area.h; ++yy)
            {
                GYpx*  row = (GYpx*)d->buf1 + (int32_t)yy * YMGUI_PORT_W;
                uint32 x;
                for (x = 0; x < (uint32_t)area.w; ++x)
                    row[x] = dith[x & 3u];
            }
            g_micro_cyc = DWT->CYCCNT - c0;
        }
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    case 11u:     /* 渐变行内直写（4 路展开）：无逐像素地址计算（P1-5 的写法） */
        {
            GYpx dith[4] = { 0x1234, 0x5678, 0x9abc, 0xdef0 };
            const GYpx d0 = dith[0], d1 = dith[1], d2 = dith[2], d3 = dith[3];
            uint32 yy;
            c0 = DWT->CYCCNT;
            for (yy = 0; yy < area.h; ++yy)
            {
                GYpx*  p = (GYpx*)d->buf1 + (int32_t)yy * YMGUI_PORT_W;
                uint32 x = 0;
                while (x + 4u <= (uint32_t)area.w)
                {
                    *p++ = d0; *p++ = d1; *p++ = d2; *p++ = d3;
                    x += 4u;
                }
                while (x < (uint32_t)area.w)
                {
                    *p++ = dith[x & 3u];
                    ++x;
                }
            }
            g_micro_cyc = DWT->CYCCNT - c0;
        }
        g_micro_px  = (uint32_t)area.w * (uint32_t)area.h;
        break;
    default:
        break;
    }
    g_micro_test = 0u;
}

#if defined(YMGUI_XIP_BENCH)
/* ---- XIP 读延迟台架（2026-10-07）------------------------------------------
 *
 * 【为什么必须有它】docs/IME_PHRASE_DICT_FEASIBILITY.md §6：词组词典要不要上
 * 外部 flash（方案 B），唯一没解决的是 XIP 的**随机读延迟**。理论下界算得出来
 * （QSPI 100MHz：指令单线 + 地址四线 + 6 dummy + 数据四线 ≈ 22~28 clk
 * ⇒ 220~280 ns），但 XIP 区是 NOT_CACHEABLE + NOT_BUFFERABLE 强序，
 * "每条 load 一次事务"这条推论**必须上板量**，不能靠算 —— 它直接决定
 * 方案 B 是"亚毫秒"还是"每次击键掉一帧"。
 *
 * 【原语 case：ns/次事务】
 * case 20  XIP 随机 4 字节对齐读       → 单次 QSPI 事务的真实成本（随机，必踩空）
 * case 21  XIP 顺序 4 字节读（步长 16）→ 稀疏顺序流（每 16 B 取 4 B）
 * case 22  XIP 随机 1 字节读           → readLe32 逐字节拼 / 字符串比较的代价
 * case 26  内部 flash 随机 4 字节对齐读（与 20 对照，算倍率）
 * case 27  XIP 顺序 1 字节读（步长 1） → **探测有没有 read-ahead 红利**：
 *                                       若它远快于 22，说明 QUADSPI 会把顺序流
 *                                       预取进 FIFO，那么"布局密度"就是决定性变量
 *
 * 【扫描模型 case：ms/次查询】每条记录 16 B：3 次对齐 u32 读（拼音/词/词频三个
 * 偏移）+ 6 次字节读（前缀比较，按查询长度 6 计）+ 3 次字节读（词串 UTF-8）
 * = **12 次事务/条**。保守上界：真实查询里只有前缀命中的条目才需要读词串。
 *   case 23  472 条（二级桶均值）   · 密集：字符串步长 7 / 4（重排后的理想布局）
 *   case 24  3256 条（最坏桶 sh）   · 密集
 *   case 28  472 条                 · **真实步长**：字符串步长 17（实测 17.4 B/条）
 *   case 29  3256 条                · 真实步长
 *   case 30  472 条                 · 记录间地址全随机（只有记录内 3 次读连续）
 *   case 25  472 条，打内部 flash   · 与 23 同模式，算倍率
 *
 * ⚠ 模型不依赖被读区域的内容（地址按固定步长推进、每次读固定字节数），
 *   所以可以直接打在还是空白（全 0xFF）的 IME 保留区 0x90300000 上 ——
 *   事务时序只跟地址和总线属性有关，跟读到什么值无关。
 *
 * 【窗口】外部与内部都用 960 KB（0xF0000），苹果对苹果。内部那份是
 * .rodata.gb2312（982 KB，运行期唯一的大块只读常量），并向上对齐到 4 字节，
 * 免得未对齐 u32 读把内部那一侧人为拖慢。
 */
extern const uint8_t _binary_gb2312_bin_start[];

#define XB_WIN           0xF0000u                     /* 960 KB */
#define XB_XIP           (QSPI_XIP_BASE + 0x300000u)  /* 0x90300000：IME 词典预留区 */
#define XB_N_RAND        8192u
#define XB_ENTRIES_MEAN  472u                         /* 二级桶均值 */
#define XB_ENTRIES_WORST 3256u                        /* 二级桶最大（'sh'） */

static uint32_t xb_lcg(uint32_t s) { return s * 1103515245u + 12345u; }

static void run_xip_micro(void)
{
    uint32_t t = g_micro_test;
    volatile uint32_t sink = 0u;
    uint32_t cyc = 0u, ops = 0u;

    if (t < 20u || t > 30u) return;

    if (t == 20u || t == 21u || t == 22u || t == 26u || t == 27u)
    {
        const uint8_t* base = (t == 26u)
            ? (const uint8_t*)(uintptr_t)(((uint32_t)_binary_gb2312_bin_start + 3u) & ~3u)
            : (const uint8_t*)(uintptr_t)XB_XIP;
        volatile const uint8_t*  b8  = (volatile const uint8_t*)base;
        volatile const uint32_t* b32 = (volatile const uint32_t*)base;
        uint32_t s = 0x12345678u, i, acc = 0u;
        uint32_t c0 = DWT->CYCCNT;
        for (i = 0u; i < XB_N_RAND; ++i)
        {
            uint32_t off;
            if (t == 21u)      off = (i * 16u) & (XB_WIN - 4u);
            else if (t == 27u) off = i & (XB_WIN - 1u);
            else { s = xb_lcg(s); off = ((s >> 9) & (XB_WIN - 4u)) & ~3u; }
            if (t == 22u || t == 27u) acc += b8[off];
            else                      acc += b32[off >> 2];
        }
        cyc = DWT->CYCCNT - c0;
        ops = XB_N_RAND;
        sink = acc;
    }
    else
    {
        const uint8_t* base = (t == 25u)
            ? (const uint8_t*)(uintptr_t)(((uint32_t)_binary_gb2312_bin_start + 3u) & ~3u)
            : (const uint8_t*)(uintptr_t)XB_XIP;
        uint32_t entries = XB_ENTRIES_MEAN, stride_py = 7u, stride_wd = 4u, rnd = 0u;
        uint32_t i, k, acc = 0u, s = 0x87654321u;
        uint32_t c0;

        if (t == 24u) { entries = XB_ENTRIES_WORST; stride_py = 7u;  stride_wd = 4u;  }
        else if (t == 28u) { stride_py = 17u; stride_wd = 17u; }            /* 真实步长 */
        else if (t == 29u) { entries = XB_ENTRIES_WORST; stride_py = 17u; stride_wd = 17u; }
        else if (t == 30u) { stride_py = 17u; stride_wd = 17u; rnd = 1u; }  /* 记录间全随机 */

        c0 = DWT->CYCCNT;
        for (i = 0u; i < entries; ++i)
        {
            uint32_t off_rec = rnd ? ((((s = xb_lcg(s)) >> 9) & (XB_WIN - 16u)) & ~3u) : (i * 16u);
            uint32_t off_py  = rnd ?  (((s = xb_lcg(s)) >> 9) & (XB_WIN - 16u))
                                   : (0x40000u + i * stride_py);
            uint32_t off_wd  = rnd ?  (((s = xb_lcg(s)) >> 9) & (XB_WIN - 16u))
                                   : (0x80000u + i * stride_wd);
            volatile const uint32_t* rec = (volatile const uint32_t*)(const void*)(base + off_rec);
            volatile const uint8_t*  py  = (volatile const uint8_t*)(base + off_py);
            volatile const uint8_t*  wd  = (volatile const uint8_t*)(base + off_wd);
            acc += rec[0]; acc += rec[1]; acc += rec[2];
            for (k = 0u; k < 6u; ++k) acc += py[k];
            for (k = 0u; k < 3u; ++k) acc += wd[k];
        }
        cyc = DWT->CYCCNT - c0;
        ops = entries * 12u;
        sink = acc;
    }
    g_micro_cyc  = cyc;
    g_micro_px   = ops;
    g_micro_test = 0u;
    (void)sink;
}
#endif

static GYCTX s_ctx;

/* 语言持久化回调：phone_lang 在语言**真的变化**时调它一次。
 * 这样"切来切去又切回原值"不会反复擦写 flash。
 * 写失败也不致命 —— AppConfig_Save 内部保留 dirty 标志，下次切换会重试。 */
static void app_lang_persist(uint8_t lang)
{
    AppConfig_SetLang((uint32_t)lang);
    AppConfig_Save();
}

/* 半透明持久化回调：由 phone_shell 的 PhoneHost_SetRibbonTranslucent() 回调过来。
 * 与 app_lang_persist 同构：shell 层不认识 app_config，由 main.c 做桥。
 * ★ 语义 1 = 半透明（出厂默认）、0 = 不透明 —— 与 g_ribbon_translucent 一致。 */
static void app_ribbon_persist(uint8_t translucent)
{
    AppConfig_SetRibbonTranslucent((uint32_t)translucent);
    AppConfig_Save();
}

/* 亮度持久化回调（2026-10-04 新增，第三份同构桥）。
 * ⚠ 与上面两个布尔/枚举项不同，亮度是 0..100 的 uint8_t。
 *   滑杆拖动过程中会连续回调，但 AppConfig_SetBrightness 内有
 *   "值相同则不写"、AppConfig_Save 内有整块 memcmp ⇒ 只有用户
 *   松手那一下的最终值才会真正触发擦写。 */
static void app_brightness_persist(uint8_t brightness)
{
    AppConfig_SetBrightness((uint32_t)brightness);
    AppConfig_Save();
}

/* 注：阶段 2/3 的"最小界面 + 按钮点击计数"已被 phone_shell 取代，
 * 原来的 g_click_cnt / on_click 一并删除（否则是引用不到的死代码）。
 * 阶段 5 的交互证据改为"抓屏序列"：开机 → 桌面 → 打开应用 → 最近任务。 */

int main(void)
{
    /* 镜像身份标记：置成 "YMG1"。SWD 读到这里说明**新固件确实在跑**。
     * （写在这里而非初始化器里，是为了让 main 引用该段，避免被 --gc-sections 回收） */
    g_boot_magic = 0x594D4731u;

    /* 注意：L1 Cache 的使能放到下面 MPU 配置**之后**，顺序原因见那里的注释。 */
    HAL_Init();
    g_clock_rc = (int32_t)sys_stm32_clock_init(160, 5, 2, 4);   /* HSE25 → 400MHz，HCLK3 = 200MHz */
    g_clock_stage = g_sysclk_stage;  /* 镜像 sys.c 的阶段号（那边是唯一的写入方） */
    g_clock_vos   = PWR->D3CR;          /* 快照电压档位握手现场：bit13(VOSRDY)=0 即未就绪 */

    /* 时钟没配成功时，VOSRDY 超时会让 VOS 停在复位档，主频只有 HSI 的 64 MHz。
     * 功能上仍可继续（各初始化路径都有错误码），但所有 FMC/LCD 时序都会变慢，
     * 所以这里留一个可 SWD 直读的标记，避免"慢"被误当成"代码有问题"。 */
    delay_init(400);
    usart_init(115200);
    led_init();

    /* MPU 必须在任何 FMC 访问之前；FMC 区保持强序（计划书红线 1） */
    mpu_memory_protection();

    /* L1 I/D-Cache：YMGUI 是软件光栅化，逐像素读写 framebuffer，不开 Cache 会慢一个数量级。
     * ⚠ 顺序：**必须在 MPU 配置之后**再开 —— 厂商例程是反过来的（cache 在前、MPU 在后），
     * 那属于 ARM 不建议的次序（Cache 可能先于 MPU 属性生效）。 */
    sys_cache_enable();

    /* ---- 阶段 4 + 5：QSPI（indirect + XIP）与 GB2312 字库 ----
     * 位置有硬约束：
     *   ① 必须在 MPU 配好之后（XIP 只读区 = region 0，见 BSP/MPU/mpu.c）；
     *   ② 必须在**任何中文文本渲染之前**（中文字体靠这里的 fallback 挂上去）。
     * 三步：
     *   qspi_port_init/verify —— 打通 indirect 与 memory-mapped，并跑计划书 §6.6 验收；
     *   font_provision_ensure —— 把内嵌的 gb2312_glyphs.bin 灌进 QSPI 字模区。
     *      首次约 20 s（240 个扇区要擦+写）；之后靠记账头的大小+CRC 复核，直接跳过；
     *   PhoneLocale_Init —— 中文字体挂进 YMGUI 全局 fallback，
     *      走 bitmap = 0x9000_0000 + glyph_read = NULL（计划书 §6.5 方案一）。 */
    if (qspi_port_init() == QSPI_OK)
    {
        (void)qspi_port_verify();
        (void)font_provision_ensure();
    }

    /* ★ 读持久化的 UI 设置（2026-10-04）★
     * 位置有硬约束：必须在 QSPI 就绪**之后** —— 配置存在 W25Q128 的扇区 240。
     * ⚠ AppConfig_Load 内部的 qspi_read 会退出 memory-mapped 模式，
     *   它在返回前会把映射**重新打开**；少了这一步，下面的中文字库就读不到
     *   （0x9000_0000 读回来全是 0）⇒ 整屏中文消失。 */
    AppConfig_Load();
    PhoneLang_Init((uint8_t)AppConfig_GetLang());
    PhoneLang_SetPersistCallback(app_lang_persist);

    /* 半透明开关的初值从配置读出，**必须在 PhoneShell_BoardInit 之前** ——
     * 那里会 build_ui() 并首次上屏，晚一步会让首帧用错的值画一帧再纠正（可见闪一下）。
     * 持久化回调也要在 BoardInit 前挂好，否则用户在设置页拨动开关时无处落盘。 */
    g_ribbon_translucent = AppConfig_GetRibbonTranslucent();
    PhoneShell_SetRibbonPersistCallback(app_ribbon_persist);

    /* 屏幕亮度：出厂默认 50（用户 2026-10-04 决策），用户拖过之后重启保持。
     * 同样必须在 PhoneShell_BoardInit **之前**应用 —— 那里面 build_ui() 会
     * 首次上屏，晚一步首帧会用 display_brightness 的编译期初值画一帧再纠正
     * （可见闪一下）。此时值与 phone_shell 的初值通常已经相同，
     * PhoneHost_SetBrightness 的"值相同直接 return"会把它挡掉，不触发落盘。 */
    PhoneHost_SetBrightness((int)AppConfig_GetBrightness());
    PhoneShell_SetBrightnessPersistCallback(app_brightness_persist);

    PhoneLocale_SetExternalFontReady((int)g_font_ok);
    PhoneLocale_Init();
    g_mark_qspi = 1;

    lcd_init();
    g_lcd_id = lcddev.id;

    /* ---- YMGUI 显示端口 + phone_shell（阶段 5）----
     * 板级 main 不再自建"标签+按钮"的最小界面，而是把 ctx 交给 phone_shell：
     *   PhoneShell_BoardInit() 内部会校验应用表、挂中文字体（已在上面做过一次，
     *   幂等）、注册快捷开关、build_ui()（末尾 begin_boot() 进开机动画），
     *   并把 disp->flush_cb 换成 phone_flush（先做快照累积，再转给 lcd_flush_cb）。 */
    g_port_rc = ymgui_port_init();
    if (g_port_rc == 0)
    {
        s_ctx = YMGUI_Creat_Ctx_Creat(ymgui_port_disp(), YMGUI_PORT_W, YMGUI_PORT_H);
        if (s_ctx != NULL)
        {
            g_ctx_rc  = 1;
            g_shell_rc = PhoneShell_BoardInit(ymgui_port_disp(), s_ctx);
            if (g_shell_rc != 0)
            {
                /* phone_shell 起不来也不能黑屏：留一个能说明问题的标签。
                 * （正常路径不会走到这里。） */
                GYOBJ lb = YMGUI_Creat_Label_Creat(s_ctx->root, 16, 24, 288, 32);
                if (lb != NULL)
                {
                    YMGUI_Label_SetText(lb, "phone_shell init failed");
                }
                YMGUI_Inject_SetCtx(s_ctx);
            }
        }
    }

    /* 先把开机动画第一帧推到面板：lcd_init() 末尾刚把屏清成白色，
     * 不刷这一帧的话，白屏会一直停留到主循环第一次刷新才被覆盖
     * （中间还隔着 touch_port_init 的 I2C 通信）。 */
    if ((g_ctx_rc == 1) && (g_shell_rc == 0))
    {
        YMGUI_Refresh(s_ctx);
    }

#if YMGUI_PORT_BOOT_SELFTEST
    /* 显示链路的自检顺序（两步都不能省，顺序也不能换）：
     *   ① ramp 自检：整屏写已知图案再读回 —— 先证明**读通路本身可用**；
     *   ② gram_verify：整屏回读面板，与镜像缓冲逐像素比对。
     * 没有 ①，② 的失配数无法区分「没写进面板」还是「读通路不可信」。
     * 放在这里跑：此时第一帧开机画面已经渲染过，镜像缓冲里是真实内容
     * （ramp 结束要靠它把面板恢复回去）。
     *
     * ⚠ 跑的时候面板会闪一下彩色波纹（i & 0xFFFF 图案），约 86 ms，属预期。
     * 开关见 ymgui_port.h 的 YMGUI_PORT_BOOT_SELFTEST（默认 0）。
     * 即使关掉，主循环里的 g_gram_recheck 钩子仍能随时重跑这两步。 */
    if ((g_ctx_rc == 1) && (g_shell_rc == 0))
    {
        ymgui_port_ramp_selftest();   /* 会临时改写面板，结束时用镜像缓冲恢复 */
        ymgui_port_gram_verify();
    }
#endif /* YMGUI_PORT_BOOT_SELFTEST */

    /* ---- 触摸（GT9xxx，本机引脚 SCL=PB11 / SDA=PB14 / RST=PB12 / INT=PB13）----
     * 失败也不影响显示：g_tp_rc != 0 时 touch_port_poll() 直接返回。 */
    (void)touch_port_init();

    /* ---- 阶段 4：W25Q128 QSPI 与阶段 5 的字库已在上面（MPU 之后、
     * 中文渲染之前）一起初始化，这里不再重复。 ---- */

    /* ---- DWT 周期计数器：给 YMGUI_Inject_Tick 提供真实经过的毫秒数 ----
     * ⚠ 顺序不能颠倒：global trace 未使能时 CYCCNTENA 写入可能被忽略
     * （早期 Rust 参照工程实测过：只调 CYCCNTENA 时 CYCCNT 恒为 0）。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    g_dwt_hz = 400000000u;        /* = sys_ck，见 sys_stm32_clock_init(160,5,2,4) */

    /* ---- TF 卡（SDMMC1，路线图 L2-1）----
     * 放在 DWT 之后：自检要用 DWT 计时（DWT 不开则 CYCCNT 恒 0，读到的周期数没意义）。
     * ⚠ 没插卡 / 卡不识别**不影响任何其他功能**：sd_card_init 只把自己的失败
     *   记进 g_sd_init_rc 就返回，LCD / 触摸 / UI 全部照常。
     * 自检本身**不在开机时跑** —— 它会写卡（虽然选在卡尾且带备份恢复），
     * 必须由人显式触发：SWD 写 g_sd_test = 1，或 python tools/sd_check.py。 */
    (void)sd_card_init();

    /* ---- ESP32 上行链路（UART4 / PB9-TX / PB8-RX，921600）----
     * ⚠ 2026-10-09 换外设：原 USART6(PC6/PC7)。PC6 那根命令线双向验证为无电气连接，
     *   而 USART6 在 LQFP100 只有 PC6/PC7 一组 ⇒ 换脚只能换外设。见 uart_link.h 顶部。
     * 对端每 10 s 发一帧 $DT（NTP 时间）、每 30 min 一帧 $WD（天气）。
     * ⚠ 没接对端 / 波特率不对**不影响任何其他功能**：uart_link_init 只把失败
     *   记进 g_uart_rc 就返回，UI 上时间显示"--:--"、天气显示"等待天气"。
     * 判据看 g_uart_rx_bytes 有没有在涨、g_uart_err_fe 是不是 0。
     *
     * ⚠ 波特率 2026-10-09 从 115200 提到 **921600**（对端同步改，
     *   CONFIG_UPLINK_UART_BAUD）：为 P2 的 $!BD 大块数据通道准备的 ——
     *   115200 下传 2 KB 要 178 ms，921600 只要 22 ms。
     *   误差账（两侧都算过，不是拍的）：
     *     UART4 内核时钟 = PCLK1 = 100 MHz（APB1 与 APB2 同为 /2，故与旧值一致）；
     *     HAL 的 UART_DIV_SAMPLING16
     *     = (100e6 + 460800) / 921600 = **BRR 109** ⇒ 实际 917 431（−0.45%）
     *     对端 ESP32 是 80 MHz APB + 小数分频，误差 <0.02%
     *     ⇒ 合计约 0.46%，远在 8N1/16 倍采样的容差（约 ±2%）内。
     *   实测判据：稳定期 g_uart_err_fe / ne 增量为 0（见 tools/uart_check.py）。 */
    (void)uart_link_init(921600u);

    /* ---- SPI1 主 ↔ ESP32（VSPI 从）**业务链路**（2026-10-10 升格）----
     * 替代 UART4 上行：ESP32 的时间/天气帧（$DT/$WD/$WF/$WX）与命令响应
     * （$!RS/$!BD）改走 SPI，STM32 的命令（$?PING/$?WEA/$?WGET）也走 SPI。
     * 默认 /64 ≈ 1.56 MHz（杜邦线先求稳）。
     * ⚠ 没接对端 / 对端没跑从机**不影响任何其他功能**：失败只记 g_spi_init_rc；
     *   判据看 g_spi_init_rc==0、g_spi_rx_bytes 涨、g_net_dt_pkts/g_net_wd_pkts 涨。
     *   接线与协议见 src/spi_link.h 顶部。判据脚本：tools/spi_biz_check.py。 */
    (void)spi_link_init(64u);

    /* ---- 板载 RTC（LSE 32.768 kHz，原理图 Y1 / PC14-PC15）2026-10-09 ----
     * 解决"每次复位都要等首帧 $DT 才有时间与日期"：VDD 不断时备份域保持，
     * 复位后 RTC 直接给出正确时间（$DT 到达后再校准一次）。
     * ⚠ 最多阻塞约 1.5 s 等 LSE 起振（实测本板 <300 ms）；起不来会回退 LSI，
     *   两者都失败只记 g_rtc_rc 并返回，**不影响任何其他功能**
     *   —— 此时时间退回 uart_link 的 tick 递推（旧行为）。
     * 判据：g_rtc_rc=0 / g_rtc_src=0(LSE) / g_rtc_valid=1 / g_rtc_sets 在涨。 */
    (void)Rtc_Init();

    uint32_t last_cyc = DWT->CYCCNT;

    /* 开机以来的累计毫秒（给 uart_link 做时基）。
     * ⚠ 不能用 DWT->CYCCNT 直接换算：32 位 @400 MHz 约 10.7 秒就回绕一次，
     *   拿它当绝对时基会让"当前时间"每 10 秒跳一次。 */
    uint32_t uptime_ms = 0;

    /* 帧节拍目标：**60 Hz**（16 ms）。取 16 而不是 16.67 是为了让 DWT 整数换算不丢进位；
     * 差出来的 0.67 ms/帧在空转等待里，不产生任何收益差异。 */
    const uint32_t FRAME_TARGET_MS = 16u;
    uint32_t spent_ms;
    uint32_t rest_ms;

    /* LED 心跳状态：与帧节拍解耦，避免帧率变化被误读成主循环异常 */
    uint32_t s_led_last_cyc = DWT->CYCCNT;
    int      s_led_on       = 0;

    /* 注：cache 策略已固化为厂商配置（D-Cache 常开 + FORCEWT 恒 1），
     * 不再有运行期开关，故这里没有 s_cache_wb_now 之类状态。见文件上方长注释。 */

    while (1)
    {
        uint32_t now = DWT->CYCCNT;
        uint32_t dt  = now - last_cyc;
        uint32_t ms  = dt / (g_dwt_hz / 1000u);
        last_cyc = now;

        if (ms == 0u)
        {
            ms = 1u;              /* 至少推进 1ms，避免长按等计时永远不动 */
        }
        g_tick_ms = ms;
        YMGUI_Inject_Tick(ms);
        g_mark_tick = g_loop_count + 1;

        g_loop_count++;

        /* 触摸：扫描厂商驱动并把坐标注入 YMGUI（拉取式，无中断） */
        /* 图库导入期间（g_img_busy=1）连触摸也不处理：输入路径会查中文输入法，
         * 而 IME 词典就放在 XIP 上，退出映射时读它会拿到未知数据。
         * ⚠ 当前 img_store_poll 是**阻塞式**一次性跑完（几秒），主循环卡在里面，
         *   下面这两个判断在同一拍里其实轮不到；留着是为两件事：
         *   ① 将来把导入改成"分帧推进"（每帧写一个块、让出主循环）时，
         *      这道门闩就是必须的了，先按正确结构摆好；
         *   ② 防御任何中断/回调路径在导入期间碰 UI。
         *   别把它当成"现在就有用"的装饰 —— 它的价值在分帧化之后。 */
        if (g_img_busy == 0u)
        {
            touch_port_poll();
        }
        g_mark_poll = g_loop_count;

        /* 恢复出厂设置（SWD 写 1 触发，跑完自动清 0）。
         * ⚠ 必须放在触摸之后、渲染之前：擦除 flash 要几百 ms，
         *   期间不重绘没关系，但要保证同一拍里不再往屏上推半帧内容。 */
        if (g_cfg_factory_reset != 0u)
        {
            g_cfg_factory_reset = 0u;
            AppConfig_FactoryReset();
        }

        /* QSPI 分块灌库（方案 B 的一次性产线动作，见 src/qspi_provision.h）。
         * ⚠ 与恢复出厂设置同样的位置理由：擦写扇区要几十~几百 ms，期间不重绘，
         *   但必须保证同一拍里不会往屏上推半帧。放在渲染之前正好满足。
         *   非灌库构建里这个函数是空的（g_prov_enabled = 0）。 */
        qspi_provision_poll();

        /* 图库导入 / XIP 探针（写 g_img_test 触发，跑完自动清 0）。
         * 位置理由同上：它会擦写 W25Q128 的扇区（每个约 45 ms），期间 XIP 不可读，
         * 必须保证同一拍里不往屏上推内容。见 src/img_store.h 顶部。 */
        img_store_poll();

        /* 写文件夹具：把 PC 侧的测试图分片送进 TF 卡（见 img_store.h 的 g_img_mk）。
         * 它不碰 QSPI，所以不必放在渲染之前 —— 但挨着放便于一眼看清顺序。 */
        img_store_mk_poll();

        /* ESP32 上行链路：把 ISR 收进环形缓冲的字节抽出来组帧、校验、解析。
         * 不碰 QSPI、不碰屏幕，放哪都行；挨着放只是为了让顺序一眼看清。 */
        uptime_ms += ms;
        uart_link_poll(uptime_ms);

        /* SPI 业务链路：每 ~5 ms 一次 64 字节全双工事务（内部节流）。
         * 主发命令、从回数据，下行字节喂给 uart_link_feed 走同一套解析。
         * 轮询 @≥1.5 MHz 只有几十微秒，不影响 60 Hz 帧节拍。 */
        spi_link_poll(uptime_ms);

        /* 蓝牙文件接收会话（P4，见 src/bt_recv.h）：
         * 按序号拉块 → 校验 CRC → 写 TF → 收尾导入图库。
         * ⚠ 必须排在 uart_link_poll / spi_link_poll **之后** ——
         *   它读的是本拍刚解析出来的 $!BD 收齐标志与 $!RS,BTF / $BT 状态帧。 */
        bt_recv_poll(uptime_ms);

        if (g_ctx_rc == 1 && g_img_busy == 0u)
        {
            uint32_t c0 = DWT->CYCCNT;
            if (g_shell_rc == 0)
            {
                /* 抓屏前强制重绘：脏区为空时 YMGUI_Refresh 直接 return，
                 * 镜像就停在上一份老帧上（见 g_force_redraw 定义处的说明）。
                 * 必须放在 PhoneShell_BoardTick **之前** —— 它内部那一拍会调
                 * YMGUI_Refresh，我们标脏的意图正好由它来完成。 */
                if (g_force_redraw != 0u)
                {
                    GYrect full = { 0, 0, (GYcoord)YMGUI_PORT_W, (GYcoord)YMGUI_PORT_H };
                    YMGUI_Ctx_InvalidateArea(s_ctx, &full);
                    g_force_redraw = 0u;
                }
                /* phone_shell 每拍：advance(ms) + YMGUI_Refresh(ctx)。
                 * 时基用 DWT 实测毫秒（不是固定 30ms），长按/动画计时才准。 */
                PhoneShell_BoardTick(ms);
            }
            else
            {
                YMGUI_Refresh(s_ctx);
            }
            g_refr_cyc = DWT->CYCCNT - c0;
        }
        g_mark_refr = g_loop_count;

        /* 满负载帧基准：写 N 到 g_bench_frames 启动，跑满自动停。 */
        if (g_bench_frames > 0u && g_ctx_rc == 1)
        {
            GYrect   full = { 0, 0, (GYcoord)YMGUI_PORT_W, (GYcoord)YMGUI_PORT_H };
            uint32_t b0;
#if defined(YMGUI_DIAG_FILLPX)
            static uint32_t s_px0 = 0, s_c0 = 0, s_o0 = 0, s_u0 = 0, s_b0 = 0;
#endif

            if (g_bench_done == 0u)          /* 第一帧开始前记下计数起点 */
            {
#if defined(YMGUI_DIAG_FILLPX)
                s_px0 = g_diag_fill_px;
                s_c0  = g_diag_fill_calls;
                s_o0  = g_diag_fill_opa_px;
                s_u0  = g_diag_putpx;
                s_b0  = g_diag_blendpx;
#endif
#if defined(YMGUI_DIAG_OBJTIME)
                /* per-object 计时同步清零 —— 读到的就是"这 N 帧"的分布 */
                YMGUI_Diag_ObjTimeReset();
#endif
            }

            /* 渲染瓶颈定位：每拍都把 band 行数同步成 g_band_rows（见变量处注释）。
             * 上限 DRAW_BAND_ROWS —— buffer 只按那么宽分配过，超了会越界写。 */
            /* 每拍都同步（而不是只在非 0 时写一次），这样把 g_band_rows 写回 0
             * 就能立刻恢复编译期默认值，不必复位板子。 */
            {
                uint32_t rows = g_band_rows;
                if (rows == 0u || rows > (uint32_t)DRAW_BAND_ROWS)
                {
                    rows = (uint32_t)DRAW_BAND_ROWS;
                }
                ymgui_port_disp()->buf_px_cnt = (uint32_t)YMGUI_PORT_W * rows;
            }

            b0 = DWT->CYCCNT;
            YMGUI_Ctx_InvalidateArea(s_ctx, &full);
            YMGUI_Refresh(s_ctx);
            g_bench_cyc += DWT->CYCCNT - b0;
            g_bench_frames--;
            g_bench_done++;

            /* 最后一帧跑完：把 Draw_Fill 的计数增量摊到每帧，得到 overdraw */
            if (g_bench_frames == 0u && g_bench_done > 0u)
            {
#if defined(YMGUI_DIAG_FILLPX)
                uint32_t nf = g_bench_done;
                g_diag_px_per_frame     = (g_diag_fill_px     - s_px0) / nf;
                g_diag_calls_per_frame  = (g_diag_fill_calls  - s_c0)  / nf;
                g_diag_opa_px_per_frame = (g_diag_fill_opa_px - s_o0)  / nf;
                g_diag_put_per_frame    = (g_diag_putpx       - s_u0)  / nf;
                g_diag_blend_per_frame  = (g_diag_blendpx     - s_b0)  / nf;
#endif
            }

            /* 顺手统计对象树规模（每帧一次，成本远小于基准本身） */
            {
                int depth = 0;
                int n = PhoneShell_BoardObjStats(&depth);
                g_obj_count = (uint32_t)n;
                g_obj_depth = (uint32_t)depth;
            }
        }

        /* per-object 计时手动清零（见变量处注释）。基准启动时会自动清，
         * 这个钩子用于"只清一次、然后手动操作界面看看哪个对象贵"。 */
        if (g_objtime_reset != 0u)
        {
            g_objtime_reset = 0u;
#if defined(YMGUI_DIAG_OBJTIME)
            YMGUI_Diag_ObjTimeReset();
#endif
        }

        /* ---- cache 策略：已固化，这里不再有运行期切换 ----
         * FORCEWT 恒为 1（厂商策略），好处是"CPU 写必达内存"变成结构性保证，
         * SWD 抓屏 / 读变量永远拿到最新值，不需要任何手动 clean。
         * 开关为何删除、删之前做过哪些确认，见文件上方 g_cache_wb 的替换注释。 */

#if defined(YMGUI_XIP_BENCH)
#if defined(YMGUI_XIP_BENCH)
        /* 输入法按键耗时台架（见 phone_shell_board.h 的说明）。
         * 与 case 20~30 同一口径：只在测量构建里存在。 */
        PhoneIME_BoardBenchPoll();
#endif

        /* XIP 读延迟台架（case 20~26，见 run_xip_micro 上方注释）：
         * 默认不编译（CMake 的 YMGUI_XIP_BENCH），只为量 XIP 延迟而临时打开。 */
        run_xip_micro();
#endif

        /* 渲染原语微基准（见变量处注释）：写 g_micro_test 触发，跑完自动清 0 */
        run_render_micro();

        /* 文件系统自检（路线图 L2-3）：写 g_fs_test=1 触发，跑完自动清 0。
         * 在卡上建 /YMGUI 目录 → 写 1 KB 文件 → 读回逐字节比对 → 删掉该文件。
         * 只在被触发时跑（阻塞几百毫秒），不影响正常 UI 帧率。见 src/fatfs_port.h。 */
        fatfs_bench_poll();

        /* TF 卡自检（路线图 L2-2）：写 g_sd_test=1 触发，跑完自动清 0。
         * 会阻塞几百毫秒（轮询读写 4 KB + 备份恢复），所以只在被触发时跑，
         * 不影响正常 UI 帧率。见 src/sd_card.h 顶部。 */
        sd_bench_poll();

        /* DMA 缓存一致性双向台架：写 g_dma_test=1 触发，跑完自动清 0。
         * 用真实 DMA（DMA1 MEM2MEM）验证"CPU→DMA 通、DMA→CPU 需 invalidate"，
         * 为引入 TF 卡 / 任何 DMA 外设之前先把这条风险量清楚。见 src/dma_bench.c。 */
        run_dma_bench();

        /* 亮度 → 背光 PWM 桥（步骤 2a 硬件版）。
         *
         * 【映射为什么不是 duty = 亮度】原软件叠加的亮度系数是
         *     L(v) = 1 − opa/255，其中 opa = (100−v)×160/100
         * 也就是 v=50 时实际亮度只有 68.6%（不是 50%），v=1 时 37.9%。
         * 这里按 **L(v) 等效**换算占空比，让换机制后观感基本不变：
         *     duty(v) = 1000 × L(v) = 1000 − (100−v)×160×1000/25500
         *   v=100 → 1000‰   v=50 → 687‰   v=1 → 379‰
         * 好处是**对比度反而更高**：原来是把画面整体压暗（黑也变灰），
         * 现在是背光变暗（黑仍然是黑的）。
         * 若更想要"50 就是 50% 背光"的直觉映射，把下面那行换成 duty = v*10 即可。
         *
         * mode != 2 时强制关 PWM（保证退到软件叠加或完全不调暗时背光是常亮，
         * 不会出现"两条路各调一半"的状态）。 */
        if (g_dim_mode == 2u)
        {
            uint32_t v = (uint32_t)PhoneHost_GetBrightness();
            if (v < 1u)   { v = 1u; }        /* 与滑杆下限/config 钳制一致：亮度不会是 0 */
            if (v > 100u) { v = 100u; }
            g_bl_pwm_duty = 1000u - ((100u - v) * 160u * 1000u) / 25500u;
            g_bl_pwm_mode = 1u;
        }
        else
        {
            g_bl_pwm_mode = 0u;              /* 交回 GPIO 常亮 */
        }
        bl_pwm_tick();

        /* A1 测量钩子：整屏 flush 净耗时（与 flush_cb 同一写路径，DWT 计时） */
        if (g_flush_measure != 0u)
        {
            g_flush_measure = 0u;
            ymgui_port_measure_flush();
        }

        /* FMC 写时序扫描钩子（步骤 3，见变量处注释）：apply 一档并顺手重测整屏 flush，
         * 因此"写档位 → 写 1 → 读 g_flush_cyc"就是一次完整的档位测量。 */
        if (g_fmc_timing_sweep != 0u)
        {
            g_fmc_timing_sweep = 0u;
            lcd_fmc_write_timing_apply();
            g_fmc_applied_addset = (uint32_t)g_lcd_fmc_addset;
            g_fmc_applied_datast = (uint32_t)g_lcd_fmc_datast;
            ymgui_port_measure_flush();
        }

        /* A2 复测钩子：界面跑过很多帧之后，随时可以重跑双自检。
         * ramp 会临时改写面板、结束用镜像缓冲恢复，画面会闪一下属正常。 */
        if (g_gram_recheck != 0u)
        {
            g_gram_recheck = 0u;
            ymgui_port_ramp_selftest();
            ymgui_port_gram_verify();
        }

        /* ---- 帧节拍：按实际用量补足到目标帧间隔，而不是无脑等固定毫秒数 ----
         *
         * 原来是 `delay_ms(15); delay_ms(15);`（固定 30 ms ⇒ 上限 33.3 FPS）。
         * 那是 2026-10-03 优化之前定的：当时整屏 flush 要 20.5 ms，30 ms 一拍确实合理。
         * 现在 flush 降到 7.7 ms（见 pointer g_flush_cyc），30 ms 的固定节拍把
         * 一半以上的帧预算白白空转掉了 —— flushing 再快也帧率不涨，瓶颈在 delay 上。
         *
         * 改成：本轮花了多久就补多少，永远凑够 TARGET_FRAME_MS。
         * 好处是无论有没有重绘负载，节拍恒定；ymgui 的动画时基本来就走 DWT 实测毫秒
         * （Inject_Tick(ms)），所以动画速度不会因为帧率变化而变快或变慢。 */
        spent_ms = (DWT->CYCCNT - now) / (g_dwt_hz / 1000u);
        rest_ms  = (spent_ms < FRAME_TARGET_MS) ? (FRAME_TARGET_MS - spent_ms) : 1u;
        delay_ms((uint16_t)rest_ms);

        /* LED 心跳：固定 ~2Hz，与帧节拍解耦。
         * 若把 LED 挂在帧循环里翻转，帧率一变 LED 闪烁频率就跟着变，
         * 很容易被读成"主循环卡了/挂了"，所以独立计时。 */
        if (DWT->CYCCNT - s_led_last_cyc >= g_dwt_hz / 2u)
        {
            s_led_last_cyc = DWT->CYCCNT;
            s_led_on = !s_led_on;
            LED0(s_led_on);
        }
    }
}
