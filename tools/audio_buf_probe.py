#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""抓"真正送进 ES8311 的采样"：把 I2S TX 的 DMA 乒乓缓冲（s_dma_buf）与环形缓冲（s_ring）
整块读出来做统计体检 —— 用来一刀切开两类噪声/咔哒来源：

    A. **数据通路**问题（补数没跟上 ⇒ 填硬静音 / 采样被改写 / 声道错位）
       ⇒ 在缓冲里表现为：连续 0 样点段（run）、相邻样点巨跳、L≠R。
    B. **codec / 模拟 / 时钟**问题（ES8311 通路、VMIDSEL、软 ramp、MCLK 比例…）
       ⇒ 缓冲里的波形**本身是干净的**，问题发生在芯片之后。

判据（本工具只报事实，不猜）
--------------------------
  Z  : 长度 ≥ MIN_RUN 的"全 0 连续段"个数  —— 音乐里出现概率极低 ⇒ 人为静音填充
  J  : 相邻样点跳变 > JMAX 的次数          —— 波形不连续（爆音/硬切断）
  M  : L≠R 的样点数                        —— 单声道展开是否一致
  S  : 每段的统计（min/max/中位/99.9% 分位）

用法
----
    python tools/audio_buf_probe.py            # 起播 → 播 4 s → 暂停 → 冻结后整块抓取
    python tools/audio_buf_probe.py 8          # 播 8 s 再抓
    python tools/audio_buf_probe.py --keep      # 不清暂停前的现场（默认会先 reset 计数）

⚠ 必须在"暂停/停播之后"读：DMA 停下后缓冲才是静止快照；播放中读会读到跨周期的撕裂数据。
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

MIN_RUN = 4      # 连续 ≥4 个 0 才当异常（单点 0 有可能是真实静音）
JMAX = 6000      # 相邻样点跳变阈值（满幅 32767；音乐相邻差分极少过 6000）

DMA_WORDS = 2048    # s_dma_buf = 4096 半字（2048 帧 × 2 声道）
RING_WORDS = 4096   # s_ring   = 8192 半字


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def split(words):
    """32 位字 → (L, R) 两个 int16 序列。"""
    L, R = [], []
    for w in words:
        L.append(s16(w))
        R.append(s16((w >> 16) & 0xFFFF))
    return L, R


def report(name, words, halves=2):
    """按**半缓冲**分别体检。

    ⚠ 为什么必须分半、而不是把 4096 半字当一条连续流：s_dma_buf 是**双缓冲**（乒乓），
      两个半缓冲是**两次独立填充**写进去的。播放中两者在流序上前后相接，但
      "前半末尾 → 后半开头"这个线性接缝**不是**音频上的相邻样点（环形/回卷语义）
      ⇒ 线性分析会在这里报一个巨大的假跳变（2026-10-14 实测 Δ34109，纯伪影）。
      所以：半缓冲**内部**的连续性是硬判据；两个接缝只单独报出来、不当缺陷。"""
    if not words:
        print("  %s: 抓不到数据" % name)
        return None
    L, R = split(words)
    n = len(L)
    per = n // halves

    all_runs, all_jumps, lr_neq = [], [], 0
    print("  ---- %s（%d 帧 = %d 个半缓冲）----" % (name, n, halves))
    for h in range(halves):
        a, b = h * per, (h + 1) * per
        seg = L[a:b]
        runs, cur, start = [], 0, 0
        for i, x in enumerate(seg):
            if x == 0:
                if cur == 0:
                    start = i
                cur += 1
            else:
                if cur >= MIN_RUN:
                    runs.append((a + start, cur))
                cur = 0
        if cur >= MIN_RUN:
            runs.append((a + start, cur))
        d = [abs(seg[i] - seg[i - 1]) for i in range(1, len(seg))]
        jumps = [(a + i + 1, d[i]) for i in range(len(d)) if d[i] > JMAX]
        peak = max((abs(x) for x in seg), default=0)
        nz = sum(1 for x in seg if x == 0)
        all_runs += runs
        all_jumps += jumps
        lr_neq += sum(1 for i in range(a, b) if L[i] != R[i])
        print("    半%d：峰值 %d  样点=0 的 %d 个  连续0段 %d 个  跳变>%d 的 %d 处"
              % (h, peak, nz, len(runs), JMAX, len(jumps)))

    # 接缝：只报告、不计入缺陷（见函数头说明）
    seams = []
    for h in range(1, halves):
        i = h * per
        seams.append((i, abs(L[i] - L[i - 1])))
    print("    接缝（结构性，非缺陷）：" + ", ".join("第%d点Δ%d" % s for s in seams))
    print("    L≠R 样点数 %d" % lr_neq)
    if all_runs:
        print("   ⚠ 连续 0 段：# " + ", ".join("起点%d长%d" % r for r in all_runs[:8])
              + ("  …" if len(all_runs) > 8 else ""))
    if all_jumps:
        print("   ⚠ 大跳变：# " + ", ".join("第%d点Δ%d" % j for j in all_jumps[:8])
              + ("  …" if len(all_jumps) > 8 else ""))
    if not all_runs and not all_jumps and lr_neq == 0:
        print("   ✅ 半缓冲内部波形干净：无静音填充、无断点、声道一致")
    return {"runs": all_runs, "jumps": all_jumps, "lr_neq": lr_neq}


def main():
    secs = 4.0
    for a in sys.argv[1:]:
        if not a.startswith("-"):
            secs = float(a)

    bm = bench.rd("g_boot_magic", 1)[0]
    if bm != 0x594D4731:
        print("固件没在跑（g_boot_magic=0x%08X）—— probe-rs download 后必须 reset" % bm)
        return 1

    bench.wr("g_wav_test", [1])          # 起播固定路径 WAV
    time.sleep(secs)
    keys = ("g_ao_underrun", "g_ao_under_dma", "g_ao_under_feed", "g_ao_under_short",
            "g_ao_under_first_ms", "g_ao_frames", "g_ao_dma_half", "g_ao_dma_cplt",
            "g_ao_ring_drop", "g_ao_ring_used_min", "g_ao_udr", "g_wav_underrun",
            "g_wav_read_ms", "g_ao_dma_gap_max", "g_ao_t_pump1_ms", "g_ao_t_dacwake_ms",
            "g_ao_t_ramp_ms", "g_wav_rate", "g_wav_state")
    c = {}
    for k in keys:
        if k in bench.T:
            c[k] = bench.rd(k, 1)[0]
    print("播放 %.1f s 后计数：%s" % (secs, json.dumps(c, ensure_ascii=False)))
    if c.get("g_ao_dma_half") and c.get("g_ao_frames"):
        print("   ⇒ 平均帧/中断 = %.1f（满值 1024 表示每半缓冲都被喂满）"
              % (c["g_ao_frames"] / max(1, c["g_ao_dma_half"] + c["g_ao_dma_cplt"])))

    bench.wr("g_wav_test", [3])          # 暂停 → DMA 停 → 缓冲冻结
    time.sleep(1.5)

    dma = bench.rd("s_dma_buf", DMA_WORDS)
    ring = bench.rd("s_ring", RING_WORDS)
    out = os.path.join("build", "_audio_buf.json")
    with open(out, "w") as f:
        json.dump({"dma": dma, "ring": ring, "counters": c}, f)
    print("原始数据 → %s" % out)
    print()
    report("I2S TX DMA 乒乓缓冲（真正送进 ES8311 的采样）", dma)
    bench.wr("g_wav_test", [2])          # 停播
    return 0


if __name__ == "__main__":
    sys.exit(main())
