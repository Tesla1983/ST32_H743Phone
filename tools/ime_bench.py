#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""量「每次按键要多久」—— 方案 B（词组词典上 XIP）的核心验收数。

做的是**输入法引擎自己那一段**，不含渲染：
    cyc_r = imeCandidatesReset(py)      扫词典 + 组合 DP（随拼音长度线性放大）
    cyc_f = extendPrefixCandidates(6)   惰性取满 6 个候选（基本恒定）

需要在 YMGUI_XIP_BENCH=ON 的构建里跑（台架与 case 20~30 同一口径，不进出货构建）。

用法
----
    python tools/ime_bench.py                       # 默认一组拼音
    python tools/ime_bench.py zhongguo nihao        # 指定
    python tools/ime_bench.py --repeat 5 zhongguo   # 每个词跑 5 次，取最小

输出：cycles / ms（按 400 MHz 换算）+ 候选串，便于直接抄进验收表。
"""
import struct
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
ELF = "build/ymgui-h743.elf"
HZ = 400_000_000

DEFAULT = ["a", "wo", "ni", "nihao", "zhongguo", "zhongguoren", "xianzai", "shijie"]


def symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    tab = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            try:
                tab[p[2]] = int(p[0], 16)
            except ValueError:
                pass
    return tab


def rd32(addr, words=1):
    cmd = f"probe-rs read {PROBE} --chip {CHIP} b32 0x{addr:08x} {words}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    toks = [t for t in r.stdout.split()
            if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    if len(toks) < words:
        raise RuntimeError(f"读 0x{addr:08x} 失败: {(r.stderr or r.stdout).strip()}")
    return [int(t, 16) for t in toks[:words]]


def wr32(addr, words):
    vals = " ".join(str(w) for w in words)
    cmd = f"probe-rs write {PROBE} --chip {CHIP} b32 0x{addr:08x} {vals}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    if r.returncode != 0:
        raise RuntimeError(f"写 0x{addr:08x} 失败: {(r.stderr or r.stdout).strip()}")


def read_cstr(tab, name, n):
    words = (n + 3) // 4
    vals = rd32(tab[name], words)
    return b"".join(struct.pack("<I", v) for v in vals)[:n].split(b"\x00")[0].decode(
        "utf-8", errors="replace")


def main():
    args = sys.argv[1:]
    repeat = 1
    words = []
    i = 0
    while i < len(args):
        if args[i] == "--repeat":
            repeat = int(args[i + 1])
            i += 2
        else:
            words.append(args[i])
            i += 1
    if not words:
        words = DEFAULT

    tab = symtab()
    for need in ("g_ime_bench_py", "g_ime_bench_cmd", "g_ime_bench_cyc_r",
                 "g_ime_bench_cyc_f", "g_ime_bench_words",
                 "PhoneIME_BoardCand", "PhoneIME_BoardCandCount"):
        if need not in tab:
            print(f"ELF 里没有 {need} —— 请用 -DYMGUI_XIP_BENCH=ON 重新编译")
            return 1

    print("=== 输入法按键耗时（@400 MHz，每个词跑 %d 次取最小）===" % repeat)
    print("%-14s %10s %10s %10s   %s" % ("拼音", "reset ms", "fill ms", "合计 ms", "候选"))
    worst = 0.0
    for py in words:
        best = None
        cand = ""
        for _ in range(repeat):
            payload = py.encode()[:31]
            buf = payload + b"\x00" * (4 - len(payload) % 4)
            wr32(tab["g_ime_bench_py"],
                 [int.from_bytes(buf[i:i + 4], "little") for i in range(0, len(buf), 4)])
            wr32(tab["g_ime_bench_cmd"], [1])
            for _ in range(80):
                time.sleep(0.05)
                if rd32(tab["g_ime_bench_cmd"], 1)[0] == 0:
                    break
            r = rd32(tab["g_ime_bench_cyc_r"], 1)[0]
            f = rd32(tab["g_ime_bench_cyc_f"], 1)[0]
            n = rd32(tab["g_ime_bench_words"], 1)[0]
            if best is None or r + f < best[0] + best[1]:
                best = (r, f)
                cnt = min(n, 6)
                words_n = (6 * 24 + 3) // 4
                vals = rd32(tab["PhoneIME_BoardCand"], words_n)
                blob = b"".join(struct.pack("<I", v) for v in vals)
                cand = " ".join(
                    blob[i * 24:(i + 1) * 24].split(b"\x00")[0].decode("utf-8", "replace")
                    for i in range(cnt))
        rms = best[0] / HZ * 1e3
        fms = best[1] / HZ * 1e3
        worst = max(worst, rms + fms)
        print("%-14s %10.3f %10.3f %10.3f   %s" % (py, rms, fms, rms + fms, cand))
    print()
    print("最坏一次按键（引擎部分）= %.3f ms；一帧预算 14.71 ms ⇒ 占 %.0f%%"
          % (worst, worst / 14.71 * 100))
    return 0


if __name__ == "__main__":
    sys.exit(main())
