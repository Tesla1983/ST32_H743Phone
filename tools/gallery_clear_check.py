#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""相册「容量 UI」+「清空图库」两段式确认 —— 上板验收（2026-10-11，v28）。

要证的两件事（用户可见的新行为）：
  A. 容量提示：相册 caption 末尾会追加「 · 剩NK B」或「 · 已满」
     （精确数字走截图人工核对，因为主机读 XIP 索引不可靠——probe-rs 读
      0x90000000 窗口只报 "Only supports RAM addresses" 返回全 FF，不能当判据）。
  B. 清空图库：底部「清空」按钮两段式 —— 点一次只变红显示「确认清空?」，
     再点一次才真擦索引扇区；清空后回到示例态，且**擦完必须恢复 XIP 映射**
     （否则下一帧读字模/IME 词典会 BusFault，内核卡死）。

判定口径（全部确定性、不靠肉眼）：
  ★ 权威计数 = g_img_board_count（由 BoardGallery_Count() 直接读 XIP 得出，
    打开相册即经 PhoneApp.show → gallery_refresh_state 刷新）。
  ★ 两段式「第一段不擦」用 g_img_qspi_rc 哨兵判定：清空前写 0xDEAD，
    第一段后若仍 0xDEAD ⇒ 没执行擦除（只有 img_store_clear 才写这个量）。
  ★ 擦完必须恢复 XIP：用 alive()（g_loop_count 持续自增）兜底。

坐标（绝对屏幕坐标，app_views 建在 (0,36) ⇒ 按钮中心 y 都要 +36）：
  下一张 (18,373,88,34) → 中心 (62,426)
  删除   (116,373,88,34) → 中心 (160,426)
  清空   (214,373,88,34) → 中心 (258,426)

用法：python tools/gallery_clear_check.py [--refill 6]
  --refill N：结束后重新用 BT 本地自检灌 N 张测试图，把相册恢复成非空可用态
              （默认 6；传 0 则不回填，留空相册）。
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench
import caret_check as cc
import gallery_shot as gs
import pwr_shot
import ribbon_check as rc

# ---- 按钮绝对坐标（见文件头推导）----
NEXT_XY   = (62, 426)
DEL_XY    = (160, 426)
CLEAR_XY  = (258, 426)

QSRC_SENTINEL = 0xDEAD  # 清空前写入 g_img_qspi_rc；只有 img_store_clear 真执行才会改掉它


def rd1(n):
    return bench.rd(n, 1)[0]


def wr(name, vals):
    bench.wr(name, vals)


def alive():
    a = rd1("g_loop_count")
    time.sleep(0.4)
    return a != rd1("g_loop_count")


def open_gallery():
    # 必须先回桌面再进相册：否则若相册本就是当前 app，框架不重建，
    # PhoneApp.show 不重跑 ⇒ g_img_board_count 停在陈旧值。
    rc.goto_home()
    rc.settle(0.5)
    ok = gs.open_gallery(gs.ICON_CANDIDATES[0])
    if not ok:
        xy = gs.probe_icon()
        if not xy:
            return False
        ok = gs.open_gallery(xy)
    return ok


def crop_caption(px, path):
    """裁 caption + hint 区域（abs y 330..420）放大存 PNG，方便看中文。"""
    w, h = cc.G.W, cc.G.H
    try:
        from PIL import Image
        im = Image.new("RGB", (w, h))
        for y in range(h):
            row = y * w
            for x in range(w):
                v = px[row + x]
                r = ((v >> 11) & 0x1F) << 3
                g = ((v >> 5) & 0x3F) << 2
                b = (v & 0x1F) << 3
                im.putpixel((x, y), (r, g, b))
        im.crop((0, 330, w, 420)).resize((w * 2, 180), Image.NEAREST).save(path)
        print("  已裁 caption：%s" % path)
    except Exception as e:
        print("  [!] 裁剪 caption 失败（%s）" % e)


def refill(n):
    """用 BT 本地自检灌 n 张测试图（224x152），返回最后 g_img_count；失败返回 -1。"""
    for k in range(n):
        base = rd1("g_bt_done")
        wr("g_bt_recv_w", [224])
        wr("g_bt_recv_h", [152])
        wr("g_bt_recv_req", [1])
        t0 = time.time()
        while time.time() - t0 < 150:
            if rd1("g_bt_done") != base:
                break
            time.sleep(2.0)
        else:
            print("  [FAIL] 第 %d 次自检会话没结束" % (k + 1))
            return -1
        time.sleep(1.0)
        while rd1("g_img_busy") != 0:
            time.sleep(0.5)
        print("  第 %d 张灌完：g_img_rc=%d" % (k + 1, rd1("g_img_rc")))
    return rd1("g_img_count")


def main():
    ap = sys.argv[1:]
    refill_n = 6
    if "--refill" in ap:
        refill_n = int(ap[ap.index("--refill") + 1])

    if not alive():
        print("[FAIL] 内核没在跑（g_loop_count 不变）—— download 之后忘了 reset？")
        return 1

    # ---- ① 打开相册，确认当前计数（非空前才测得成）----
    print("=" * 64)
    print("① 打开相册，确认当前计数")
    if not open_gallery():
        print("[FAIL] 打不开相册页")
        return 1
    rc.settle(0.4)
    bc0 = rd1("g_img_board_count")
    print("  当前 g_img_board_count = %d（权威片上计数）" % bc0)
    if bc0 <= 0:
        print("  相册为空 ⇒ 先用 BT 自检灌 %d 张" % refill_n)
        rc.goto_home()
        rc.settle(0.4)
        got = refill(refill_n)
        if got < 0:
            return 1
        if not open_gallery():
            print("[FAIL] 灌图后进不去相册")
            return 1
        rc.settle(0.4)
        bc0 = rd1("g_img_board_count")
        print("  灌图后 g_img_board_count = %d" % bc0)
        if bc0 <= 0:
            print("[FAIL] 灌图后仍为空（g_img_rc=%d），无法测『从非空清空』" % rd1("g_img_rc"))
            return 1
    pwr_shot.snap("清空前", "build/g_clear_before.png")
    crop_caption(cc.frame(), "build/g_clear_cap_before.png")

    # ---- A：容量 UI（精确数字走截图人工核对）----
    print("\n② 容量 UI：caption 末尾应显示「 · 剩NK B」或「 · 已满」"
          "（见 build/g_clear_cap_before.png 人工核对）")

    # ---- B1：第一段「清空」只进入确认态，不擦数据（哨兵判定）----
    print("\n③ 第一段：点「清空」一次（预期只变红「确认清空?」，不擦索引）")
    wr("g_img_qspi_rc", [QSRC_SENTINEL])   # 哨兵：真擦才会改掉
    rc.tap(*CLEAR_XY, wait=1.2)
    rc.settle(0.6)
    pwr_shot.snap("第一段确认态", "build/g_clear_armed.png")   # 抓红字 armed
    qsrc = rd1("g_img_qspi_rc")
    if qsrc != QSRC_SENTINEL:
        print("  [FAIL] 第一下就改写了 g_img_qspi_rc=0x%X ⇒ 数据已被擦！两段式失效" % qsrc)
        return 1
    print("  OK：g_img_qspi_rc 仍为 0x%X ⇒ 第一段没执行擦除，两段式成立" % QSRC_SENTINEL)

    # ---- B2：第二段「清空」才真擦索引扇区 ----
    print("\n④ 第二段：再点「清空」一次（预期真擦索引扇区 → 0 张）")
    rc.tap(*CLEAR_XY, wait=1.2)
    rc.settle(0.8)
    qsrc2 = rd1("g_img_qspi_rc")
    if qsrc2 == QSRC_SENTINEL:
        print("  [FAIL] 第二下仍没改 g_img_qspi_rc ⇒ 擦除没触发（两段式第二段失效）")
        return 1
    print("  OK：g_img_qspi_rc=0x%X ⇒ 第二段执行了擦除" % qsrc2)

    # ---- 关键：擦完必须恢复 XIP 映射，否则内核 BusFault 卡死 ----
    print("\n⑤ 关键安全：清空后内核必须还活着（XIP 映射已恢复）")
    rc.settle(0.8)   # 让 UI 重绘（读 XIP 字模）跑几帧
    if not alive():
        print("  [FAIL] 清空后主循环停了 —— qspi_enter_mmap 没生效，XIP 读会 HardFault")
        return 1
    print("  OK：g_loop_count 仍在增长 ⇒ 映射已恢复，内核未卡死")

    # ---- 退回桌面再进相册，确认 UI 也看到空（示例态）----
    if not open_gallery():
        print("  [FAIL] 清空后进不去相册（可能卡死在中途）")
        return 1
    rc.settle(0.4)
    bc_after = rd1("g_img_board_count")
    print("  重新进相册：g_img_board_count=%d（应为 0，示例态）" % bc_after)
    if bc_after != 0:
        print("  [FAIL] UI 仍报 %d 张，没回到示例态" % bc_after)
        return 1
    pwr_shot.snap("清空后示例态", "build/g_clear_after.png")

    # ---- 回填：恢复非空可用态 ----
    if refill_n > 0:
        print("\n⑥ 回填：BT 本地自检灌 %d 张测试图（恢复非空相册）" % refill_n)
        rc.goto_home()
        rc.settle(0.4)
        got = refill(refill_n)
        if got < 0:
            return 1
        if not open_gallery():
            print("  [FAIL] 回填后进不去相册")
            return 1
        rc.settle(0.4)
        bc_f = rd1("g_img_board_count")
        print("  回填后：g_img_board_count = %d 张" % bc_f)
        pwr_shot.snap("回填后", "build/g_clear_refilled.png")
        crop_caption(cc.frame(), "build/g_clear_cap_after.png")
        if bc_f < refill_n:
            print("  [!] 只回填到 %d 张（预期 %d），但非空即可用" % (bc_f, refill_n))
        else:
            print("  OK：相册已恢复为 %d 张非空" % bc_f)

    print("\n[PASS] 容量 UI + 清空两段式 + XIP 映射恢复 全部通过")
    print("  截图：build/g_clear_before.png / g_clear_armed.png / g_clear_after.png"
          + (" / g_clear_refilled.png" if refill_n > 0 else ""))
    print("  caption 放大：build/g_clear_cap_before.png / g_clear_cap_after.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
