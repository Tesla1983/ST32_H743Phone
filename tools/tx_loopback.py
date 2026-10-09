#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""TX 环回自检：拿本板自己的 RX 去收自己 TX 发的帧
=============================================================================
什么时候用它
-----------------------------------------------------------------------------
逻辑分析仪暂时不可用时（软件起不来 / 夹具没到 / 只想一分钟出结论），
这是**唯一能证明"TX 脚真的在往外打字节"**的低成本办法：
固件走真实 `uart_link_cmd()` 发出去，同一个 UART4 的 RX 原样收回来，
看 `g_uart_rx_bytes` 涨不涨。

⚠⚠ 接线（顺序不能错，否则两个推挽输出会对撞）
-----------------------------------------------------------------------------
  1. **先拔掉 PB8 上来自对端的那根线**（对端帧口 TX → 本板 PB8）。
     不拔的话，本板 PB9 与对端 TX 两个推挽同时驱动同一条线，
     一个拉高一个拉低就是直通电流 —— 违反"禁止让 GPIO 灌入/灌出大电流"的红线。
  2. 用一根杜邦线把 **PB9 ↔ PB8** 短接（排针 IO 序列第 78 / 77 脚，相邻）。
  3. 跑本脚本。
  4. 测完**先拆短接线，再把对端那根线插回 PB8**。

判据
-----------------------------------------------------------------------------
  - rx 增量 ≥ 8 × 触发帧数 ⇒ **PB9 在按真实波特率往外打字节**，且排针位置找对了
    ⇒ 问题在"PB9 → 对端"那根线/对端那一侧
  - rx 增量 = 0 但 g_cmd_tx 涨 ⇒ 固件写了 TDR 但**电平没出脚**
    ⇒ 排针位置找错了，或这一脚坏了 ⇒ 用 pin_beacon.py 重新定位
"""

import subprocess
import sys
import time

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

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


def rd1(name, tab):
    a = tab[name][0]
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1"
                       % (PROBE, CHIP, a),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    return int(tok[0], 16) if tok else 0


def wr1(name, v, tab):
    a = tab[name][0]
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True,
                   encoding="utf-8", errors="ignore")


def main():
    n = 6
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            pass

    tab = symtab()
    magic = rd1("g_boot_magic", tab)
    if magic != 0x594D4731:
        print("[FAIL] g_boot_magic=0x%08X —— 固件没在跑（烧完要 reset）" % magic)
        return 1

    print("=" * 72)
    print("TX 环回自检（PB9 → PB8）")
    print("=" * 72)
    print("⚠ 确认过这两件事再做：")
    print("   ① 已拔掉 PB8 上来自对端的线（防止两个推挽对撞）")
    print("   ② 已用杜邦线把 PB9 与 PB8 短接")
    print("")

    rx0 = rd1("g_uart_rx_bytes", tab)
    tx0 = rd1("g_cmd_tx", tab)
    print("  基线：g_uart_rx_bytes=%d  g_cmd_tx=%d" % (rx0, tx0))
    print("  连发 %d 帧 $?PING ..." % n)
    sys.stdout.flush()

    for _ in range(n):
        wr1("g_cmd_req", 1, tab)
        time.sleep(1.0)
    time.sleep(1.0)

    rx1 = rd1("g_uart_rx_bytes", tab)
    tx1 = rd1("g_cmd_tx", tab)
    d_rx, d_tx = rx1 - rx0, tx1 - tx0
    print("")
    print("  结果：g_uart_rx_bytes %d → %d（+%d）" % (rx0, rx1, d_rx))
    print("        g_cmd_tx        %d → %d（+%d）" % (tx0, tx1, d_tx))
    print("")

    if d_tx == 0:
        print("  [FAIL] 连命令都没发出去 —— 状态机没被触发，先查 g_cmd_phase")
        return 1
    if d_rx >= 8 * d_tx:
        print("  [PASS] 环回成功：每帧都收到了 ≥8 字节")
        print("        ⇒ PB9 确实在按 UART4 的真实波特率往外打字节，排针位置也找对了")
        print("        ⇒ 断点在『PB9 → 对端 GPIO22』这一根线，或对端那一侧")
        return 0
    if d_rx > 0:
        print("  [可疑] 收到了 %d 字节（不足 %d）—— 波特率差太多或对端在抢线"
              % (d_rx, 8 * d_tx))
        return 1
    print("  [FAIL] 一个字节都没收到，但固件确实发了 %d 条" % d_tx)
    print("        ⇒ 电平没出脚：PB9 排针位置找错、短接线没接好、或这一脚坏了")
    print("        ⇒ 下一步：python tools/pin_beacon.py --pin PB9 用万用表重新定位")
    return 1


if __name__ == "__main__":
    sys.exit(main())
