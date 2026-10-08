#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""量"一帧到底刷了多少" —— 判断置脏范围 / 裁剪剔除类优化值不值得做的**依据**。

为什么要这个脚本
----------------
README §10 里长期挂着三条"未做"：InvalidateOldNew、MDMA+双 band buffer、
遮挡/子树剔除。先动手还是先量化，差别很大：本项目有过四个"看着该优化、
实测没收益于是回退"的先例。所以先把规模量出来，用整数说话：

  g_inv_px_max   这段时间里**最糟糕的一帧**的脏区面积（满屏 320×480 = 153 600）
  g_visit_max    同一口径下访问到的对象次数（含被裁剪掉的）
  g_draw_max     同一口径下真正调用 draw_cb 的次数  ⇒ visit−draw = 被裁剪挡掉的

⚠ **必须用 _max 版**：普通版只保留"最近一次有脏区那帧"的值，操作之后等 1 s
  再读，早就被时间刷新的小脏区覆盖了 —— 2026-10-09 第一版就是这么读出
  三个一模一样的 1452，看起来像"各场景没差异"，其实是采样点错了。

用法
----
    python tools/render_scale.py
"""
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench              # noqa: E402
import ribbon_check as rc  # noqa: E402

FULL = 320 * 480


def rd1(n):
    return bench.rd(n, 1)[0]


def zero():
    for n in ("g_inv_px_max", "g_visit_max", "g_draw_max"):
        bench.wr(n, [0])


def show(tag):
    px = rd1("g_inv_px_max")
    vis = rd1("g_visit_max")
    drw = rd1("g_draw_max")
    print("  %-20s 峰值脏区 %7d px（%5.1f%% 屏）   visit %5d  draw %5d  剔除 %5d"
          % (tag, px, px * 100.0 / FULL, vis, drw, vis - drw))
    return px


def main():
    need = ["g_inv_px_max", "g_visit_max", "g_draw_max"]
    missing = [n for n in need if n not in bench.T]
    if missing:
        print("[FAIL] 固件里找不到：%s（烧的是最新固件吗？）" % ", ".join(missing))
        return 1

    print("=" * 82)
    print("① 桌面静止（只有时间在走）")
    print("=" * 82)
    rc.goto_home()
    time.sleep(0.6)
    zero()
    time.sleep(2.0)
    show("桌面·时间刷新")

    print("=" * 82)
    print("② 打开设置页（整页切换）")
    print("=" * 82)
    zero()
    rc.open_settings()
    time.sleep(1.2)
    show("打开设置页")

    print("=" * 82)
    print("③ 拖**亮度滑块**（README 里说它会导致整屏重绘）")
    print("=" * 82)
    # 滑块坐标由固件每帧现算（settings.c 的 app_tick），不要自己推 ——
    # 滚动位置一动算式就失效（与 ribbon_check 里那些坐标同一个道理）。
    x0 = rd1("g_diag_bright_x0")
    x1 = rd1("g_diag_bright_x1")
    by = rd1("g_diag_bright_y")
    print("     滑块 abs：x0=%d x1=%d y=%d" % (x0, x1, by))
    if 0 < by < 480 and x1 > x0:
        zero()
        for _ in range(3):
            rc.drag(x0 + 10, by, (x1 - x0) - 30, 0, wait=0.25)
            rc.drag(x1 - 10, by, -((x1 - x0) - 30), 0, wait=0.25)
        time.sleep(0.5)
        drag_px = show("拖动亮度滑块")
    else:
        print("     [跳过] 滑块不在屏幕内（先滚到它可见）")
        drag_px = 0

    print("=" * 82)
    print("④ 滑到设置页底部再滚回来（滚动容器）")
    print("=" * 82)
    zero()
    for _ in range(4):
        rc.drag(rc.DRAG_XY[0], rc.DRAG_XY[1], 0, -200, wait=0.3)
    time.sleep(0.5)
    show("滚动设置页")

    print("=" * 82)
    print("结论口径")
    print("=" * 82)
    print("  满屏 = %d px。" % FULL)
    print("  · 拖动滑块若为满屏 ⇒ 整屏重绘成立，值得做 InvalidateOldNew；")
    print("  · 若远小于满屏 ⇒ 那条已不成立（硬件 PWM 模式下本来就不脏 root）。")
    print("  · visit−draw = 裁剪已经挡掉的对象访问；draw 本身很小 ⇒ 遮挡剔除余量也小。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
