#ifndef YAOMI_PHONE_LOCALE_H
#define YAOMI_PHONE_LOCALE_H

void PhoneLocale_Init(void);
void PhoneLocale_Shutdown(void);

/* 板级专用：外部字库（QSPI 映射区的 GB2312 字模）是否可用。
 * 桌面路径靠 fopen 自己判断，不需要调用本函数（默认即"可用"）。
 * 必须在 PhoneLocale_Init() 之前调用。 */
void PhoneLocale_SetExternalFontReady(int ready);

#endif
