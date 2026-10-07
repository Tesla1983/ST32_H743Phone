#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
heap_peak_check.py —— 上板读双档堆（heap0 / heap1）的实测峰值，并顺带跑
「删单字词典 entries[] 索引」这次改动的三条回归判据。

    python tools/heap_peak_check.py

前提：固件**已经烧好并在跑**（probe-rs download 之后必须跟一次 reset，
      否则内核还 halt）。本脚本不烧录。

三条判据
--------
  H1  heap1 峰值 —— 删索引后应从 176.1 KB(83%) 降到 ~69.4 KB(33%)
  H2  tools/ime_probe.py nihao zhongguo —— 候选内容必须与改动前一致（成功 2/2）
  H3  A2 逐像素自检 —— g_gram_mismatch == 0 且 g_ramp_mismatch == 0

另外把 heap0 的峰值一并打出来做参照，以及 g_fault.magic（0 = 从未内核故障）。

【g_h0/g_h1 是 file-scope static】nm 里是小写 'b'，但地址照样能取到。
  Heap 结构（src/board_alloc.c）：
      Block *free_head;  /* +0  */
      uint8_t *base;     /* +4  */
      uint32_t total;    /* +8  */
      uint32_t used;     /* +12 */
      uint32_t peak;     /* +16 */
      uint32_t n_alloc;  /* +20 */
  本脚本一次读 6 个字，不需要逐字段连 probe。
"""
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
ELF = "build/ymgui-h743.elf"
ENV = {"PYTHONIOENCODING": "utf-8"}

KB = 1024.0


def nm_tab():
    import os
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    tab = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                tab[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return tab


def rd32(addr, words):
    cmd = "probe-rs read %s --chip %s b32 0x%08X %d" % (PROBE, CHIP, addr, words)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore", env=dict(__import__("os").environ, **ENV))
    toks = [t for t in r.stdout.split()
            if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    if len(toks) < words:
        raise RuntimeError("只读到 %d/%d 个字：%s" % (len(toks), words, r.stdout or r.stderr))
    return [int(t, 16) for t in toks[:words]]


def heap_of(tab, name):
    """返回 (base, total, used, peak, n_alloc)。"""
    a = tab[name]
    w = rd32(a, 6)
    return w[1], w[2], w[3], w[4], w[5]


def show(tag, base, total, used, peak, n_alloc):
    print("  %-6s base=0x%08X  total=%7d B (%6.1f KB)" % (tag, base, total, total / KB))
    print("           used =%7d B (%6.1f KB)   peak=%7d B (%6.1f KB)  %.0f%%"
          % (used, used / KB, peak, peak / KB, (peak / total * 100) if total else 0))
    print("           剩余 =%7d B (%6.1f KB)   n_alloc=%d"
          % (total - peak, (total - peak) / KB, n_alloc))


def main():
    tab = nm_tab()

    # ---- 前置：主循环必须在跑 ----
    a = rd32(tab["g_loop_count"], 1)[0]
    time.sleep(1.5)
    if rd32(tab["g_loop_count"], 1)[0] == a:
        print("[FAIL] 主循环没在跑（g_loop_count 两次不变）→ 先 probe-rs reset")
        return 1
    print("[OK ] 主循环在跑\n")

    print("H1 双档堆实测峰值")
    show("heap0", *heap_of(tab, "g_h0"))
    show("heap1", *heap_of(tab, "g_h1"))
    _, t1, _, p1, _ = heap_of(tab, "g_h1")
    ok_h1 = p1 < 100 * 1024          # 期望 ~69.4 KB；给到 100 KB 作为硬判据
    print("       判据 heap1 peak < 100 KB（改动前 180376 B / 176.1 KB）: %s\n"
          % ("PASS" if ok_h1 else "FAIL"))

    # ---- H3：A2 逐像素自检 ----
    subprocess.run("probe-rs write %s --chip %s b32 0x%08X %d"
                   % (PROBE, CHIP, tab["g_gram_recheck"], 1), shell=True,
                   capture_output=True, text=True)
    time.sleep(6)
    ramp = rd32(tab["g_ramp_mismatch"], 1)[0]
    gram = rd32(tab["g_gram_mismatch"], 1)[0]
    print("H3 A2 逐像素自检")
    print("       g_ramp_mismatch = %d（期望 0） %s" % (ramp, "PASS" if ramp == 0 else "FAIL"))
    print("       g_gram_mismatch = %d（期望 0） %s" % (gram, "PASS" if gram == 0 else "FAIL"))

    fault = rd32(tab["g_fault"], 1)[0]
    print("       g_fault.magic    = %d（期望 0） %s" % (fault, "PASS" if fault == 0 else "FAIL"))

    # ---- H2：输入法候选一致性 ----
    print("\nH2 输入法候选（tools/ime_probe.py nihao zhongguo）")
    r = subprocess.run("%s tools/ime_probe.py nihao zhongguo" % sys.executable,
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=300,
                       env=dict(__import__("os").environ, **ENV))
    out = r.stdout or ""
    ok_ime = "成功 2/2" in out
    print("       %s" % ("成功 2/2" if ok_ime else "未通过"))
    if not ok_ime:
        print(out)

    ok = ok_h1 and ramp == 0 and gram == 0 and fault == 0 and ok_ime
    print("\n%s" % ("[PASS] 三条判据全通过" if ok else "[FAIL] 有未通过项，见上方"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
