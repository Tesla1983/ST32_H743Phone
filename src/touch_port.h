#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H

#include <stdint.h>

/* ===========================================================================
 * 触摸移植桥：厂商 GT9xxx 驱动 → YMGUI 的输入注入
 *
 * 厂商驱动把扫描结果写进全局 tp_dev（x[]/y[]/sta），本文件把 tp_dev.x[0]/y[0]
 * 翻译成 YMGUI_Inject_Pointer(x, y, pressed)。
 *
 * 引脚（本机实测：SCL=PB11 / SDA=PB14 / RST=PB12 / INT=PB13）定义在
 * third_party/BSP/TOUCH/ctiic.h 与 gt9xxx.h 里，已按实测值改好。
 * =========================================================================== */

int  touch_port_init(void);
void touch_port_poll(void);

/* 供 SWD 核对的量 */
extern volatile int32_t  g_tp_rc;        /* gt9xxx_init 返回值，0 = 成功 */
extern volatile uint32_t g_tp_pid;       /* 产品 ID（0x8140 读回的 4 字节 ASCII 打包），期望 0x31313538 = "1158" */
extern volatile uint32_t g_tp_points;    /* 累计识别到的有效触摸点数（手指按屏时增长） */
extern volatile uint16_t g_tp_last_x;    /* 最近一次触摸坐标 */
extern volatile uint16_t g_tp_last_y;

extern volatile uint32_t g_inject_ok;    /* YMGUI_Inject_Pointer() 成功返回的次数。
                                          * ★这是"注入链路不再卡死"的直接证据★：
                                          * 故障时它不再增长（且 g_fault.magic 会置起）。 */

/* ---- 合成触摸自测（无需真的碰屏，用 SWD 触发端到端验证）----
 * 验证整条链路：注入 → 命中测试 → 事件派发 → 控件响应 → 标脏重绘。
 *   g_synth_test = 1  单击 (g_synth_x, g_synth_y)，两拍完成
 *                = 2  从右向左滑（桌面翻到下一页，~10 拍）
 *                = 3  从左向右滑（桌面回到上一页）
 *   完成后 g_synth_test 自动清 0、g_synth_done +1。
 * （本工程曾把一次 BusFault 误判为"死循环"，所以这类**能给出确定整数结论**的
 *   自测比"看图对不对"可靠得多。） */
extern volatile uint32_t g_synth_test;   /* 写入动作编号（见上），完成自动清 0 */
extern volatile uint32_t g_synth_done;   /* 已完成的合成动作数 */
extern volatile int32_t  g_synth_x;      /* 单击模式 1 的落点 */
extern volatile int32_t  g_synth_y;

#endif /* TOUCH_PORT_H */
