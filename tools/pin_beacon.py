#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""单脚方波定位探针（开漏，0.5 Hz）——用万用表找出"某引脚在排针上是哪一脚"
=============================================================================
为什么需要它
-----------------------------------------------------------------------------
换串口脚时最常见的问题不是软件，而是**杜邦线插错了排针位置**（PC6 那次就是这个）。
寄存器读出来全对（AF 号、外设使能都在），但对端就是收不到字节 —— 此时唯一能
闭合证据链的办法，是让那根脚输出一个**慢到万用表能看见**的方波，让人去量。

用法
-----------------------------------------------------------------------------
    python tools/pin_beacon.py --pin PB9
    python tools/pin_beacon.py --pin PE1 --minutes 3 --period 0.5

判据（拿万用表直流电压档，黑表笔接 GND）
-----------------------------------------------------------------------------
  - 探针所在脚：0 V ↔ 3.3 V 之间来回跳（默认 1 s 一跳）
  - **再量对端对应脚**：也跳 ⇒ 线是通的、问题在对端固件；
                        不跳 ⇒ 这根线根本没接上（或接在别的脚上）

安全
-----------------------------------------------------------------------------
⚠ 本脚本只把目标脚设成 **开漏输出 + 内部上拉**，即**只拉低、从不推高**。
  这样即便该脚外部正被别的芯片推挽驱动着，也不会形成直通电流 —— 遵守
  "禁止推挽强灌一批 GPIO" 这条红线。**不要**把它改成推挽。
⚠ 运行期间会占用调试口，此时不能同时用 probe-rs 读别的寄存器。
⚠ 结束后自动把 MODER/OTYPER/PUPDR 原值写回（AFR 不动，因为本脚本不碰它）。
"""

import argparse
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

# GPIOA..GPIOK 在 AHB2 上，间隔 0x400
GPIO_BASE = {
    "A": 0x58020000, "B": 0x58020400, "C": 0x58020800, "D": 0x58020C00,
    "E": 0x58021000, "F": 0x58021400, "G": 0x58021800, "H": 0x58021C00,
    "I": 0x58022000, "J": 0x58022400, "K": 0x58022800,
}
MODER_OFF, OTYPER_OFF, PUPDR_OFF, BSRR_OFF = 0x00, 0x04, 0x0C, 0x18


def rd32(addr):
    out = subprocess.run(
        "probe-rs read %s --chip %s b32 0x%08X 1" % (PROBE, CHIP, addr),
        shell=True, capture_output=True, text=True)
    for line in out.stdout.splitlines():
        for tok in line.split():
            # probe-rs 输出是裸十六进制，没有 0x 前缀
            if len(tok) == 8 and all(c in "0123456789abcdefABCDEF" for c in tok):
                return int(tok, 16)
    return None


def wr32(addr, val):
    subprocess.run(
        "probe-rs write %s --chip %s b32 0x%08X %d" % (PROBE, CHIP, addr, val),
        shell=True, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin", required=True, help="形如 PB9 / PC6 / PE1")
    ap.add_argument("--minutes", type=float, default=10.0, help="运行时长，默认 10 分钟")
    ap.add_argument("--period", type=float, default=1.0, help="半周期秒数，默认 1 s（0.5 Hz）")
    a = ap.parse_args()

    pin = a.pin.upper()
    port, num = pin[1], int(pin[2:])
    if port not in GPIO_BASE or not (0 <= num <= 15):
        print("引脚解析失败：%s（示例：PB9）" % a.pin)
        return 1
    base = GPIO_BASE[port]

    m0 = rd32(base + MODER_OFF)
    o0 = rd32(base + OTYPER_OFF)
    p0 = rd32(base + PUPDR_OFF)
    if m0 is None:
        print("读寄存器失败（调试口被占用？）")
        return 1
    print("原值 MODER=0x%08X OTYPER=0x%08X PUPDR=0x%08X" % (m0, o0, p0))

    sh = 2 * num
    wr32(base + MODER_OFF, (m0 & ~(3 << sh)) | (1 << sh))    # 通用输出
    wr32(base + OTYPER_OFF, o0 | (1 << num))                  # 开漏（只拉低）
    wr32(base + PUPDR_OFF, (p0 & ~(3 << sh)) | (1 << sh))     # 上拉

    print("")
    print("  %s 已设为 开漏输出+上拉，开始 %.2f Hz 方波（%.1f s 一跳）"
          % (pin, 1.0 / (2 * a.period), a.period))
    print("  万用表黑表笔接 GND，红表笔点 %s —— 应在 0V 与 3.3V 之间来回跳" % pin)
    print("  同时量对端对应脚：也跳 ⇒ 线通；不跳 ⇒ 这根线没接上")
    print("  %.0f 分钟后自动恢复；也可以 Ctrl-C 提前结束（同样会恢复）" % a.minutes)
    print("")
    sys.stdout.flush()

    try:
        t_end = time.time() + a.minutes * 60.0
        while time.time() < t_end:
            wr32(base + BSRR_OFF, 1 << (num + 16))   # 拉低
            time.sleep(a.period)
            wr32(base + BSRR_OFF, 1 << num)          # 释放（上拉回高）
            time.sleep(a.period)
    except KeyboardInterrupt:
        pass
    finally:
        wr32(base + MODER_OFF, m0)
        wr32(base + OTYPER_OFF, o0)
        wr32(base + PUPDR_OFF, p0)
        print("已恢复 %s 原配置" % pin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
