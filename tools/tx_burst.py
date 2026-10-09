#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""让本板**用真实固件路径**连续发命令帧 —— 给逻辑分析仪 / 示波器抓 TX 脚用
=============================================================================
为什么不用 pin_beacon.py
-----------------------------------------------------------------------------
`pin_beacon.py` 是把脚改成 GPIO 开漏打 0.5 Hz 方波，只能回答"这一脚在排针上
是哪个位置"。但它**绕过了 UART 外设**，所以证明不了三件真正关键的事：

  1. 固件走 `uart_link_cmd()` 发出去的那一帧，波形到底出没出脚；
  2. 波特率对不对（BRR 算错、时钟源选错都会"发了但对方收不到"）；
  3. 空闲电平是不是高（UART 空闲必须是高，恒低 ⇒ 对方永远看到起始位）。

本脚本反过来：**不改引脚、不改外设**，只反复写固件自己的触发变量
`g_cmd_req`，让固件一遍遍把真实的命令帧打到 TX 脚上。抓到的波形因此是
"上线后真正会出现在线上的那个波形"。

用法
-----------------------------------------------------------------------------
    python tools/tx_burst.py                       # 默认连发 10 分钟
    python tools/tx_burst.py --seconds 60          # 只发 1 分钟
    python tools/tx_burst.py --req 2               # 发 $?WEA 而不是 $?PING

判据（逻辑分析仪 Async Serial 解码器：921600 / 8N1 / 非反相）
-----------------------------------------------------------------------------
  - 空闲电平 = **高**；每帧之间也是高
  - 一帧内容 ≈ `$?PING*2F\\r\\n`（11 字节）
  - 量到的位宽 ≈ 1.085 µs（= 1/921600）。若量到的是 8.7 µs ⇒ 实际跑在 115200，
    说明固件里的波特率没按预期生效
  - 波形有但**边沿很缓 / 振铃** ⇒ 线太长或没共地，921600 下必挂，降到 115200 试试

安全
-----------------------------------------------------------------------------
⚠ 运行期间占用调试口（每次写都是一次 probe-rs 事务），此时别同时跑别的
   probe-rs 命令。结束（或 Ctrl-C）后不残留任何状态 —— 本脚本**不改任何寄存器**，
   只写固件自己的 `g_cmd_req`。
"""

import argparse
import subprocess
import sys
import time

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

REQ_NAME = {1: "$?PING", 2: "$?WEA", 3: "$?WGET"}

# 覆盖命令通道当前用到的引脚与外设（换脚时改这里）
TX_PIN = ("B", 9)      # PB9
RX_PIN = ("B", 8)      # PB8
UART_BASE = 0x40004C00  # UART4
GPIO_BASE = {"B": 0x58020400}

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


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


def rd32(addr):
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1"
                       % (PROBE, CHIP, addr),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    return int(tok[0], 16) if tok else None


def rd1(name, tab):
    a = tab.get(name, [None])[0]
    if a is None:
        return None
    return rd32(a)


def wr1(name, v, tab):
    a = tab.get(name, [None])[0]
    if a is None:
        return False
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True,
                   encoding="utf-8", errors="ignore")
    return True


def show_pin_state():
    """抓之前先把引脚/外设状态打出来，省得抓完再回头查软件。"""
    base = GPIO_BASE[TX_PIN[0]]
    moder = rd32(base + 0x00)
    otyper = rd32(base + 0x04)
    afrh = rd32(base + 0x24)          # PIN8..15 走 AFRH
    cr1 = rd32(UART_BASE + 0x00)
    brr = rd32(UART_BASE + 0x0C)

    def bits(v, hi, lo):
        return None if v is None else (v >> lo) & ((1 << (hi - lo + 1)) - 1)

    print("---- 抓之前先确认软件侧（免得抓完再查）----")
    if moder is not None:
        m = bits(moder, 2 * TX_PIN[1] + 1, 2 * TX_PIN[1])
        print("  PB9 MODER=%d（%s）" % (m, "复用AF" if m == 2 else "!! 不是 AF"))
        m = bits(moder, 2 * RX_PIN[1] + 1, 2 * RX_PIN[1])
        print("  PB8 MODER=%d（%s）" % (m, "复用AF" if m == 2 else "!! 不是 AF"))
    if afrh is not None:
        print("  PB9 AFR =%d（UART4 应为 8）  PB8 AFR =%d"
              % (bits(afrh, 4 * (TX_PIN[1] - 8) + 3, 4 * (TX_PIN[1] - 8)),
                 bits(afrh, 4 * (RX_PIN[1] - 8) + 3, 4 * (RX_PIN[1] - 8))))
    if otyper is not None:
        print("  PB9 OT  =%d（0=推挽）" % bits(otyper, TX_PIN[1], TX_PIN[1]))
    if cr1 is not None:
        print("  UART4 CR1=0x%08X  UE=%d TE=%d RE=%d"
              % (cr1, (cr1 >> 0) & 1, (cr1 >> 3) & 1, (cr1 >> 2) & 1))
    if brr:
        print("  UART4 BRR=%d  ⇒ 实际 %d baud（标称 921600）" % (brr, 100000000 // brr))
    print("")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=600.0,
                    help="连发时长，默认 600 s（给足你开软件、接夹子的时间）")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="两次触发的间隔秒数，默认 1 s")
    ap.add_argument("--req", type=int, default=1,
                    help="1=$?PING（默认） 2=$?WEA 3=$?WGET")
    a = ap.parse_args()

    tab = symtab()
    if "g_cmd_req" not in tab:
        print("[FAIL] 固件里没有 g_cmd_req（版本不对？）")
        return 1

    magic = rd1("g_boot_magic", tab)
    if magic != 0x594D4731:
        print("[FAIL] g_boot_magic=0x%08X —— 固件没在跑（烧完要 reset）"
              % (magic or 0))
        return 1

    show_pin_state()

    tx0 = rd1("g_cmd_tx", tab)
    print("  开始连发 %s，%.0f 秒，每 %.1f s 一帧" % (REQ_NAME.get(a.req, "?"),
                                                   a.seconds, a.interval))
    print("  现在就可以开 Logic 抓 %s 了" % ("PB%d" % (TX_PIN[1],)))
    print("")
    sys.stdout.flush()

    n = 0
    t_end = time.time() + a.seconds
    try:
        while time.time() < t_end:
            wr1("g_cmd_req", a.req, tab)
            n += 1
            if n % 10 == 0:
                print("  已触发 %d 帧（g_cmd_tx %d → %d）"
                      % (n, tx0, rd1("g_cmd_tx", tab)))
                sys.stdout.flush()
            time.sleep(a.interval)
    except KeyboardInterrupt:
        pass

    tx1 = rd1("g_cmd_tx", tab)
    print("")
    print("结束：共触发 %d 次，g_cmd_tx %d → %d（+%d）"
          % (n, tx0, tx1, tx1 - tx0))
    print("⚠ 固件侧计数只证明'写进了 TDR'，不证明电平出了脚 —— 以逻辑分析仪为准")
    return 0


if __name__ == "__main__":
    sys.exit(main())
