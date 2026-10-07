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
    python tools/micro.py          # 全跑一遍
    python tools/micro.py 1 2 6 7  # 只跑指定 case

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
}


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


def main():
    cases = [int(a) for a in sys.argv[1:] if a.isdigit()] or sorted(CASE)

    h = {k: bench.rd(k, 1)[0] for k in ("g_boot_magic", "g_shell_rc")}
    if h["g_boot_magic"] != 0x594D4731 or h["g_shell_rc"] != 0:
        print("固件没在跑：", h, "（probe-rs download 后必须 reset）")
        return 1
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    if bench.rd("g_loop_count", 1)[0] == l0:
        print("内核没在跑")
        return 1

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
