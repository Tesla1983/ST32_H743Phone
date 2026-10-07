/* ===========================================================================
 * UI 设置持久化实现 —— 设计说明见 src/app_config.h 顶部
 * =========================================================================== */

#include "app_config.h"
#include "qspi_port.h"

#include <string.h>

/* 扇区里只写前 32 字节，其余保持 0xFF（便于将来在同一扇区追加配置项） */
#define APP_CFG_MAGIC     0x59434F4Eu    /* "YCON" */
#define APP_CFG_VERSION   1u
#define APP_CFG_BLOB_LEN  32u

/* 屏幕亮度取值范围与出厂默认（2026-10-04 用户决策：默认 50、且可持久化）。
 * ⚠ MIN 取 1 而不是 0：brightness=0 会让 phone_flush 叠一层 alpha=160 的
 *   全屏黑罩，屏上什么都看不见（"屏坏了"与"亮度调到 0"无法区分）。
 *   越界值（含旧扇区里从没写过的 0）一律回退到默认。 */
#define APP_CFG_BRIGHTNESS_DEFAULT 50u
#define APP_CFG_BRIGHTNESS_MIN      1u
#define APP_CFG_BRIGHTNESS_MAX    100u

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t lang;          /* APP_CFG_LANG_ZH / _EN */
    uint32_t brightness;    /* 预留（阶段 2 可能启用） */
    uint32_t theme;         /* 预留（"海蓝壁纸"目前只在内存，不落盘） */
    /* ★2026-10-04 占用 reserved[0] 做"壁纸 ribbon 半透明"。
     *
     * 【为什么占 reserved[0] 而不是 theme】theme 是"海蓝壁纸"的预留字段，
     * 那个功能将来要做持久化时会用到。若把半透明塞进 theme，该字段就变成
     * 位域，将来加壁纸持久化必须改结构 ⇒ 已落盘的旧扇区读出来会错位。
     * 占 reserved[0] 则：字段语义各自独立、加字段不动结构、不升版本。
     *
     * 【为什么能这样做】blob 里只有 3 个字（magic/version/lang）会随设置变化，
     * 整块 memcmp 判"值未变"因此天然覆盖新字段 —— 改了半透明开关一样能触发落盘，
     * 且值相同照样跳过擦除（见 AppConfig_Save 的 memcmp）。 */
    uint32_t ribbon_translucent;   /* 0=不透明(快) 1=半透明/透光。★默认 1 */
    uint32_t reserved[1];
    uint32_t crc;           /* 覆盖 crc 之前的全部字节 */
} AppConfigBlob;

/* ---- 当前内存中的设置（DTCM .bss）---- */
static AppConfigBlob s_cur;        /* 当前生效值 */
static AppConfigBlob s_flashed;    /* 最近一次成功落盘的值，用于"值未变则不写" */
static uint32_t      s_have_flashed;

/* ---- 诊断量 ---- */
volatile uint32_t g_cfg_rc         = 0xFFFFFFFFu;
volatile uint32_t g_cfg_loaded     = 0;
volatile uint32_t g_cfg_magic_read = 0;
volatile uint32_t g_cfg_crc_ok     = 0;
volatile uint32_t g_cfg_lang       = APP_CFG_LANG_ZH;
volatile uint32_t g_cfg_save_cnt   = 0;
volatile uint32_t g_cfg_dirty      = 0;
volatile uint32_t g_cfg_ribbon     = 1;   /* 1=半透明（默认）；开关当前值 */
volatile uint32_t g_cfg_brightness = APP_CFG_BRIGHTNESS_DEFAULT; /* 0..100，出厂 50 */
/* SWD 写 1 → 擦掉配置扇区（恢复出厂），完成自动清 0。结果看 g_cfg_factory_rc。 */
volatile uint32_t g_cfg_factory_reset = 0;
volatile uint32_t g_cfg_factory_rc    = 0xFFFFFFFFu;
volatile uint32_t g_cfg_write_rc   = 0xFFFFFFFFu;
volatile uint32_t g_cfg_erase_last = 0;

/* ---- CRC32（IEEE 802.3，反射，多项式 0xEDB88320）----
 * 与 src/font_provision.c 同一算法（16 项半字节表，结果与 zlib.crc32 一致）。
 * 两处各自持有一份 static 表而不共享：这张表只有 64 B rodata，
 * 为它引入跨模块依赖不划算。 */
static const uint32_t s_crc_nib[16] =
{
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
    }
    return crc;
}

static uint32_t blob_crc(const AppConfigBlob *b)
{
    return crc32_update(0xFFFFFFFFu, (const uint8_t *)b,
                        (uint32_t)((const uint8_t *)&b->crc - (const uint8_t *)b))
           ^ 0xFFFFFFFFu;
}

/* 把默认值填进结构（中文 = 出厂默认）
 *
 * ★ ribbon_translucent 默认 1（半透明/透光）—— 用户 2026-10-04 决策。
 * 理由：半透明是最初的设计意图（壁纸渐变要透出来），
 * 开关存在的前提就是它曾经是默认；默认关会让用户困惑"为什么这个功能默认是关的"。
 * 代价是满负载帧率从 48 FPS 回到 41 FPS（α 混合 176877 px × 17.33 cy/px ≈ 7.66 ms），
 * 这是用户主动选的视觉优先取舍，不是缺陷。 */
static void blob_defaults(AppConfigBlob *b)
{
    memset(b, 0, sizeof(*b));
    b->magic      = APP_CFG_MAGIC;
    b->version    = APP_CFG_VERSION;
    b->lang       = APP_CFG_LANG_ZH;
    /* ★ brightness 默认 50（半亮）—— 用户 2026-10-04 决策。
     * 理由：纯硬件直驱这块屏在全亮下看久了很刺眼，出厂给个中档更合适；
     * 且它是**可持久化**项，用户拖过之后重启保持，不会被默认值覆盖。 */
    b->brightness = APP_CFG_BRIGHTNESS_DEFAULT;
    b->theme      = 0u;
    b->ribbon_translucent = 1u;
    b->crc        = blob_crc(b);
}

void AppConfig_SetLang(uint32_t lang)
{
    uint32_t v = (lang == APP_CFG_LANG_EN) ? APP_CFG_LANG_EN : APP_CFG_LANG_ZH;

    if (s_cur.lang != v)
    {
        s_cur.lang  = v;
        s_cur.crc   = blob_crc(&s_cur);
        g_cfg_lang  = v;
        g_cfg_dirty = 1u;
    }
}

uint32_t AppConfig_GetLang(void)
{
    return s_cur.lang;
}

/* ---- 壁纸 ribbon 半透明（2026-10-04 新增）----
 *
 * 【语义】1 = 半透明（壁纸渐变透出来，用户 2026-10-04 定的默认）
 *         0 = 不透明（ribbon 是实色，帧率高 3.5~5 ms）
 * ⚠ 与 phone_shell.c 里的 g_ribbon_translucent 语义一致（都是 1=半透明），
 *   历史上那个变量叫 g_ribbon_opaque 且 1=不透明，已翻转 —— 别再混用。 */
void AppConfig_SetRibbonTranslucent(uint32_t on)
{
    uint32_t v = (on != 0u) ? 1u : 0u;

    if (s_cur.ribbon_translucent != v)
    {
        s_cur.ribbon_translucent = v;
        s_cur.crc   = blob_crc(&s_cur);
        g_cfg_ribbon = v;
        g_cfg_dirty  = 1u;
    }
}

uint32_t AppConfig_GetRibbonTranslucent(void)
{
    return s_cur.ribbon_translucent;
}

/* ---- 屏幕亮度（2026-10-04 启用，原为预留字段）----
 *
 * 【语义】0..100 的整数，直接对应 phone_shell.c 的 display_brightness：
 *   100 = 不加暗罩（正常）；<100 时 phone_flush 叠一层黑色半透明罩，
 *   罩的 alpha = (100 - brightness) * 160 / 100。
 *   ⚠ 160/100 是屏侧的固定系数（不是 255），所以 brightness 线性
 *     不等于亮度线性 —— 那是屏的 gamma，不是本文件的 bug。
 *
 * 【为什么默认值是 50 而不是 100】用户 2026-10-04 明确要求：
 *   出厂默认调到 50（半亮），并且用户拖过之后要能保持。
 *
 * 【为什么不升 blob 版本】brightness 字段本来就在结构里（预留位），
 * 改的只是"默认值的来源"和"是否落盘"，**结构布局一个字节没动** ⇒
 * 已落盘的旧扇区（brightness=0）读出来仍然合法，不需要迁移。
 * ⚠ 但旧扇区的 brightness 是 0（从没写过）⇒ 加载时会被下面的
 *   有效区间检查挡掉、回退到默认 50。这正是想要的行为。 */
static uint32_t brightness_valid(uint32_t v)
{
    if (v < APP_CFG_BRIGHTNESS_MIN || v > APP_CFG_BRIGHTNESS_MAX)
    {
        return APP_CFG_BRIGHTNESS_DEFAULT;
    }
    return v;
}

void AppConfig_SetBrightness(uint32_t v)
{
    uint32_t nv = brightness_valid(v);

    if (s_cur.brightness != nv)
    {
        s_cur.brightness = nv;
        s_cur.crc         = blob_crc(&s_cur);
        g_cfg_brightness  = nv;
        g_cfg_dirty       = 1u;
    }
}

uint32_t AppConfig_GetBrightness(void)
{
    return s_cur.brightness;
}

uint32_t AppConfig_IsDirty(void)
{
    return g_cfg_dirty;
}

void AppConfig_Load(void)
{
    uint8_t        raw[APP_CFG_BLOB_LEN];
    AppConfigBlob  tmp;
    int            rc;

    /* 先铺默认值 —— 后面任何一步失败都保持"中文可用"，绝不因为读不到配置就不启动 */
    blob_defaults(&s_cur);

    /* 扇区地址与结构长度必须匹配，否则下面按 sizeof 读会越界到别处 */
    if (APP_CFG_BLOB_LEN < (uint32_t)sizeof(AppConfigBlob))
    {
        g_cfg_rc = 0xFFFFFFFEu;      /* 编译期配置错误：Blob 比扇区预留区还大 */
        g_cfg_lang = s_cur.lang;
        g_cfg_ribbon = s_cur.ribbon_translucent;
        g_cfg_brightness = s_cur.brightness;
        return;
    }

    memset(raw, 0, sizeof(raw));
    rc = qspi_read(raw, APP_CFG_SECTOR_ADDR, APP_CFG_BLOB_LEN);
    g_cfg_rc = (uint32_t)rc;

    /* ⚠ qspi_read 内部退出了映射模式，必须重新进 —— 否则中文字模读不到 */
    (void)qspi_enter_mmap();

    if (rc != QSPI_OK)
    {
        g_cfg_loaded = 0u;
        /* g_cfg_ribbon 保持默认；下面统一在成功路径末尾同步一次 */
        g_cfg_lang   = s_cur.lang;
        g_cfg_ribbon = s_cur.ribbon_translucent;
        g_cfg_brightness = s_cur.brightness;
        return;
    }

    memcpy(&tmp, raw, sizeof(tmp));
    g_cfg_magic_read = tmp.magic;

    if (tmp.magic != APP_CFG_MAGIC)
    {
        g_cfg_loaded = 0u;
        /* g_cfg_ribbon 保持默认；下面统一在成功路径末尾同步一次 */           /* 空扇区（首次上电是 0xFF）或格式不符 */
        g_cfg_lang   = s_cur.lang;
        g_cfg_brightness = s_cur.brightness;
        return;
    }

    if (tmp.crc != blob_crc(&tmp))
    {
        g_cfg_loaded = 0u;
        /* g_cfg_ribbon 保持默认；下面统一在成功路径末尾同步一次 */           /* 掉电写坏 / 位翻转 */
        g_cfg_crc_ok = 0u;
        g_cfg_lang   = s_cur.lang;
        g_cfg_brightness = s_cur.brightness;
        return;
    }

    g_cfg_crc_ok = 1u;
    g_cfg_loaded = 1u;

    /* 校验通过：采纳 flash 里的值（版本号兼容处理见下） */
    if (tmp.version == APP_CFG_VERSION)
    {
        s_cur = tmp;
    }
    else
    {
        /* 版本不符：保守地只迁移已知字段，其余留默认。
         * 目前只有 v1，这里先原样采纳 lang，避免将来加字段时读到垃圾值。 */
        s_cur.version = APP_CFG_VERSION;
        s_cur.lang    = (tmp.lang == APP_CFG_LANG_EN) ? APP_CFG_LANG_EN : APP_CFG_LANG_ZH;
        s_cur.crc     = blob_crc(&s_cur);
    }

    /* ⚠ brightness 区间钳制放在采纳之后、s_flashed 之前。
     *   旧扇区里这个字段从没写过（=0），或将来被写坏，直接采纳会得到
     *   "全黑屏"（alpha=160 的黑罩）且被当成合法值记进 s_flashed，
     *   之后 Save 的 memcmp 会认为"没变"而永不修正 ⇒ 永久黑屏。
     *   所以：越界即回退默认，并让钳制后的结果参与后面的 s_flashed 快照。 */
    s_cur.brightness = brightness_valid(s_cur.brightness);
    /* ⚠ 钳制动了内容就必须重算 crc —— 否则 s_flashed 快照带着"旧 crc"，
     *   下次 Save 写出去的 blob 会 crc != blob_crc(blob)，
     *   重启后 AppConfig_Load 的 CRC 校验直接判失败 ⇒ 整套设置全丢。
     *   （本项目的 CRC 覆盖 crc 之前的全部字节，见 blob_crc。） */
    s_cur.crc = blob_crc(&s_cur);

    /* 记下"已落盘值"，这样开机后如果用户不动它，Save 会被跳过 */
    s_flashed     = s_cur;
    s_have_flashed = 1u;
    g_cfg_dirty   = 0u;
    g_cfg_lang    = s_cur.lang;
    g_cfg_ribbon  = s_cur.ribbon_translucent;
    g_cfg_brightness = s_cur.brightness;
}

void AppConfig_Save(void)
{
    int      rc;
    uint32_t erase_before = g_qspi_erase_cnt;

    if (s_have_flashed != 0u &&
        (memcmp(&s_cur, &s_flashed, sizeof(AppConfigBlob)) == 0))
    {
        /* 与 flash 里那份逐字节相同 ⇒ 不写。
         * 这一层判断是为了让"打开设置页又原样返回"之类操作零磨损
         * （qspi_write 自己也会跳过擦除，但那次仍要走完整扇区读 + 比对）。 */
        g_cfg_dirty      = 0u;
        g_cfg_erase_last = 0u;
        return;
    }

    s_cur.crc = blob_crc(&s_cur);

    rc = qspi_write((const uint8_t *)&s_cur, APP_CFG_SECTOR_ADDR, (uint32_t)sizeof(s_cur));
    g_cfg_write_rc = (uint32_t)rc;

    /* ⚠ 同上：写完也必须重新进映射，否则字库读不到 */
    (void)qspi_enter_mmap();

    if (rc == QSPI_OK)
    {
        s_flashed      = s_cur;
        s_have_flashed = 1u;
        g_cfg_save_cnt++;
        g_cfg_dirty = 0u;
    }

    g_cfg_erase_last = g_qspi_erase_cnt - erase_before;
}

/* ---- 恢复出厂设置（2026-10-04 新增，供 SWD 触发）----
 *
 * 【为什么必须有它】出厂默认亮度是 50，但要**验证**它就得让扇区回到
 * "从没写过"的状态。而扇区里若存着 brightness=100（上一轮旧固件写的，
 * 上轮验收脚本最后会把半透明恢复成默认、连带把整块 blob 重新落盘），
 * 那 100 在 1..100 区间内、钳制逻辑正确地不会改它 —— 也就是说
 * "默认值 50"永远等不到生效，只能靠人为构造。
 *
 * ⚠ 为什么不能从主机侧擦：配置在 W25Q128 的 XIP 映射区（0x900F0000），
 *   那个窗口是**只读**的，probe-rs 往那儿写会报
 *   "Failed to write register DRW"（试过）。必须让固件自己调
 *   qspi_erase_sector —— 它会先退出映射、擦完再把映射打开。
 *
 * 【安全性】只擦 APP_CFG_SECTOR_NO 这一个扇区（4 KB），该扇区只存
 *   32 字节 blob，其余是预留空间。擦完等价于"设置回默认值"，
 *   没有任何不可恢复的数据 —— 不碰字库、不碰其它扇区。
 *
 * 【用法】SWD 写 1 到 g_cfg_factory_reset ⇒ 下一拍自动清 0。
 *   结果看 g_cfg_factory_rc（0=成功）。重启后 g_cfg_loaded 会变 0。 */
void AppConfig_FactoryReset(void)
{
    int      rc;
    uint32_t erase_before = g_qspi_erase_cnt;

    g_cfg_factory_rc = 0xFFFFFFFFu;

    rc = qspi_erase_sector(APP_CFG_SECTOR_NO);

    /* ⚠ 与 Save/Load 同理：擦除操作会退出 XIP 映射，必须重新打开，
     *   否则中文字库（0x9000_0000）读不到 ⇒ 整屏中文消失。 */
    (void)qspi_enter_mmap();

    g_cfg_factory_rc = (uint32_t)rc;
    g_cfg_erase_last = g_qspi_erase_cnt - erase_before;

    if (rc == QSPI_OK)
    {
        /* 内存里的值也回到默认，并清 dirty —— 否则下一次 Save 会把
         * 刚擦掉的旧配置又写回去。 */
        blob_defaults(&s_cur);
        s_flashed      = s_cur;
        s_have_flashed = 1u;    /* 认为"当前值已在 flash"，避免立刻重写 */
        g_cfg_dirty   = 0u;
    }
    /* 诊断量同步：让 SWD 一眼能看出当前生效值就是出厂默认 */
    g_cfg_lang       = s_cur.lang;
    g_cfg_ribbon     = s_cur.ribbon_translucent;
    g_cfg_brightness = s_cur.brightness;
}
