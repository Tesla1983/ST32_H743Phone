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
    return 0;
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
         * 每拍只注入一个事件，让 YMGUI 的动画在两拍之间正常推进。 */
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
            YMGUI_Inject_Pointer((GYcoord)x, (GYcoord)y, 1);   /* 按下 */
            g_inject_ok++;
            g_poll_stage = 4;
            return;
        }
    }

    g_poll_stage = 3;

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
