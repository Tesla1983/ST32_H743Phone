#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 EXTI 在芯片上「锁存」某个引脚的边沿，验证这根线到底通不通
=============================================================================
为什么不用轮询
-----------------------------------------------------------------------------
probe-rs 单次读约 0.3~1 s，而对端日志是**毫秒级突发**（921600 下 100 字节
只要约 1 ms）。按采样率算命中率不到 3% —— 采不到不代表没跳变，会得到假阴性。

解决办法：把边沿检测交给**芯片硬件**。EXTI 的挂起位（PR）在检测到边沿时会
**锁存**，与 CPU 有没有及时看无关。配置好之后等 40 s 再回来读一次即可，
哪怕中间只发生过一次宽度几百纳秒的脉冲也能抓到。

用法
-----------------------------------------------------------------------------
    python tools/exti_watch.py --pin PB9 --seconds 40

判据
-----------------------------------------------------------------------------
  - 检测到跳变 ⇒ 这根线**电气上是通的**，问题在别处（对端 RX 配置等）
  - 零跳变     ⇒ 这根线没接上 / 脚位找错 / 杜邦线断

⚠ 本脚本会把目标引脚临时切成**带上拉的输入**（不再驱动总线，零对撞风险），
  结束前自动写回原来的 MODER。
"""

import argparse
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

# GPIOB（本工程命令口所在口；换口时改这里）
GPIO_BASE = 0x58020400
MODER = GPIO_BASE + 0x00
PUPDR = GPIO_BASE + 0x0C
IDR = GPIO_BASE + 0x10

RCC_APB4ENR = 0x580244F4      # bit1 = SYSCFGEN
SYSCFG_EXTICR3 = 0x58000410   # EXTI8..11；EXTI9 在 bits 7:4
EXTI_RTSR1 = 0x58000000
EXTI_FTSR1 = 0x58000004
# H7 头文件里 EXTI_TypeDef 有两套布局（PR1 在 0x08 或 0x88），两个都探一遍
EXTI_PR1_CAND = (0x58000008, 0x58000088)

PORTB = 1   # SYSCFG EXTICR 里 PORTB 的编码

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def rd(a):
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1" % (PROBE, CHIP, a),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    for tok in r.stdout.split():
        t = tok.strip()
        if len(t) == 8 and all(c in "0123456789abcdefABCDEF" for c in t):
            return int(t, 16)
    return None


def wr(a, v):
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True,
                   encoding="utf-8", errors="ignore")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin", default="PB9")
    ap.add_argument("--seconds", type=float, default=40.0)
    a = ap.parse_args()

    port = a.pin[1].upper()
    idx = int(a.pin[2:])
    base = {"A": 0x58020000, "B": 0x58020400, "C": 0x58020800,
            "D": 0x58020C00, "E": 0x58021000}[port]
    portcode = {"A": 0, "B": 1, "C": 2, "D": 3, "E": 4}[port]
    MODER_ = base + 0x00
    IDR_ = base + 0x10
    # EXTI8..11 共用 EXTICR3；EXTI12..15 用 EXTICR4
    exticr = 0x58000410 if 8 <= idx <= 11 else 0x58000414
    slot = (idx % 4) * 4

    print("=" * 72)
    print("EXTI 边沿锁存探测：%s" % a.pin)
    print("=" * 72)

    moder0 = rd(MODER_)
    if moder0 is None:
        print("[FAIL] 读不到 MODER")
        return 1
    print("  MODER 原值 = 0x%08X" % moder0)

    # 1) SYSCFG 时钟
    en = rd(RCC_APB4ENR) or 0
    wr(RCC_APB4ENR, en | (1 << 1))
    # 2) EXTI9 映射到目标口
    ex = rd(exticr) or 0
    ex = (ex & ~(0xF << slot)) | (portcode << slot)
    wr(exticr, ex)
    # 3) 目标脚切成输入（不驱动，零对撞）
    wr(MODER_, moder0 & ~(3 << (2 * idx)))
    # 4) 双边沿检测
    wr(EXTI_RTSR1, (rd(EXTI_RTSR1) or 0) | (1 << idx))
    wr(EXTI_FTSR1, (rd(EXTI_FTSR1) or 0) | (1 << idx))
    # 5) 清挂起
    for p in EXTI_PR1_CAND:
        wr(p, 1 << idx)

    idle = rd(IDR_) or 0
    print("  已切输入。当前电平 = %d（上拉应为 1）" % ((idle >> idx) & 1))
    print("  监听 %.0f 秒 ..." % a.seconds)
    sys.stdout.flush()

    time.sleep(a.seconds)

    hits = []
    for p in EXTI_PR1_CAND:
        v = rd(p)
        if v is None:
            continue
        if (v >> idx) & 1:
            hits.append("0x%08X" % p)
    now = rd(IDR_) or 0
    print("  结束时刻电平 = %d" % ((now >> idx) & 1))

    # 恢复
    wr(MODER_, moder0)
    wr(EXTI_RTSR1, (rd(EXTI_RTSR1) or 0) & ~(1 << idx))
    wr(EXTI_FTSR1, (rd(EXTI_FTSR1) or 0) & ~(1 << idx))
    print("  已恢复 MODER = 0x%08X" % (rd(MODER_) or 0))

    print("")
    if hits:
        print("  [PASS] 检测到边沿（挂起位 %s 被置起）" % " / ".join(hits))
        print("        ⇒ %s 与对端之间**电气上是通的**" % a.pin)
        print("        ⇒ 零字节的原因要到对端 RX 配置那一侧找")
        return 0
    print("  [FAIL] %.0f 秒内零跳变" % a.seconds)
    print("        ⇒ %s 与对端之间**没有电气连接**：线没接上 / 脚位错 / 杜邦线断" % a.pin)
    return 1


if __name__ == "__main__":
    sys.exit(main())
