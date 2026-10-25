/* ===========================================================================
 * 触摸移植桥实现
 *
 * YMGUI 的输入是**拉取式注入**（裸机没有消息泵）：
 *   主循环每拍调 touch_port_poll() → 驱动扫描 → YMGUI_Inject_Pointer()
 * 时钟由主循环另外调 YMGUI_Inject_Tick(实际经过毫秒数)。
 * =========================================================================== */

#include "touch_port.h"
#include "ymgui_port.h"

#include "./BSP/TOUCH/touch.h"
#include "./BSP/TOUCH/gt9xxx.h"

/* ★ tp_dev 的实体 ★
 * 厂商把它定义在 touch.c 里，但 touch.c 还依赖电阻屏驱动 ft5206 与 LCD 绘图函数，
 * 本工程不需要，所以在这里自己定义（字段与厂商 touch.h 完全一致）。 */
_m_tp_dev tp_dev = {
    .init      = gt9xxx_init,
    .scan      = gt9xxx_scan,
    .adjust    = 0,
    .touchtype = 0x80,   /* b7=1 电容屏；b0=0 竖屏（本板屏是竖直安装的） */
};

volatile int32_t  g_tp_rc     = 99;
volatile uint32_t g_tp_pid    = 0;
volatile uint32_t g_tp_points = 0;
volatile uint16_t g_tp_last_x = 0xFFFF;
volatile uint16_t g_tp_last_y = 0xFFFF;
/* 注入成功返回的累计次数 —— "注入链路不再卡死"的直接证据 */
volatile uint32_t g_inject_ok = 0;

/* 分级打点：1=进入 poll，2=scan 前，3=scan 返回后，4=注入完成 */
volatile uint32_t g_poll_stage = 0;

/* 未按下时是否发抬起事件。
 * ★必须为 1★：YMGUI 靠"按下 → 抬起"配对才判 Clicked；只发按下不发抬起，
 * 按钮永远不会响应。早期为定位"注入卡死"把它临时置 0（只扫描不注入），
 * 根因修好后恢复为 1（见文件末尾的注释）。 */
volatile uint32_t g_tp_inject_enable = 1;

/* ===========================================================================
 * 2026-10-13：触摸 INT(PB13) → EXTI 边沿中断，做"有触摸才读 I2C"的门控
 *
 * 背景：位拍 I2C 一次 GSTID 读约 210 µs（ct_iic_delay 每步 2 µs × ~105 步）。
 * 原实现每拍都进 gt9xxx_scan()，靠驱动内部"每 10 次才读 1 次"的粗节流兜着 ——
 * 代价是**触摸起始最多要等 9 帧（~130 ms）才被探到**（明显的按压延迟）。
 *
 * 现在：GT9xxx 一有触摸报告就拉 INT(PB13)，EXTI15_10 双边沿中断只做一件事 ——
 * 给 g_tp_int_count 自增（见 HAL_GPIO_EXTI_Callback）。touch_port_poll() 用
 * "中断计数变了 / 正在按住 / 空闲心跳到期"三者之一作为"本拍需读 I2C"的判据：
 *   · 有触摸 → 中断一到，下一拍立刻读（延迟从 ~130 ms 降到 ≤1 帧）；
 *   · 空闲   → 完全不碰 I2C（心跳周期 g_tp_poll_period 帧才读 1 次兜底）。
 * 判据里**故意不依赖 INT 的极性/电平模式**（双边沿 + 按住期间逐帧强制扫描），
 * 所以即便模块 INT 是"低电平保持"而非脉冲，行为也正确。
 *
 * ⚠ 若 EXTI 完全没动静（INT 线没接 / 模块不驱动），退化行为 = 心跳周期帧读 1 次，
 *   与旧版"每 10 帧读 1 次"同量级 ⇒ 最坏情况不劣于改动前（见 g_tp_poll_period）。
 * =========================================================================== */

/* SWD 诊断量 */
volatile uint32_t g_tp_int_count   = 0;  /* EXTI(PB13) 边沿中断累计次数（触碰屏时应增长）*/
volatile uint32_t g_tp_scan_count  = 0;  /* 实际进 I2C 扫描的拍数 */
volatile uint32_t g_tp_poll_skip   = 0;  /* 因门控跳过 I2C 的拍数（空闲时快速增长）*/
/* 空闲心跳周期（帧）。原值 10 ⇒ 若 EXTI 漏沿，按下最多等 160 ms 才被探到
 * （"点了没反应"的直接来源）。降到 4（≈64 ms）：代价只是空闲时每秒多读 ~15 次 I2C
 * （每次约 0.5 ms，≈0.7% CPU），换来的是点击手感。 */
volatile uint32_t g_tp_poll_period = 4;

/* ---- 补丁25e：单帧丢点保护 ----
 * 背景：一次 GSTID+TP1 的位拍 I2C 读约 0.5 ms，本板 I2C **没有外部上拉**
 *   （只有 MCU 内部 ~40 kΩ），自然丢读/丢答是基线常态。旧实现只要某次 scan
 *   返回"无触点"就立刻注入"抬起" ⇒ 拖动中途被切成 按下/抬起/按下…
 *   ⇒ 上层的手势基准每帧重置、dy 恒 ≈0，表现为**完全滑不动**；
 *   同一次假抬起还会被当成一次点击（选中错卡片 / 触发导入）。
 * 现在：读到"无触点"时**先补读**（TP_UP_CONFIRM 次连续无触点才认抬起），
 *   并且由 INT 触发却读空的那一帧额外重试一次。
 * 代价：真实抬起最多晚 ~2 帧（≈32 ms），肉眼不可见。 */
#define TP_UP_CONFIRM 2u
volatile uint32_t g_tp_up_jitter = 0;   /* 被抑制的"单帧无触点"次数（曾经会把拖动打断）*/
volatile uint32_t g_tp_retry_used = 0;  /* INT 触发但读空后的补读次数 */

static uint32_t s_tp_int_seen = 0;       /* 上次 poll 见到的中断计数 */
static uint8_t  s_tp_active   = 0;       /* 1 = 上一拍判为"按住中"（逐帧强制扫描：拖动平滑/抬起可测）*/
static uint32_t s_tp_idle_cnt = 0;       /* 空闲心跳计数 */
static uint32_t s_tp_up_cnt   = 0;       /* 连续"无触点"次数（>=TP_UP_CONFIRM 才认抬起）*/
static uint32_t s_tp_retry    = 0;       /* 剩余强制补读拍数（INT 触发却读空时置 1）*/

/* 合成触摸自测（SWD 写 g_synth_test 触发，不依赖真实手指）：
 *   = 1  单击 (g_synth_x, g_synth_y)          —— 两拍：按下 → 抬起
 *   = 2  从右向左滑（桌面翻到下一页）          —— 按下 → 8 拍移动 → 抬起
 *   = 3  从左向右滑（桌面回到上一页）
 * 起点固定 (285,200)/(25,200)（与宿主验证脚本同一轨迹），每拍走一步。
 * g_synth_done 累计完成的动作数。 */
volatile uint32_t g_synth_test = 0;
volatile uint32_t g_synth_done = 0;
volatile int32_t  g_synth_x    = 160;
volatile int32_t  g_synth_y    = 114;

#define SYNTH_SWIPE_STEPS 8
#define SYNTH_SWIPE_DX    32          /* 8 步 × 32px ≈ 256px，足够触发翻页 */
#define SYNTH_SWIPE_Y     200

static uint32_t s_synth_step  = 0;
static int32_t  s_synth_cur_x = 0;
static int32_t  s_synth_cur_y = 0;

static uint8_t s_pid[4];

/* ---- 触摸 INT(PB13) → EXTI15_10 边沿中断 ----
 * HAL_GPIO_Init 内部会开 SYSCFG 时钟并写 EXTICR（见 stm32h7xx_hal_gpio.c:247），
 * 所以这里只要给 GPIOB 时钟即可。双边沿：不假设模块 INT 的极性/触发模式。 */
static void touch_int_exti_init(void)
{
    GPIO_InitTypeDef gi;

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gi.Pin   = GT9XXX_INT_GPIO_PIN;             /* PB13 */
    gi.Mode  = GPIO_MODE_IT_RISING_FALLING;     /* 双边沿 */
    gi.Pull  = GPIO_PULLUP;                     /* 悬空输入易误触发；上拉在开漏/推挽下都安全 */
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GT9XXX_INT_GPIO_PORT, &gi);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6u, 0u);  /* 比音频 TIM6(5)/SD(5) 低一档：ISR 只自增 */
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

int touch_port_init(void)
{
    g_tp_rc = gt9xxx_init();
    if (g_tp_rc != 0)
    {
        return -1;      /* 白名单不匹配 / I2C 无应答 */
    }

    /* 读产品 ID（寄存器 0x8140，4 字节 ASCII）。这是本阶段的验收判据：
     * 期望读出 "1158"（GT1158）⇒ 打包值 0x31313538。 */
    gt9xxx_rd_reg(GT9XXX_PID_REG, s_pid, 4);
    g_tp_pid = ((uint32_t)s_pid[0] << 24) | ((uint32_t)s_pid[1] << 16) |
               ((uint32_t)s_pid[2] << 8)  |  (uint32_t)s_pid[3];

    /* I2C 通了才开 INT 门控：失败路径下 poll 会因 g_tp_rc != 0 直接返回，
     * 那时开 EXTI 没意义（只会白发中断）。 */
    touch_int_exti_init();
    return 0;
}

/* EXTI15_10（IRQ40）：强定义（弱别名在 src/startup_gcc.s 兜底）。 */
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GT9XXX_INT_GPIO_PIN);
}

/* HAL 的 EXTI 分发回调（覆写 __weak 版本）。ISR 里**只自增**，绝不碰 I2C ——
 * 位拍 I2C 一次要 ~210 µs，放 ISR 会严重拖长中断、干扰音频滴灌与渲染。 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GT9XXX_INT_GPIO_PIN)
    {
        g_tp_int_count++;
    }
}

void touch_port_poll(void)
{
    uint16_t x;
    uint16_t y;

    g_poll_stage = 1;

    if (g_tp_rc != 0)
    {
        g_poll_stage = 99;
        return;             /* 触摸不可用时静默跳过，界面照常显示 */
    }

    /* ---- 合成触摸自测：脚本化走完整条注入链路（按下/移动/抬起）----
     * 与真实触摸无关（不碰驱动），纯粹验"注入→命中→派发→控件响应"是否通。 */
    if (g_synth_test != 0)
    {
        if (g_synth_test == 1u)
        {
            /* 单击：两拍完成 */
            if (s_synth_step == 0u)
            {
                s_synth_cur_x = g_synth_x;
                s_synth_cur_y = g_synth_y;
                YMGUI_Inject_Pointer((GYcoord)s_synth_cur_x, (GYcoord)s_synth_cur_y, 1);
                g_inject_ok++;
                s_synth_step = 1u;
                g_poll_stage = 4;
                return;
            }
            YMGUI_Inject_Pointer((GYcoord)s_synth_cur_x, (GYcoord)s_synth_cur_y, 0);
            g_inject_ok++;
            s_synth_step = 0u;
            g_synth_test = 0u;
            g_synth_done++;
            g_poll_stage = 4;
            return;
        }

        /* 滑动（模式 2/3）：第一拍按下起点 → 中间拍逐步移动 → 出屏前抬起。
         * 每拍只注入一个事件，让 YMGUI 的动画在两拍之间正常推进。
         * （竖直拖拽不在这里做：phone_shell 的 g_bdtap_* 已经支持 dx/dy 拖拽，
         *   拉开/收起控制中心走那条路即可，避免两套等价注入。） */
        {
            int32_t dir = (g_synth_test == 2u) ? -(int32_t)SYNTH_SWIPE_DX
                                               :  (int32_t)SYNTH_SWIPE_DX;
            if (s_synth_step == 0u)
            {
                s_synth_cur_x = (dir < 0) ? 285 : 25;
                s_synth_cur_y = SYNTH_SWIPE_Y;
                YMGUI_Inject_Pointer((GYcoord)s_synth_cur_x, (GYcoord)s_synth_cur_y, 1);
                g_inject_ok++;
                s_synth_step = 1u;
                g_poll_stage = 4;
                return;
            }
            if (s_synth_step <= SYNTH_SWIPE_STEPS)
            {
                s_synth_cur_x += dir;
                YMGUI_Inject_Pointer((GYcoord)s_synth_cur_x, (GYcoord)s_synth_cur_y, 1);
                g_inject_ok++;
                s_synth_step++;
                g_poll_stage = 4;
                return;
            }
            YMGUI_Inject_Pointer((GYcoord)s_synth_cur_x, (GYcoord)s_synth_cur_y, 0);
            g_inject_ok++;
            s_synth_step = 0u;
            g_synth_test = 0u;
            g_synth_done++;
            g_poll_stage = 4;
            return;
        }
    }

    g_poll_stage = 2;

    /* ---- INT 门控：空闲时不碰 I2C（省下每帧 ~210 µs 的位拍事务）----
     * 四个判据满足任一即"本拍要真读 I2C"：
     *   ① 中断计数变了（GT9xxx 说有新报告，紧接着就来）；
     *   ② 上一拍判为按住中（逐帧采样：拖动要平滑，抬起要能及时测到）；
     *   ③ 上一次是"INT 触发却读空" ⇒ 补读一拍（防单帧丢读把按下漏掉）；
     *   ④ 空闲心跳到期（纯兜底：即便 EXTI 没动静，延迟也不劣于旧版）。 */
    {
        uint8_t need = 0;
        uint8_t int_new = 0;

        if (g_tp_int_count != s_tp_int_seen)
        {
            s_tp_int_seen = g_tp_int_count;   /* 用"计数变化"而非"标志位"，避免与 ISR 抢标志 */
            int_new = 1;
            need = 1;
        }
        else if (s_tp_active)
        {
            need = 1;
        }
        else if (s_tp_retry != 0u)
        {
            s_tp_retry--;
            g_tp_retry_used++;
            need = 1;
        }
        else if (++s_tp_idle_cnt >= g_tp_poll_period)
        {
            s_tp_idle_cnt = 0;
            need = 1;
        }

        if (need == 0)
        {
            g_tp_poll_skip++;
            g_poll_stage = 4;
            return;                            /* 空闲：跳过 I2C（上一拍已发过抬起）*/
        }

        g_tp_scan_count++;

        /* gt9xxx_scan(0) = 返回**屏幕坐标**（1 则返回物理坐标）；
         * 返回非 0 表示本次扫描有有效触点，坐标在 tp_dev.x[0] / tp_dev.y[0]。 */
        if (gt9xxx_scan(0) != 0)
        {
            x = tp_dev.x[0];
            y = tp_dev.y[0];

            if (x < YMGUI_PORT_W && y < YMGUI_PORT_H)
            {
                g_poll_stage = 3;
                g_tp_points++;
                g_tp_last_x = x;
                g_tp_last_y = y;
                s_tp_active   = 1;             /* 进入按住：下一拍逐帧强制扫描 */
                s_tp_idle_cnt = 0;
                s_tp_up_cnt   = 0;             /* 有触点 ⇒ 抬起计数清零 */
                s_tp_retry    = 0;
                YMGUI_Inject_Pointer((GYcoord)x, (GYcoord)y, 1);   /* 按下 / 按住移动 */
                g_inject_ok++;
                g_poll_stage = 4;
                return;
            }
        }

        g_poll_stage = 3;

        /* ---- 读到"无触点"：先确认，再认抬起（补丁25e，见 TP_UP_CONFIRM 说明）----
         * ★这一道是"能拖动滑动"的前提：单帧丢读若立刻发抬起，上层的手势就被打断，
         *   dy 每帧归零 ⇒ 永远滑不动（同时假抬起会被当成一次点击）。 */
        s_tp_up_cnt++;
        if (s_tp_up_cnt < TP_UP_CONFIRM)
        {
            g_tp_up_jitter++;
            s_tp_retry = 1;                    /* 下一拍强制再读一次 */
            return;                            /* 本拍不注入抬起：保持原按下状态 */
        }
        if (int_new != 0u)
        {
            /* INT 说有事件却连读两次都空：可能是一次性丢读。再多补读一拍，
             * 但仍按"疑似抬起"处理下一拍（不无限拖着）。 */
            s_tp_retry = 1;
        }

        s_tp_up_cnt = 0;
        s_tp_active = 0;                       /* 本拍无有效触点 */

        if (g_tp_inject_enable == 0)
        {
            g_poll_stage = 4;
            return;
        }

        /* 未按下（或坐标越界）：以最后已知位置发抬起事件。
         * YMGUI 需要这次抬起才会判定 Clicked，只发按下不发抬起按钮永远不会响应。 */
        YMGUI_Inject_Pointer((GYcoord)(g_tp_last_x == 0xFFFF ? 0 : g_tp_last_x),
                             (GYcoord)(g_tp_last_y == 0xFFFF ? 0 : g_tp_last_y),
                             0);                               /* 抬起 */
        g_inject_ok++;
        g_poll_stage = 4;
    }
}
