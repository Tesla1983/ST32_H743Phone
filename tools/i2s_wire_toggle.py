# -*- coding: utf-8 -*-
"""I2S 信号线通断自检（SWD 驱动，不依赖固件，可自行反复运行）。

把 STM32 侧的 I2S2 引脚逐个配成**开漏输出**（内部上拉开），以 2 秒周期
拉低/释放。你在**模块那头**用万用表直流档量对应引脚：
  量到 0V↔3.3V 跳变 ⇒ 这根线通；一动不动 ⇒ 这根线断或接错脚。

安全说明：全程开漏（只拉低+释放，从不推挽驱动），与 tools/i2c_wire_toggle.py
同一套路；跑完自动把引脚恢复成 I2S2 的 AF5 配置。

排针位置（方案 A）：P1-21=PC6(MCLK)  P1-22=PD3(SCK)  P1-23=PB9(LRCK)
                      P1-24=PB15(SDO→模块 DIN)（排针全表见 docs/HEADER_PINOUT.md）

用法：
    python i2s_wire_toggle.py                    # 四根全测：各 20 秒，2 秒周期
    python i2s_wire_toggle.py --period 1 --seconds 12
    python i2s_wire_toggle.py --pin 9            # 只测 PB9(LRCK)
"""
import argparse
import subprocess
import time

PROBE = "0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

# 引脚号 -> (端口基址, 名)
PORTS = {"B": 0x58020400, "C": 0x58020800, "D": 0x58020C00}
PINS = [
    (9,  "B", "PB9  = I2S2_WS  (LRCK 帧时钟)"),
    (3,  "D", "PD3  = I2S2_CK  (SCLK 位时钟)"),
    (6,  "C", "PC6  = I2S2_MCK (MCLK 主时钟)"),
    (15, "B", "PB15 = I2S2_SDO (DOUT 数据)"),
]


def r(addr, tries=4):
    """读一个 32 位寄存器。CMSIS-DAP 连续快速 attach/detach 偶发失败，带重试。"""
    for i in range(tries):
        o = subprocess.run(["probe-rs", "read", "--probe", PROBE, "--chip", CHIP,
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
        rc = subprocess.run(["probe-rs", "write", "--probe", PROBE, "--chip", CHIP,
                             "b32", "0x%08x" % addr, "0x%08x" % (val & 0xFFFFFFFF)],
                            capture_output=True).returncode
        if rc == 0:
            return True
        time.sleep(0.4)
    print("  [warn] 写 0x%08x 重试 %d 次仍失败" % (addr, tries))
    return False


def toggle_pin(port_letter, pin, period, seconds):
    base = PORTS[port_letter]
    moder = r(base + 0x00)
    otyper = r(base + 0x04)
    pupdr = r(base + 0x0C)
    if moder is None or otyper is None or pupdr is None:
        print("  [warn] 读 %s 端口寄存器失败，跳过" % port_letter)
        return

    shift = pin * 2
    # 开漏输出(01) + 内部上拉(01) —— 与 I2C 同款安全驱动
    w(base + 0x00, (moder & ~(3 << shift)) | (1 << shift))
    w(base + 0x04, otyper | (1 << pin))                 # 开漏
    w(base + 0x0C, (pupdr & ~(3 << shift)) | (1 << shift))  # 上拉

    half = period
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        w(base + 0x14, 0)                # 拉低
        time.sleep(half)
        w(base + 0x14, 1 << pin)         # 释放（由上拉拉高）
        time.sleep(half)
        n += 1

    # 恢复 AF5（I2S2 全链），推挽
    w(base + 0x04, otyper & ~(1 << pin))
    w(base + 0x00, (moder & ~(3 << shift)) | (2 << shift))
    w(base + 0x0C, pupdr)
    print("  已恢复 AF5（跳变 %d 个周期）" % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--period", type=float, default=2.0, help="半周期秒数（一个完整周期=2×此值）")
    ap.add_argument("--seconds", type=float, default=20.0, help="每颗脚的测试时长")
    ap.add_argument("--pin", type=int, default=None, help="只测指定引脚号（9/3/6/15）")
    args = ap.parse_args()

    sel = PINS if args.pin is None else [p for p in PINS if p[0] == args.pin]
    if not sel:
        print("没有这个引脚：%s（可选 9/3/6/15）" % args.pin)
        return

    print("I2S 通断自检：每颗脚 %g 秒，半周期 %g 秒" % (args.seconds, args.period))
    print("万用表直流档：黑笔接模块 GND（或排针 P1-1/2），红笔接模块对应引脚。")
    for pin, port, name in sel:
        print("\n== %s ==  （%.0f 秒内应看到 0V↔3.3V 跳变）" % (name, args.seconds))
        toggle_pin(port, pin, args.period, args.seconds)
    print("\n全部测完，现场已恢复 AF5。")


if __name__ == "__main__":
    main()
