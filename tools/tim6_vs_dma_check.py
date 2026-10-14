#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""TIM6 滴灌 vs I2S TX DMA：同板 A/B 对照（2026-10-14 新增，回答"为什么 DMA 更容易爆音"）

用户提问原话：「为什么昨天用 TIM6 就没有爆音呢？后面走 DMA 路线就一直有爆音？」

做法：**同一段音乐、同一个出音引擎、同一块板**，只切 `g_ao_use_dma`（0=TIM6，1=DMA），
在两个相位下各量一轮：
  相位 A【静窗】起播后 10 s **一个字都不读**  —— 两条路径都该是 0 欠载（基线）。
  相位 B【干扰窗】同样的 10 s 里**每秒批读一次**（probe-rs 每次读写都会短暂停机内核，
    而 DMA 是独立总线主设备、停机期间照跑）—— 这一相位放大的是"主循环被耽误一拍"的后果，
    正是现场里 FatFs 落盘 / QSPI 写 / I2C 卡住时发生的事。

两条路径的**结构性差别**（本脚本要量的就是这些）：
  1 单次搬运粒度：TIM6 = 1 个半字(10.24 µs) / DMA = 2048 个半字(21.0 ms) —— 差 2048 倍
  2 缺数据时做什么：TIM6 = ISR 空转、**保持上一个采样**（无电平跳变）；
                     DMA  = `dma_fill_half` 尾部**冻结上一采样 + 下一块头 ramp-in**
                            （★ 补丁15c 起；15 之前是**填硬 0** ⇒ 一块 21 ms 的直流台阶
                             ⇒ 那才是"咔"。现在两端 C0 连续，与 TIM6 的天然行为对齐）
  3 停机瞬间"已锁定待搬"的数据量：TIM6 = 0 半字；DMA = 0~2048 半字（实测掐钟前 NDTR=1688）
  4 是否经过 DMA1 仲裁：DMA 与 LCD band(MEM2MEM/HIGH) 争端口（补丁5d 实测 UDR≈1/s）；TIM6 不经过

测量手法（沿用本工程铁律）：
  * 诊断量在内存里**不连续** ⇒ 一次批读整个窗口，再按 nm 取值。
    ⚠ 窗口**由符号地址现算**（见下方 LO/NW），不再硬编码 0x20006700/48 ——
      补丁15 新增诊断量后 .bss 排布变了，硬编码窗口会漏符号 ⇒ 直接 IndexError 崩掉。
  * 「干扰窗」的读**故意保留** —— 它就是这个相位要施加的干扰；「静窗」内绝不读。

判据：
  V1  静窗：两条路径欠载都应 0（基线干净）
  V2  干扰窗：DMA 出现 under_short > 0 而 TIM6 为 0 ⇒ 失败模式不对称成立。
       ★ 但**注意口径**（补丁15c 起）：under_short>0 本身**不再等于"能听见咔"** ——
         缺口已改成"冻结 + ramp-in"（两端 C0 连续）。它现在只说明"环被抽干过"。
         真正判"有没有咔"要看 g_ao_seam_n（跨半缓冲接缝的大跳变次数）与听感。
  V3  TIM6 路径 DMA 半传/全传中断必须为 0（不进 DMA1）
  V4  报告环最低水位：它就是"抗卡顿垫子"，DMA 若为 0 即"零余量"

用法：
    python tools/tim6_vs_dma_check.py            # 静窗 10 s + 干扰窗 10 s，两条路径
    python tools/tim6_vs_dma_check.py 6          # 每相位 6 s（快跑）
"""
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench

PROBE = bench.PROBE
CHIP = bench.CHIP

DMA1_STREAM4_NDTR = 0x40020074          # DMA1_BASE(0x40020000)+Stream4(0x70)+NDTR(0x04)
HALFWORD_US = 10.24                     # 48 828 Hz 立体声下一个半字 = 10.24 µs
HALF_BUF_HW = 2048                      # 一个半缓冲 = 1024 帧 = 2048 半字 = 21.0 ms

LO, NW = 0x20006700, 48
NAMES = ["g_ao_t_ramp_ms", "g_ao_t_dacwake_ms", "g_ao_t_pump1_ms", "g_ao_t_cstart_ms",
         "g_ao_under_first_ms", "g_ao_under_short", "g_ao_under_feed", "g_ao_under_dma",
         "g_ao_dma_gap_max", "g_ao_dma_cplt", "g_ao_dma_half", "g_ao_stage",
         "g_ao_isr_dry", "g_ao_isr", "g_ao_pump", "g_ao_underrun", "g_ao_frames",
         "g_ao_udr", "g_ao_playing"]
# ★ 2026-10-14（补丁15c）：窗口**由符号地址现算**，不再硬编码 0x20006700/48。
#   起因：补丁15 新增了若干诊断量（g_ao_seam_*/g_ao_dbg3/s_gap_tail/s_wake_settle_ms …），
#   .bss 的排布随之变化 ⇒ 硬编码的窗口会漏掉某个符号 ⇒ `w[o]` 直接 IndexError 崩掉
#   （本轮实测：DMA 相位一进来就 Traceback）。现按 NAMES 的最小/最大地址取窗口，
#   窗口变大只是多读几个无关字，不影响按偏移取值。
_addrs = [bench.T[n][0] for n in NAMES]
LO = min(_addrs) & ~3
NW = (max(_addrs) - LO) // 4 + 1
if NW > 256:
    raise RuntimeError("批读窗口跨度过大（%d 字）—— 诊断量可能已被挪到不连续的位置" % NW)
OFF = {n: (bench.T[n][0] - LO) // 4 for n in NAMES}


def batch():
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d"
                       % (PROBE, CHIP, LO, NW),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < NW:
        raise RuntimeError("批读失败：%s%s" % (r.stdout, r.stderr))
    w = [int(x, 16) for x in tok[:NW]]
    return {n: w[o] for n, o in OFF.items()}


def alive():
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.4)
    return bench.rd("g_loop_count", 1)[0] != l0


def stop():
    bench.wr("g_wav_test", [2])
    time.sleep(0.6)


def clear():
    for s in ("g_ao_underrun", "g_ao_under_dma", "g_ao_under_feed", "g_ao_under_short",
              "g_ao_udr", "g_ao_isr", "g_ao_isr_dry", "g_ao_dma_half", "g_ao_dma_cplt",
              "g_ao_dma_gap_max", "g_ao_under_first_ms", "g_ao_ring_drop"):
        bench.wr(s, [0])


def run_phase(mode, secs, disturb):
    """disturb=True 时每秒批读一次（施加"主循环被耽误"的干扰）。"""
    stop()
    bench.wr("g_ao_use_dma", [mode])
    time.sleep(0.2)
    clear()
    bench.wr("g_wav_test", [1])
    t0 = time.time()
    if disturb:
        while time.time() - t0 < secs:
            time.sleep(1.0)
            batch()
    else:
        time.sleep(secs)
    d = batch()
    d["_ring_min"] = bench.rd("g_ao_ring_used_min", 1)[0]
    d["_drop"] = bench.rd("g_ao_ring_drop", 1)[0]
    stop()
    return d


def show(tag, d):
    print("  %-10s underrun=%d (dma类=%d feed类=%d) 最大缺口=%4d 半字=%6.1f ms | "
          "环最低=%5d 半字=%5.1f ms | UDR=%d | 半传=%d 全传=%d | TIM6中断=%d(空转%d)"
          % (tag, d["g_ao_underrun"], d["g_ao_under_dma"], d["g_ao_under_feed"],
             d["g_ao_under_short"], d["g_ao_under_short"] * HALFWORD_US / 1000.0,
             d["_ring_min"], d["_ring_min"] * HALFWORD_US / 1000.0, d["g_ao_udr"],
             d["g_ao_dma_half"], d["g_ao_dma_cplt"], d["g_ao_isr"], d["g_ao_isr_dry"]))


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1

    stop()
    print("批读窗口 0x%08X..0x%08X（%d 字），映射 %d 个诊断量；每相位 %.0f s"
          % (LO, LO + NW * 4, NW, len(NAMES), secs))

    res = {}
    for mode, label in ((1, "DMA"), (0, "TIM6")):
        print("\n=== %s 路径（g_ao_use_dma=%d）===" % (label, mode))
        q = run_phase(mode, secs, disturb=False)
        show("静窗", q)
        s = run_phase(mode, secs, disturb=True)
        show("干扰窗", s)
        res[label] = (q, s)

    bench.wr("g_ao_use_dma", [1])
    stop()
    print("\n已恢复 g_ao_use_dma=1（默认走 DMA），播放已停。")

    dq, ds = res["DMA"]
    tq, ts = res["TIM6"]
    print("\n" + "=" * 78)
    print("  对照结论")
    print("=" * 78)
    ok = True
    if dq["g_ao_underrun"] != 0 or tq["g_ao_underrun"] != 0:
        print("[WARN] V1 静窗出现欠载：DMA=%d TIM6=%d（应都为 0）"
              % (dq["g_ao_underrun"], tq["g_ao_underrun"]))
    else:
        print("[PASS] V1 静窗两条路径欠载都 = 0（基线干净 ⇒ 差别只在'被耽误'时暴露）")

    print("       干扰窗：DMA 缺口 %d 半字（%.1f ms）%d 次 | TIM6 缺口 %d 半字 %d 次"
          % (ds["g_ao_under_short"], ds["g_ao_under_short"] * HALFWORD_US / 1000.0,
             ds["g_ao_under_dma"], ts["g_ao_under_short"], ts["g_ao_under_dma"]))
    if ds["g_ao_under_dma"] > ts["g_ao_under_dma"]:
        print("[INFO] V2 失败模式不对称成立：同样干扰下 DMA 会把整块换成'冻结样本'（21 ms），")
        print("          TIM6 只是 FIFO 饿一下（引擎重复上一采样，粒度 10 µs）")
        print("         ⚠ 补丁15c 起缺口已 ramp-in 补回（两端 C0 连续）⇒ 该计数只说明'环被抽干'，")
        print("           不再等价于'能听见咔'；判听感请看 g_ao_seam_n 与实测听感。")
    else:
        print("[INFO] V2 本相位未拉开差距（干扰强度不够或环余量已足够）")

    if ts["g_ao_dma_half"] != 0 or ts["g_ao_dma_cplt"] != 0:
        print("[FAIL] V3 TIM6 路径竟然有 DMA 中断计数（不该有）")
        ok = False
    else:
        print("[PASS] V3 TIM6 路径不进 DMA1（半传/全传 = 0 ⇒ 无端口争用）")

    print("[INFO] V4 环最低水位（抗卡顿垫子）：DMA 静窗=%d 半字 / TIM6 静窗=%d 半字"
          % (dq["_ring_min"], tq["_ring_min"]))
    print("         掐钟前一瞬在途（DMA 侧 NDTR，另测）：见 tools/i2c_stall_check.py 说明")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
