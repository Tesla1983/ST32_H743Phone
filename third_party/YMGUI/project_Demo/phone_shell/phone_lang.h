/* ===========================================================================
 * phone_shell 的界面语言（中 / 英）
 *
 * 【为什么以"中文原文"为 key】
 *   界面上原有的 254 条文案全是硬编码中文。若改成"每条一个枚举 ID"，
 *   就要动全部调用点、还要单独维护一张 ID 表，改动面等于文案总数。
 *   改成 `T("设置")` 这种**原文查表**后：中文模式下直接返回原文（零查表），
 *   英文模式下查表替换；而 `PhoneApps_Get(id)->title` 这类**本来就指向中文原文**
 *   的表达式可以直接包一层 `T()` 就能工作，不必为 16 个应用各加一个字段。
 *
 * 【分层】本文件**不依赖任何 flash / 板级代码**，因此桌面（SDL）构建同样可用。
 *   持久化通过 `PhoneLang_SetPersistCallback()` 由板级注入：
 *     板级在 main 里注册 `AppConfig` 的写入口，切换语言时自动落盘；
 *     桌面构建不注册，就退化成"本次运行有效"。
 *
 * 【惰性刷新】`PhoneLang_Version()` 每次切换自增。各 APP 在 `show()` 里比对这个
 *   版本号，变了才刷新自己那些**创建期设定**的文案 —— 避免每次进应用都全量重设。
 *   （绘制期从对象里取文案的控件不需要处理：它们拿到的已经是翻译后的副本。）
 *
 * 【翻译表找不到怎么办】返回中文原文。宁可显示中文，也不要显示空白或乱码。
 * =========================================================================== */

#ifndef PHONE_LANG_H
#define PHONE_LANG_H

#include <stdint.h>

#define PHONE_LANG_ZH   0u
#define PHONE_LANG_EN   1u

/* 持久化回调：由板级注入（写 W25Q128）。传 NULL 解除。
 * 只在语言**真的变化**时被调用一次。 */
typedef void (*PhoneLang_persist_cb)(uint8_t lang);

void PhoneLang_SetPersistCallback(PhoneLang_persist_cb cb);

/* 初始化：板级把从 flash 读到的语言传进来。桌面构建传 PHONE_LANG_ZH 即可。 */
void PhoneLang_Init(uint8_t initial_lang);

uint8_t PhoneLang_Get(void);

/* 切换语言：改状态 → 通知持久化 → 版本号 +1。
 * 传非法值按中文处理。语言未变时不做任何事（不回调、不加版本号）。 */
void PhoneLang_Set(uint8_t lang);

/* 语言版本号（从 1 开始，每次真正切换 +1）。供各 APP 做惰性刷新判断。 */
uint32_t PhoneLang_Version(void);

/* 中文原文 → 当前语言的文案。返回的指针要么是入参本身（中文模式/查不到），
 * 要么是表里的静态英文串 —— 两种都无需释放，可长期持有。 */
const char* PhoneLang_Tr(const char* zh);

/* 逐点替换用的短别名：`T("设置")` */
#define T(s) PhoneLang_Tr(s)

#endif /* PHONE_LANG_H */
