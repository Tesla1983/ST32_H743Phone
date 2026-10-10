#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""SPI 业务链路验收（STM32 侧，SWD 直读，不需屏幕 / 不需串口助手）
=============================================================================
SPI 升格为业务链路后（2026-10-10），本脚本来替代**已退役**的 tools/spi_check.py
（旧脚本验的是「自检通道」协议：5A 签名 + 滚动计数 + MISO 全 00/FF 判悬空；
新业务协议的诊断量完全是另一套，旧脚本的判据已失效，故不再使用）。

本脚本只读 STM32 固件里的诊断量（全部经 probe-rs 直读 RAM），给出三条结论：

  PH0  SPI 接管    g_spi_init_rc == 0
  PH1  下行通      观察窗口内 g_spi_rx_bytes 涨，且 g_net_dt_pkts / g_net_wd_pkts
                    至少其一涨  —— MISO(PA7→GPIO19 已接) ⇒ 时间/天气帧能到
  PH2  命令往返     g_cmd_tx > 0 且 g_cmd_rx > 0
                    g_cmd_tx > 0 但 g_cmd_rx == 0
                      ⇒ **上行命令线(MOSI)没接**：PA7 ↔ GPIO23 还没连
                      （下行已经能通，只是命令发不过去，对端收不到也就不回）

  这条「下行先通、上行待 MOSI」的分离判据，正好对应目前「MISO 已接、MOSI 待接」
  的硬件状态：没接 MOSI 时 PH1 就该 PASS、PH2 该 FAIL，不是 bug。

用法
-----------------------------------------------------------------------------
  python tools/spi_biz_check.py              # 静默快照 + PASS/FAIL（默认观察 5 s）
  python tools/spi_biz_check.py --ping       # 额外强制触发一次 $?PING 再判
  python tools/spi_biz_check.py --watch 10   # 连续观察 10 秒（PH1 观察窗口拉长）

⚠ 跑之前：probe-rs download 之后必须 reset，否则内核 halt、g_loop_count 不涨。
"""
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
ELF = "build/ymgui-h743.elf"

# 诊断量名（与 src/spi_link.h、src/uart_link.h 一一对应）
SPI_KEYS = ["g_spi_init_rc", "g_spi_presc", "g_spi_hz", "g_spi_tx_n",
            "g_spi_rx_bytes", "g_spi_tx_bytes", "g_spi_irq_level",
            "g_spi_last_code"]
NET_KEYS = ["g_net_rx_bytes", "g_net_dt_pkts", "g_net_wd_pkts",
            "g_cmd_tx", "g_cmd_rx"]


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


def rd(name, n=1):
    a = T[name][0]
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d"
                       % (PROBE, CHIP, a, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 %s 失败：%s%s" % (name, r.stdout, r.stderr))
    return [int(x, 16) for x in tok[:n]]


def rd1(name):
    return rd(name, 1)[0]


def wr1(name, v):
    a = T[name][0]
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True)


CODE_TEXT = {
    -2: "还没跑过事务",
    -1: "HAL 传输报错（SPI 外设/时钟没起来）",
    0:  "本次事务有数据收到（下行正常）",
    1:  "本次事务空（从机出站队列正好空，正常）",
}


def snapshot():
    d = {}
    for k in SPI_KEYS + NET_KEYS:
        d[k] = rd1(k)
    return d


def show(tag, d):
    print("---- %s ----" % tag)
    print("  SPI   init_rc=%-3d presc=/%-4d SCK=%s Hz  事务累计 %d"
          % (d["g_spi_init_rc"], d["g_spi_presc"], d["g_spi_hz"], d["g_spi_tx_n"]))
    print("        rx_bytes(下行喂解析)=%-7d  tx_bytes(命令已发)=%-7d"
          % (d["g_spi_rx_bytes"], d["g_spi_tx_bytes"]))
    print("        ready(PC0)=%d  上次事务=%s"
          % (d["g_spi_irq_level"], CODE_TEXT.get(d["g_spi_last_code"], "?")))
    print("  网络  net_rx_bytes=%-7d  $DT=%d  $WD=%d"
          % (d["g_net_rx_bytes"], d["g_net_dt_pkts"], d["g_net_wd_pkts"]))
    print("  命令  g_cmd_tx=%-6d  g_cmd_rx=%-6d  （双向握手看这一对）"
          % (d["g_cmd_tx"], d["g_cmd_rx"]))


def main():
    args = sys.argv[1:]
    watch = 5
    if "--watch" in args:
        watch = int(args[args.index("--watch") + 1])

    magic = rd1("g_boot_magic")
    if magic != 0x594D4731:
        print("[FAIL] g_boot_magic=0x%08X —— 固件没在跑（probe-rs download 后要 reset）"
              % magic)
        sys.exit(1)
    l0 = rd1("g_loop_count")
    time.sleep(1.0)
    l1 = rd1("g_loop_count")
    if l0 == l1:
        print("[FAIL] 内核没在跑（g_loop_count 1 秒内没变：%d）—— 先 reset" % l0)
        sys.exit(1)
    print("[OK] 固件在跑，主循环 %d 拍/秒" % (l1 - l0))

    if "--ping" in args:
        wr1("g_cmd_req", 1)
        print("[*] 已写 g_cmd_req=1（强制触发一次 $?PING），等 %d s 观察窗口..." % watch)
    else:
        print("[*] 观察窗口 %d s（不开 --ping：靠开机自动流程；下行通后自动发 PING）" % watch)

    s0 = snapshot()
    show("t0（窗口起点）", s0)
    time.sleep(watch)
    s1 = snapshot()
    show("t1（窗口终点）", s1)

    results = []
    print("=" * 70)

    # ---------- PH0：SPI 接管 ----------
    print("PH0  SPI 接管（g_spi_init_rc==0）")
    if s1["g_spi_init_rc"] == 0:
        print("  [PASS] SPI1 主初始化成功（SCK=%s Hz，分频 /%d）"
              % (s1["g_spi_hz"], s1["g_spi_presc"]))
        results.append(("PH0", True))
    else:
        print("  [FAIL] g_spi_init_rc=%d —— SPI 外设没起来" % s1["g_spi_init_rc"])
        print("         ⇒ 查 PA5/PA7/PB4(AF5) 与 PC5(CS) 初始化；看固件启动日志有无报错")
        results.append(("PH0", False))

    # ---------- PH1：下行通 ----------
    print("=" * 70)
    print("PH1  下行（ESP32 → STM32，MISO 已接即通）")
    d_rx = s1["g_spi_rx_bytes"] - s0["g_spi_rx_bytes"]
    d_dt = s1["g_net_dt_pkts"] - s0["g_net_dt_pkts"]
    d_wd = s1["g_net_wd_pkts"] - s0["g_net_wd_pkts"]
    d_net = s1["g_net_rx_bytes"] - s0["g_net_rx_bytes"]
    print("  窗口内增量：下行字节 +%d  $DT +%d  $WD +%d  net_rx总 +%d"
          % (d_rx, d_dt, d_wd, d_net))
    if d_rx > 0 and (d_dt > 0 or d_wd > 0):
        print("  [PASS] 下行帧真到了（时间/天气已解析）")
        results.append(("PH1", True))
    elif d_rx > 0:
        print("  [WARN] 下行有字节但不是 $DT/$WD（可能正在缓冲 / 对端刚启动）")
        print("         再观察一会；若持续如此，查对端是否真的在推帧")
        results.append(("PH1", False))
    else:
        print("  [FAIL] 一个下行字节都没收到")
        print("         ⇒ MISO(PB4←GPIO19) 没接 / 对端从机没跑 / CS 没落到 GPIO4")
        print("           对端日志应有 'VSPI(SPI3_HOST) slave ready'")
        results.append(("PH1", False))

    # ---------- PH2：命令往返 ----------
    print("=" * 70)
    print("PH2  命令往返（STM32 → ESP32，MOSI 接好才通）")
    tx = s1["g_cmd_tx"]
    rx = s1["g_cmd_rx"]
    print("  累计：g_cmd_tx=%d  g_cmd_rx=%d" % (tx, rx))
    if tx > 0 and rx > 0:
        print("  [PASS] 双向通（命令发得出去、对端回得来）")
        results.append(("PH2", True))
    elif tx > 0:
        print("  [FAIL] 本板发了 %d 条命令，一条回应都没有" % tx)
        print("         ⇒ **上行命令线(MOSI)没接**：PA7 ↔ GPIO23 还没连")
        print("           下行已通 ⇒ 不是线全断；只差这一根 MOSI。")
        print("           ⚠ 别落在 GPIO21（那是 ready 输出脚，两输出互灌会打架）")
        results.append(("PH2", False))
    else:
        print("  [FAIL] 连命令都没发出去（g_cmd_tx=0）")
        print("         ⇒ 多半是 PH1 下行还没确认（g_net_rx_bytes==0 时命令机不发车）")
        print("           先把下行跑通，或加 --ping 强制发一次再看")
        results.append(("PH2", False))

    print("=" * 70)
    bad = [n for n, ok in results if not ok]
    for n, ok in results:
        print("  %-4s %s" % (n, "PASS" if ok else "FAIL"))
    if bad:
        print("[FAIL] 未通过：%s" % ", ".join(bad))
        sys.exit(1)
    print("[PASS] SPI 业务链路在板验收通过")
    sys.exit(0)


if __name__ == "__main__":
    main()
