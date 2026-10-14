#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""补丁15d 回归哨兵：「快速连点 暂停→播放」那条**幂等分支**（2026-10-14）

【这条路径为什么重要】
    补丁15 把 audio_out_stop() 改成**异步淡出**后，`audio_out_start()` 开头多了一条
    幂等分支：`g_ao_playing != 0` 且 `s_pause_fade_ticks > 0` ⇒ 取消淡出、把音量 ramp
    回去。**真人快速连点「暂停→播放」正好落在这条分支上**。
    补丁15 首版该分支一个诊断量都不重置 ⇒ 板上实测 `g_ao_t_ramp_ms = 87783`（87.8 s）、
    平均帧/中断 1024 → 169.8。15b 修了时间原点；本脚本是它的回归哨兵。

【为什么必须用板上的「突发钩子」而不是主机两次写】
    ⚠ 一开始用主机做「写 2（停）→ sleep 30 ms → 写 1（起播）」。**实测无效**：
      `bench.wr()` 每次都要 spawn 一个 probe-rs 进程，单次实测 ≈ **400 ms**；
      而淡出只有 12 拍 × 主循环 **16.98 ms** ≈ **204 ms**
      ⇒ 两次写之间的真实间隔必然 > 淡出窗口 ⇒ 淡出每次都走完了，**永远不进幂等分支**。
      板上实测：`Δcalls=1 Δdone=1`（成对），5/5 轮判成"空测"。
    ⇒ 改用板上钩子 `g_wav_test = 5`：板子在**一拍**里发起暂停，`g_wav_burst_ms` 毫秒后
      自己续播。中间主循环照常跑 ⇒ 淡出正常推进，与真人连点时序一致。一次写即触发。

【判据（都是无竞态的直接量）】
    · 主证据：`g_ao_idem_cancel` **增量**（见 audio_out.c：只在幂等分支里 ++，单调只增）。
      ⚠ 不能用 `g_ao_stop_calls − g_ao_stop_done`：pause 路径两者都不动 ⇒ 恒 0。
    · 时序量 `pump1/dacwake/ramp` 必须 < SANE_MS（修复前实测 87783 ms）。
    · 对照组（间隔 1500 ms > 淡出 204 ms）**必须** Δidem == 0。

用法
----
    python tools/rapid_stop_start_check.py            # 默认 6 轮快连
    python tools/rapid_stop_start_check.py 10
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

SANE_MS  = 3000         # 时序量上限：正常起播 pump1≈4~11 ms、dacwake≈17~60、ramp≈124~296
FADE_MS  = 204          # AO_PAUSE_FADE_TICKS(12) × 主循环 16.98 ms（实测）
QUICK_MS = 30           # 快连间隔：必须 < FADE_MS 才能踩进幂等分支
SLOW_MS  = 1500         # 对照组间隔：必须 > FADE_MS，淡出应已走完

WATCH = ("g_ao_playing", "g_ao_frames", "g_ao_t_cstart_ms",
         "g_ao_t_pump1_ms", "g_ao_t_dacwake_ms", "g_ao_t_ramp_ms",
         "g_ao_wake_cnt", "g_ao_resume_calls", "g_ao_ring_used_min")


def idem():
    """幂等分支被走到的次数（单调只增，见文件头）。"""
    return bench.rd("g_ao_idem_cancel", 1)[0]


def burst(gap_ms):
    """板上跑一次「暂停 → gap_ms → 续播」。返回 (读数, idem 增量, burst_done 增量)。"""
    i0 = idem()
    d0 = bench.rd("g_wav_burst_done", 1)[0]

    bench.wr("g_wav_burst_ms", [gap_ms])
    bench.wr("g_wav_test", [5])          # 一次写即触发整段
    time.sleep(2.5)                      # 等 DAC 唤醒 + ramp 走完

    s = {}
    for k in WATCH:
        if k in bench.T:
            s[k] = bench.rd(k, 1)[0]
    return s, (idem() - i0), (bench.rd("g_wav_burst_done", 1)[0] - d0)


def report(tag, s, di, dd, need_hit, gap_ms):
    bad = []
    for k in ("g_ao_t_pump1_ms", "g_ao_t_dacwake_ms", "g_ao_t_ramp_ms"):
        v = s.get(k, 0)
        if v > SANE_MS:
            bad.append("%s=%d ms（>%d ⇒ 时间原点没被这次起播重新标定）" % (k, v, SANE_MS))
    if s.get("g_ao_frames", 0) == 0:
        bad.append("g_ao_frames==0（起播后没在推数据）")
    if s.get("g_ao_playing", 0) == 0:
        bad.append("g_ao_playing==0（突发结束后没在播）")
    if dd == 0:
        bad.append("g_wav_burst_done 没动 ⇒ 板上突发根本没跑（脚本/固件不配对？）")
    if need_hit and di == 0:
        bad.append("Δidem==0 ⇒ 没进幂等分支（间隔 %d ms 应 < 淡出 %d ms）" % (gap_ms, FADE_MS))
    if (not need_hit) and di != 0:
        bad.append("对照组进了幂等分支（间隔 %d ms 应 > 淡出 %d ms）" % (gap_ms, FADE_MS))
    print("  [%s] Δidem=%d Δburst_done=%d | 时序 pump1=%s dacwake=%s ramp=%s ms | playing=%s"
          % (tag, di, dd, s.get("g_ao_t_pump1_ms", -1), s.get("g_ao_t_dacwake_ms", -1),
             s.get("g_ao_t_ramp_ms", -1), s.get("g_ao_playing", -1)))
    print("        帧=%d wake=%d resume=%d ring_min=%s"
          % (s.get("g_ao_frames", -1), s.get("g_ao_wake_cnt", -1),
             s.get("g_ao_resume_calls", -1),
             ("-" if s.get("g_ao_ring_used_min", 0xFFFFFFFF) == 0xFFFFFFFF
              else str(s["g_ao_ring_used_min"]))))
    for b in bad:
        print("        ⚠ FAIL %s" % b)
    return len(bad) == 0


def main():
    n = 6
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        n = int(sys.argv[1])

    if bench.rd("g_boot_magic", 1)[0] != 0x594D4731:
        print("固件没在跑")
        return 1
    if "g_ao_idem_cancel" not in bench.T or "g_wav_burst_done" not in bench.T:
        print("固件缺补丁15d 诊断量（g_ao_idem_cancel / g_wav_burst_done）⇒ 请先烧 15d 固件")
        return 1

    bench.wr("g_ao_use_dma", [1])        # DMA 路径（用户听到"咔"的那条）
    time.sleep(0.3)

    print("=" * 78)
    print("补丁15d 哨兵：『暂停→立刻续播』的幂等分支会不会把诊断量时间原点带歪")
    print("  主证据 = g_ao_idem_cancel 增量（进一次分支 +1，单调只增，无竞态）")
    print("  时序判据 < %d ms（修复前实测 87783 ms）；快连 %d ms < 淡出 %d ms < 对照 %d ms"
          % (SANE_MS, QUICK_MS, FADE_MS, SLOW_MS))
    print("=" * 78)

    ok = True

    bench.wr("g_wav_test", [1]); time.sleep(1.5)           # 先起来

    s, di, dd = burst(QUICK_MS)
    ok &= report("T1 暂停→%d ms→续播" % QUICK_MS, s, di, dd, True, QUICK_MS)
    hit_n = 1 if di > 0 else 0

    s, di, dd = burst(SLOW_MS)
    ok &= report("T2 暂停→%d ms→续播（对照）" % SLOW_MS, s, di, dd, False, SLOW_MS)

    for i in range(n):
        s, di, dd = burst(QUICK_MS)
        hit_n += 1 if di > 0 else 0
        ok &= report("T3.%d 快连第 %d 轮" % (i + 1, i + 1), s, di, dd, True, QUICK_MS)

    # T4：极端值 gap=0（同一拍内暂停就续播）——最刁的边界，仍应走幂等分支且不死。
    s, di, dd = burst(0)
    t4 = report("T4 暂停→0 ms→续播（同拍边界）", s, di, dd, True, 0)
    ok &= t4
    hit_n += 1 if di > 0 else 0

    print("\n  踩中幂等分支：%d/%d 轮（T1 + T3×%d + T4；T2 为对照组，不要求命中）"
          % (hit_n, n + 2, n))
    print("  累计：idem_cancel=%d burst_done=%d"
          % (idem(), bench.rd("g_wav_burst_done", 1)[0]))

    bench.wr("g_wav_test", [2]); time.sleep(1.2)           # 收尾
    print("  收尾后 playing=%d（应为 0）" % bench.rd("g_ao_playing", 1)[0])
    print("=" * 78)
    print("  ⇒ %s" % ("全部 PASS" if ok else "存在 FAIL（见上）"))
    print("=" * 78)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
