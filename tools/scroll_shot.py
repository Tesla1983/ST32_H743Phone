#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""设置页滚动条的**视觉**验收（数据层判据在 tools/ribbon_check.py ③）。

为什么要单独一个脚本：ribbon_check 只验"scroll_y 会变、clamp 生效、滚到底
关机按钮可点" —— 那是**数据**。滚动条是画出来的，**数据对了不代表条画对了**
（条可能被视口遮住、位置算错、或者干脆 hidden）。这类只能看图。

做法：进设置 → 抓顶部态 → 一路拖到底 → 抓底部态，两次都在 x=315 那一列
（滚动条 abs x = 视口右 320 − 宽 3 − 边距 3 = 314..317）找滑块像素段，
报告它的上下边与高度。

几何期望（内容总高 491、视口高 356、clamp 上限 135）：
    滑块高 = 视口高² / 内容高 = 356×356/491 = 258 px
    顶部态 y_top = 92 （视口 abs 顶边）；底部态 y_top = 92 + 98 = 190
    ⇒ 两次的 y_top 差应 ≈ 98（= 视口高 − 滑块高）
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench          # noqa: E402
import caret_check as cc  # noqa: E402
import pwr_shot      # noqa: E402
import ribbon_check as rc  # noqa: E402

OUT = "build"

# 滚动条所在列：视口 x=0、w=320，条宽 3、右边距 3 ⇒ 占 314..316，取中点 315。
BAR_X = 315
# 条色：apps/settings.c 的 scroll_bar_draw 用 RGB(120,132,150) + GY_OPA_COVER
# ⇒ RGB565 = (120>>3)<<11 | (132>>2)<<5 | (150>>3) = 0x7C32（实测列扫描确认）。
# ⚠ 固件里改了这个颜色，这里要同步改。
BAR_COLOR = 0x7C32
# 扫描范围：视口 abs y=92..448，上下各留点余量。
SCAN_Y0 = 80
SCAN_Y1 = 460


def find_thumb(px):
    """在 BAR_X 列上找滑块：返回 (y_top, y_bottom, height)。

    ⚠ 判定用**条的确切颜色**，不要用"取该列众数当背景再找异色段"那套 ——
      那套会翻车：滑块（258 px）比页面背景（110 px）**更长**，众数取出来
      是滑块自己的颜色 ⇒ 反而把背景段当成了"滑块"（实测报成高 110 px、
      且滚到底后"上移"）。条色是固件里的常量 RGB(120,132,150)，
      GY_OPA_COVER 不透明 ⇒ RGB565 是精确的 0x7C32，直接比即可。
    """
    W = cc.G.W
    col = [px[y * W + BAR_X] for y in range(SCAN_Y0, SCAN_Y1)]

    best = (0, 0, 0)     # (len, start, end)
    run_start = None
    for i, v in enumerate(col):
        if v == BAR_COLOR:
            if run_start is None:
                run_start = i
        else:
            if run_start is not None:
                n = i - run_start
                if n > best[0]:
                    best = (n, run_start, i)
                run_start = None
    if run_start is not None:
        n = len(col) - run_start
        if n > best[0]:
            best = (n, run_start, len(col))

    if best[0] == 0:
        return None
    top = SCAN_Y0 + best[1]
    bot = SCAN_Y0 + best[2]
    return (top, bot, bot - top)


def shot(tag, name):
    """抓一帧：存全屏 PNG，并顺手裁一条右侧竖条放大图。

    ⚠ 裁剪必须**在这次抓屏的同一帧**上做。别"先抓两次、回头再裁" ——
      cc.frame() 读的是**当前**显存，不是刚才那一帧，回头裁会得到两遍
      都是最后一次的画面（实测就这么踩过）。"""
    pwr_shot.snap(tag, "%s/%s" % (OUT, name))
    px = cc.frame()
    stem = name[:-4] if name.endswith(".png") else name
    crop(px, 306, 84, 14, 372, "%s/%s_bar.png" % (OUT, stem))
    return px


def crop(px, x0, y0, w, h, path, scale=4):
    """裁一条放大存 PNG（纯标准库，不依赖 PIL）。"""
    W = cc.G.W
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        sy = y0 + y // scale
        for x in range(w * scale):
            v = px[sy * W + (x0 + x // scale)]
            raw += bytes((((v >> 11) & 0x1F) << 3,
                          ((v >> 5) & 0x3F) << 2,
                          (v & 0x1F) << 3))

    import struct
    import zlib

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))

    w2 = w * scale
    h2 = h * scale
    ihdr = struct.pack(">IIBBBBB", w2, h2, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) \
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print("  裁剪图 %s" % path)


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    a = rc.rd1("g_loop_count")
    time.sleep(0.3)
    if a == rc.rd1("g_loop_count"):
        print("[FAIL] 内核没在跑（g_loop_count 不变）—— download 之后忘了 reset？")
        return 1

    print("① 打开设置页")
    if not rc.open_settings():
        print("  [FAIL] 没进到设置页")
        return 1
    rc.settle(0.3)

    sy0, smax = rc.scroll_pos()
    content = rc.rd1("g_diag_scroll_content")
    print("  scroll_y=%d  上限=%d  内容总高=%d" % (sy0, smax, content))
    if smax <= 0:
        print("  [FAIL] 没有可滚动范围（内容没超出视口，条本来就该隐藏）")
        return 1

    print("\n② 顶部态抓屏")
    px0 = shot("顶部", "scroll_top.png")
    t0 = find_thumb(px0)
    print("  滑块：%s" % ("没找到（条没画出来？）" if t0 is None
                          else "y=%d..%d  高 %d" % (t0[0], t0[1], t0[2])))
    if t0 is None:
        print("  [FAIL] 顶部态看不到滚动条")
        return 1

    print("\n③ 拖到底")
    for _ in range(4):
        rc.drag(*rc.DRAG_XY, 0, -200, wait=0.7)
    rc.settle(0.6)
    sy1, _ = rc.scroll_pos()
    print("  scroll_y=%d（上限 %d）" % (sy1, smax))
    if sy1 != smax:
        print("  [!] 没滚到底（scroll_y=%d ≠ %d），下面的位移对不上是正常的" % (sy1, smax))

    px1 = shot("底部", "scroll_bottom.png")
    t1 = find_thumb(px1)
    print("  滑块：%s" % ("没找到" if t1 is None
                          else "y=%d..%d  高 %d" % (t1[0], t1[1], t1[2])))
    if t1 is None:
        print("  [FAIL] 底部态看不到滚动条")
        return 1

    print("\n判据：")
    print("  滑块高   %d px（期望 ≈ %d = 视口高²/内容高 = 356²/%d）"
          % (t1[2], 356 * 356 // content, content))
    print("  滑块下移 %d px（期望 ≈ %d = 视口高 356 − 滑块高）" % (t1[0] - t0[0], 356 - t1[2]))
    print("  两张全屏图：build/scroll_top.png 与 build/scroll_bottom.png")
    print("  肉眼核对两点：① 条在屏幕最右侧、细、灰色；② 滚到底后条走到了最下面。")

    ok = True
    if abs(t1[2] - 356 * 356 // content) > 6:
        print("  [FAIL] 滑块高度不对")
        ok = False
    if t1[0] <= t0[0]:
        print("  [FAIL] 滚到底后滑块没有下移")
        ok = False
    if abs((t1[0] - t0[0]) - (356 - t1[2])) > 6:
        print("  [FAIL] 滑块位移与几何不符（条的位置映射算错了）")
        ok = False

    print("\n%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
