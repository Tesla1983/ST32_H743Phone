#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""复现「收键盘 → 滚到底 → 点关机…」并抓屏存 PNG。

用途：查"面板出来了但关机按钮看不见"这类**视觉**问题。
变量断言只能证明状态对不对，看不见的东西必须看图。

⚠⚠ 抓屏前**必须**置 g_force_redraw=1（2026-10-04 修的坑）：
  g_frame_mirror 只在 phone_flush 被调时拷帧，而画面静止时 YMGUI_Refresh
  因脏区为空直接 return ⇒ 镜像停在老帧。症状是"画面明明变了，两张 PNG
  却逐字节相同（md5 一致）"，曾据此误判"面板没画出来"。
  现在 snap() 每次都强制重绘一拍，图与画面对齐。
"""
import struct
import sys
import time
import zlib

sys.path.insert(0, "tools")
import bench
import caret_check as cc
import ribbon_check as rc


def save_png(px, path):
    w, h = cc.G.W, cc.G.H
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = y * w
        for x in range(w):
            v = px[row + x]
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            raw += bytes((r << 3, g << 2, b << 3))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)
    print("  已存 %s" % path)


def snap(tag, path, settle=0.35):
    """抓一帧存 PNG。**每次都强制重绘**，避免读到静止画面的旧帧。"""
    bench.wr("g_frame_mirror", [1])
    bench.wr("g_force_redraw", [1])
    # 等它跑满 2 拍：写 1 → 主循环标脏并重绘 → 自动清 0。
    # 不用忙等 g_force_redraw 归零（读它会暂停内核，反而干扰时序）。
    time.sleep(settle)
    save_png(cc.frame(), path)
    print("  [%s] 抓屏完成（已强制重绘）" % tag)


def main():
    print("准备：进设置页")
    if not rc.open_settings():
        print("[FAIL] 没进到设置页")
        return 1
    print("  current_app=%d scroll=(%d,%d)"
          % (rc.rd1("current_app"), *rc.scroll_pos()))

    print("滚到底")
    # 拖动起点用固件约定的 DRAG_XY（x=6 是卡片圆角外空白）——
    # 用 (160,380) 会落在设备名输入框上，一次拖动就把键盘拉起来。
    for _ in range(4):
        rc.drag(rc.DRAG_XY[0], rc.DRAG_XY[1], 0, -200, wait=0.6)
    print("  scroll=(%d,%d)" % rc.scroll_pos())
    snap("滚到底", "build/pwr_scroll_bottom.png")

    print("点关机按钮（固件每帧现算的坐标）")
    bx, by = rc.power_btn_xy()
    print("  坐标=(%d,%d)" % (bx, by))
    rc.tap(bx, by, wait=0.25)
    print("  立刻读：g_diag_power_dialog=%d scene=%d app=%d"
          % (rc.rd1("g_diag_power_dialog"), rc.rd1("scene"), rc.rd1("current_app")))
    time.sleep(1.5)
    print("  1.5s 后：g_diag_power_dialog=%d scene=%d app=%d"
          % (rc.rd1("g_diag_power_dialog"), rc.rd1("scene"), rc.rd1("current_app")))
    snap("点关机后", "build/pwr_after_tap.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
