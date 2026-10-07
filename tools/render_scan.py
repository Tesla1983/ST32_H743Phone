#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""渲染瓶颈定位：扫描 band 行数，把「每 band 固定开销 A」与「每像素填充开销 b」分离出来。

原理（详见 src/main.c 里 g_band_rows 的注释）
------------------------------------------------
满负载一帧（整屏标脏）的总时间可以拆成

    T = n_bands × A  +  P × b

  A      每 band 的固定开销：从 root 递归整棵对象树、每个对象算绝对区域、做相交测试。
        与 band 内有多少像素无关。
  P      本帧实际填充的像素数（整屏标脏时恒为 320×480 = 153600），与 band 数无关。
  b      每像素填充开销：光栅化、alpha 混合、缩放采样。

因为**每个 band 都要从 root 重画一遍**，n_bands 越大 A 付得越多，而 P 不变。
所以量两个不同 n_bands 的 T，就是二元一次方程组，可直接解出 A 和 b。

用法
----
    python tools/render_scan.py            # 默认扫 32/16/8/4 行
    python tools/render_scan.py 32 16 8    # 自定义档位
"""
import subprocess
import sys
import time

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

W = 320
H = 480
FULL_PX = W * H          # 153600
NFRAMES = 40             # 每档跑多少帧


def symtab():
    out = subprocess.run([NM, "build/ymgui-h743.elf"],
                         capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    t = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            t.setdefault(p[2], []).append(int(p[0], 16))
    return t


T = symtab()


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


def wr(name, vals):
    a = T[name][0]
    r = subprocess.run("probe-rs write %s --chip %s b32 0x%08x %s"
                       % (PROBE, CHIP, a, " ".join(str(v) for v in vals)),
                       shell=True, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("写 %s 失败：%s%s" % (name, r.stdout, r.stderr))


def run_rows(rows):
    """设定 band 行数，跑 NFRAMES 帧，返回 (每帧 cycles, 每帧 band 数)。"""
    wr("g_band_rows", [rows])
    wr("g_bench_cyc", [0])
    wr("g_bench_done", [0])
    c0 = rd("g_flush_cb_calls", 1)[0]
    wr("g_bench_frames", [NFRAMES])
    for _ in range(180):
        time.sleep(1)
        if rd("g_bench_frames", 1)[0] == 0:
            break
    else:
        raise RuntimeError("band=%d 基准未在时限内跑完" % rows)
    cyc = rd("g_bench_cyc", 1)[0]
    done = rd("g_bench_done", 1)[0]
    c1 = rd("g_flush_cb_calls", 1)[0]
    if done == 0:
        raise RuntimeError("band=%d 没有产出数据" % rows)
    return cyc / done, (c1 - c0) / done


def main():
    levels = [int(x) for x in sys.argv[1:]] or [32, 16, 8, 4]

    health = {k: rd(k, 1)[0] for k in
              ("g_boot_magic", "g_loop_count", "g_shell_rc", "g_font_ok")}
    print("健康度：", health)
    if health["g_shell_rc"] != 0:
        print("固件未正常起来（g_shell_rc != 0），先复位再跑")
        return 1

    print()
    print("每档跑 %d 帧，整屏标脏" % NFRAMES)
    print("-" * 78)
    print("%6s %8s %14s %12s %14s" % ("行数", "band/帧", "每帧 ms", "等效 FPS", "每帧 cycles"))
    print("-" * 78)

    results = []
    for rows in levels:
        cyc, nb = run_rows(rows)
        ms = cyc / 400e3
        print("%6d %8.1f %14.2f %12.1f %14.0f" % (rows, nb, ms, 1000 / ms, cyc))
        results.append((rows, nb, cyc))
        sys.stdout.flush()

    # 恢复默认，免得留着影响后续交互
    wr("g_band_rows", [0])

    print("-" * 78)

    # 二元一次拟合：T = nb*A + P*b
    # 用最小二乘（档位多于两个时更稳）
    n = len(results)
    if n < 2:
        print("至少需要两档才能解 A/b")
        return 0
    sx = sum(r[1] for r in results)
    sy = sum(r[2] for r in results)
    sxx = sum(r[1] ** 2 for r in results)
    sxy = sum(r[1] * r[2] for r in results)
    den = n * sxx - sx * sx
    if abs(den) < 1e-9:
        print("档位 band 数太接近，无法求解")
        return 0
    A = (n * sxy - sx * sy) / den          # 每 band 固定开销（cycles）
    Pxb = (sy - A * sx) / n                # 一帧的总填充开销（cycles）
    b = Pxb / FULL_PX                      # 每像素填充开销（cycles）

    print()
    print("=== 分解结果（T = n_bands × A + 153600 × b）===")
    print("  每 band 固定开销 A = %.0f cycles = %.3f ms" % (A, A / 400e3))
    print("  每像素填充开销 b   = %.2f cycles/px = %.2f ns/px" % (b, b / 400 * 1000))
    print("  一帧填充总开销     = %.0f cycles = %.2f ms" % (Pxb, Pxb / 400e3))
    print()
    print("  按当前 15 bands 折算：")
    cur = results[0]
    print("    固定开销合计 %.2f ms（%.0f%%）" % (cur[1] * A / 400e3,
                                                 100 * cur[1] * A / cur[2]))
    print("    填充开销合计 %.2f ms（%.0f%%）" % (Pxb / 400e3, 100 * Pxb / cur[2]))
    print()
    print("  理论极限（A 全部消除、只留填充）：%.1f FPS"
          % (400e6 / Pxb))
    print("  理论极限（b 全部消除、只留固定）：%.1f FPS"
          % (400e6 / (cur[1] * A)) if cur[1] * A > 0 else "  （A≈0）")

    oc = rd("g_obj_count", 1)[0]
    od = rd("g_obj_depth", 1)[0]
    print()
    print("=== 对象树规模 ===")
    print("  可见对象数 = %d    最大树深 = %d" % (oc, od))
    if oc:
        print("  每 band 每对象摊到的固定开销 = %.1f cycles" % (A / oc))

    return 0


if __name__ == "__main__":
    sys.exit(main())
