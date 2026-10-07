#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""FMC 写时序扫描：逐档重配 ADDSET/DATAST 并量整屏 flush 耗时。

为什么需要它
------------
A1 记的"整屏 flush 7.68 ms ≈ 50.0 ns/px，已打到总线地板"**只对当前寄存器设置成立**。
一次 16 位写的真实成本是

    (ADDSET + DATAST + 1) / fmc_ker_ck        本板 fmc_ker_ck = PLL2_R = 220 MHz

5/5 ⇒ 11 周期 ⇒ 50.0 ns，与板上实测的 50.0 ns/px 完全吻合。
但面板规格书只约束**写脉宽 tWRL >= 19 ns**，那是 DATAST 的事（>=4 周期 = 22.7 ns 就够了）；
ADDSET 只决定 CS/RS 建立时间，从来没有被单独扫过。
所以 50 ns/px 是"阶段 0 那一轮七档整体扫描挑中的一个点"，不是硬件极限。

本脚本利用固件里的三个钩子（都由 SWD 驱动，**不需要重新编译、不需要复位**）：

    g_lcd_fmc_addset / g_lcd_fmc_datast   目标档位（定义在 third_party/BSP/LCD/lcd.c）
    g_fmc_timing_sweep                    写 1 ⇒ 下一拍 apply 时序 + 重测整屏 flush
    g_flush_cyc / g_flush_nspx            apply 后立刻可读的整屏 flush 数字

用法
----
    python tools/fmc_sweep.py                    # 默认扫 5/5、5/4、2/4、1/4
    python tools/fmc_sweep.py 5/5 4/4 3/4 2/4    # 自定义档位
    python tools/fmc_sweep.py --verify 5/5 1/4   # 每档顺带跑 A2 双自检并报失配数

判据
----
* `g_flush_cyc` 越小越好，理论上与 (ADDSET+DATAST+1) 成正比。
* **通过与否只看双自检**：`g_ramp_mismatch` 与 `g_gram_mismatch` 必须都是 0。
  用 `--verify` 让脚本每档自动跑一次；不跑自检的数字只能当"快"看，不能当"对"看。
* 换档后画面可能错乱（太快就丢写）—— 那是预期现象，不代表固件坏了；
  扫完记得把档位写回一个验证过的值（默认 5/5）。
"""
import subprocess
import sys
import time

# 本脚本的输出里有 ⚠ / ⇒ 这类不在 GBK 里的字符，而 Windows 控制台默认是 GBK，
# 直接 print 会在最后一行抛 UnicodeEncodeError（实测：整张表都打出来了，却以
# 退出码 1 结束，看起来像"脚本失败了"）。统一把 stdout 改成 utf-8。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
ELF = "build/ymgui-h743.elf"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
FULL_PX = 320 * 480          # 153600
CPU_HZ = 400e6               # DWT 时基（主频），与 ymgui_port.c 的口径一致
FMC_HZ = 220e6               # PLL2_R，见 sys.c

DEFAULT_JOBS = ["5/5", "5/4", "2/4", "1/4"]


def symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    t = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            t.setdefault(p[2], []).append(int(p[0], 16))
    return t


T = symtab()


def need(name):
    if name not in T:
        raise SystemExit(
            "符号 %s 不在符号表里。\n"
            "  · 若缺 g_lcd_fmc_* / g_fmc_timing_sweep：当前 ELF 是改这个功能之前构建的，先 ninja。\n"
            "  · 若缺 g_objtime_on：诊断已默认关闭（YMGUI_DIAG=OFF），用 tools/bench.py 时别加 --objtime。" % name)
    return T[name][0]


def rd(name, n=1):
    a = need(name)
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d" % (PROBE, CHIP, a, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 %s 失败：%s%s" % (name, r.stdout, r.stderr))
    return [int(x, 16) for x in tok[:n]]


def wr32(name, vals):
    """写一组 32 位变量（负数转 32 位补码十六进制，probe-rs 的 clap 会把 -x 当选项）。"""
    a = need(name)
    payload = " ".join("0x%08x" % (v & 0xFFFFFFFF) for v in vals)
    r = subprocess.run("probe-rs write %s --chip %s b32 0x%08x %s"
                       % (PROBE, CHIP, a, payload),
                       shell=True, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("写 %s 失败：%s%s" % (name, r.stdout, r.stderr))


def health_check():
    boot, rc = rd("g_boot_magic", 1)[0], rd("g_shell_rc", 1)[0]
    if boot != 0x594D4731 or rc != 0:
        print("固件没在跑：g_boot_magic=0x%08x g_shell_rc=%d（download 后必须 reset）" % (boot, rc))
        return False
    l0 = rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    l1 = rd("g_loop_count", 1)[0]
    if l0 == l1:
        print("内核没在跑（g_loop_count 1 秒内没变：%d）" % l0)
        return False
    print("健康度 OK，主循环 %d 拍/秒" % (l1 - l0))
    return True


def measure_one(addset, datast, verify):
    """重配一档并量一次整屏 flush。返回 (cyc, nspx_x100, ramp, gram)。"""
    wr32("g_lcd_fmc_addset", [addset])
    wr32("g_lcd_fmc_datast", [datast])
    wr32("g_fmc_timing_sweep", [1])
    # 钩子跑完会自动把 g_fmc_timing_sweep 清 0
    for _ in range(50):
        time.sleep(0.1)
        if rd("g_fmc_timing_sweep", 1)[0] == 0:
            break
    else:
        raise RuntimeError("g_fmc_timing_sweep 没有在 5 秒内清 0（主循环卡住了？）")

    # 主循环是"清标志 → apply → 重测"的顺序，所以读到标志为 0 时测量可能还没跑完。
    # 等一拍再读，避免读到上一档的陈旧 g_flush_cyc。
    time.sleep(0.3)

    applied = (rd("g_fmc_applied_addset", 1)[0], rd("g_fmc_applied_datast", 1)[0])
    if applied != (addset, datast):
        print("  ⚠ 回读档位 %s 与请求 %s 不一致 —— 时序没写成功" % (applied, (addset, datast)))

    cyc = rd("g_flush_cyc", 1)[0]
    nspx = rd("g_flush_nspx", 1)[0]

    ramp = gram = None
    if verify:
        # A2 双自检：ramp 会临时改写面板、结束用镜像缓冲恢复（画面闪一下属正常）。
        # ⚠ gram_verify 要求镜像是最新的：先确保镜像开着并让它跑满一帧。
        wr32("g_frame_mirror", [1])
        wr32("g_force_redraw", [1])
        time.sleep(0.3)
        wr32("g_gram_recheck", [1])
        for _ in range(100):
            time.sleep(0.1)
            if rd("g_gram_recheck", 1)[0] == 0:
                break
        time.sleep(0.2)
        ramp = rd("g_ramp_mismatch", 1)[0]
        gram = rd("g_gram_mismatch", 1)[0]
    return cyc, nspx, ramp, gram


def main():
    jobs = [a for a in sys.argv[1:] if not a.startswith("--")]
    verify = "--verify" in sys.argv
    if not jobs:
        jobs = DEFAULT_JOBS

    parsed = []
    for j in jobs:
        if "/" not in j:
            print("档位格式应为 ADDSET/DATAST，例如 5/5；收到 %r" % j)
            return 1
        a, d = j.split("/", 1)
        parsed.append((int(a), int(d)))

    if not health_check():
        return 1

    print()
    print("=== FMC 写时序扫描（每档 apply 后立刻重测整屏 flush）===")
    print("  一次写的理论成本 = (ADDSET+DATAST+1)/%.0fMHz；整屏 = 该值 × %d px"
          % (FMC_HZ / 1e6, FULL_PX))
    print()
    print("  %-10s %-12s %-12s %-10s %s" % ("档位", "整屏 ms", "ns/px", "FPS", "双自检"))
    rows = []
    for addset, datast in parsed:
        cyc, nspx, ramp, gram = measure_one(addset, datast, verify)
        ms = cyc / (CPU_HZ / 1000.0)
        theory = (addset + datast + 1) / FMC_HZ * 1e9 * FULL_PX / 1e6
        if verify:
            ok = (ramp == 0 and gram == 0)
            verdict = "OK" if ok else "FAIL (ramp=%d gram=%d)" % (ramp, gram)
        else:
            verdict = "未跑（加 --verify）"
        rows.append((addset, datast, ms, ok if verify else None))
        print("  %-10s %-12.2f %-12.2f %-10.1f %s   [理论 %.2f ms]"
              % ("%d/%d" % (addset, datast), ms, nspx / 100.0, 1000.0 / ms, verdict, theory))

    print()
    if verify:
        good = [r for r in rows if r[3]]
        if good:
            best = min(good, key=lambda r: r[2])
            print("  通过自检的最快档位： %d/%d  = %.2f ms（%.1f FPS），比 5/5 省 %.2f ms"
                  % (best[0], best[1], best[2], 1000.0 / best[2],
                     rows[0][2] - best[2]))
            print("  ⇒ 确认后把 lcd.c 里 g_lcd_fmc_addset/datast 的初值改成这一档，再重新构建。")
        else:
            print("  没有任何档位通过双自检 —— 保持 5/5，并检查 FMC 时钟到底是 200 还是 220 MHz。")
    else:
        print("  提示：以上只有速度，没有正确性。加 --verify 让每档都跑一遍 A2 双自检")
        print("        （ramp + gram_verify；需要全帧镜像，脚本会临时打开它）。")

    print()
    print("  ⚠ 扫描结束后把档位恢复成验证过的值：")
    print("     python tools/fmc_sweep.py --verify 5/5")
    return 0


if __name__ == "__main__":
    sys.exit(main())
