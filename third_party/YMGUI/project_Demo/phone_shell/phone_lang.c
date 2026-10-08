/* ===========================================================================
 * phone_shell 界面语言实现 —— 设计说明见 phone_lang.h 顶部
 *
 * 本文件是"阶段 1"的翻译覆盖范围：
 *   设置页 · 桌面（含小组件/菜单/卸载对话框）· 底部导航与 Dock ·
 *   16 个应用标题 · 开机/关机/最近任务/提示层
 * 其余 17 个 APP 的内部文案属阶段 2，暂不在表内 —— 它们会原样显示中文
 * （`PhoneLang_Tr` 查不到就返回原文），不会出现空白。
 * =========================================================================== */

#include "phone_lang.h"

#include <string.h>

typedef struct
{
    const char* zh;     /* 中文原文（调用点写的就是它） */
    const char* en;     /* 英文译文 */
} PhoneLangEntry;

/* 说明：
 *  · 表内**不需要** "中文" → "中文" 这类自映射项（中文模式下根本不查表）；
 *    但语言卡片上的分段按钮要显示 "中文 / English" 两个词，那两个词在两种语言下
 *    写法相同，所以也不进表 —— 由控件直接写常量。
 *  · 含 `\n` 的多行文案照原样写，与调用点逐字节一致才查得到（否则退化为原文）。 */
static const PhoneLangEntry s_dict[] =
{
    /* ---- 设置页 ---- */
    { "设置",                   "Settings" },
    { "把手机调成喜欢的样子",   "Make it yours" },
    { "无线网络",               "Wi-Fi" },
    { "Yaomi 工作室",           "Yaomi Studio" },
    { "未连接",                 "Not connected" },
    { "海蓝色壁纸",             "Blue wallpaper" },
    { "海蓝",                   "Ocean" },
    { "暮色",                   "Dusk" },
    { "预览亮度",               "Brightness" },
    { "关机...",                "Power off..." },
    { "界面语言",               "Language" },
    { "切换后立即生效",         "Applies at once" },
    /* 半透明卡片（2026-10-04 新增）。标题是功能名，副标题是当前效果，
     * 英文侧同样拆成两行，避免 "Translucent: On" 这种读不通的直译。 */
    { "半透明",                 "Translucency" },
    { "透出",                   "Show-through" },
    { "实色",                   "Solid" },

    /* ---- 桌面：长按菜单 / 卸载对话框 ---- */
    { "桌面管理",               "Desktop" },
    { "应用信息",               "App info" },
    { "知道了",                 "Got it" },
    { "移除演示应用",           "Remove demo app" },
    { "取消",                   "Cancel" },
    { "卸载",                   "Remove" },
    { "恢复默认桌面",           "Restore default" },
    { "关闭应用并移除桌面入口。\n不会删除源码或电脑上的程序。\n长按桌面空白处可恢复。",
      "Removes the app and its desktop entry.\nSource code is not deleted.\nLong-press the desktop to restore." },
    { "%s\n模块：%s\n本地界面演示，数据保留于本次运行。",
      "%s\nModule: %s\nLocal UI demo, data kept for this session." },

    /* ---- 桌面小组件（日期 / 欢迎语 / 天气 / 音乐卡片）---- */
    { "9月29日  星期二",        "Sep 29  Tue" },
    { "给生活留一点空白",       "Leave a little room" },
    { "多云 / 22 C",            "Cloudy / 22 C" },
    /* 上行链路未同步时 board 层给这两个占位串（真实数据查不到表 ⇒ 原样显示） */
    { "未同步",                 "Not synced" },
    { "等待天气",               "Waiting for weather" },
    { "晚间海浪",               "Evening waves" },
    { "Yaomi 音乐 / 播放中",    "Yaomi Music / Playing" },
    { "Yaomi 音乐 / 已暂停",    "Yaomi Music / Paused" },

    /* ---- 开机 / 关机 / 重启 ---- */
    { "正在启动",               "Starting" },
    { "点亮你的小小世界",       "Light up your little world" },
    { "正在关机",               "Powering off" },
    { "已关机",                 "Powered off" },
    { "重新开机",               "Power on again" },
    { "确认关机？",             "Power off?" },
    { "关闭演示，稍后可以重新开机。", "Closes the demo. You can power on again later." },
    { "关机",                   "Power off" },

    /* ---- 最近任务 ---- */
    { "最近应用",               "Recent apps" },
    { "全部清理",               "Clear all" },
    { "已清理，轻装出发。",     "Cleared. Ready to go." },
    { "%d 个应用已打开",        "%d apps open" },

    /* ---- 提示层 / 欢迎 ---- */
    { "关闭弹窗",               "Close" },
    { "返回桌面",               "Home" },
    /* 注：笔记正文（"你好，Yaomi。\n记录今天的小想法。"）**不进本表** ——
     *   它是 app 内容（模拟已有笔记），不是 UI 文案，属阶段 2 范围。
     *   放进来会变成"表里有但没人引用"的死条目（tools/check_lang_keys.py 会报）。 */

    /* ---- 16 个应用标题 ----
     * 注意用词以源码里的实际 .title 为准（是"待办"不是"任务"，是"文件管理"不是"文件管理器"）。
     * 这些 title 被 5 处引用（桌面标签 / 长按菜单 / 应用信息框 / 应用页标题 / Dock），
     * 全部通过 `T(PhoneApps_Get(id)->title)` 取，因此不必给 PhoneApp 结构加字段。 */
    { "待办",                   "Tasks" },
    { "音乐",                   "Music" },
    { "相册",                   "Gallery" },
    { "时钟",                   "Clock" },
    { "天气",                   "Weather" },
    { "笔记",                   "Notes" },
    { "计算器",                 "Calculator" },
    { "电话",                   "Phone" },
    { "短信",                   "Messages" },
    { "通讯录",                 "Contacts" },
    { "录音机",                 "Recorder" },
    { "文件管理",               "Files" },
    { "陀螺仪",                 "Gyroscope" },
    { "相机",                   "Camera" },
    { "浏览器",                 "Browser" },
};

#define DICT_N  ((uint32_t)(sizeof(s_dict) / sizeof(s_dict[0])))

static uint8_t             s_lang    = PHONE_LANG_ZH;
static uint32_t            s_version = 1u;
static PhoneLang_persist_cb s_persist = NULL;

void PhoneLang_SetPersistCallback(PhoneLang_persist_cb cb)
{
    s_persist = cb;
}

void PhoneLang_Init(uint8_t initial_lang)
{
    s_lang    = (initial_lang == PHONE_LANG_EN) ? PHONE_LANG_EN : PHONE_LANG_ZH;
    s_version = 1u;
}

uint8_t PhoneLang_Get(void)
{
    return s_lang;
}

uint32_t PhoneLang_Version(void)
{
    return s_version;
}

void PhoneLang_Set(uint8_t lang)
{
    uint8_t v = (lang == PHONE_LANG_EN) ? PHONE_LANG_EN : PHONE_LANG_ZH;

    if (v == s_lang)
    {
        return;                     /* 值没变：不回调、不加版本号（避免无谓落盘与刷新） */
    }

    s_lang = v;
    s_version++;

    if (s_persist != NULL)
    {
        s_persist(v);               /* 只在这里落盘，切来切去才不会有冗余写入 */
    }
}

const char* PhoneLang_Tr(const char* zh)
{
    uint32_t i;

    if (zh == NULL)
    {
        return "";
    }

    /* 中文模式：原文即结果。**不查表** ⇒ 中文用户零额外开销。 */
    if (s_lang == PHONE_LANG_ZH)
    {
        return zh;
    }

    /* 英文模式：线性查表。表约 50 条，且只在"创建 / 刷新"期调用（不是每帧绘制），
     * 因此不做哈希/二分 —— 可读性优先。 */
    for (i = 0u; i < DICT_N; i++)
    {
        if (strcmp(zh, s_dict[i].zh) == 0)
        {
            return s_dict[i].en;
        }
    }

    /* 表里没有（属阶段 2 范围，或调用点拼写与原文不一致）：
     * 返回中文原文 —— 宁可显示中文，也不要空白/乱码。 */
    return zh;
}
