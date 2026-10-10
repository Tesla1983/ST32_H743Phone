#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""真实路径验收：点「开始接收」→ `$?BTF,OPEN` → 对端开经典蓝牙 + 起 SPP 服务端

【为什么单开一个脚本】
`tools/bt_file_check.py` 与 `tools/bt_ui_shot.py` 走的都是**自测路径**
（`$?BTF,TEST,<w>,<h>`：让对端合成一张 BMP）。那条路能验整条下游管道，
但**验不到真实入口**：用户实际点的是「开始接收」按钮，它走的是
`$?BTF,OPEN` —— 对端要真的把**经典蓝牙无线电打开、把 SPP 服务端拉起来**。
这条路径在 2026-10-10 首次上板跑，**一次就暴露出一个真缺陷**（见下 R2）。

【本条路径不需要手机也能验到哪一步】
能验到"对端蓝牙已开、SPP 服务端已就绪、本板进入等待"；
再往后（手机连上来发字节）必须有手机，不在本脚本范围内。

判据
-----------------------------------------------------------------------------
  R0  真实 OPEN 通    点「开始接收」后 g_bt_sess +1、g_bt_err 仍 0、进入 WAIT
  R1  对端蓝牙被打开  $RD 回流 g_net_radio_bt 0→1、$BT 帧 state=1（对端 WAIT）
  R2  等待期不被误判死 **持续观察 ≥20 s（远超旧的 12 s 判死线）后仍 err==0**
                      ⚠ 这正是首跑暴露的缺陷：`s_retry` 语义写错成"累计失败"，
                        等待期里每一笔 GET 丢包都被记成失败，12 s 累积 8 次
                        就判 BTE_BADSEQ 死掉（实测 err=6、get_n=322、一个字节没传）
  R3  UI 文字真的翻了  抓屏看状态行 = "等待对端发送"
                      ⚠ 旧代码的变化检测只看 (state, progress)，而 s_opening 1→0
                        时这两者都没变 ⇒ 文字会永远卡在"正在启动蓝牙…"
  R4  中止            点「中止接收」→ 会话结束、UI 回到可开始态、不留半截文件
  R5  等待上限仍在    把 g_bt_wait_max_ms 写小 → 再开一次 → 到点应判 STALL
                      （证明"修掉 R2 之后没有变成无限挂着"）

用法
----
    python tools/bt_real_check.py              # 全部（约 70 s）
    python tools/bt_real_check.py --keep       # 跑完**不中止**，把会话留着手工看
    python tools/bt_real_check.py --wait-max 6000   # R5 用别的上限（ms）
"""
import argparse
import sys
import time

sys.path.insert(0, "tools")
import bench
import bt_ui_shot as ui          # 复用导航与坐标（避免两处各写一份导致漂移）
import caret_check as cc
import pwr_shot
import ribbon_check as rc

BT_WAIT = 1                      # 与 src/bt_recv.h 的 BT_RECV_WAIT 同值
BT_ERROR = 4
BTE_STALL = 5

# 等待期观察时长：必须**明显大于**旧的 12 s 判死线，否则验不出 R2。
OBSERVE_S = 22


def rd(name):
    """读一个 32 位量并按有符号解释（脚本里判负数用）。"""
    v = bench.rd(name, 1)[0]
    return v - 0x100000000 if v >= 0x80000000 else v


def state():
    return {
        "sess": rd("g_bt_sess"), "err": rd("g_bt_err"), "rc": rd("g_bt_rc"),
        "btf_state": rd("g_net_btf_state"), "bt_frames": rd("g_net_bt_pkts"),
        "rd_frames": rd("g_net_rd_pkts"), "radio_bt": rd("g_net_radio_bt"),
        "get_n": rd("g_bt_get_n"), "bytes": rd("g_bt_bytes"),
    }


def show(tag, s):
    print("  [%s] sess=%d err=%d rc=%d | 对端 $BT: state=%d 帧=%d | $RD 帧=%d radio_bt=%d"
          " | GET=%d 已收=%d B"
          % (tag, s["sess"], s["err"], s["rc"], s["btf_state"], s["bt_frames"],
             s["rd_frames"], s["radio_bt"], s["get_n"], s["bytes"]))


def click_start():
    rc.tap(*ui.BT_ACT_XY, wait=0.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true",
                    help="跑完不中止（把会话留着，方便自己抓屏/看日志）")
    ap.add_argument("--wait-max", type=int, default=6000,
                    help="R5 里写入 g_bt_wait_max_ms 的值（ms）")
    a = ap.parse_args()

    if rc.rd1("g_boot_magic") != 0x594D4731:
        print("[FAIL] g_boot_magic 不对 —— 固件没在跑（download 后要 reset）")
        return 1

    print("导航：桌面第 2 页 → 文件管理 → 蓝牙接收")
    rc.goto_home()
    if not ui.open_files():
        print("[FAIL] 打不开文件管理")
        return 1
    for i in range(6):
        rc.tap(*ui.BT_ROW_XY, wait=1.0)
        rc.settle(0.5)
        if ui.in_bt_page():
            break
    if not ui.in_bt_page():
        print("[FAIL] 点不进去「蓝牙接收」分类")
        return 1
    print("  ✅ 已在蓝牙接收页（g_files_folder=%d）" % rd("g_files_folder"))

    res = []
    s0 = state()
    show("触发前", s0)

    # ================= R0 / R1：真实 OPEN =================
    print("=" * 70)
    print("点「开始接收」@%s（真实路径：$?BTF,OPEN）" % (ui.BT_ACT_XY,))
    t_click = time.time()
    click_start()
    # 等对端把蓝牙拉起来（板上实测 ~560 ms），给 3 s 余量
    time.sleep(3.0)
    s1 = state()
    show("+3 s", s1)

    print("R0  真实 OPEN 通（g_bt_sess +1 / err==0）")
    if s1["sess"] > s0["sess"] and s1["err"] == 0:
        print("  [PASS] 会话 %d → %d，err 仍 0" % (s0["sess"], s1["sess"]))
        res.append(("R0", True))
    else:
        print("  [FAIL] sess=%d err=%d（%s）"
              % (s1["sess"], s1["err"],
                 "对端没回 $!RS,BTF,1" if s1["err"] == 2 else "见 src/bt_recv.c 的 BTE_*"))
        res.append(("R0", False))

    print("=" * 70)
    print("R1  对端蓝牙被打开（$RD 回流 radio_bt 0→1；$BT state=%d）" % BT_WAIT)
    bt_up = s1["radio_bt"] == 1 and s1["rd_frames"] > s0["rd_frames"]
    btf_ok = s1["btf_state"] == BT_WAIT
    if bt_up and btf_ok:
        print("  [PASS] radio_bt %d → %d（$RD +%d 帧）、对端 state=%d（WAIT，在等手机发）"
              % (s0["radio_bt"], s1["radio_bt"],
                 s1["rd_frames"] - s0["rd_frames"], s1["btf_state"]))
        res.append(("R1", True))
    else:
        print("  [FAIL] radio_bt=%d（$RD +%d 帧）对端 state=%d"
              % (s1["radio_bt"], s1["rd_frames"] - s0["rd_frames"], s1["btf_state"]))
        res.append(("R1", False))

    # ================= R2：等待期不被误判死 =================
    print("=" * 70)
    print("R2  等待期不被误判死：持续观察 %d s（旧的判死线是 12 s）" % OBSERVE_S)
    t0 = time.time()
    while time.time() - t0 < OBSERVE_S:
        time.sleep(4.0)
        s = state()
        # ⚠ 不要中途抓屏：抓一帧要停核好几秒，会把 GET 超时喂饱（虽然 rc==0 会清零 s_retry，
        #   但没必要自找干扰）。观察期只读计数器，一次 ~0.1 s。
        print("    t=%4.1f s  err=%d  GET=%d  （已收 %d B）"
              % (time.time() - t_click, s["err"], s["get_n"], s["bytes"]))
    s2 = state()
    show("观察结束", s2)
    if s2["err"] == 0 and s2["sess"] == s1["sess"] and s2["get_n"] > s1["get_n"]:
        print("  [PASS] 等了 %.0f s 仍在 WAIT、err 仍 0，且 GET 一直在轮询（%d → %d 笔）"
              % (time.time() - t_click, s1["get_n"], s2["get_n"]))
        res.append(("R2", True))
    else:
        print("  [FAIL] err=%d（5=STALL，6=BADSEQ）—— 等待期被误判死了"
              % s2["err"])
        res.append(("R2", False))

    # ================= R3：UI 文字真的翻了 =================
    print("=" * 70)
    print("R3  UI 状态行应是「等待对端发送」（不能卡在「正在启动蓝牙…」）")
    px = ui.shot("真实路径-等待中", "build/bt_real_wait.png")
    txt = ui.crop_and_save("realwait", px, ui.TEXT_ABS)
    print("  放大图：%s （人眼确认这一行文字）" % txt)
    # 文字没法从画面外读，所以这里只产出证据图，结论由人眼看；
    # 脚本能自动判的是"进度条仍未填"（等待期 total 未知 ⇒ 画 0）。
    fg, w = ui.bar_stats("R3", px)
    if fg <= 3:
        print("  [PASS] 等待期进度条仍为 0（没拿 0/0 算成百分比）")
        res.append(("R3", True))
    else:
        print("  [FAIL] 等待期进度条非空（%d/%d）" % (fg, w))
        res.append(("R3", False))

    # ================= R4：中止 =================
    print("=" * 70)
    if a.keep:
        print("R4  跳过（--keep：故意把会话留着）")
    else:
        print("R4  「中止接收」：中止后**能立刻开起一个新会话**（判据见下）")
        rc.tap(*ui.BT_ACT_XY, wait=1.2)
        rc.settle(1.0)
        s3 = state()
        show("中止后", s3)
        ui.shot("真实路径-中止后", "build/bt_real_abort.png")
        print("  证据图：build/bt_real_abort.png（人眼确认按钮回到「开始接收」）")

        # ⚠ 判据为什么是"能立刻重开"，而不是"err 归零 / 对方 state 变 0"
        #   （2026-10-10 首跑亲测，两个看着最自然的判据都是错的）：
        #   · `g_bt_err` —— abort 走 `reset_session()`，**不清** g_bt_err。
        #     那个量按设计是"最近一次会话的结果"，会一直留着。
        #   · `g_bt_rc == -1` —— `begin()` 里也写 -1，跟 abort 撞在一起，区分不了。
        #   · 对端 `$BT` state 变 0 —— 依赖对方**收到**了 ABORT。而命令通道有
        #     6%~20% 丢包，首跑实测三遍全丢（$BT帧 不增、对端日志无 ABORT、
        #     对端 state 停在 1）⇒ 拿它当判据会把"一次正常的中止"判成失败。
        #   ⇒ 真正该问的是"**用户能不能接着用**"：`begin()` 在 WAIT/RECV 时会
        #     返回 -1 拒绝重开，所以"能开起新会话"就证明本板状态机确实复位了。
        #     对端那边的兜底是 `bt_file_open()` 第一行就 `session_reset()`（幂等）。
        click_start()
        time.sleep(2.2)
        s3b = state()
        show("中止→重开", s3b)
        if s3b["sess"] == s3["sess"] + 1 and s3b["err"] == 0:
            print("  [PASS] 会话 %d → %d 且 err=0 ⇒ 中止已生效（否则 begin() 会拒绝重开）"
                  % (s3["sess"], s3b["sess"]))
            res.append(("R4", True))
        else:
            print("  [FAIL] 中止后重开失败（sess %d → %d、err=%d）"
                  % (s3["sess"], s3b["sess"], s3b["err"]))
            res.append(("R4", False))
        # 把这个刚开的会话收掉，把干净态交给 R5
        rc.tap(*ui.BT_ACT_XY, wait=1.2)
        rc.settle(0.8)
        show("清理后", state())

    # ================= R5：等待上限仍在（防无限挂着）=================
    print("=" * 70)
    print("R5  等待上限仍生效：把 g_bt_wait_max_ms 写为 %d ms 后重开，到点应判 STALL"
          % a.wait_max)
    bench.wr("g_bt_wait_max_ms", [a.wait_max])
    print("  回读 g_bt_wait_max_ms=%d" % rd("g_bt_wait_max_ms"))
    click_start()
    t0 = time.time()
    s4 = None
    while time.time() - t0 < (a.wait_max / 1000.0) + 12:
        time.sleep(3.0)
        s4 = state()
        if s4["err"] != 0:
            break
    show("到点后", s4)
    if s4 and s4["err"] == BTE_STALL:
        print("  [PASS] 到点判 BTE_STALL(5)，等了 %.0f s（上限 %d ms + 余量）"
              % (time.time() - t0, a.wait_max))
        res.append(("R5", True))
    else:
        print("  [FAIL] 期望 err=%d(STALL)，实际 err=%d —— 等待上限没生效（会无限挂着）"
              % (BTE_STALL, s4["err"] if s4 else -1))
        res.append(("R5", False))
    # 还原默认值，别把 3 分钟的上限留在板上
    bench.wr("g_bt_wait_max_ms", [180000])

    print("=" * 70)
    for k, v in res:
        print("  %-4s %s" % (k, "PASS" if v else "FAIL"))
    allp = all(v for _, v in res)
    print("-" * 70)
    print("证据图：build/bt_real_wait.png（等待态整屏）+ build/bt_ui_realwait_zoom.png（说明区放大）")
    print("[%s] 真实路径（$?BTF,OPEN → 对端开蓝牙 + SPP）在板验收%s"
          % ("PASS" if allp else "FAIL", "通过" if allp else "有未通过项"))
    return 0 if allp else 1


if __name__ == "__main__":
    sys.exit(main())
