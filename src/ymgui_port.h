#ifndef YMGUI_PORT_H_INCLUDED
#define YMGUI_PORT_H_INCLUDED

#include "YMGUI_Hal.h"

/* ===========================================================================
 * YMGUI → STM32H743 的移植桥
 *
 * 库对硬件**唯一的**要求就是 GYdisp 里的 flush_cb；本文件把它接到
 * 厂商 lcd.c（FMC 8080 并口 ST7796，320×480 RGB565）。
 * =========================================================================== */

/* 板级屏幕参数（与厂商 lcd.c 的竖屏初始化一致） */
#define YMGUI_PORT_W  320
#define YMGUI_PORT_H  480

/* 开机自检（A2 双自检：ramp + gram_verify）—— **默认 0（关）**，见 main.c 的调用点。
 *
 * 打开（=1）后每次上电会跑：ramp 自检整屏写「像素 i = i & 0xFFFF」再读回比对，
 * 然后 gram_verify 整屏回读与镜像缓冲比对。
 * ⚠ **现象预告**：面板会闪一下花花绿绿的彩色波纹（横向 32 px 一条蓝循环、
 * 每 6.4 行跳一次红分量，叠加成斜向摩尔纹），约 86 ms 后自动恢复成开机画面
 * （写 7.68 ms + 读 71 ms + 恢复写 7.68 ms，读时序 ADDSET=15/DATAST=78 @200 MHz）。
 * **这是预期现象，不是花屏、也不是 FMC 时序故障** —— 时序真出问题时表现为
 * ramp 图案错位且 `g_ramp_mismatch` ≠ 0。
 *
 * 关掉**不损失任何自检能力**：主循环里有 `g_gram_recheck` 钩子，SWD 写 1 即可
 * 随时重跑同一套自检（ramp + gram_verify），AI/脚本走这条通道即可，不用重编译。
 * 默认关的唯一目的就是别让正常上电看到这一闪。
 *
 * 打开方式（任选其一）：
 *   ① CMake：`add_compile_definitions(YMGUI_PORT_BOOT_SELFTEST=1)`
 *   ② 命令行：`cmake -S . -B build -DYMGUI_PORT_BOOT_SELFTEST=1`
 *   ③ 改本文件的默认值 */
#ifndef YMGUI_PORT_BOOT_SELFTEST
#define YMGUI_PORT_BOOT_SELFTEST  0
#endif

/* 单条 band 的行数：库按 `buf_px_cnt = W × 本值` 把脏区切成多条 band 逐条重绘。
 * 放在头文件是为了让 main.c 的 g_band_rows 实验钩子能拿到上限（buffer 只按本值
 * 分配过，运行时往上调会越界）。
 * 值的取舍：320 × 32 × 2B = 20 KB。调小 ⇒ band 更多、每 band 固定开销付得更多；
 * 调大 ⇒ 省固定开销但要吃掉 heap1（当前 heap1 峰值已 97%，放不下更大的）。 */
#define DRAW_BAND_ROWS  32

/* 初始化显示端口：填 GYdisp（尺寸 / draw buffer / flush_cb）。
 * 返回 0 成功，-1 表示 draw buffer 申请失败。必须在 YMGUI_Creat_Ctx_Creat 之前调用。 */
int ymgui_port_init(void);

/* 取已初始化的 GYdisp（供创建 ctx 用） */
GYDISP ymgui_port_disp(void);

/* 全帧镜像缓冲（320×480 RGB565，位于 AXI SRAM 起始 300 KB）。
 * flush_cb 会把每条 band 累积到这里；用 SWD 读出来即可还原成 PNG 目视核对。
 * 地址用 `arm-none-eabi-nm -n build/ymgui-h743.elf | grep _frame_buf` 查。 */
GYpx *ymgui_port_frame(void);

/* 运行期计数：供 SWD 核对"渲染是否真的在发生" */
extern volatile unsigned g_flush_calls;    /* flush_cb 被调用次数 */
extern volatile unsigned g_flush_pixels;   /* 累计推送像素数 */
extern volatile unsigned g_frame_seq;      /* 完成的整帧数（frame-done 回调） */

/* ---- DMA 双缓冲（2026-10-04 第 7a 项）----
 * flush 与渲染重叠：flush_cb 只起 DMA 就返回，DMA 传完由 wait_cb 轮询到并交还 buffer。
 *
 * ⚠ 这两个量的**含义分界**是读这一组数的前提：
 *   g_flush_cb_cyc   只含"设窗口 + 起 DMA"，改造后会掉到几微秒 → 单看它会以为 flush 免费；
 *   g_flush_wait_cyc 才是真正在等面板的时间（wait_cb 里轮询 TC）。
 *   有重叠时一帧的 flush 成本 ≈ max(...)，不再是两者之和 —— 报数时别直接相加。 */
extern volatile unsigned g_flush_wait_cyc;   /* wait_cb 里等 DMA 的累计周期 */
extern volatile unsigned g_flush_wait_calls;
extern volatile unsigned g_lcd_dma_ok;       /* 1 = 已启用 DMA 双缓冲（0 = 退回单缓冲同步）*/
extern volatile unsigned g_lcd_dma_init_rc;  /* HAL_DMA_Init 返回值（0 = HAL_OK）*/
extern volatile unsigned g_lcd_dma_poll_rc;  /* 最近一次等 DMA 的结局：1 = 等到，2 = 超时兜底 */
extern volatile unsigned g_lcd_dma_timeout;  /* 轮询超时次数（>0 = 这条通路不可靠）*/
extern volatile unsigned g_lcd_dma_reenable; /* 起新传输时上一笔还开着而被迫关流的次数（>0 = 上游契约被破坏）*/
extern volatile unsigned g_lcd_dma_waited;   /* flush_cb 开头真的等到上一笔的次数（= 重叠真实发生的次数）*/
extern volatile unsigned g_lcd_dma_xfers;    /* 交给 DMA 的传输次数 */
extern volatile unsigned g_lcd_dma_pixels;   /* 交给 DMA 的像素总数 */
extern volatile unsigned g_lcd_dma_wait_enter;/* wait_cb 被调用次数（含提前返回）*/

/* flush 通路开关：0 = CPU 直推（备用）1 = DMA 双缓冲（**出厂默认**）。
 * 同场景背靠背 A/B 实测 DMA 快 3.43 ms（18.14 → 14.71 ms，−18.9%，两次复测差 27 cyc），
 * 面板 GRAM 回读逐像素一致。保留开关是为了不重编译就能 A/B —— SWD 写 0 即切回 CPU。
 * ⚠ A/B 必须**同一页面**背靠背跑：本板基准随页面变化，跨场景比会得出反结论。详见 .c。 */
extern volatile unsigned g_lcd_dma_enable;

/* 面板 GRAM 回读校验：整屏连续读回，与全帧镜像缓冲逐像素比对。
 *   g_gram_mismatch  = 失配像素数（0 = 面板里的内容与 YMGUI 渲染结果完全一致）
 *   g_gram_first_bad = 第一个失配的像素下标
 *
 * 为什么用**整屏连续读**而不是单点读：本板的 LCD 读通路对单点读不稳定
 * （厂商 lcd_read_point 返回 0xFFFF），而本工程自己的整屏 ramp 自检证明
 * 连续读是可用的。单点读的哑读/指针推进语义与连续读不同，容易误判。 */
extern volatile uint32_t g_gram_mismatch;
extern volatile uint32_t g_gram_first_bad;
void ymgui_port_gram_verify(void);

/* **读通路有效性自检**：整屏写「像素 i = i」，复位读指针后整屏读回比对。
 *   g_ramp_mismatch = 失配数
 *
 * 为什么必须先跑它：`g_gram_mismatch` 只能说明"面板里读回的内容 ≠ 镜像缓冲"，
 * 无法区分两种可能 ——（a）写没进面板；还是（b）**读通路本身就不可信**。
 * 只有 ramp 自检自身通过（写入的 i 能原样读回），后面 gram_verify 的失配数才有意义。
 *
 * 结束时会把镜像缓冲重推一遍恢复面板，免得破坏后续比对。 */
extern volatile uint32_t g_ramp_mismatch;
void ymgui_port_ramp_selftest(void);

/* A1 验收测量：把镜像缓冲整屏推一遍（与 flush_cb 同一写路径），DWT 计时。
 *   g_flush_cyc  = 整屏推送的 DWT 周期数（400 MHz 主频）
 *   g_flush_nspx = 每像素耗时（ns × 100）
 * 换算：单帧 ms = g_flush_cyc / 400000；FPS = 400000000 / g_flush_cyc。
 * 验收线：≤ 53 ms（≥ 19 FPS）。由 main 循环里的 g_flush_measure 钩子触发。 */
extern volatile uint32_t g_flush_cyc;
extern volatile uint32_t g_flush_nspx;
void ymgui_port_measure_flush(void);

/* ===========================================================================
 * 背光硬件 PWM（PB1 = TIM3_CH4 / AF2）—— 步骤 2a 的硬件版，实验性
 *
 * 目的：把"软件全屏 α 叠黑调暗"那 3.65 ms/帧 换成硬件 PWM（0 ms）。
 * 已查实：PB1 = TIM3_CH4/AF2（ST 官方 AF 表）；TIM3 在本工程完全没被占用；
 *         不需要 HAL_TIM（直接写 CMSIS 设备头里的寄存器位定义）。
 * **未知**：背光驱动电路不在仓库里（docs/ 与 BSP 注释都只有"PB1 = LCD_BL、
 *         置高点亮"）—— 若 PB1 是 PWM 输入则本方案成立；若它是某颗升压/背光
 *         芯片的使能脚，PWM 可能表现为闪烁、非线性或干脆没反应。只能上板试。
 * 因此默认 **mode = 0（完全不碰 PB1，保持厂商的 GPIO 常亮）**，且复位后一定
 * 回到这个态 —— 任何情况下 `probe-rs reset` 都能把背光恢复到全亮。
 *
 * 用法：SWD 写 g_bl_pwm_mode = 1 接管，再改 g_bl_pwm_duty（千分比）；
 *       写 g_bl_pwm_mode = 0 立刻恢复全亮。
 * 判据：duty = 0 面板应完全黑掉（背光灭）⇒ PWM 真的在控制背光；
 *       duty = 1000 应与常亮一模一样。两者都不成立 ⇒ 属于"使能脚"那一类。
 * =========================================================================== */
extern volatile uint32_t g_bl_pwm_mode;    /* 0 = GPIO 常亮（默认） 1 = PWM */
extern volatile uint32_t g_bl_pwm_duty;    /* 千分比 0..1000 */
extern volatile uint32_t g_bl_pwm_hz;      /* 回读：实际频率 Hz（0 = 没在 PWM） */
extern volatile uint32_t g_bl_pwm_rc;      /* 回读：0 成功，非 0 已自动回退 GPIO */
/* 寄存器快照：让主机侧能直接确认 PB1 到底归谁，不必靠"看屏幕亮不亮" */
extern volatile uint32_t g_bl_pin_moder;   /* 2 = 复用功能(AF) */
extern volatile uint32_t g_bl_pin_afr;     /* 应为 2 (AF2 = TIM3_CH4) */
extern volatile uint32_t g_bl_tim_ccer;
extern volatile uint32_t g_bl_tim_ccr4;
extern volatile uint32_t g_bl_tim_arr;
extern volatile uint32_t g_bl_tim_psc;
void bl_pwm_tick(void);                    /* 主循环每拍调一次 */

#endif /* YMGUI_PORT_H_INCLUDED */
