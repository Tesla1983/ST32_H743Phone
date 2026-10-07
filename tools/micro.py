#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""图元微基准：写 case 号到 g_micro_test，读 g_micro_cyc / g_micro_px。

case 表
-------
    1  Draw_Fill 不透明填充（opa=255）
    2  Draw_Fill 半透明填充（opa=160）
    3  Draw_ImgScaled 缩放 blit（源 320x480 → 目标 320x32）
    4  Draw_Text 中文 4 个 24px 汉字
    5  Draw_ArcThick r12-14 整圈 x100
    6  裸 16 位 store（纯内存写地板，不含图元）
    7  裸 32 位 store（一次两个像素）
    8  裸 16 位 store，但临时关 D-Cache 强制透写（测完自动恢复）
    9  Draw_Fill 不透明，但临时关强制透写
   10  渐变行内直写（原模式：查 4 色抖动表 + 索引 store）
   11  渐变行内直写（4 路展开：P1-5 的写法）

用法
----
    python tools/micro.py                # 全跑一遍（渲染图元）
    python tools/micro.py 1 2 6 7        # 只跑指定 case
    python tools/micro.py 20 21 22 23 24 25 26   # XIP 读延迟台架

case 20~26 是 **XIP/内部 flash 读延迟台架**（2026-10-07 加），用来给
"词组词典上不上外部 flash"拍板，见 docs/IME_PHRASE_DICT_FEASIBILITY.md §6。
⚠ 它们由 CMake 的 YMGUI_XIP_BENCH 控制，**默认不编译进出货固件**；
   没打开时写 case 号进去，g_micro_test 会被 run_render_micro 的 default 分支清 0、
   g_micro_cyc 保持 0，本脚本会报"没跑起来"而不是给一个假数。

判据：g_loop_count 连读两次必须变化，否则内核还 halt（probe-rs download 后必须 reset）。
"""
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench  # noqa: E402  复用它的 rd/wr（从 ELF 取地址）

CASE = {
    1: ("Draw_Fill 不透明 (opa=255)", None),
    2: ("Draw_Fill 半透明 (opa=160)", None),
    3: ("Draw_ImgScaled 缩放 blit", None),
    4: ("Draw_Text 中文 4x24px", None),
    5: ("Draw_ArcThick r12-14 x100", None),
    6: ("裸 16 位 store（内存写地板）", None),
    7: ("裸 32 位 store（双像素）", None),
    8: ("裸 16 位 store + 关强制透写", None),
    9: ("Draw_Fill 不透明 + 关强制透写", None),
    10: ("渐变行内直写（原模式）", None),
    11: ("渐变行内直写（4 路展开）", None),
    20: ("XIP 随机 4B 对齐读", None),
    21: ("XIP 顺序 4B 读（步长 16，稀疏）", None),
    22: ("XIP 随机 1B 读", None),
    23: ("XIP 模拟查询·密集（472 条）", None),
    24: ("XIP 模拟查询·密集（3256 条 sh）", None),
    25: ("内部 flash 模拟查询（472 条）", None),
    26: ("内部 flash 随机 4B 对齐读", None),
    27: ("XIP 顺序 1B 读（步长 1，探 read-ahead）", None),
    28: ("XIP 模拟查询·真实步长（472 条）", None),
    29: ("XIP 模拟查询·真实步长（3256 条 sh）", None),
    30: ("XIP 模拟查询·记录间随机（472 条）", None),
}

XC = set(range(20, 31))


def run(case, repeat=3):
    best = None
    for _ in range(repeat):
        bench.wr("g_micro_cyc", [0])
        bench.wr("g_micro_px", [0])
        bench.wr("g_micro_test", [case])
        for _ in range(60):
            time.sleep(0.3)
            if bench.rd("g_micro_test", 1)[0] == 0:
                break
        else:
            raise RuntimeError("case %d 未在时限内跑完" % case)
        cyc = bench.rd("g_micro_cyc", 1)[0]
        px = max(1, bench.rd("g_micro_px", 1)[0])
        v = cyc / px
        best = v if best is None else min(best, v)
    return best


def run_raw(case, repeat=3):
    """跑一次并取**最小**总周期（XIP 台架用：要的是总时长，不是每事务均摊）。"""
    best_cyc, ops = None, 0
    for _ in range(repeat):
        bench.wr("g_micro_cyc", [0])
        bench.wr("g_micro_px", [0])
        bench.wr("g_micro_test", [case])
        for _ in range(60):
            time.sleep(0.3)
            if bench.rd("g_micro_test", 1)[0] == 0:
                break
        else:
            raise RuntimeError("case %d 未在时限内跑完" % case)
        cyc = bench.rd("g_micro_cyc", 1)[0]
        ops = bench.rd("g_micro_px", 1)[0]
        best_cyc = cyc if best_cyc is None else min(best_cyc, cyc)
    return best_cyc, ops


def xreport(cases):
    """XIP / 内部 flash 读延迟台架的输出（case 20~26）。"""
    print("=== XIP 读延迟台架（@400MHz，取 %d 次最小值）===" % 3)
    print("  %-34s %10s %8s %11s %10s" % ("case", "总 cycles", "事务数", "ns/事务", "合计 ms"))
    res = {}
    for c in cases:
        cyc, ops = run_raw(c)
        res[c] = (cyc, ops)
        if cyc == 0 or ops == 0:
            print("  %-34s %10s %8s %11s %10s"
                  % (CASE.get(c, ("case %d" % c, None))[0], cyc, ops, "-", "-"))
            continue
        ns_tx = cyc / ops / 400e6 * 1e9
        print("  %-34s %10d %8d %11.1f %10.3f"
              % (CASE.get(c, ("case %d" % c, None))[0], cyc, ops, ns_tx, cyc / 400e3))

    print()
    if 20 in res and 26 in res and res[26][0]:
        print("  %-30s %.1f → %.1f ns/事务（外部是内部的 %.1f 倍）"
              % ("随机 4B 对齐读 外 vs 内",
                 res[20][0] / res[20][1] / 400e6 * 1e9,
                 res[26][0] / res[26][1] / 400e6 * 1e9,
                 (res[20][0] / res[20][1]) / (res[26][0] / res[26][1])))
    if 23 in res and 25 in res and res[25][0]:
        print("  %-30s %.3f → %.3f ms（外部是内部的 %.1f 倍）"
              % ("模拟查询 外 vs 内", res[23][0] / 400e3, res[25][0] / 400e3,
                 res[23][0] / res[25][0]))
    if 23 in res and 20 in res and 22 in res and res[22][0]:
        t32 = res[20][0] / res[20][1]
        t8 = res[22][0] / res[22][1]
        print("  %-30s 4B=%.1f ns / 1B=%.1f ns ⇒ readLe32 逐字节拼是 %.2f 倍代价"
              % ("对齐字读 vs 逐字节读", t32 / 400e6 * 1e9, t8 / 400e6 * 1e9, t8 / t32))

    print()
    for c, label in ((23, "密集 472 条"), (24, "密集 3256 条"),
                     (28, "真实步长 472 条"), (29, "真实步长 3256 条"),
                     (30, "记录间随机 472 条")):
        if c not in res or not res[c][0]:
            continue
        ms = res[c][0] / 400e3
        verdict = "≤2ms ⇒ 方案 B 可直接做" if ms <= 2 else (
            "2~10ms ⇒ 走方案 A（或 A+B 双层）" if ms <= 10 else ">10ms ⇒ 只做方案 A")
        print("  %s：%.3f ms（保守模型 12 事务/条）；只做前缀比较(9 事务/条) %.3f ms  ⇒ %s"
              % (label, ms, ms * 9 / 12, verdict))
    return 0


def main():
    cases = [int(a) for a in sys.argv[1:] if a.isdigit()] or sorted(CASE)
    cases = [c for c in cases if c in CASE]

    h = {k: bench.rd(k, 1)[0] for k in ("g_boot_magic", "g_shell_rc")}
    if h["g_boot_magic"] != 0x594D4731 or h["g_shell_rc"] != 0:
        print("固件没在跑：", h, "（probe-rs download 后必须 reset）")
        return 1
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    if bench.rd("g_loop_count", 1)[0] == l0:
        print("内核没在跑")
        return 1

    xcases = [c for c in cases if c in XC]
    rcases = [c for c in cases if c not in XC]
    if xcases and not rcases:
        return xreport(xcases)
    if not rcases:
        return 0

    print("=== 图元微基准（@400MHz，取 %d 次最小值）===" % 3)
    print("  %-32s %10s %12s %12s" % ("case", "cycles/px", "整屏 153600px", "备注"))
    res = {}
    for c in cases:
        name = CASE.get(c, ("case %d" % c, None))[0]
        v = run(c)
        res[c] = v
        note = ""
        if c in (4, 5):
            note = "px 口径非整屏，仅看相对变化"
        print("  %-32s %10.2f %12s %12s"
              % (name, v, "%.2f ms" % (v * 153600 / 400e3), note))

    print()
    for a, b, label in ((6, 7, "16 位 → 32 位 store"),
                        (6, 8, "透写 → 写回（裸 store）"),
                        (1, 9, "透写 → 写回（Draw_Fill）")):
        if a in res and b in res:
            print("  %-24s %.2f → %.2f cycles/px  （%.2fx）"
                  % (label, res[a], res[b], res[a] / res[b]))

    if xcases:
        print()
        return xreport(xcases)
    return 0


if __name__ == "__main__":
    sys.exit(main())
