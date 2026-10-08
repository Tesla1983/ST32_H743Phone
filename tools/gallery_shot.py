#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""相册页抓屏验收：打开相册 → 抓屏 → 点「下一张」→ 再抓屏。

为什么要有一个独立脚本
----------------------
`tools/img_check.py` 验的是**数据**（XIP 读回 vs 主机参考，逐字节失配 0），
但"失配 0"不等于"屏幕上长得对"——行序反了、缩放拉歪了、caption 中文被截断了，
数据比对全绿也照样显示错。这类问题只有看图能抓出来。

本脚本只回答两个问题：
  ① 相册第 1 张画出来了吗（且 caption 是 "1 / N 张卡上图片"）
  ② 点「下一张」之后，caption 变成 "2 / N" 且画面确实换了一张

⚠⚠ 抓屏前必须同时置 g_frame_mirror=1 与 g_force_redraw=1（画面静止时脏区为空，
YMGUI_Refresh 直接 return ⇒ 镜像停在上一次抓的老帧）。现成封装在 pwr_shot.snap()，
别自己重写（重写过一次，踩完坑才发现）。

用法
----
    python tools/gallery_shot.py                 # 完整流程：抓 2 张
    python tools/gallery_shot.py --probe-icon    # 只探测相册图标坐标，不抓屏
    python tools/gallery_shot.py --next-xy 160 390
"""
import argparse
import sys
import time

sys.path.insert(0, "tools")
import bench
import caret_check as cc
import pwr_shot
import ribbon_check as rc

GALLERY_ID = 2          # PHONE_APP 顺序：TASKS=0 MUSIC=1 GALLERY=2 SETTINGS=3

# 「下一张风景」按钮的**绝对屏幕坐标**推导（别再猜，2026-10-08 踩过）：
#   gallery.c 里 PhoneUI_button(view, 18, 373, 284, 34) ⇒ 按钮中心内容坐标 (160, 390)
#   但那是 **view 坐标**，不是屏幕坐标。app_views[i] 在 phone_shell.c:1483 建在
#   (0, 36, W, 412) ⇒ 要加 y=+36。
#   ⚠ 曾按"设置页 abs y = 92 + 内容 y"的印象直接点 (160,390)，那其实落在
#     "示例风景 / Yaomi 相册" 那个**纯 label** 上（无 event_cb），点了当然没反应，
#     两张抓屏 md5 完全一致，很容易误判成"切换逻辑坏了"。
NEXT_XY = (160, 426)    # = (18 + 284/2, 36 + 373 + 34/2)

# 候选图标坐标：设置图标(SETTINGS=3)实测在 (250,268)，图标 49×51、栅格间距约 65px
# ⇒ 同排往左推 65 就是 GALLERY=2。作为首选，探测失败再逐个试。
ICON_CANDIDATES = [(185, 268), (250, 268), (120, 268), (185, 196), (250, 196)]


def alive():
    a = rc.rd1("g_loop_count")
    time.sleep(0.3)
    return a != rc.rd1("g_loop_count")


def probe_icon():
    """逐个点候选坐标，看 current_app 是否变成 GALLERY_ID。"""
    print("探测相册图标坐标（current_app 应变成 %d）" % GALLERY_ID)
    for x, y in ICON_CANDIDATES:
        rc.goto_home()
        time.sleep(0.5)
        rc.tap(x, y, wait=1.2)
        app = rc.rd1("current_app")
        print("  点 (%3d,%3d) → current_app=%d %s"
              % (x, y, app, "✅命中" if app == GALLERY_ID else ""))
        if app == GALLERY_ID:
            return (x, y)
    return None


def open_gallery(xy):
    rc.goto_home()
    time.sleep(0.5)
    rc.tap(*xy, wait=1.4)
    return rc.rd1("current_app") == GALLERY_ID


def report(tag):
    """打印相册侧采样。
    ⚠ g_img_count 是"**上一次导入结束时**写下的快照"，不是当前值 ——
      复位后没跑过导入时它一直是 0，而 board_count（UI 实时读索引）才是真的。
      2026-10-08 曾把它当"当前图片数"去查，白绕一轮。"""
    print("  [%s] board_count(当前值)=%d  g_img_count(上次导入的快照，0=本次开机还没导入过)=%d  slot_snap=%s"
          % (tag, rc.rd1("g_img_board_count"), rc.rd1("g_img_count"),
             bench.rd("g_img_slot_snap", 4)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe-icon", action="store_true")
    ap.add_argument("--icon-xy", type=int, nargs=2, default=None)
    ap.add_argument("--next-xy", type=int, nargs=2, default=None)
    a = ap.parse_args()

    if not alive():
        print("[FAIL] 内核没在跑（g_loop_count 不变）—— download 之后忘了 reset？")
        return 1

    if a.probe_icon:
        xy = probe_icon()
        print("结果：%s" % (str(xy) if xy else "所有候选都没命中"))
        return 0 if xy else 1

    icon = tuple(a.icon_xy) if a.icon_xy else ICON_CANDIDATES[0]
    nxt = tuple(a.next_xy) if a.next_xy else NEXT_XY

    print("打开相册 @%s" % (icon,))
    if not open_gallery(icon):
        # 首选不中就现场探测
        print("  首选没命中，改探测…")
        icon = probe_icon()
        if not icon:
            print("[FAIL] 打不开相册页")
            return 1
    rc.settle(0.4)
    report("第 1 张")
    pwr_shot.snap("第 1 张", "build/g_shot_1.png")

    print("点「下一张」@%s" % (nxt,))
    rc.tap(*nxt, wait=1.0)
    rc.settle(0.4)
    report("第 2 张")
    pwr_shot.snap("第 2 张", "build/g_shot_2.png")

    # caption 区域（相册面板底部文字）单独裁出来，方便放大看中文
    try:
        from PIL import Image
        for i in (1, 2):
            im = Image.open("build/g_shot_%d.png" % i)
            im.crop((0, 330, 320, 420)).resize((640, 180), Image.NEAREST)\
              .save("build/g_cap_%d.png" % i)
        print("  已裁 caption：build/g_cap_1.png / g_cap_2.png（2 倍放大）")
    except Exception as e:
        print("  [!] 裁剪 caption 失败（%s）—— 看图时手动放大即可" % e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
