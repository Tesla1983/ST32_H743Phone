#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""查「ASCII 字形被 cell_w 截断」——字母 W 残缺的根因定位工具。

背景
----
`third_party/YMGUI/tools/gen_font.py` 的 `emit_ascii()` 用
`CELL_W, CELL_H, PT, Y_OFF = 8, 16, 15, -1` 生成内嵌 ASCII 字模，
`glyph_bytes()` 里那句 `nib = (px[x, y] >> 4) if x < cell_w else 0`
会把 x >= 8 的墨迹**直接丢掉**。

DejaVuSansMono 是等宽字体：15pt 时**步进宽度** = 15 * 1233/2048 ≈ **9.03 px**。
也就是说字形的墨迹本来就可能画到第 9 列（x=8），而单元只有 8 列 ——
最宽的那几个字（W / w / M / m …）右边就会被切掉一条。

本脚本做的事
------------
  A1 用同一个 TTF、同一组参数重算 95 个字形，与仓库里已生成的
     `YMGUI_FontData.c` 逐字节比对 → 证明"仓库数据确实是这么生成的"（排除别的原因）。
  A2 把每个字形画进**足够宽**（32 列）的画布，量它真实的墨迹横向范围
     （真实 bbox）→ 找出所有 `真实右边界 >= 8` 的字形，即被截断的那些。
  A3 对 W 单独打印"完整 32px 版 / 8px 截断版 / 仓库实际值"三张图，肉眼可验。

字体来源：脚本会自己找 DejaVuSansMono.ttf
  1. --ttf 参数指定；
  2. ./third_party/YMGUI/tools/ 下；
  3. 从 matplotlib 的 wheel 里提取（需要联网，一次性）。
若都拿不到，只跑 A3 里不依赖 TTF 的部分并明确报"未验证"。

用法
----
    python tools/font_clip_check.py
    python tools/font_clip_check.py --ttf /path/DejaVuSansMono.ttf
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import zipfile

RAMP = " .:-=+*#%@"
CELL_W, CELL_H, PT, Y_OFF = 8, 16, 15, -1
FIRST, LAST = 32, 126
GLYPH_Y_OFF = {ord('_'): -2}          # 与 gen_font.py 保持一致

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONTDATA = os.path.join(
    REPO, "third_party", "YMGUI", "YMGUI", "CORE", "YMGUI_FontData.c")


# ---------------------------------------------------------------- 读仓库字模
def repo_bitmap():
    src = open(FONTDATA, encoding="utf-8").read()
    body = re.search(r"s_font_bitmap\[\] = \{(.*?)\n\};", src, re.S).group(1)
    body = re.sub(r"//.*", "", body)          # 行尾注释里的 0xNN 是字符码，不是数据
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    assert len(vals) == (LAST - FIRST + 1) * CELL_H * (CELL_W // 2), \
        "字节数 %d 与 95*16*4 不符" % len(vals)
    return vals


def repo_rows(ch):
    i = ord(ch) - FIRST
    v = repo_bitmap()[i * 64:(i + 1) * 64]
    return [[t for b in v[r * 4:(r + 1) * 4] for t in ((b >> 4) & 0xF, b & 0xF)]
            for r in range(CELL_H)]


# ---------------------------------------------------------------- 找 TTF
def find_ttf(given=None):
    if given and os.path.exists(given):
        return given
    for here in (os.path.join(REPO, "third_party", "YMGUI", "tools"),
                 os.path.join(REPO, "build", "_fonttmp")):
        for n in ("DejaVuSansMono.ttf", "dejavu-sans-mono.ttf"):
            p = os.path.join(here, n)
            if os.path.exists(p):
                return p
    # 从 matplotlib wheel 里提（联网，一次性；不装进环境）
    d = os.path.join(REPO, "build", "_fonttmp")
    os.makedirs(d, exist_ok=True)
    try:
        subprocess.run([sys.executable, "-m", "pip", "download", "--no-deps",
                        "--quiet", "-d", d, "matplotlib"], check=True,
                       capture_output=True)
        whl = glob.glob(os.path.join(d, "*.whl"))
        if whl:
            z = zipfile.ZipFile(whl[0])
            n = "matplotlib/mpl-data/fonts/ttf/DejaVuSansMono.ttf"
            if n in z.namelist():
                out = os.path.join(d, "DejaVuSansMono.ttf")
                open(out, "wb").write(z.read(n))
                return out
    except Exception:
        pass
    return None


# ---------------------------------------------------------------- 重算
def render(font, ch, cell_w, y_off):
    """完全照抄 gen_font.py 的 glyph_bytes()，只是画布宽度可放宽"""
    from PIL import Image, ImageDraw
    img = Image.new("L", (cell_w, CELL_H), 0)
    ImageDraw.Draw(img).text((0, y_off), ch, fill=255, font=font)
    px = img.load()
    bpr = (cell_w * 4 + 7) // 8
    rows = []
    for y in range(CELL_H):
        row = []
        for bx in range(bpr):
            for half in range(2):
                x = bx * 2 + half
                row.append((px[x, y] >> 4) if x < cell_w else 0)
        rows.append(row[:cell_w] if cell_w != 32 else row)
    return rows


def rows8(font, ch):
    """gen_font.py 的原始行为：8 列画布、4bpp 打包"""
    return render(font, ch, CELL_W, GLYPH_Y_OFF.get(ord(ch), Y_OFF))


def ink_bbox(rows):
    xs = [x for r in rows for x, v in enumerate(r) if v > 0]
    return (min(xs), max(xs)) if xs else (None, None)


def emit_fixed(font, out_path, cell_w):
    """按指定 cell_w 重新生成一份 YMGUI_FontData.c（**写到 out_path，不覆盖仓库文件**）。

    生成格式与 gen_font.py 的 emit_ascii() 一致（4bpp、高 nibble 在左、'_' 单独上抬）。
    目的是把"修好之后长什么样"变成可以直接看、可以直接比对的产物，
    而不是停在口头方案上。
    """
    bpr = (cell_w * 4 + 7) // 8
    lines = ['#include "YMGUI_Font.h"', "",
             "/* 由 tools/font_clip_check.py --emit-fixed 生成："
             "cell_w=%d（原 8，会把 x>=8 的墨迹切掉，W/w 的右边一竖因此残缺）*/" % cell_w,
             "//每字 %d 行 x %d 字节,4bpp,每字节2像素,高nibble在左" % (CELL_H, bpr),
             "static const uint8 s_font_bitmap[] = {"]
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        rows = render(font, ch, cell_w, GLYPH_Y_OFF.get(code, Y_OFF))
        flat = []
        for r in rows:
            for bx in range(bpr):
                hi = r[bx * 2] if bx * 2 < len(r) else 0
                lo = r[bx * 2 + 1] if bx * 2 + 1 < len(r) else 0
                flat.append((hi << 4) | lo)
        hexs = ", ".join("0x%02X" % b for b in flat)
        label = ch if ch != '\\' else '\\\\'
        lines.append("\t%s, //0x%02X '%s'" % (hexs, code, label))
    lines += ["};", "", '#include "YMGUI_PubDefine.h"',
              "#if YMGUI_FONT_CJK", "extern const GYfont YMGUI_Font_CJK;", "#endif", "",
              "const GYfont YMGUI_Font_Default =", "{", "\ts_font_bitmap, //bitmap",
              "\tNULL,          //codepoints(连续区间模式)", "\t0,             //glyph_count",
              "\t%d,          //first_char" % FIRST, "\t%d,         //last_char" % LAST,
              "\t%d,             //cell_w ★由 8 改为 %d：15pt 的 DejaVuSansMono 步进是 9px" % (cell_w, cell_w),
              "\t%d,            //cell_h" % CELL_H,
              "\t%d,             //bytes_per_row" % bpr,
              "\t4,             //bpp",
              "#if YMGUI_FONT_CJK", "\t&YMGUI_Font_CJK, //fallback → CJK", "#else",
              "\tNULL,          //fallback", "#endif",
              "\tNULL,          //glyph_read", "};", ""]
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    return (LAST - FIRST + 1) * CELL_H * bpr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", default=None)
    ap.add_argument("--emit-fixed", nargs="?", const="build/YMGUI_FontData_fixed.c",
                    default=None,
                    help="按 cell_w=9 重新生成一份字模文件写到该路径（不覆盖仓库文件）")
    args = ap.parse_args()

    print("=" * 74)
    print("W 残缺根因核查")
    print("=" * 74)

    # ---------- A3（不依赖 TTF）：仓库里 W 长什么样 ----------
    print("\n【A3-a】仓库 YMGUI_FontData.c 里的 'W'（4bpp，@=15 最实）")
    for r in repo_rows('W'):
        print("        |%s|" % "".join(RAMP[min(v, 9)] for v in r))

    ttf = find_ttf(args.ttf)
    if ttf is None:
        print("\n[未验证] 拿不到 DejaVuSansMono.ttf，A1/A2 跳过（可 --ttf 指定）")
        return 1
    print("\n字体：%s" % ttf)
    from PIL import ImageFont
    font = ImageFont.truetype(ttf, PT)

    # ---------- A1：重算 vs 仓库 ----------
    same = diff = 0
    diffchars = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        a = repo_rows(ch)
        b = rows8(font, ch)
        if a == b:
            same += 1
        else:
            diff += 1
            diffchars.append(ch)
    print("\n【A1】用同一 TTF + 同一参数重算 95 个字形，与仓库数据逐像素比对")
    print("        完全一致 %d 个，不一致 %d 个 %s"
          % (same, diff, ("（不一致：" + "".join(diffchars) + "）") if diffchars else ""))
    print("        ⇒ %s" % ("确认仓库数据就是这套参数生成的，问题出在参数本身"
                            if same >= 90 else
                            "⚠ 差异较大：TTF 版本可能不同，A2 的结论仍可看趋势"))

    # ---------- A2：真实墨迹范围 ----------
    print("\n【A2】把每个字画进 32 列宽画布，量真实墨迹横向范围")
    print("        （单元格只有 8 列 ⇒ 真实右边界 >= 8 的字一定被切掉）")
    wide = {}
    clipped = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        R = render(font, ch, 32, GLYPH_Y_OFF.get(code, Y_OFF))
        wide[ch] = R
        x0, x1 = ink_bbox(R)
        if x1 is not None and x1 >= CELL_W:
            clipped.append((ch, x0, x1, max(r[CELL_W] for r in R)))
    print("\n        %-6s %-10s %-12s %s" % ("字符", "墨迹 x 范围", "第 8 列(x=8)", "结论"))
    for ch, x0, x1, v8 in clipped:
        print("        %-6r %-10s %-12d 被 cell_w=8 截断，丢了 %d 列"
              % (ch, "%d..%d" % (x0, x1), v8, x1 - CELL_W + 1))
    if not clipped:
        print("        （无）")

    # ---------- A4：把修好的字模真的生成出来 ----------
    if args.emit_fixed:
        n = emit_fixed(font, args.emit_fixed, 9)
        print("\n【A4】已按 cell_w=9 重新生成字模 → %s" % args.emit_fixed)
        print("        95 字形 × 16 行 × 5 字节 = %d B（现役 6 080 B，%+.0f B）"
              % (n, n - 6080))
        print("        该文件**没有**覆盖仓库里的 YMGUI_FontData.c")

    # ---------- A3：W 三图对比 ----------
    if 'W' in wide:
        W = wide['W']
        print("\n【A3-b】同一个 TTF 画出来的 'W'（完整 32 列，只打印前 12 列）")
        for r in W:
            print("        |%s|" % "".join(RAMP[min(v, 9)] for v in r[:12]))
        print("\n【A3-c】gen_font.py 实际产出（8 列，x>=8 被丢）—— 与上面并排看，"
              "右边那一竖的**实心部分**在第 8 列，被丢后只剩 x=7 的一道半透明边")
        for r in rows8(font, 'W'):
            print("        |%s|" % "".join(RAMP[min(v, 9)] for v in r))

    return 0


if __name__ == "__main__":
    sys.exit(main())
