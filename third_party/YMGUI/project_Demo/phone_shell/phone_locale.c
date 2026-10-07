/* ===========================================================================
 * phone_shell 的本地化/中文字体挂接
 *
 * 两条路径，靠 PHONE_LOCALE_XIP_BASE 二选一：
 *
 *   桌面（未定义该宏）—— 原有路径：fopen(gb2312_glyphs.bin) + fseek/fread 读回调。
 *                        宿主验证就是走这条，行为**未改动**。
 *
 *   板级（定义 PHONE_LOCALE_XIP_BASE=0x90000000）—— 计划书 §6.5 的方案一：
 *                        字模已经在 QSPI 的内存映射区，直接
 *                        `bitmap = 0x9000_0000` + `glyph_read = NULL`，
 *                        库按 `bitmap + off` 取点阵，**一次拷贝都不做**。
 *                        字模由 src/font_provision.c 灌入（第 i 字在 i×128）。
 *
 * 为什么不用 glyph_read 回调走 QSPI：回调那条路是给"非内存映射的 SPI flash"准备的，
 * 每个字要先拷 128 B 进 RAM 再画。映射区上没必要，且省下的正是这一趟。
 * =========================================================================== */

#include "phone_locale.h"
#include "YMGUI_Font.h"

extern const uint16 YMGUI_GB2312_cps[], YMGUI_GB2312_glyph_count;

static GYFONT old_fallback;

/* 板级可选：告知"外部字库是否真的可用"。
 * 桌面路径靠 fopen 自己判断；板级没有 fopen，由板级 main 在
 * font_provision_ensure() 之后显式设置。默认 1 —— 于是桌面行为完全不变。 */
static int s_ext_ready = 1;

void PhoneLocale_SetExternalFontReady(int ready)
{
    s_ext_ready = ready;
}

#if defined(PHONE_LOCALE_XIP_BASE)

/* ===================== 板级：字模在 QSPI 映射区 ===================== */

static GYfont chinese_font =
{
    (const uint8 *)(PHONE_LOCALE_XIP_BASE),   /* bitmap = QSPI XIP 基址 */
    YMGUI_GB2312_cps,
    0, 0, 0,
    16, 16, 8, 4,                             /* 16×16 4bpp，每行 8 字节 */
    NULL,
    NULL                                      /* glyph_read = NULL：直接用 bitmap 指针 */
};

void PhoneLocale_Init(void)
{
    /* ⚠ 必须幂等（2026-10-04 修复，此前会直接把 App 卡死）：
     * 板级开机有两条路径都会调到这里 —— src/main.c 一次、PhoneShell_BoardInit() 又一次。
     * 第二次进来时 GetFallback() 已经是 &chinese_font，原写法
     *     old_fallback = GetFallback(); chinese_font.fallback = old_fallback;
     * 会把 chinese_font 接到**它自己**后面 ⇒ resolveGlyph() 沿 fallback 链找字时，
     * 遇到"字库里没有的码点"（如 '\n'）永远走不到链尾 ⇒ 死循环，App 冻结。
     * （实测证据：chinese_font.fallback == 0x200000A0 == &chinese_font；
     *   量 '\n' 宽度必现，PC 停在 glyphIndex+36 不动。）
     * 修法两条：① 已经挂上就忽略重复调用；② 链尾用第一次记下的原兜底，不接当前值。 */
    if (YMGUI_Font_GetFallback() == &chinese_font)
    {
        return;                          /* 已挂：重复调用直接忽略 */
    }
    if (old_fallback == NULL)
    {
        old_fallback = YMGUI_Font_GetFallback();   /* 只记第一次，之后不再覆盖 */
    }
    if (!s_ext_ready)
    {
        return;      /* 字库没灌好：不挂它，退回库内置的少量 CJK 字模 */
    }
    chinese_font.glyph_count = YMGUI_GB2312_glyph_count;
    chinese_font.fallback = old_fallback;          /* 链尾恒为原始兜底，不可能是自己 */
    YMGUI_Font_SetFallback(&chinese_font);
}

void PhoneLocale_Shutdown(void)
{
    YMGUI_Font_SetFallback(old_fallback);
}

#else

/* ===================== 桌面：从文件按需读（原有实现，未改动）===================== */

#include <stdio.h>

static FILE* font_file;

static uint32 font_read(const GYfont* font, uint32 off, uint32 len, uint8* buf)
{
	(void)font;
	if (!font_file || fseek(font_file, (long)off, SEEK_SET))
		return 0;
	return (uint32)fread(buf, 1, len, font_file);
}

static GYfont chinese_font = {NULL, YMGUI_GB2312_cps, 0, 0, 0, 16, 16, 8, 4, NULL, font_read};

void PhoneLocale_Init(void)
{
	old_fallback = YMGUI_Font_GetFallback();
	font_file = fopen(GB2312_BIN_PATH, "rb");
	if (font_file)
	{
		chinese_font.glyph_count = YMGUI_GB2312_glyph_count;
		chinese_font.fallback = old_fallback;
		YMGUI_Font_SetFallback(&chinese_font);
	}
	else
		fprintf(stderr, "Yaomi: Chinese font resource unavailable\n");
}

void PhoneLocale_Shutdown(void)
{
	YMGUI_Font_SetFallback(old_fallback);
	if (font_file)
		fclose(font_file);
	font_file = NULL;
}

#endif
