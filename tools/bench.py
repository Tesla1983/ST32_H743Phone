#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""满负载整帧基准：写 N 到 g_bench_frames，跑完读 g_bench_cyc / g_bench_done。

与 tools/render_scan.py 的区别
------------------------------
render_scan.py 扫多个 band 行数、解 A/b；本脚本只回答一个问题：
**当前固件满负载一帧多少毫秒（等效多少 FPS）**，用于"改前 / 改后"对照。

用法
----
    python tools/bench.py                # 默认 60 帧
    python tools/bench.py 100
    python tools/bench.py 60 --mirror 0  # 顺手把全帧镜像关掉再测（P0-2）

判据：g_loop_count 连读两次必须变化，否则内核还 halt（probe-rs download 后必须 reset）。
"""
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Windows 控制台默认是 GBK，而本工程的脚本经常在 print 里用 ⚠ / ⇒ / ✅ 这类
# 不在 GBK 里的字符。一旦某个分支被走到，就会在**那一行**抛
# UnicodeEncodeError（前面的输出都正常，看起来像"脚本自己崩了"）。
#
# 2026-10-04 实测踩到两次：`tools/fmc_sweep.py` 打完整张扫描表后崩在最后一行；
# `tools/brightness_check.py` 崩在"必须先擦扇区"那句提示上，**导致整个亮度验收
# 根本没跑起来**。这两个脚本都 import 本模块，所以修在这里就等于给整套
# 验收脚本（brightness/ribbon/lang/caret/dma/micro/render_scan/objtime_scan/
# flash_and_verify/factory_reset/pwr_shot）一起修掉了。
# ---------------------------------------------------------------------------
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

W = 320
H = 480
FULL_PX = W * H          # 153600


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
    """写一组 32 位变量。

    ⚠ 负数必须转成 32 位补码的十六进制再传（2026-10-04 踩过）：
      直接把 -200 拼进命令行，probe-rs 的 clap 会把它当成"选项"
      ⇒ `error: unexpected argument '-2' found`，写失败。
      变量在板上是有符号 int，用无符号十六进制写进去，
      C 侧按同一位模式读出来就是 -200（补码）。
    """
    a = T[name][0]
    out = []
    for v in vals:
        u = v & 0xFFFFFFFF
        out.append("0x%08x" % u)
    r = subprocess.run("probe-rs write %s --chip %s b32 0x%08x %s"
                       % (PROBE, CHIP, a, " ".join(out)),
                       shell=True, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("写 %s 失败：%s%s" % (name, r.stdout, r.stderr))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nframes = int(args[0]) if args else 60

    mirror = None
    if "--mirror" in sys.argv:
        i = sys.argv.index("--mirror")
        mirror = int(sys.argv[i + 1])

    objtime = None
    if "--objtime" in sys.argv:
        i = sys.argv.index("--objtime")
        objtime = int(sys.argv[i + 1])

    health = {k: rd(k, 1)[0] for k in ("g_boot_magic", "g_shell_rc", "g_font_ok")}
    if health["g_boot_magic"] != 0x594D4731 or health["g_shell_rc"] != 0:
        print("固件没在跑：", health, "（probe-rs download 后必须 reset）")
        return 1
    l0 = rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    l1 = rd("g_loop_count", 1)[0]
    if l0 == l1:
        print("内核没在跑（g_loop_count 1 秒内没变：%d）" % l0)
        return 1
    print("健康度 OK，主循环 %d 拍/秒" % (l1 - l0))

    if mirror is not None:
        wr("g_frame_mirror", [1 if mirror else 0])
        print("全帧镜像 = %s" % ("开" if mirror else "关"))
        time.sleep(0.5)
    if objtime is not None:
        if "g_objtime_on" not in T:
            print("per-object 计时不在本固件里：诊断已默认关闭（CMakeLists 的 YMGUI_DIAG=OFF）。")
            print("  要用它请先 `cmake -S . -B build -DYMGUI_DIAG=ON` 再 ninja。")
            return 1
        wr("g_objtime_on", [1 if objtime else 0])
        print("per-object 计时 = %s" % ("开" if objtime else "关"))
        time.sleep(0.5)

    wr("g_band_rows", [0])
    wr("g_bench_cyc", [0])
    wr("g_bench_done", [0])
    wr("g_bench_frames", [nframes])
    for _ in range(300):
        time.sleep(1)
        if rd("g_bench_frames", 1)[0] == 0:
            break
    else:
        print("基准未在时限内跑完")
        return 1

    cyc = rd("g_bench_cyc", 1)[0]
    done = rd("g_bench_done", 1)[0]
    if not done:
        print("没有产出数据")
        return 1
    per = cyc / done
    ms = per / 400e3
    print()
    print("=== 满负载整帧基准（%d 帧，整屏标脏，band=%d 行）===" % (done, H // 32))
    print("  每帧 = %.0f cycles = %.2f ms = %.1f FPS" % (per, ms, 1000.0 / ms))
    print("  每像素 = %.1f cycles" % (per / FULL_PX))
    try:
        per_call = rd("g_flush_cb_cyc", 1)[0] / max(1, rd("g_flush_cb_calls", 1)[0])
        nb = rd("g_bench_done", 1)[0] and (H // 32)
        print("  flush_cb 单次均值 = %.3f ms → 每帧 %d 次 ≈ %.2f ms"
              % (per_call / 400e3, nb, per_call * nb / 400e3))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
