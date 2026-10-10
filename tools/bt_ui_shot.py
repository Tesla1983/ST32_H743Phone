#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P5 蓝牙接收页抓屏验收（STM32 UI 侧；不需要手机）

回答三个问题（数据 PASS 不等于屏幕对，所以必须看图）：
  U0  文件管理里那一行确实进了「蓝牙接收」页（4 个按钮文案 + 路径标签正确）
  U1  空闲态画的是「开始接收」+ 状态"未开始"，进度条不显眼（值为 0）
  U2  **接收中**：进度条真的在涨（按前景色像素数量化），状态文字跟着变
  U3  完成后文案变成"已收 <名字>（N KB）"且进度条满

【为什么"接收中"必须能抓到】进度条是本页唯一的新控件，而它只在几秒里非零。
抓不到就等于没验。所以本脚本先进入该页，再用 SWD 触发**自测会话**
（g_bt_recv_req=1，对端合成一张 BMP 走与手机完全相同的下游管道），
会话跑 15~20 s，足够在中间抓一帧。

【抓屏纪律】抓屏前必须同时置 g_frame_mirror=1 与 g_force_redraw=1，
否则画面静止时脏区为空、拿到的还是老帧。现成封装在 pwr_shot.snap()，别自己写。

用法
----
    python tools/bt_ui_shot.py                # 完整流程（约 60 s）
    python tools/bt_ui_shot.py --probe-home   # 只探测：翻到 FILES 所在页并报坐标
    python tools/bt_ui_shot.py --no-session   # 只抓空闲态，不触发接收
"""
import argparse
import os
import sys
import time

sys.path.insert(0, "tools")
import bench
import caret_check as cc
import pwr_shot
import ribbon_check as rc

FILES_ID = 12           # phone_app_catalog.def 的顺序：TASKS0 MUSIC1 GALLERY2 SETTINGS3
                        # CLOCK4 WEATHER5 NOTES6 CALC7 PHONE8 MSG9 CONTACTS10 REC11 FILES12
FILES_PAGE = 2          # PhoneLauncher_Place：FIRST_CAPACITY=4、PAGE_CAPACITY=8
                        # ⇒ index12 → page = 1 + (12-4)/8 = 2

# 桌面图标中心（"条带坐标" + 30 才是绝对 y）。
#   槽位算法（phone_launcher.c:54）：x = 23 + (local%4)*75；page>0 时 y = 28 + (local/4)*110
#   ⇒ FILES: local = (12-4)%8 = 0 ⇒ 槽位 (23,28)，按钮 49×51 ⇒ 中心 (47,53)
#   条带 y 与绝对 y 的偏移 = +30（用图库图标反推：槽位 y=238 实测可点在绝对 268）。
FILES_ICON = (47, 53 + 30)

# 页内坐标 → 绝对：app_views[i] 建在 (0, 36)（phone_shell.c:1782），所以 +36。
BT_FOLDER   = 3                          # folder_names[] 里「蓝牙接收」的下标
BT_ROW_XY   = (160, 246 + 20 + 36)      # 第 4 行按钮（目录里的「蓝牙接收」）
BT_ACT_XY   = (160,  99 + 20 + 36)      # 第 1 行按钮（开始接收 / 中止接收）
BAR_ABS     = (24, 291 + 36, 272, 12)   # 进度条（app_create 里建在 view y=291）
TEXT_ABS    = (24, 307 + 36, 272, 92)   # 说明区

FG = (231, 173, 65)     # 进度条前景色（files.c 的 YMGUI_Bar_SetColors 第二参数）


def home_page():
    """读桌面当前页号（phone_shell.c 的 static int home_index）。"""
    return rc.rd1("home_index")


def goto_page(n, tries=4):
    """横向拖到第 n 页。拖动的 dx 取负 = 往下一页。"""
    for _ in range(tries):
        p = home_page()
        if p == n:
            return True
        rc.drag(280, 150, -250, 0, wait=1.0)
        rc.settle(0.5)
    return home_page() == n


def open_files():
    if not goto_page(FILES_PAGE):
        print("  [!] 没能翻到第 %d 页（home_index=%d）" % (FILES_PAGE, home_page()))
        return False
    rc.tap(*FILES_ICON, wait=1.6)
    rc.settle(0.6)
    return rc.rd1("current_app") == FILES_ID


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


FGV = rgb565(*FG)
BGV = rgb565(70, 62, 52)        # 槽色（files.c 的 YMGUI_Bar_SetColors 第一参数）


def shot(tag, path, settle=0.35):
    """抓一帧，**并把原始 RGB565 像素留下**。

    ⚠ 这就是 pwr_shot.snap() 的body，只多返回一份像素：抓屏前必须同时置
      g_frame_mirror=1 与 g_force_redraw=1（画面静止时脏区为空 ⇒ 拿到老帧），
      再等 0.35 s 让它跑满两拍。顺序照抄，别改。
    ⚠ 自己再调一次 cc.frame() 也能拿像素，但那会**再停核一次**（每次读
      320×480 的镜像 ~1 s），会话正在跑的时候白挨一次停顿没意义。"""
    bench.wr("g_frame_mirror", [1])
    bench.wr("g_force_redraw", [1])
    time.sleep(settle)
    px = cc.frame()
    pwr_shot.save_png(px, path)
    print("  [%s] 抓屏完成" % tag)
    return px


def _c565(v):
    """RGB565 → 三个分量（5/6/5 原始刻度，不做 8 位还原）。"""
    return ((v >> 11) & 31, (v >> 5) & 63, v & 31)


def bar_stats(tag, px):
    """量进度条：条**中线那一行**里前景色有多少个像素。

    ⚠ 不引 PIL/numpy —— 本机 workbuddy 的 Python 两个都没有（各踩过一次）。
      cc.frame() 本来就是 RGB565 原始像素，直接在这里判色即可。
    ⚠ 取一行而不是整个矩形：圆角/边框在上下两行，避开之后这一行就是"填了多长"。
    ⚠⚠ 判据必须是"**就是前景色**（容差内）"，**不能**用"离前景色比离槽色近" ——
      2026-10-10 踩过：浅色底纸 (240,244,248) 到亮橙前景 (224,172,64) 的
      曼哈顿距离 43，到暗槽色 (64,60,48) 是 93 ⇒ 底纸被算成"前景"，
      空闲态直接报 272/272（100%）。改成近色判定后空闲态正确读到 0。"""
    x, y, w, h = BAR_ABS
    row = y + h // 2
    fr, fg_, fb = _c565(FGV)
    fg = 0
    for xx in range(x, x + w):
        r, g, b = _c565(px[row * cc.G.W + xx])
        if abs(r - fr) + abs(g - fg_) + abs(b - fb) <= 6:
            fg += 1
    print("  [%s] 进度条：前景 %d / %d 像素（%.0f%%）" % (tag, fg, w, 100.0 * fg / w))
    return fg, w


def in_bt_page():
    """当前分类是不是「蓝牙接收」（读固件诊断量 g_files_folder == 3）。

    ⚠ 为什么不用"抓一帧看进度条在不在"：bdtap 是**异步注入**、偶发丢一次，
      而抓一帧要停核好几秒。用抓帧做重试判据既慢（每轮多几秒）又会让
      被测的注入更容易出问题。读一个 4 字节变量则只要 ~0.1 s。"""
    return rc.rd1("g_files_folder") == BT_FOLDER


def crop_and_save(tag, px, box, scale=3):
    """把某块区域放大存成 PNG（整数倍最近邻，不依赖 PIL）。

    ⚠ 不能用 pwr_shot.save_png —— 它把尺寸写死成整屏 320×480。
      这里自己编一个最小 PNG（与 pwr_shot 同一套 zlib + CRC，只是尺寸可变）。"""
    import struct
    import zlib
    x0, y0, w, h = box
    W2, H2 = w * scale, h * scale
    raw = bytearray()
    for y in range(y0, y0 + h):
        row = y * cc.G.W
        line = bytearray()
        for x in range(x0, x0 + w):
            v = px[row + x]
            line += bytes((((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)) * scale
        for _ in range(scale):
            raw.append(0)
            raw += line

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W2, H2, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    path = "build/bt_ui_%s_zoom.png" % tag
    open(path, "wb").write(png)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe-home", action="store_true")
    ap.add_argument("--no-session", action="store_true")
    # ⚠ 默认用 448×304（≈400 KB）而不是 224×152（100 KB）：抓一帧要停核
    #   好几秒（cc.frame() 一次读 76800 个字），100 KB 的会话只要 10~15 s，
    #   抓帧落在会话尾巴上就会抓到"已完成"的画面（第一版就是这么误判 100% 的）。
    #   400 KB 会话约 1 分钟，抓帧落在中途稳稳的。
    ap.add_argument("--w", type=int, default=448)
    ap.add_argument("--h", type=int, default=304)
    a = ap.parse_args()

    if rc.rd1("g_boot_magic") != 0x594D4731:
        print("[FAIL] g_boot_magic 不对 —— 固件没在跑（download 后要 reset）")
        return 1

    print("回桌面 → 翻到第 %d 页 → 点文件管理" % FILES_PAGE)
    rc.goto_home()
    if a.probe_home:
        ok = goto_page(FILES_PAGE)
        print("  home_index=%d  %s" % (home_page(), "OK" if ok else "翻页失败"))
        return 0 if ok else 1

    if not open_files():
        print("[FAIL] 打不开文件管理（current_app=%d）" % rc.rd1("current_app"))
        return 1
    print("  current_app=%d ✅" % rc.rd1("current_app"))

    os.makedirs("build", exist_ok=True)
    res = []

    # ---------- 进「蓝牙接收」分类 ----------
    # ⚠ 必须**验证真的进去了**再往下走：bdtap 是异步注入，偶发有一次没被消费
    #   （第一版就踩了 —— 画面还停在分类列表，却被当成"空闲态"PASS 了）。
    #   判据用进度条区域是不是"槽色"：那个控件只在这个分类里可见，
    #   它出现 = 分类切成功了。比读文字稳（文字没法从画面外读）。
    print("进「蓝牙接收」分类 @%s" % (BT_ROW_XY,))
    for attempt in range(6):
        rc.tap(*BT_ROW_XY, wait=1.0)
        rc.settle(0.5)
        if in_bt_page():
            break
        print("  [!] 第 %d 次点击没被消费（g_files_folder=%d），重试"
              % (attempt + 1, rc.rd1("g_files_folder")))
    if not in_bt_page():
        print("[FAIL] 点不进去「蓝牙接收」分类 —— 坐标 %s 可能不对" % (BT_ROW_XY,))
        return 1
    print("  ✅ 已进入（g_files_folder=%d）" % rc.rd1("g_files_folder"))

    # ---------- U0/U1：空闲态 ----------
    px = shot("U1 空闲态", "build/bt_ui_1_idle.png")
    fg_idle, wpx = bar_stats("U1", px)
    print("  放大图：%s（条）/ %s（说明区）"
          % (crop_and_save("1bar", px, BAR_ABS),
             crop_and_save("1text", px, TEXT_ABS)))
    res.append(("U1 空闲态进度条为 0", fg_idle == 0))
    if a.no_session:
        return 0 if all(v for _, v in res) else 1

    # ---------- U2：接收中 ----------
    done0 = rc.rd1("g_bt_done")
    print("SWD 触发自测会话：%d×%d BMP" % (a.w, a.h))
    bench.wr("g_bt_recv_w", [a.w])
    bench.wr("g_bt_recv_h", [a.h])
    bench.wr("g_bt_recv_req", [1])
    time.sleep(6.0)
    b_before = rc.rd1("g_bt_bytes")
    px = shot("U2 接收中", "build/bt_ui_2_recv.png")
    b_after = rc.rd1("g_bt_bytes")
    print("  会话中：sess=%d  err=%d  抓帧前后字节 %d → %d"
          % (rc.rd1("g_bt_sess"), rc.rd1("g_bt_err"), b_before, b_after))
    fg_mid, _ = bar_stats("U2", px)
    print("  放大图：%s / %s"
          % (crop_and_save("2bar", px, BAR_ABS),
             crop_and_save("2text", px, TEXT_ABS)))
    res.append(("U2 进度条真的在涨（0<p<100）", 0 < fg_mid < wpx))

    # ---------- U3：完成 ----------
    t0 = time.time()
    while time.time() - t0 < 180:
        if rc.rd1("g_bt_done") > done0 or rc.rd1("g_bt_err") != 0:
            break
        time.sleep(2.0)
    rc.settle(1.0)
    got = rc.rd1("g_bt_bytes")
    done = rc.rd1("g_bt_done")
    print("  收尾：g_bt_done=%d（本轮 +%d）  g_bt_bytes=%d  err=%d  rc=%d"
          % (done, done - done0, got, rc.rd1("g_bt_err"), rc.rd1("g_bt_rc")))
    px = shot("U3 完成", "build/bt_ui_3_done.png")
    fg_full, _ = bar_stats("U3", px)
    print("  放大图：%s / %s"
          % (crop_and_save("3bar", px, BAR_ABS),
             crop_and_save("3text", px, TEXT_ABS)))
    res.append(("U3 完成后进度条满", fg_full >= wpx * 0.9))

    print("=" * 68)
    for k, v in res:
        print("  %-28s %s" % (k, "PASS" if v else "FAIL"))
    print("-" * 68)
    print("三张整屏：build/bt_ui_1_idle.png / _2_recv.png / _3_done.png")
    print("裁图（2 倍放大）：每张的 _bar.png（进度条）与 _text.png（说明区）")
    allp = all(v for _, v in res)
    print("[%s] P5 蓝牙接收页在板验收%s"
          % ("PASS" if allp else "FAIL", "通过" if allp else "有未通过项"))
    return 0 if allp else 1


if __name__ == "__main__":
    sys.exit(main())
