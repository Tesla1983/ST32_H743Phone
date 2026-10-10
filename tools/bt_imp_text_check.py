#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""蓝牙收文件：验证「UI 说真话」这条修复（2026-10-11）。

背景（不改代码前必读）：
  收完字节 ≠ 图进了相册。`src/bt_recv.c` 的 `g_bt_imp_rc` 只表示「有没有排上队」，
  真正的导入是 `img_store_poll()` 异步做的，结果在 `g_img_rc`。实测两次发的都是
  PNG：字节全收到、`g_bt_imp_rc=0`，但 `g_img_rc=4`(RC_NOT_BMP)，图没进图库 ——
  而 UI 在 DONE 时一律写「已排队导入图库，去相册看」，是一句谎话。

  修复：`bt_recv_poll()` 在 DONE 后等 `g_img_test==0 && g_img_busy==0`
  把 `g_img_rc` 锁进 `s_imp_rc` / `g_bt_imp_final`，`bt_recv_status_text()`
  按它说话（成功 ⇒ "已存入相册"，RC_NOT_BMP ⇒ "格式不支持（仅 BMP/JPG）"）。

本脚本两路验收：
  A 数据 —— 跑一次自测会话（对端合成 24 位 BMP，走与手机完全相同的下游管道），
    判 g_bt_rc==0 / g_bt_imp_rc==0 / **g_bt_imp_final==0**（真进相册了）。
  B 视觉 —— 进「蓝牙接收」分类抓屏：
    B1 成功态 ⇒ 说明区应出现「已存入相册」；
    B2 把 `s_imp_rc` 改成 4（模拟导入失败）⇒ 同一位置应变成「格式不支持（仅 BMP/JPG）」，
       且**不再**出现「去相册看」那句无条件的话。
      ⚠ 能这么做是因为 files.c 用**指针相等**做变化检测，换文案 = 换指针 ⇒ 必然重刷。

用法：
  python tools/bt_imp_text_check.py [--w 224] [--h 152] [--no-visual]
产物：build/bt_imp_ok.png / build/bt_imp_fail.png（说明区放大图）
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench
import ribbon_check as rc
import pwr_shot
import bt_ui_shot as bu

W = 224
H = 152


def rd1(name):
    return bench.rd(name, 1)[0]


def wait_done(base, timeout_s=150):
    """等 g_bt_done 变化。只读 1 个字（一次整段快照要停核 ~6 s，会打断 SPI）。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if rd1("g_bt_done") != base:
            return True
        time.sleep(2.0)
    return False


def main():
    args = sys.argv[1:]
    no_visual = "--no-visual" in args

    if rd1("g_boot_magic") != 0x594D4731:
        print("[FAIL] g_boot_magic 不对 —— 固件没在跑（download 后必须 reset）")
        return 1

    a = rc.rd1("g_loop_count")
    time.sleep(0.4)
    if rc.rd1("g_loop_count") == a:
        print("[FAIL] 主循环没在跑（g_loop_count=%d 不变）" % a)
        return 1

    # ---------------- A 数据：跑一次自测会话 ----------------
    print("A 数据：触发自测会话 %dx%d（对端合成 BMP，走真实下游管道）" % (W, H))
    base_done = rd1("g_bt_done")
    bench.wr("g_bt_recv_w", [W])
    bench.wr("g_bt_recv_h", [H])
    bench.wr("g_bt_recv_req", [1])

    if not wait_done(base_done):
        print("[FAIL] 150 s 内会话没结束（g_bt_rc=%d g_bt_err=%d）"
              % (rd1("g_bt_rc"), rd1("g_bt_err")))
        return 1

    got = {
        "g_bt_rc":        rd1("g_bt_rc"),
        "g_bt_imp_rc":    rd1("g_bt_imp_rc"),
        "g_bt_imp_final": rd1("g_bt_imp_final"),
        "g_img_rc":       rd1("g_img_rc"),
        "g_bt_ms":        rd1("g_bt_ms"),
    }
    time.sleep(1.5)   # 导入是异步的，多给一拍让它把结果锁出来
    got["g_bt_imp_final"] = rd1("g_bt_imp_final")
    got["g_img_rc"] = rd1("g_img_rc")

    for k in sorted(got):
        print("    %-16s = %d" % (k, got[k]))

    ok_a = (got["g_bt_rc"] == 0 and got["g_bt_imp_rc"] == 0
            and got["g_bt_imp_final"] == 0)
    print("  A %s（收完字节且真进了相册 ⇒ g_bt_imp_final=0）"
          % ("PASS" if ok_a else "FAIL"))

    if no_visual:
        return 0 if ok_a else 1

    # ---------------- B 视觉：进「蓝牙接收」分类抓屏 ----------------
    os.makedirs("build", exist_ok=True)
    print("B 视觉：回桌面 → 文件管理 → 「蓝牙接收」")
    rc.goto_home()
    if not bu.open_files():
        print("[FAIL] 打不开文件管理（current_app=%d）" % rc.rd1("current_app"))
        return 1
    for attempt in range(6):
        rc.tap(*bu.BT_ROW_XY, wait=1.0)
        rc.settle(0.5)
        if bu.in_bt_page():
            break
        print("  [!] 第 %d 次点击没被消费（g_files_folder=%d），重试"
              % (attempt + 1, rc.rd1("g_files_folder")))
    if not bu.in_bt_page():
        print("[FAIL] 点不进「蓝牙接收」分类")
        return 1

    px = bu.shot("B1 成功态", "build/bt_imp_ok_full.png")
    p1 = bu.crop_and_save("ok", px, bu.TEXT_ABS)

    # 模拟"导入失败"：改 s_imp_rc（静态变量，nm 里是小写 b）。
    # 文案函数会返回另一个静态字面量 ⇒ 指针变了 ⇒ files.c 必然重刷。
    bench.wr("s_imp_rc", [4])
    rc.settle(1.2)
    px = bu.shot("B2 失败态（s_imp_rc=4）", "build/bt_imp_fail_full.png")
    p2 = bu.crop_and_save("fail", px, bu.TEXT_ABS)
    bench.wr("s_imp_rc", [0])     # 还原，别把界面留在假失败态

    print("  B 产物：%s / %s —— 请目视确认" % (p1, p2))
    print("     B1 应含「已存入相册」；B2 应含「格式不支持（仅 BMP/JPG）」，")
    print("     且两处都**不该**出现「去相册看」那句无条件的话。")
    return 0 if ok_a else 1


if __name__ == "__main__":
    sys.exit(main())
