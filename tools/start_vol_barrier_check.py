#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""补丁16 回归哨兵：**起播安全栅**（先静音、再开时钟）—— 2026-10-16

【要证明的东西】
用户报障："点击播放、刚开始播放的瞬间有爆音（杂音/破音）"。
根因（代码级）：停止/暂停**不给 codec 断电**（`es8311_dac_become_silent()` 只写 REG32=0；
`ao_pause_stop_transport()` 只关传输）⇒ 停止后芯片是"通电 + REG31 未静音"，
只差一个非零 REG32 就能出声。而 `wav_play_start()` 在 `audio_out_start()` **之前**调
`audio_out_set_volume()`，旧实现里那句**无条件写 REG32** ⇒ 开 CSTART 那一刻音量已是满值
⇒ DAC 立刻以满音量送出音乐前几毫秒，直到下一拍 tick 的 `dac_path_regs(0x00)` 才拉回 0。
实测这段窗口 = `g_ao_t_dacwake_ms`（≈17~33 ms）⇒ 一声短促爆音 + 一次"掉静音再升起"的顿挫。

【怎么把"有没有带着音量开钟"变成可测数字】
`audio_out_start()` 在 CSTART **之前**把芯片 REG32 回读进 `g_ao_vol_at_cstart`。
  0x00 = 安全；非 0 = 带着音量开钟（爆音条件成立）；高位 0x100 置 1 = 读失败。

【判据】
  B1（复现）关栅 `g_ao_start_barrier_en=0` + 起播前把 REG32 弄满 ⇒ `g_ao_vol_at_cstart` 必须 **>0**
     —— 不然这个栅根本不承重，"修好了"就无从谈起。
  B2（修复）开栅（默认 1）+ 同样前置 ⇒ 必须 **== 0x00**，重复 N 轮全成立。
  B3（没吞音量）开栅起播后 ramp 走完，REG32 回读必须 == 目标（UI 音量换算值）。
  B4（在放）`g_ao_frames` 在涨、`g_ao_dac_ready=1`。

⚠ 前置条件是**必须**的：复位后第一次起播时 `s_codec_ready==0`，`es8311_set_volume_ui()`
  会直接返回（不动总线）⇒ 必须先跑一次正常播放把 codec 带起来，"弄满音量"才有效。

用法
----
    python tools/start_vol_barrier_check.py          # 默认 4 轮
    python tools/start_vol_barrier_check.py 8
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

STOP_MS = 1200          # 等淡出(≈204 ms) + 传输收尾
PLAY_MS = 2500          # 等唤醒 + 8 拍 ramp 走完
WAIT_MS = 300


def W(n, v):
    bench.wr(n, [v])


def rd(n):
    return bench.rd(n, 1)[0]


def stop():
    W("g_wav_test", 2)
    time.sleep(STOP_MS / 1000.0)


def arm_full_volume():
    """模拟"用户在停止态拖过音量滑杆"（main.c 的 app_volume_persist 同一条调用）。"""
    W("g_wav_test", 6)
    time.sleep(WAIT_MS / 1000.0)


def play():
    W("g_wav_test", 1)
    time.sleep(PLAY_MS / 1000.0)


def snap():
    return dict(
        vol_at_cstart=rd("g_ao_vol_at_cstart"),
        dacwake=rd("g_ao_t_dacwake_ms"),
        ready=rd("g_ao_dac_ready"),
        frames=rd("g_ao_frames"),
        rb_vol=rd("g_es_rb_vol"),
        ui=rd("g_audio_volume"),
        playing=rd("g_ao_playing"),
    )


def fmt(s):
    v = s["vol_at_cstart"]
    vt = ("读失败" if (v & 0x100) else "0x%02X" % (v & 0xFF))
    return ("vol_at_cstart=%s dacwake=%d ms ready=%d frames=%d rb_vol=0x%02X(UI=%d) playing=%d"
            % (vt, s["dacwake"], s["ready"], s["frames"], s["rb_vol"], s["ui"], s["playing"]))


def main():
    n = 4
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        n = int(sys.argv[1])

    if rd("g_boot_magic") != 0x594D4731:
        print("固件没在跑")
        return 1
    for k in ("g_ao_vol_at_cstart", "g_ao_start_barrier_en"):
        if k not in bench.T:
            print("固件缺补丁16 诊断量（%s）⇒ 请先烧补丁16 固件" % k)
            return 1

    # 目标 REG32（UI 音量换算值）：0..100 → 0..0xBF
    ui = rd("g_audio_volume")
    target = (min(ui, 100) * 0xBF) // 100

    print("=" * 84)
    print("补丁16 哨兵：起播安全栅（先静音、再开时钟）")
    print("  证据 = 开 CSTART 前从芯片回读的 REG32（g_ao_vol_at_cstart）；0x00 = 安全")
    print("  UI 音量=%d ⇒ 目标 REG32=0x%02X；栅开关 = g_ao_start_barrier_en" % (ui, target))
    print("=" * 84)

    bad = []

    W("g_ao_use_dma", 1)
    time.sleep(0.3)

    # ---- 先把 codec 带起来（复位后第一次起播会跑完整 init）----
    stop()
    play()
    print("预热起播: %s" % fmt(snap()))

    # ---- B1：关栅 + 起播前把 REG32 弄满 ⇒ 必须能复现"带着音量开钟" ----
    print("\n【B1】关栅（g_ao_start_barrier_en=0）—— 预期：复现爆音条件（vol_at_cstart > 0）")
    W("g_ao_start_barrier_en", 0)
    stop()
    arm_full_volume()
    play()
    s_off = snap()
    print("      %s" % fmt(s_off))
    if (s_off["vol_at_cstart"] & 0x100) != 0:
        bad.append("B1 关栅时 REG32 回读失败，无法判定")
    elif s_off["vol_at_cstart"] == 0:
        bad.append("B1 关栅却仍读到 0x00 ⇒ 前置条件没造出来（arm 音量失败？）⇒ 本测试是空测")
    else:
        print("      ✅ 复现成功：关栅时确实带着 REG32=0x%02X 开时钟，窗口 = %d ms"
              % (s_off["vol_at_cstart"] & 0xFF, s_off["dacwake"]))

    # ---- B2/B3/B4：开栅，重复 N 轮 ----
    print("\n【B2】开栅（默认 1）—— 预期：vol_at_cstart 恒 == 0x00，共 %d 轮" % n)
    W("g_ao_start_barrier_en", 1)
    ok_n = 0
    for i in range(n):
        stop()
        arm_full_volume()
        play()
        s = snap()
        print("      轮 %d: %s" % (i + 1, fmt(s)))
        if (s["vol_at_cstart"] & 0x100) != 0:
            bad.append("B2 轮%d REG32 回读失败" % (i + 1))
        elif s["vol_at_cstart"] != 0:
            bad.append("B2 轮%d 带着 REG32=0x%02X 开钟（栅没生效）"
                       % (i + 1, s["vol_at_cstart"] & 0xFF))
        else:
            ok_n += 1
        if s["ready"] != 1:
            bad.append("B2 轮%d DAC 没唤醒" % (i + 1))
        if s["frames"] == 0:
            bad.append("B2 轮%d 没在推数据" % (i + 1))
        if s["rb_vol"] != target:
            bad.append("B3 轮%d ramp 后 REG32 回读=0x%02X ≠ 目标 0x%02X（音量被吞）"
                       % (i + 1, s["rb_vol"], target))

    # ---- 收尾 ----
    stop()
    print("\n【B4】收尾：playing=%d（应为 0）" % rd("g_ao_playing"))

    print("\n" + "=" * 84)
    print("  开栅后 vol_at_cstart==0x00 的轮次：%d/%d" % (ok_n, n))
    if bad:
        for b in bad:
            print("  ⚠ FAIL %s" % b)
        print("  ⇒ 存在 FAIL（见上）")
    else:
        print("  ⇒ 全部 PASS（关栅能复现、开栅能挡住、音量未被吞、仍在出声）")
    print("=" * 84)
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
