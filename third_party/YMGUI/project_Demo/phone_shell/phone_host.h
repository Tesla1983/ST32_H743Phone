#ifndef PHONE_HOST_H
#define PHONE_HOST_H

#include "phone_app.h"

/* APP 只能经这些服务操作桌面，不引用桌面全局变量或其他 APP 的控件。 */
int PhoneHost_Open(int id, const char* argument);
void PhoneHost_Close(void);
void PhoneHost_Power(void);
void PhoneHost_Notice(const char* title, const char* message);
void PhoneHost_SetTheme(int alternate);
void PhoneHost_SetBrightness(int value);
int PhoneHost_GetBrightness(void);

/* 亮度"只预览不落盘"（2026-10-04，与 SetBrightness 语义分离）
 *
 * 【为什么需要它】滑杆的回调在 Pressed 和每一次 Pressing 都触发，
 *   一次拖动会经过 5 个不同的中间值；而 AppConfig_Save 是"值变就擦一个
 *   扇区" ⇒ 若拖动中就走 SetBrightness，板级实测一次拖动擦 6 次
 *   W25Q128 扇区（标称 10 万次/扇区）。
 *   所以约定：**拖动中走本函数（只改显示），松手时走
 *   PhoneHost_SetBrightness（落盘一次）**。
 *
 * ⚠ 与 SetBrightness 一样带"值相同直接 return"和 1..100 钳位。
 * ⚠ 本函数**不发** PhoneApps_Command(SETTINGS,"sync")：那条命令会调到
 *   app_show 把滚动位置复位成 scroll_y=0，拖动中发等于"用户正拖着滑杆、
 *   页面却弹回顶部" ⇒ 滑杆屏幕坐标从 330 跳到 465，后续位移全部落空。
 * ⚠ 本函数会置内部"用户动过"标志。**必须在拖动中调用它**、而不是直接调
 *   SetBrightness，否则松手那次 SetBrightness 会因为"值与 Preview 设的相同"
 *   而跳过落盘（这个坑 2026-10-04 实际踩过，现象是亮度永远存不下来）。
 * ⚠ 允许在 ctx 还没建立时调用（内部有 NULL 守卫）。 */
void PhoneHost_SetBrightnessPreview(int value);
void PhoneHost_SetWifi(int on);
int PhoneHost_GetWifi(void);

/* 壁纸 ribbon 半透明（2026-10-04 新增）
 *
 * 【语义】on = 1 半透明（壁纸渐变透出来，出厂默认，满负载 ~41 FPS）
 *         on = 0 不透明（ribbon 实色，满负载 ~48 FPS）
 * 值真的变化时才置脏并落盘，重复设同一个值是无操作。
 * ⚠ 历史上 phone_shell.c 里的变量叫 g_ribbon_opaque 且 1=不透明，
 *   已翻转成 g_ribbon_translucent（1=半透明）以与本函数和 UI 开关语义一致。 */
void PhoneHost_SetRibbonTranslucent(int on);
int  PhoneHost_GetRibbonTranslucent(void);
/* 同步复制未调暗的当前手机画面；调用方提供 320*480 native 像素。 */
void PhoneHost_Capture(GYpx* pixels);
/* 抓屏的降采样倍数（0 = 原尺寸 320×480，2 = 1/4 即 80×120）。
 *
 * 为什么需要它：全尺寸抓屏缓冲要 300 KB，小 RAM 目标申请不到（本板 heap1 只有
 * 212 KB）。调用方按**自己实际申请到的缓冲大小**设置倍数，抓屏侧据此用相同步长写，
 * 两边才不会错位。用完请立刻设回 0，避免影响别人的全尺寸抓屏（自检里就有）。 */
void PhoneHost_SetCaptureShift(int shift);
void PhoneHost_MusicChanged(int playing);
/* 语言切换后的全局重设：把"常驻页面"上创建期设定的文案重新按当前语言写一遍。
 * 设置页切换语言后调用一次。各 APP 内部文案由它们自己的 show() 惰性刷新。 */
void PhoneHost_RefreshLanguage(void);

#endif
