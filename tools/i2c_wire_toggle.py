#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""I2C 物理链路自检：SWD 把 PB6/PB7 配成开漏输出直接拉低/释放，万用表在模块那头看跟不跟变。

用途
----
排查"模块不回 ACK"时，区分两种完全不同的情况：
  A. 板子→排针→杜邦线→模块 这条物理链路是通的 ⇒ 问题在模块本身（器件坏/没真正通电）；
  B. 链路不通（线松/插错排针位）⇒ 跟模块无关，先修接线。
本脚本直接用 SWD 写寄存器完成 toggle，**不依赖任何固件**（固件里有没有音频代码都能跑），
跑完自动恢复现场，I2C 随即恢复正常。

原理
----
  1. 保存 GPIOB 的 MODER/OTYPER/PUPDR 和 I2C1 CR1；
  2. 关掉 I2C1（CR1=0），把目标引脚配成**开漏输出 + 内部上拉**；
  3. ODR 写 0 = 拉低（约 0 V）；写 1 = 释放（被上拉回高，约 3.3 V）；
  4. 测完把保存的寄存器原样写回，I2C1 重新使能。

安全性
----
开漏 + 只灌不拉：最坏情况是经模块自带的 4.7k 上拉灌入约 0.7 mA，
远低于 GPIO 额定电流；不推挽、不驱动、不改任何系统状态。
（本脚本只碰 GPIOB 的 PB6/PB7 与 I2C1 的 CR1，且全程先保存后恢复。）

用法
----
    python tools/i2c_wire_toggle.py                 # SDA(PB7) 20 s + SCL(PB6) 20 s，2 s 周期
    python tools/i2c_wire_toggle.py --pin 7 --seconds 30
    python tools/i2c_wire_toggle.py --period 1      # 1 s 低 / 1 s 高，跳快一点

测量方法
--------
万用表拨直流电压档：黑表笔接模块 GND（或排针 P1-1/2），
红表笔接模块的 SDA 脚（前一段）/ SCL 脚（后一段）。
  跟着 0 V↔3.3 V 跳 ⇒ 链路通，模块侧有问题；
  一动不动（恒定一个电平）⇒ 链路断/接错脚，查排针 P1-19(PB6/SCL)、P1-20(PB7/SDA) 和杜邦线。

前置：probe-rs 在 PATH 里（本机已装）；板子已上电、SWD 已连接。
"""
import subprocess
import sys
import time

PROBE_SN = "0416:5021:0123456789AB"   # FireDAP 序列号（本板那个探针）
CHIP = "STM32H743VITx"
GPIOB = 0x58020400          # MODER +0x00, OTYPER +0x04, OSPEEDR +0x08, PUPDR +0x0C, IDR +0x10, ODR +0x14
I2C1_CR1 = 0x40005400


def r(addr, tries=4):
    """读一个 32 位寄存器。CMSIS-DAP 在连续快速 attach/detach 时偶发失败，
    加重试（间隔 0.4 s）——你自己跑脚本时也更稳。"""
    for i in range(tries):
        o = subprocess.run(["probe-rs", "read", "--probe", PROBE_SN, "--chip", CHIP,
                            "b32", "0x%08x" % addr, "1"],
                           capture_output=True, text=True).stdout
        tok = [x for x in o.split()
               if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
        if tok:
            return int(tok[0], 16)
        time.sleep(0.4)
    return None


def w(addr, val, tries=4):
    for i in range(tries):
        rc = subprocess.run(["probe-rs", "write", "--probe", PROBE_SN, "--chip", CHIP,
                             "b32", "0x%08x" % addr, "0x%08x" % (val & 0xFFFFFFFF)],
                            capture_output=True).returncode
        if rc == 0:
            return True
        time.sleep(0.4)
    print("  [warn] 写 0x%08x 重试 %d 次仍失败" % (addr, tries))
    return False


def parse_args(argv):
    pin, seconds, period = 0, 20, 2.0        # pin=0 ⇒ 先 PB7 后 PB6
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--pin":
            pin = int(argv[i + 1]); i += 2
        elif a == "--seconds":
            seconds = int(argv[i + 1]); i += 2
        elif a == "--period":
            period = float(argv[i + 1]); i += 2
        else:
            i += 1
    return pin, seconds, period


def set_pin_open_drain(pin):
    m = r(GPIOB + 0x00)
    w(GPIOB + 0x00, (m & ~(3 << (pin * 2))) | (1 << (pin * 2)))   # MODER = 01 输出
    o = r(GPIOB + 0x04)
    w(GPIOB + 0x04, o | (1 << pin))                               # OTYPER = 1 开漏
    p = r(GPIOB + 0x0C)
    w(GPIOB + 0x0C, (p & ~(3 << (pin * 2))) | (1 << (pin * 2)))   # PUPDR = 01 上拉


def toggle(pin, seconds, period):
    set_pin_open_drain(pin)
    name = "PB7 = SDA（排针 P1-20）" if pin == 7 else "PB6 = SCL（排针 P1-19）"
    print(">>> %s 开始跳变（%.1f s 低 / %.1f s 高，持续 %d s）—— 请用万用表盯着模块对应引脚"
          % (name, period, period, seconds))
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        w(GPIOB + 0x14, r(GPIOB + 0x14) & ~(1 << pin))   # 拉低
        time.sleep(period)
        w(GPIOB + 0x14, r(GPIOB + 0x14) | (1 << pin))    # 释放（上拉回高）
        time.sleep(period)
        n += 1
    print("    %s 跳了 %d 个周期" % (name, n))


def main():
    pin, seconds, period = parse_args(sys.argv[1:])

    moder, otyper, pupdr, cr1 = (r(GPIOB + 0x00), r(GPIOB + 0x04),
                                 r(GPIOB + 0x0C), r(I2C1_CR1))
    if moder is None or cr1 is None:
        print("读不到寄存器：probe-rs 在 PATH 里吗？板子上电了吗？SWD 连上了吗？")
        return 1
    print("已保存现场：MODER=%08x OTYPER=%08x PUPDR=%08x I2C1_CR1=%08x"
          % (moder, otyper, pupdr, cr1))

    w(I2C1_CR1, 0)                       # 关 I2C1，GPIO 才归我们管
    try:
        if pin in (6, 7):
            toggle(pin, seconds, period)
        else:
            toggle(7, seconds, period)   # 先 SDA
            toggle(6, seconds, period)   # 后 SCL
    finally:
        w(GPIOB + 0x00, moder)           # 恢复现场（finally 保证异常也恢复）
        w(GPIOB + 0x04, otyper)
        w(GPIOB + 0x0C, pupdr)
        w(I2C1_CR1, cr1)
        print("已恢复现场：MODER=%08x OTYPER=%08x PUPDR=%08x I2C1_CR1=%08x"
              % (r(GPIOB + 0x00), r(GPIOB + 0x04), r(GPIOB + 0x0C), r(I2C1_CR1)))

    print()
    print("判读：模块引脚跟着跳 ⇒ 链路通（问题在模块侧）；不动 ⇒ 链路断/接错脚。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
