#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""状态栏 WiFi 图标验收：写 g_net_wf_* 覆盖各态 → 抓屏 → 裁图标区放大。

为什么要有这个脚本
------------------
`uart_check.py` 只能证明 `$WF` 帧**解析对了**（g_net_wf_pkts 在涨），
证明不了**屏幕上画的是不是这个值**。图标是 draw_cb 画的，而 draw_cb
只在对象被置脏时才跑 —— 链路没问题但界面永远不刷新，是一种很容易漏的失败。

覆盖的六种态（BoardNet_WifiUp / BoardNet_WifiBars 的分支）
-------------------------------------------------------
  unknown   pkts=0              → 全暗轮廓，无斜杠（开机到首帧 $WF 之间）
  down      pkts=1 up=0         → 全暗轮廓 + **红色斜杠**（已知断开）
  up0       up=1 rssi=-90       → 只点亮圆点（连着但很虚）
  up1       up=1 rssi=-76       → 圆点 + 内弧 ← 本板实测就是这档
  up2       up=1 rssi=-65       → 圆点 + 内弧 + 中弧
  up3       up=1 rssi=-50       → 三层弧全亮（满格）

⚠ "未知"和"断开"现在**轮廓一样**（都是全暗），区分只靠红色斜杠
   ⇒ 自动判据改成数红色像素（red），不再用 mid 像素数。
   这条不是洁癖：开机头几十秒是"还没收到帧"，画成"已确认断网"会被误读。

★ 形状照抄 E:\\esp32S3_TFT2.8_project（main/gui.c 的状态栏 WiFi 图标）：
  16×14 位图，三层扇面弧 + 底座圆点，未点亮的弧保留暗色轮廓。

⚠⚠ 抓屏前必须同时置 g_frame_mirror=1 与 g_force_redraw=1 —— 画面静止时
脏区为空，YMGUI_Refresh 直接 return，镜像会停在老帧。封装在 pwr_shot.snap()。

用法
----
    python tools/wifi_shot.py              # 完整流程（回首页 + 六态抓屏）
    python tools/wifi_shot.py --no-home    # 已在首页时
    python tools/wifi_shot.py --real       # 只抓真实链路那一档，不覆盖写值
"""
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench            # noqa: E402
import caret_check as cc  # noqa: E402
import pwr_shot         # noqa: E402
import ribbon_check as rc  # noqa: E402
from clock_shot import crop_png  # noqa: E402

rd = bench.rd
# ⚠ main() 里**不要**再用 rd 当局部变量名（red 像素数）—— 那会把模块级的 rd
#   （= bench.rd）遮蔽成局部变量，第一次调用就 UnboundLocalError（踩过一次）。

# 图标在屏幕上的**绝对坐标**。推导（别靠印象猜，2026-10-08 踩过一次）：
#   status_bar(parent) 里 icon 建在 parent 的 (230, 3)，尺寸 74×18（2026-10-10 第二轮
#   为塞下 WiFi 图标加宽：WiFi 16 + 间隙 2 + 4 格柱 18 + 电池 28 = 74）；
#   首页 home_page 就是全屏 (0,0) ⇒ 绝对 (230, 3)。
#   WiFi 部分占图标框的 x=0..15、y 下移 2 ⇒ 绝对 (230, 5) 起 16×14。
ICON_BOX = (230, 5, 16, 14)      # 只框 WiFi 那 16×14
BAR_BOX = (228, 0, 78, 24)       # 带一点上下文（能看到 4 格柱与电池的相对位置）


def lum(v):
    r = ((v >> 11) & 0x1F) << 3
    g = ((v >> 5) & 0x3F) << 2
    b = (v & 0x1F) << 3
    return (r + g + b) // 3


def icon_stats(px):
    """统计 WiFi 图标区里的：不透明像素 / 半透明像素 / 红色斜杠像素。

    为什么要数：目视只能定性（"看着像三格"），数出来才能确认**弧数真的在变**。
    已连的四档 bright 必须严格递增（每多一层弧就多十几个像素）；
    未知/断开两档 bright 都是 0（全暗轮廓）；两档的区别靠 red（只有断开有红斜杠）。

    ⚠ 图形是位图（tier 表）画的，像素数是**确定的**：圆点 9、内弧 9、中弧 11、
      外弧 15 ⇒ up0/up1/up2/up3 = 9/18/29/44。脚本不写死这四个数（换形状就不对），
      只断言严格递增 —— 真要核对形状请看放大图。
    """
    W = cc.G.W
    x0, y0, w, h = ICON_BOX
    vals = []
    cols = []
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            v = px[y * W + x]
            vals.append(lum(v))
            cols.append((((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3))
    # 背景 = 出现次数最多的亮度（图标是画在桌面背景/状态栏底色上的）
    bg = max(set(vals), key=vals.count)
    bright = sum(1 for v in vals if v > 200)
    mid = sum(1 for v in vals if bg + 25 < v <= 200)
    # 红斜杠 #E74C3C：红分量明显高于绿蓝（背景是深蓝/深灰，不会误判）
    red = sum(1 for r, g, b in cols if r > 110 and r > g + 40 and r > b + 40)
    return bg, bright, mid, red


STATES = [
    # (tag,     pkts, up,  rssi, 期望)
    ("unknown", 0,    0,   0,    "全暗轮廓，无斜杠（bright=0, red=0）"),
    ("down",    1,    0,   0,    "全暗轮廓 + 红斜杠（bright=0, red>0）"),
    ("up0",     1,    1,   -90,  "只点亮圆点（9 px）"),
    ("up1",     1,    1,   -76,  "圆点 + 内弧 ← 本板实测档"),
    ("up2",     1,    1,   -65,  "圆点 + 内弧 + 中弧"),
    ("up3",     1,    1,   -50,  "三层弧全亮（满格）"),
]


def main():
    if "--no-home" not in sys.argv:
        rc.goto_home()
        time.sleep(0.8)

    print("---- 链路真实值 ----")
    pkts = rd("g_net_wf_pkts", 1)[0]
    up = rd("g_net_wf_up", 1)[0]
    rssi = rd("g_net_wf_rssi", 1)[0]
    if rssi >= 0x7FFFFFFF:
        rssi -= 0x100000000
    ipb = bench.rd("g_net_wf_ip", 4)
    ip = b"".join(
        __import__("struct").pack("<I", v) for v in ipb
    ).split(b"\x00")[0].decode("ascii", "ignore")
    print("  $WF 帧 %d   up=%d   rssi=%d dBm   ip=%s" % (pkts, up, rssi, ip))
    if pkts == 0:
        print("  ⚠ 一帧 $WF 都没收到：真实态就是 unknown（淡点）")

    real = (pkts, up, rssi)

    if "--real" in sys.argv:
        pwr_shot.snap("wifi_real", "build/wifi_full_real.png", settle=0.5)
        bg, b, m, r = icon_stats(cc.frame())
        crop_png(cc.frame(), *ICON_BOX, "build/wifi_icon_real.png", 8)
        crop_png(cc.frame(), *BAR_BOX, "build/wifi_bar_real.png", 4)
        print("  真实态：bg=%d bright=%d mid=%d red=%d → build/wifi_icon_real.png"
              % (bg, b, m, r))
        return 0

    print("\n---- 六态覆盖（写 g_net_wf_*，不是真实链路）----")
    print("  %-8s %-6s %-6s %-6s %-8s %-6s %-6s %s"
          % ("态", "pkts", "up", "rssi", "bright", "mid", "red", "期望"))
    rows = []
    for tag, p, u, r, expect in STATES:
        bench.wr("g_net_wf_pkts", [p])
        bench.wr("g_net_wf_up", [u])
        bench.wr("g_net_wf_rssi", [r])
        # 等 net_refresh（250 ms 周期）察觉变化并置脏；snap 内部再强制重绘一次
        time.sleep(0.45)
        pwr_shot.snap("wifi_" + tag, "build/wifi_full_%s.png" % tag, settle=0.4)
        px = cc.frame()
        bg, b, m, rdn = icon_stats(px)
        crop_png(px, *ICON_BOX, "build/wifi_icon_%s.png" % tag, 8)
        crop_png(px, *BAR_BOX, "build/wifi_bar_%s.png" % tag, 4)
        print("  %-8s %-6d %-6d %-6d %-8d %-6d %-6d %s"
              % (tag, p, u, r, b, m, rdn, expect))
        rows.append((tag, b, m, rdn))

    # 还原成链路真实值
    bench.wr("g_net_wf_pkts", [real[0]])
    bench.wr("g_net_wf_up", [real[1]])
    bench.wr("g_net_wf_rssi", [real[2]])
    time.sleep(0.45)
    pwr_shot.snap("wifi_restore", "build/wifi_full_restore.png", settle=0.4)
    bg, b, m, rdn = icon_stats(cc.frame())
    crop_png(cc.frame(), *ICON_BOX, "build/wifi_icon_restore.png", 8)
    print("\n  还原后：bright=%d mid=%d red=%d（应与真实档 %s 一致）"
          % (b, m, rdn, "up1" if (real[0] and real[1] and real[2] == -76) else "真实"))

    print("\n---- 自动判据 ----")
    d = dict((t, (b, m, rd)) for t, b, m, rd in rows)
    ok = True

    if not (d["up0"][0] < d["up1"][0] < d["up2"][0] < d["up3"][0]):
        print("  [FAIL] 已连四档的 bright 没有严格递增：%s"
              % [d[t][0] for t in ("up0", "up1", "up2", "up3")])
        ok = False
    else:
        print("  [OK] 已连四档 bright 严格递增 %s ⇒ 点亮的弧数确实随 RSSI 变"
              % [d[t][0] for t in ("up0", "up1", "up2", "up3")])

    if d["unknown"][0] != 0 or d["down"][0] != 0:
        print("  [FAIL] 未知/断开态出现了不透明像素 ⇒ 没画成暗色")
        ok = False
    else:
        print("  [OK] 未知/断开态 bright=0 ⇒ 确实是暗色（没有一条弧被点亮）")

    if d["down"][2] <= 0:
        print("  [FAIL] 断开态没有红色像素 ⇒ 斜杠没画出来（会被误读成'信号弱但连着'）")
        ok = False
    elif d["unknown"][2] != 0:
        print("  [FAIL] 未知态出现了红色像素 ⇒ 开机那几十秒会被误读成断网")
        ok = False
    else:
        print("  [OK] 断开 red=%d > 0、未知 red=0 ⇒ 两态可区分" % d["down"][2])

    print("\n判据：把 build/wifi_icon_*.png 六张逐张看一遍（已放大 8 倍）——")
    print("  1. up3 是三层弧叠起来 + 底部圆点，形状要认得出是 WiFi（照抄参考工程位图）")
    print("  2. down / unknown 全是暗轮廓，和已连的亮白一眼可分；down 还能看到红斜杠")
    print("  3. wifi_bar_*.png 里：WiFi 图标在 x=230..245，4 格柱在 x=248..265，")
    print("     电池在 x=275..303（右边缘与改动前一致，没被挤位）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
