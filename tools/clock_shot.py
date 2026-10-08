#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""NTP 时间 / 天气上屏验收：回首页 → 读链路状态 → 抓屏 → 裁出四块放大存图。

为什么不能只看数据
------------------
`tools/uart_check.py` 已经证明"帧解析正确"（g_net_dt_pkts 在涨、XOR 失败 0），
但**数字对了不等于屏幕上长得对**：
  - 时间换算错（多加了 8 小时）→ 数据看不出来，只有看图；
  - 中文被截断（多字节字符常量只留最低字节）→ 数据里 g_net_wx_text 是好的，
    界面上却整段消失（2026-10-08 相册 caption 已踩过一次）；
  - 控件句柄没留住 → 界面永远停在硬编码的 "9:41"。

本脚本回答三个问题：
  ① 状态栏时间是不是 NTP 给的（对比 g_net_hh:mm）
  ② 桌面大时间 / 日期 / 天气文本是不是真的（"23:22:58" / "10月8日 星期四" / "晴 / 20℃"）
  ③ 天气图标有没有按 code 换（code=0 应只有太阳、没有云）

⚠⚠ 抓屏前必须同时置 g_frame_mirror=1 与 g_force_redraw=1 —— 画面静止时脏区为空，
YMGUI_Refresh 直接 return，镜像会停在上一次抓的老帧。现成封装在 pwr_shot.snap()。

用法
----
    python tools/clock_shot.py              # 完整流程
    python tools/clock_shot.py --no-home    # 不强行回首页（已在首页时）
"""
import struct
import sys
import time
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench          # noqa: E402
import caret_check as cc  # noqa: E402
import pwr_shot       # noqa: E402
import ribbon_check as rc  # noqa: E402

rd = bench.rd

# 首页各元素的**绝对屏幕坐标**（推导见下方注释，别再靠印象猜，2026-10-08 踩过）
#   status_bar 的 label 建在 home_page 的 (19, 4)          → 绝对 (19, 4)
#   pages[0] 在 viewport(0,28) 里 ⇒ 其子元素一律 y+28：
#     大时间 (22, 9)   → (22, 37)
#     日期   (25, 75)  → (25, 103)
#     天气卡 widget (22,123) → (22,151)，卡内文本 (16,40) → (38,191)，图标 (210,12) → (232,163)
REGIONS = [
    ("status",  (8,  0,   100, 28), 4),   # 状态栏时间
    # 大时间用的是 display 字体（字号 3 ⇒ 字形高 66px），原来只裁 32 高 ⇒ 只剩上半截。
    # 按 66px 留足，再往上多带 4px 余量。
    ("clock",   (18, 32,  180, 74), 2),   # 桌面大时间（去掉秒位后只到 ~170px 宽）
    ("date",    (18, 96,  290, 36), 3),   # 日期
    ("weather", (30, 184, 210, 36), 3),   # 天气文本
    ("icon",    (226, 156, 64,  64), 4),  # 天气图标
]


def crop_png(px, x0, y0, w, h, path, scale):
    """裁一块并整数倍放大存 PNG（纯标准库，不依赖 PIL）。

    放大是**近邻复制**：小字号 UI 文本在 1:1 下几乎没法目测，放大 3~4 倍后
    "中文有没有被截断"才看得出来。"""
    W, H = cc.G.W, cc.G.H
    x0 = max(0, x0)
    y0 = max(0, y0)
    w = min(w, W - x0)
    h = min(h, H - y0)

    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        sy = y0 + y // scale
        for x in range(w * scale):
            v = px[sy * W + (x0 + x // scale)]
            raw += bytes((((v >> 11) & 0x1F) << 3,
                          ((v >> 5) & 0x3F) << 2,
                          (v & 0x1F) << 3))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if "--no-home" not in sys.argv:
        rc.goto_home()
        time.sleep(0.8)

    print("---- 链路状态 ----")
    dt = rd("g_net_dt_pkts", 1)[0]
    wd = rd("g_net_wd_pkts", 1)[0]
    print("  $DT 帧 %d   $WD 帧 %d   rx_bytes %d   FE %d"
          % (dt, wd, rd("g_uart_rx_bytes", 1)[0], rd("g_uart_err_fe", 1)[0]))
    if dt == 0:
        print("  [FAIL] 一帧时间都没收到，界面只会显示占位串 '--:--'")
        return 1

    # ⚠ 天气帧 **30 分钟**才来一次（对端 CONFIG_UPLINK_WEATHER_INTERVAL_S=1800），
    #   而复位 STM32 不会让 ESP32 重发（它按自己的周期走）。没收到就用自检注入
    #   一帧样本，把"解析 → 换算 → 上屏"这条路走完。
    #   ★ 但必须说清楚：这样验的是**显示链路**，注入值是样例（20.0℃/晴），
    #     不是当地真实天气。真实天气要等下一帧 $WD 来。
    injected = False
    if wd == 0:
        print("  ⚠ $WD 帧 0：天气 30 分钟一帧，现在还没轮到。"
              "注入一帧自检样本以便验证显示链路（**不是真实天气**）")
        bench.wr("g_uart_selftest", [1])
        time.sleep(0.6)
        injected = True
        wd = rd("g_net_wd_pkts", 1)[0]
        # 自检会把样本里的时间（22:26:40）也一起写进 g_net_*，把那时的真实时间顶掉。
        # 时间帧 10 s 一帧，等 11 s 保证真实 $DT 回来覆盖，抓到的才是"真实时间 + 样本天气"。
        print("  等 11 s 让真实 $DT 覆盖自检样本时间…")
        time.sleep(11)

    hh, mm, ss = rd("g_net_hh", 1)[0], rd("g_net_mm", 1)[0], rd("g_net_ss", 1)[0]
    code = rd("g_net_code", 1)[0]
    temp = rd("g_net_temp_x10", 1)[0]
    if code >= 0x7FFFFFFF:
        code = -1     # 探针读回来是无符号，还原成固件里的 -1
    print("  固件里的时间 %02d:%02d:%02d  天气 code=%d 温度×10=%d%s"
          % (hh, mm, ss, code, temp, "（自检样本）" if injected else "（真实帧）"))
    if code >= 0:
        print("  ⇒ 屏幕上应显示：状态栏 %02d:%02d；大时间 %02d:%02d:%02d；天气 '%s / %d℃'"
              % (hh, mm, hh, mm, ss, "晴" if code == 0 else "code%d" % code, temp // 10))
    else:
        print("  ⇒ 屏幕上应显示：状态栏 %02d:%02d；大时间 %02d:%02d:%02d；天气占位 '等待天气'"
              % (hh, mm, hh, mm, ss))

    print("\n---- 抓屏 ----")
    pwr_shot.snap("home", "build/clock_full.png", settle=0.5)
    px = cc.frame()

    print("\n---- 裁剪放大 ----")
    for name, (x, y, w, h), sc in REGIONS:
        p = "build/clock_%s.png" % name
        crop_png(px, x, y, w, h, p, sc)
        print("  %-8s (%d,%d,%d,%d) ×%d → %s" % (name, x, y, w, h, sc, p))

    # ---- 冒号闪烁验证：隔 500 ms 抓两帧，比冒号那一小格 ----
    # 为什么这么测：闪烁是"每秒在变"的东西，单张抓屏看不出对错 ——
    # 有可能是冒号一直亮（没接到闪烁逻辑），也可能是闪得太快/太慢。
    # 抓两帧对比冒号那一格的像素，能确定"它在动"；再看 blink 变量确认相位。
    if "--blink" in sys.argv:
        print("\n---- 冒号闪烁 ----")
        COLON = (84, 34, 26, 70)     # 只框冒号那一格（大时间三格里的中间那格）
        hashes = []
        for i in range(4):
            b = rd("g_net_ss", 1)[0]     # 顺带报一下当时的秒
            pwr_shot.snap("blink%d" % i, "build/clock_blink_full_%d.png" % i, settle=0.25)
            crop_png(cc.frame(), *COLON, "build/clock_blink_%d.png" % i, 5)
            h = zlib.crc32(open("build/clock_blink_%d.png" % i, "rb").read())
            hashes.append(h)
            print("  第 %d 帧 秒=%02d  冒号格 crc=%08x" % (i, b, h))
            time.sleep(0.5)
        if len(set(hashes)) == 1:
            print("  [FAIL] 四帧冒号格完全一样 ⇒ 冒号没在闪")
        else:
            print("  冒号格出现了 %d 种不同画面 ⇒ 确实在闪（对照 clock_blink_*.png 目视）"
                  % len(set(hashes)))

    # ---- 图标分派验证：直接改 g_net_code 再强制重绘 ----
    # 为什么值得单独测：真实天气帧 30 分钟才来一次，不可能等它变天。直接写码值
    # 能一次把所有分支都看一遍（每个分支都有"画错成空白"的风险）。
    if "--icons" in sys.argv:
        print("\n---- 图标分派（直接写 g_net_code，不是真实天气）----")
        for c, expect in ((0, "晴⇒纯太阳"), (1, "多云⇒太阳+云"), (2, "阴⇒双云"),
                          (3, "小雨⇒云+2滴"), (9, "雷阵雨⇒云+闪电"),
                          (12, "小雪⇒云+雪花"), (16, "雾⇒横条")):
            bench.wr("g_net_code", [c])
            pwr_shot.snap("code%d" % c, "build/clock_icon_full_%d.png" % c, settle=0.4)
            p = "build/clock_icon_code%d.png" % c
            crop_png(cc.frame(), 226, 156, 64, 64, p, 4)
            print("  code=%-3d %-14s → %s" % (c, expect, p))
        # 还原成链路真实值
        bench.wr("g_net_code", [0 if code < 0 else code])

    print("\n判据：把上面五张图逐张看一遍 ——")
    print("  1. status/clock 的时间与固件里的 %02d:%02d 一致（不能差 8 小时）" % (hh, mm))
    print("  2. date 是中文日期且**没有缺字**（缺字 = 多字节字符被截断）")
    print("  3. weather 是 \"晴 / %d℃\" 而不是 \"多云 / 22 C\"（后者说明没刷到）" % (temp // 10))
    print("  4. icon 按 code=%d 画（0=晴 ⇒ 只有太阳、没有云）" % code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
