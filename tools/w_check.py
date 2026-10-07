#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""抓"设置页"屏幕，验证字母 W 不再残缺。

复用既有工具（别重造）：
  ribbon_check.open_settings() —— 模拟触屏导航进设置页（那里有 "Wi-Fi"/"Weather"）；
  pwr_shot.snap()              —— 置 g_frame_mirror=1 + g_force_redraw=1 再抓帧
                                  （画面静止时 Refresh 因脏区为空直接 return，
                                  不强制重绘就会抓到老帧 —— 2026-10-04 的坑）。
"""
import sys

sys.path.insert(0, "tools")
import ribbon_check as rc
import pwr_shot as ps


def main():
    print("准备：进设置页")
    if not rc.open_settings():
        print("[FAIL] 没进到设置页")
        return 1
    print("  current_app=%d" % rc.rd1("current_app"))
    ps.snap("设置页", "build/w_settings.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
