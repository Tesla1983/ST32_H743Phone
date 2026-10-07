#ifndef __TOUCH_H
#define __TOUCH_H

#include "./SYSTEM/sys/sys.h"

/* ===========================================================================
 * ★移植精简版★
 * 厂商原 touch.h 还 `#include "./BSP/TOUCH/ft5206.h"`（电阻屏驱动）并定义了一整套
 * 电阻屏 SPI 引脚宏。本板只有 GT9xxx 电容屏，所以只保留 gt9xxx.c 真正用到的东西：
 *   - 状态位宏 TP_PRES_DOWN / TP_CATH_PRES
 *   - CT_MAX_TOUCH
 *   - tp_dev 结构体与它的 extern
 * tp_dev 的**实体**定义在本工程的 src/touch_port.c 里。
 * =========================================================================== */

#define TP_PRES_DOWN    0x8000      /* 当前有触摸按下 */
#define TP_CATH_PRES    0x4000      /* 有按键按下标记 */
#define CT_MAX_TOUCH    10          /* 电容屏最多支持的触点数 */

typedef struct
{
    uint8_t (*init)(void);          /* 初始化触摸屏控制器 */
    uint8_t (*scan)(uint8_t);       /* 扫描触摸屏：0=屏幕坐标 1=物理坐标 */
    void (*adjust)(void);           /* 触摸屏校准（电容屏不需要） */

    uint16_t x[CT_MAX_TOUCH];       /* 当前坐标 */
    uint16_t y[CT_MAX_TOUCH];       /* 电容屏最多 10 组坐标 */

    uint16_t sta;                   /* 笔状态：
                                     * b15 按下1/松开0；b14 有按键按下；
                                     * b9~b0 按下的点数（0=未按下） */
    /* 5 点校准参数（电容屏不需要） */
    float xfac;
    float yfac;
    short xc;
    short yc;

    uint8_t touchtype;              /* b0: 0=竖屏 1=横屏；b7: 0=电阻屏 1=电容屏 */
} _m_tp_dev;

extern _m_tp_dev tp_dev;            /* 实体在 src/touch_port.c */

#endif
