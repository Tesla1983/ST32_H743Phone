#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 TIM 输入捕获在芯片上「锁存」引脚边沿，验证一根线到底通不通
=============================================================================
什么时候用它
-----------------------------------------------------------------------------
排串口线时最常问：「这根线到底有没有电气连接？」
STM32 侧只能看到"我发了多少字节"，收不到回应时区分不了：
  ① 线根本没通
  ② 线通了但对端没回 / 回了但没解析出来

本工具直接回答 ①：把 GPIO 交给 TIM 做输入捕获，**捕获标志 CCxIF 由硬件锁存**，
与 CPU 有没有及时查看无关。哪怕中间只来过一个宽度几百纳秒的脉冲也能抓到。

为什么不用轮询 / 不用 EXTI
-----------------------------------------------------------------------------
- **轮询不可行**：probe-rs 单次读约 0.3~1 s，而对端日志是毫秒级突发
  （921600 下 100 字节约 1 ms）。按采样率算命中率 <3%，采不到 ≠ 没跳变，假阴性。
- **EXTI 在本工程实测失效**：PR1 地址有两套布局（0x08 / 0x88），本板上
  连"已知有信号"的对照脚都测不到 —— 结论不可用。

⚠⚠ **必须带正对照**
-----------------------------------------------------------------------------
本工具一次同时测**两个脚**：
  - **对照脚**（默认 PB8，接着对端帧口 TX，一直在收 $DT，必然有跳变）
  - **目标脚**（默认 PB9，待测）
判据是"对照有 + 目标无"才算"线不通"。**对照也没测到 ⇒ 本次结果无效**，
通常是 AF 号选错（见下），不要拿它下结论。

复用号（AF）是最大的坑
-----------------------------------------------------------------------------
TIM4_CH3 / TIM4_CH4 在 PB8 / PB9 上是 **AF2，不是 AF1**（实测踩过：AF1 时
对照脚都测不到）。默认已用 2，换脚前先确认目标脚的 AF 号。

用法
-----------------------------------------------------------------------------
    python tools/edge_watch.py                       # 默认 PB8 对照 / PB9 目标，听 25 s
    python tools/edge_watch.py --target PB7 --af 2
    python tools/edge_watch.py --seconds 60

⚠ 脚本会把两个脚临时切成 AF（中断原有 UART 功能），结束前自动写回 AFRH/MODER。
"""

import argparse
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

RCC_APB1LENR = 0x580244E8      # bit2 = TIM4EN
T4 = 0x40000800                # TIM4_BASE
T4_CR1, T4_SR, T4_EGR = T4 + 0x00, T4 + 0x10, T4 + 0x14
T4_CCMR2, T4_CCER = T4 + 0x1C, T4 + 0x20
T4_PSC, T4_ARR, T4_CNT = T4 + 0x28, T4 + 0x2C, T4 + 0x24

PORT_BASE = {"A": 0x58020000, "B": 0x58020400, "C": 0x58020800,
             "D": 0x58020C00, "E": 0x58021000}

# 通道号 → (CCMR 内的位偏移, CCER 使能位, SR 标志位)。只支持 TIM4 的 CH3/CH4，
# 因为 CCMR2 一个寄存器就能同时配这两个通道，正好做"对照 + 目标"。
CH = {
    3: dict(ccmr_shift=0,  ccer_bit=8,  sr_bit=3),
    4: dict(ccmr_shift=8,  ccer_bit=12, sr_bit=4),
}

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def rd(a):
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1" % (PROBE, CHIP, a),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    m = re.search(r"\b[0-9a-fA-F]{8}\b", r.stdout)
    return int(m.group(0), 16) if m else None


def wr(a, v):
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True,
                   encoding="utf-8", errors="ignore")


import re  # noqa: E402  (rd() 用到，放在这里只是为了让上面的常量先定义完)


def parse_pin(p):
    p = p.strip().upper()
    return p[1], int(p[2:])          # ('B', 9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="PB8", help="对照脚：已知有信号，用来证伪本次测量")
    ap.add_argument("--target", default="PB9", help="目标脚：待测")
    ap.add_argument("--af", type=int, default=2, help="复用号（PB8/PB9 的 TIM4 是 2）")
    ap.add_argument("--seconds", type=float, default=25.0)
    a = ap.parse_args()

    rp, ri = parse_pin(a.ref)
    tp, ti = parse_pin(a.target)
    if rp != tp:
        print("[FAIL] 两个脚必须同一个 GPIO 口（本工具用单个 TIM4 的 CH3/CH4）")
        return 1
    ri_ch = {8: 3, 9: 4}.get(ri)
    ti_ch = {8: 3, 9: 4}.get(ti)
    if ri_ch is None or ti_ch is None or ri_ch == ti_ch:
        print("[FAIL] 只支持 PB8(TIM4_CH3) 与 PB9(TIM4_CH4) 的组合")
        return 1

    base = PORT_BASE[rp]
    MODER, AFRH = base + 0x00, base + 0x24
    lo, hi = min(ri, ti), max(ri, ti)          # 8, 9
    n_lo, n_hi = (lo % 8), (hi % 8)            # AFRH 内下标

    print("=" * 72)
    print("TIM4 边沿锁存探测：对照 %s / 目标 %s   (AF%d)" % (a.ref, a.target, a.af))
    print("=" * 72)

    afr0, mod0 = rd(AFRH), rd(MODER)
    if afr0 is None or mod0 is None:
        print("[FAIL] 读不到 GPIO 寄存器")
        return 1
    print("  AFRH 原值 = 0x%08X   MODER 原值 = 0x%08X" % (afr0, mod0))

    # 两脚切 AF
    afr = afr0 & ~((0xF << (4 * n_lo)) | (0xF << (4 * n_hi)))
    afr |= (a.af << (4 * n_lo)) | (a.af << (4 * n_hi))
    wr(AFRH, afr)
    mod = mod0 & ~((3 << (2 * lo)) | (3 << (2 * hi)))
    mod |= (2 << (2 * lo)) | (2 << (2 * hi))          # 10 = 复用功能
    wr(MODER, mod)

    en = rd(RCC_APB1LENR) or 0
    wr(RCC_APB1LENR, en | (1 << 2))                   # TIM4EN

    c_ref, c_tgt = CH[ri_ch], CH[ti_ch]
    wr(T4_CR1, 0)
    wr(T4_PSC, 0)
    wr(T4_ARR, 0xFFFF)
    wr(T4_CCMR2, (1 << c_ref["ccmr_shift"]) | (1 << c_tgt["ccmr_shift"]))  # CCxS=01 输入
    wr(T4_CCER, (1 << c_ref["ccer_bit"]) | (1 << c_tgt["ccer_bit"]))       # CCxE 上升沿
    wr(T4_EGR, 1)                                     # UG 装载 PSC/ARR
    wr(T4_SR, 0)                                      # 清标志
    wr(T4_CR1, 1)                                     # CEN

    print("  CCMR2=0x%08X  CCER=0x%08X  监听 %.0f 秒 ..."
          % (rd(T4_CCMR2) or 0, rd(T4_CCER) or 0, a.seconds))
    sys.stdout.flush()
    time.sleep(a.seconds)

    sr, cnt = rd(T4_SR) or 0, rd(T4_CNT) or 0
    ref_hit = (sr >> c_ref["sr_bit"]) & 1
    tgt_hit = (sr >> c_tgt["sr_bit"]) & 1
    print("  TIM4 SR = 0x%08X   CNT = %d" % (sr, cnt))

    wr(T4_CR1, 0)
    wr(AFRH, afr0)
    wr(MODER, mod0)
    print("  已恢复：AFRH=0x%08X MODER=0x%08X" % (rd(AFRH) or 0, rd(MODER) or 0))
    print("")
    print("  对照 %s : %s" % (a.ref, "有边沿" if ref_hit else "零边沿"))
    print("  目标 %s : %s" % (a.target, "有边沿" if tgt_hit else "零边沿"))
    print("")

    if not ref_hit:
        print("  [无效] 对照脚都没测到 ⇒ 本次测量不成立（多半是 AF 号错了）")
        print("         换 --af 重跑；不要拿这个结果下结论。")
        return 1
    if tgt_hit:
        print("  [PASS] 目标脚有边沿 ⇒ 这根线电气上是通的")
        print("         ⇒ 零响应的原因要往对端 RX 配置 / 波特率那一侧找")
        return 0
    print("  [FAIL] 对照有、目标无 ⇒ %s 这一侧**没有电气连接**" % a.target)
    print("         ⇒ 杜邦线断 / 脚位找错 / 没插到底")
    return 1


if __name__ == "__main__":
    sys.exit(main())
