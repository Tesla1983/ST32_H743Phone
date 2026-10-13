#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""UI 卡死复现 / 定位（2026-10-13）

【用户报的现象】
「快速点播放、暂停，几个循环之后整个 UI 卡住无法滑动；按我说的操作多次，必然卡住。」

这把问题从"丢写（音量/静音）"升级成了**硬卡死** —— 主循环完全不转，
触摸/滑动全部无响应。二者机理完全不同，所以单独一条脚本专门定位。

【怎么判"硬卡死" vs "偶发顿挫"】
板上已有现成的判据（不必新增任何诊断代码）：
  · g_loop_count —— 主循环拍计数。硬卡死 = 它在 2 秒内完全不变。
  · g_mark_tick / g_mark_poll / g_mark_refr —— 主循环内三个断点。
      三者相等且等于 g_loop_count  ⇒ 跑完了那一拍的全部步骤（健康）
      g_mark_tick == g_loop_count 但 poll/refr 落后 ⇒ 卡在 touch_port_poll 之后
      poll 到了但 refr 落后                ⇒ 卡在渲染（YMGUI_Refresh / BoardTick）
  · g_fault.magic == 0xFA017CA7 ⇒ **是内核故障**（Hard/Bus/Usage/MemManage），
      不是业务死循环。kind/cfsr/bfar 直接给出故障类型与出错地址。

⚠ 必须用**真实 UI 注入路径**（g_bdtap_*，与手指触摸同一条路），
   不能用 g_wav_test / g_wav_snap_req 那条"直达 wav_play_*"的捷径 ——
   用户是点按钮卡的，捷径绕过了 UI 回调、动画、控件状态机。

用法：
    python tools/ui_freeze_repro.py            # 默认 12 轮播放/暂停
    python tools/ui_freeze_repro.py 30         # 30 轮
    python tools/ui_freeze_repro.py 30 --no-reset   # 不复位直接跑
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench

CYCLES = 12
for a in sys.argv[1:]:
    if a.isdigit():
        CYCLES = int(a)
DO_RESET = "--no-reset" not in sys.argv

# 音乐 app 界面坐标。
#   ⚠⚠ 坐标必须**抓屏核对**过，不能照抄 app_create 的 view 局部坐标：
#      PhoneUI_button(view,134,344,52,52) 是**相对 view** 的，view 在屏幕上有偏移
#      （app_page 从 dock 位置展开 + header 占高），实际屏幕位置要靠抓屏定位。
#      2026-10-13 实测：按局部坐标算出的 (160,370) 落在**时间文字**上，
#      点了几十次都没开始播（wav_state 恒 0）—— 这就是"复现不出来"的原因。
#      抓屏（build/_shot_music.png）量得按钮中心 ≈ (160,417)，
#      手动验证：点它 wav_state 0 → 1、ao_playing 0 → 1 ⇒ 坐标正确。
BTN_PLAY = (160, 417)          # 播放/暂停按钮中心（抓屏核对，selftest 亦用 405）
ICON_MUSIC = (123, 290)        # 桌面音乐图标（selftest 同款）
BTN_HOME = (160, 464)          # 底部返回桌面（导航栏最左）

FAULT_MAGIC = 0xFA017CA7


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          encoding="utf-8", errors="ignore")


def write_var(vals):
    bench.wr("g_bdtap_x", [vals[0]])
    bench.wr("g_bdtap_y", [vals[1]])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    seq = bench.rd("g_bdtap_seq", 1)[0]
    bench.wr("g_bdtap_seq", [seq + 1])


def alive(period=1.0):
    """返回 (是否在转, loop0, loop1)。"""
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(period)
    l1 = bench.rd("g_loop_count", 1)[0]
    return l1 != l0, l0, l1


def marks():
    return (bench.rd("g_loop_count", 1)[0],
            bench.rd("g_mark_tick", 1)[0],
            bench.rd("g_mark_poll", 1)[0],
            bench.rd("g_mark_refr", 1)[0])


def fault():
    f = sh("probe-rs read %s --chip %s b32 0x%08x 9"
           % (bench.PROBE, bench.CHIP, bench.T["g_fault"][0]))
    tok = [x for x in f.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < 9:
        return None
    v = [int(x, 16) for x in tok[:9]]
    return {"magic": v[0], "kind": v[1], "cfsr": v[2], "hfsr": v[3],
            "mmfar": v[4], "bfar": v[5], "shcsr": v[6], "ccr": v[7], "count": v[8]}


def report_kind(f):
    KIND = {1: "HardFault", 2: "MemManage", 3: "BusFault", 4: "UsageFault", 5: "NMI"}
    print("    g_fault: magic=%#x count=%d kind=%s" %
          (f["magic"], f["count"], KIND.get(f["kind"], "?")))
    if f["magic"] == FAULT_MAGIC:
        print("    cfsr=%#010x hfsr=%#010x bfar=%#010x mmfar=%#010x" %
              (f["cfsr"], f["hfsr"], f["bfar"], f["mmfar"]))
        # CFSR 分解
        if f["cfsr"] & (1 << 9):
            print("    ⇒ PRECISERR：精确总线错误，出错地址 = bfar = %#010x" % f["bfar"])
        if f["cfsr"] & (1 << 8):
            print("    ⇒ BFARVALID")
        if f["cfsr"] & (1 << 1):
            print("    ⇒ DACCVIOL：数据访问违例，地址 = mmfar = %#010x" % f["mmfar"])
        if f["hfsr"] & (1 << 30):
            print("    ⇒ HFSR.FORCED：某 fault 未被单独使能，被升级成 HardFault")
        uf = (f["cfsr"] >> 16) & 0xFFFF
        if uf:
            print("    ⇒ UFSR=%#06x（bit1=UNDEFINSTR bit3=NOCP bit4=INVPC bit8=UNALIGNED）" % uf)


def main():
    print("=" * 78)
    print("UI 卡死复现：真实点击路径（g_bdtap）快速点播放/暂停 %d 轮" % CYCLES)
    print("=" * 78)

    if DO_RESET:
        print("复位板子（干净起点）...")
        sh("probe-rs reset %s --chip %s" % (bench.PROBE, bench.CHIP))
        time.sleep(4.0)

    ok, l0, l1 = alive(1.0)
    if not ok:
        print("[ABORT] 复位后主循环就没在转（loop %d）—— 先排查基础状态" % l0)
        report_kind(fault() or {})
        return 1
    print("健康：主循环 %d 拍/秒" % (l1 - l0))
    hb = {"g_boot_magic": bench.rd("g_boot_magic", 1)[0],
          "g_shell_rc": bench.rd("g_shell_rc", 1)[0]}
    if hb["g_boot_magic"] != 0x594D4731 or hb["g_shell_rc"] != 0:
        print("[ABORT] 固件没正常起来：", hb)
        return 1

    # ---- 打开音乐 app（走真实点击：桌面图标）----
    print("点击桌面音乐图标 %s 打开 app ..." % (ICON_MUSIC,))
    write_var(ICON_MUSIC)
    time.sleep(1.2)
    st = bench.rd("g_wav_state", 1)[0]
    ok, l0, l1 = alive(1.0)
    print("  → 打开后 loop %d→%d（%s）wav_state=%d" %
          (l0, l1, "在转" if ok else "★卡住★", st))
    if not ok:
        print("[HIT] ★打开 app 就卡死了★")
        report_kind(fault() or {})
        return 2

    # ---- 快速交替点击播放/暂停 ----
    # ⚠ "快速" 是用户的描述，也是复现的关键：间隔越短，越容易撞上
    #   "起播还没走完就被暂停/再次起播"的重入窗口。
    GAP = 0.25
    for i in range(1, CYCLES + 1):
        write_var(BTN_PLAY)
        time.sleep(GAP)

        ok, l0, l1 = alive(0.35)
        if not ok:
            print("-" * 78)
            print("[HIT] ★第 %d 次点击后主循环停转★" % i)
            print("  loop: %d → %d（不涨）" % (l0, l1))
            c, tk, po, rf = marks()
            print("  marks: loop=%d tick=%d poll=%d refr=%d" % (c, tk, po, rf))
            if tk < c:
                print("  ⇒ 卡在【注入 Tick 之后】")
            elif po < c:
                print("  ⇒ 卡在【touch_port_poll 之后 / 渲染之前】")
            elif rf < c:
                print("  ⇒ 卡在【YMGUI_Refresh / BoardTick（渲染）】")
            else:
                print("  ⇒ marks 都追上了 ⇒ 不是主循环卡，可能是中断里卡住/ISR 死循环")
            print("  状态：wav_state=%d ao_playing=%d ao_stage=%d es_wr_ok=%d es_wr_ng=%d es_fail=%d reg32_wr=%#x" %
                  (bench.rd("g_wav_state", 1)[0], bench.rd("g_ao_playing", 1)[0],
                   bench.rd("g_ao_stage", 1)[0], bench.rd("g_es_wr_ok", 1)[0],
                   bench.rd("g_es_wr_ng", 1)[0], bench.rd("g_es_fail", 1)[0],
                   bench.rd("g_es_reg32_wr", 1)[0]))
            report_kind(fault() or {})
            return 2

        st = bench.rd("g_wav_state", 1)[0]
        if i % 4 == 0 or i <= 4:
            print("  轮 %2d: loop %d→%d 转  wav_state=%d ao_playing=%d" %
                  (i, l0, l1, st, bench.rd("g_ao_playing", 1)[0]))

    print("=" * 78)
    ok, l0, l1 = alive(1.0)
    print("跑满 %d 轮：%s（loop %d→%d）" % (CYCLES, "未复现卡死" if ok else "★最终卡死★", l0, l1))
    f = fault()
    if f and f["magic"] == FAULT_MAGIC:
        print("⚠ 期间发生过内核故障：")
        report_kind(f)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
