#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""per-object 绘制耗时排行（P0-0）：读库里的 g_objtime[] 表，把 draw_cb 地址解析成函数名。

表里每个对象两项时间（见 YMGUI_Invalidate.h 的注释）：
    self = 该对象 draw_cb 自己的累计耗时（**不含**后代）
    sub  = 含后代的整棵子树的累计耗时
两个分开记，是因为父与子的耗时不能混：父的 self 不含子，子的 sub 又是父 sub 的一部分。

用法
----
    python tools/objtime_scan.py            # 读上一次 g_bench_frames 跑出来的结果
    python tools/objtime_scan.py --frames 60   # 手动给帧数（默认读 g_bench_done）
    python tools/objtime_scan.py --top 20

条目布局（GYDiagObjTime，见 YMGUI_Invalidate.h）：
    void* obj / void* cb / int16 x,y,w,h / uint32 self,sub,calls  → 7 words = 28 B
"""
import bisect
import subprocess
import sys

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
ELF = "build/ymgui-h743.elf"

ENTRY_WORDS = 7          # sizeof(GYDiagObjTime) / 4


def symtab():
    # -S 才会带符号大小；表容量从 g_objtime 的大小推出来（= YMGUI_DIAG_OBJ_N）
    out = subprocess.run([NM, "-S", ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    addrs = []      # (addr, name) 供二分查找
    exact = {}      # addr -> name
    t = {}
    sz = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:                 # addr size type name
            try:
                sz[p[3]] = int(p[1], 16)
            except ValueError:
                pass
            p = [p[0], p[2], p[3]]
        if len(p) != 3:
            continue
        try:
            a = int(p[0], 16)
        except ValueError:
            continue
        t.setdefault(p[2], []).append(a)
        if p[1] in ("T", "t", "W", "w", "i", "I"):
            addrs.append((a, p[2]))
            exact.setdefault(a, p[2])
    addrs.sort()
    return t, sz, [a for a, _ in addrs], [n for _, n in addrs], exact


def rd(name, n=1):
    a = T[name][0]
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d" % (PROBE, CHIP, a, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 %s 失败：%s%s" % (name, r.stdout, r.stderr))
    return [int(x, 16) for x in tok[:n]]


def fname(addr):
    """函数地址 → 符号名（找不到就取最近的前一个符号 + 偏移）"""
    if addr == 0:
        return "(纯容器,无 draw_cb)"
    addr &= ~1          # ARM Thumb 的函数指针最低位是 1，不是地址的一部分
    if addr in EXACT:
        return EXACT[addr]
    i = bisect.bisect_right(KEYS, addr) - 1
    if i < 0:
        return "0x%08x" % addr
    return "%s+0x%x" % (NAMES[i], addr - KEYS[i])


def main():
    top = 24
    frames = None
    for i, a in enumerate(sys.argv[1:]):
        if a == "--top":
            top = int(sys.argv[i + 2])
        elif a == "--frames":
            frames = int(sys.argv[i + 2])

    n = rd("g_objtime_n", 1)[0]
    ovf = rd("g_objtime_ovf", 1)[0]
    cap = SZ.get("g_objtime", 0) // (ENTRY_WORDS * 4)   # = YMGUI_DIAG_OBJ_N
    if frames is None:
        frames = rd("g_bench_done", 1)[0]
    if frames <= 0:
        frames = 1
        print("⚠ g_bench_done = 0，按 1 帧折算（先跑一次 g_bench_frames 再读更准）")
        frames = 1

    base = T["g_objtime"][0]
    # 一次读整张表（容量从 nm 拿不到，按 objtime_scan 已知布局，读 64 项上限也行；
    # 这里用 g_objtime_n 决定读多少，另加 2 项余量确认表尾确实是空的）
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d"
                       % (PROBE, CHIP, base, (n + 2) * ENTRY_WORDS),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n * ENTRY_WORDS:
        raise RuntimeError("读 g_objtime 失败：%s%s" % (r.stdout, r.stderr))
    w = [int(x, 16) for x in tok]

    def i16(v):
        return v - 0x10000 if v & 0x8000 else v

    rows = []
    for i in range(n):
        o = i * ENTRY_WORDS
        obj = w[o]
        cb = w[o + 1]
        x, y, ww, hh = i16(w[o + 2] & 0xFFFF), i16(w[o + 2] >> 16), \
                       i16(w[o + 3] & 0xFFFF), i16(w[o + 3] >> 16)
        self_, sub, calls = w[o + 4], w[o + 5], w[o + 6]
        rows.append((self_, sub, calls, cb, obj, x, y, ww, hh))

    rows.sort(reverse=True)

    print()
    print("=== per-object 绘制耗时（%d/%d 个对象，按 %d 帧折算）===" % (n, cap, frames))
    if ovf:
        print("⚠ g_objtime_ovf = %d —— 表满过，有对象没登记进来（调大 YMGUI_DIAG_OBJ_N）" % ovf)
    print("-" * 96)
    print("%-34s %14s %14s %8s %16s" %
          ("draw_cb（对象绝对区域）", "self ms/帧", "sub ms/帧", "calls/帧", "self µs/次"))
    print("-" * 96)
    tot_self = sum(r[0] for r in rows)
    for self_, sub, calls, cb, obj, x, y, ww, hh in rows[:top]:
        nm = fname(cb)
        area = "@%d,%d %dx%d" % (x, y, ww, hh)
        label = ("%s %s" % (nm[:24], area))[:34]
        per_call = (self_ / calls / 400.0) if calls else 0.0
        print("%-34s %14.3f %14.3f %8.1f %16.2f" %
              (label, self_ / frames / 400e3, sub / frames / 400e3,
               calls / frames, per_call))
    print("-" * 96)
    print("  self 合计 = %.2f ms/帧（各对象 draw_cb 之和，不含遍历/裁剪/flush）"
          % (tot_self / frames / 400e3))
    print("  obj 地址范围：0x%08x .. 0x%08x" %
          (min(r[4] for r in rows), max(r[4] for r in rows)))
    return 0


T, SZ, KEYS, NAMES, EXACT = symtab()

if __name__ == "__main__":
    sys.exit(main())
