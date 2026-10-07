#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
coldboot_vos.py —— 冷启动 VOSRDY 现场抓取（2026-10-04）

背景
----
固件原本卡在 `sys.c:146` 的 `while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)){}`
无超时死等。加超时后固件能启动，但**冷启动那次 VOSRDY 恒为 0**，
而 reset 启动那次为 1 ⇒ 上电时序敏感。

本脚本抓「固件自己的视角」+「PWR 寄存器视角」两份证据，交叉判断：
  ① 固件视角：g_sysclk_vos_wait / g_sysclk_err / g_clock_vos
  ② 寄存器视角：D3CR(bit13=VOSRDY) / CSR1(bit13=ACTVOSRDY, bit15:14=ACTVOS)

关键判据（来自 HAL 实现 stm32h7xx_hal_pwr_ex.c:496-503）：
  **HAL 自己等的是 ACTVOSRDY(CSR1 bit13)，不是 VOSRDY(D3CR bit13)。**
  厂商代码等错了标志位 —— 这是本脚本要交叉验证的核心。

用法
----
    python tools/coldboot_vos.py            # 抓一次
    python tools/coldboot_vos.py --watch    # 持续抓（观察上电过程中的变化）
"""
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
SWD = "--protocol swd"

# ---- PWR 寄存器（偏移已对 PWR_TypeDef 注释核过，勿凭记忆改）----
PWR_CR1 = 0x58024800   # +0x00
PWR_CSR1 = 0x58024804  # +0x04
PWR_CR3 = 0x5802480C   # +0x0C
PWR_D3CR = 0x58024818  # +0x18
SYSCFG_PWRCR = 0x58000400


def rd(addr, words=1):
    r = subprocess.run(
        "probe-rs read %s %s %s --chip %s b32 0x%08X %d"
        % (PROBE, SWD, "", CHIP, addr, words),
        shell=True, capture_output=True, text=True,
        encoding="utf-8", errors="ignore")
    out = []
    for t in r.stdout.split():
        t = t.strip()
        if len(t) == 8 and all(c in "0123456789abcdefABCDEF" for c in t):
            out.append(int(t, 16))
    return out[0] if out else None


def rd_var(name):
    """经 ELF 取符号地址再读；变量不存在返回 None。"""
    nm = subprocess.run(
        r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe -S build/ymgui-h743.elf",
        shell=True, capture_output=True, text=True,
        encoding="utf-8", errors="ignore").stdout
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 4 and p[3] == name and p[2] in "bBdD":
            v = rd(int(p[0], 16))
            return v
    return None


def bits(v, spec):
    """spec: 'name:pos' 逗号分隔"""
    out = []
    for item in spec.split(','):
        n, pos = item.split(':')
        out.append("%s=%d" % (n, (v >> int(pos)) & 1))
    return " ".join(out)


def snap():
    csr1 = rd(PWR_CSR1)
    d3cr = rd(PWR_D3CR)
    print("  --- PWR 寄存器 ---")
    print("  CSR1 = 0x%08X   %s" % (csr1, bits(csr1, "ACTVOSRDY:13")))
    print("                    ACTVOS[15:14] = %d%d" % ((csr1 >> 15) & 1, (csr1 >> 14) & 1))
    print("  D3CR = 0x%08X   %s" % (d3cr, bits(d3cr, "VOSRDY:13")))
    print("                    VOS[15:14]    = %d%d" % ((d3cr >> 15) & 1, (d3cr >> 14) & 1))
    print("  PWR_CR3 = 0x%08X" % rd(PWR_CR3))
    print("  SYSCFG_PWRCR = 0x%08X  (ODEN=bit0=%d)" % (
        (lambda v: v if v is not None else 0)(rd(SYSCFG_PWRCR)),
        (rd(SYSCFG_PWRCR) or 0) & 1))
    print("  --- 固件自报 ---")
    for n in ("g_sysclk_vos_wait", "g_sysclk_err", "g_sysclk_stage", "g_loop_count"):
        v = rd_var(n)
        print("  %-20s = %s" % (n, "读不到" if v is None else v))
    return csr1, d3cr


def judge(csr1, d3cr):
    vosrdy = (d3cr >> 13) & 1
    actrdy = (csr1 >> 13) & 1
    print("\n  ==== 判断 ====")
    if vosrdy and actrdy:
        print("  VOSRDY=1 且 ACTVOSRDY=1 ⇒ 电压握手完全正常（属 reset 路径的正常情况）")
    elif not vosrdy and not actrdy:
        print("  **VOSRDY=0 且 ACTVOSRDY=0**")
        print("     ⇒ 连「当前实际档位」都没稳住。")
        print("       这不是标志位语义问题，而是供电/电压建立本身失败。")
        print("       指向：VDD 未到 Scale1 门槛 / 上电时序 / 硬件。")
    elif not vosrdy and actrdy:
        print("  **VOSRDY=0 但 ACTVOSRDY=1**")
        print("     ⇒ 实际档位已就绪，只有「目标档位」就绪位没置。")
        print("       这正是厂商代码死等的直接原因：")
        print("       它等错了标志位（等 VOSRDY，而 HAL 等的是 ACTVOSRDY）。")
        print("       ⇒ 等对了标志位就不会挂死；这属于代码缺陷，不是硬件故障。")


def main():
    print("冷启动 VOSRDY 现场抓取")
    if "--watch" in sys.argv:
        for i in range(60):
            print("[%2d] t=%.1fs" % (i, i * 0.5))
            c, d = snap()
            time.sleep(0.5)
        return 0
    c, d = snap()
    judge(c, d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
