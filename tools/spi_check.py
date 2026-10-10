#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""⚠⚠ 本脚本已退役（2026-10-10）—— 不要再用于 SPI 验收。
=============================================================================
原因：SPI 已从「自检通道」升格为**业务链路**，诊断量与协议全部换了一套
      （旧：5A 签名 + 滚动计数 + MISO 全 00/FF 判悬空；新业务协议见
       src/spi_link.h 顶部长注释）。本脚本读的 g_spi_ok/g_spi_bad/
       g_spi_float0/g_spi_run/g_spi_last_tx/... 这些变量在升格时已被删除，
       现在跑会直接 KeyError。

替代：请用 tools/spi_biz_check.py（读 g_spi_init_rc / g_spi_rx_bytes /
      g_net_dt_pkts / g_cmd_tx / g_cmd_rx 等**现存的**业务诊断量）。

-----------------------------------------------------------------------------
（以下为历史内容，仅供回看旧自检协议，不再有效）
SPI1 主 ↔ ESP32（VSPI 从）链路自检判据。

只读 STM32 侧的 g_spi_*（SWD），对照 spi_link.h 顶部的协议与判据给出结论。
**不抓屏、不碰 UI** —— 这是纯数据层验收（屏幕那一路另有脚本）。

用法
----
    python tools/spi_check.py              # 读当前状态 + 判 PASS/FAIL
    python tools/spi_check.py --sweep      # 扫分频，实测"杜邦线能跑多快"
    python tools/spi_check.py --watch 10   # 连续观察 10 秒

三种"没通"必须分开判，否则排查会走错方向
----------------------------------------
  float0  / floatff 占多数  → MISO 悬空：线没接、对端没跑从机、或 CS 没落到 GPIO4
  bad 占多数                → 线通了但校验不过：速率太高 / 相位 / 位对齐
  hwerr                     → HAL 传输本身超时：SPI 外设没配起来或时钟没出

⚠ 判"通"不等于判"对"：脚本只能证明字节往返一致且稳定，
  最终"这根线就是那根线"仍以接线表为准（见 spi_link.h 顶部）。
"""
import struct
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench  # noqa: E402

# 分频 → 实际 SCK（PCLK2=100 MHz 时），从慢到快扫
PRESC_LIST = [256, 128, 64, 32, 16, 8, 4, 2]

CODE_TEXT = {
    0:  "通过",
    -1: "HAL 传输报错（hwerr）",
    -2: "还没跑过",
    1:  "签名不是 5A（从机没应答 / MISO 不对）",
    2:  "XOR 不符（MOSI 载荷没被从机正确收到）",
    3:  "滚动计数不连续（丢事务 / 从机没在处理）",
    4:  "常量尾错位（位对齐 / 时钟相位问题）",
    5:  "收到全 00（MISO 悬空低：没接或从机没跑）",
    6:  "收到全 FF（MISO 悬空高）",
}


def rd_bytes(name, n):
    """读 uint8_t 数组：按字读再解成字节（小端）。"""
    w = bench.rd(name, (n + 3) // 4)
    b = b"".join(struct.pack("<I", x) for x in w)
    return list(b[:n])


def pause_ping():
    """暂停周期 ping，返回原 g_spi_run（供 resume 用）。

    ⚠ 为什么必须暂停：snap() 要发十几次 probe-rs 读命令、耗时几百毫秒，
       而板子每 0.5 s 就在跑新事务 —— 不暂停的话读到的 g_spi_tx_n / ok / bad /
       last_rx / last_code **来自不同的事务**，会出现"事务 25 但 通过5+不过28=33"
       这种自相矛盾的数字，把排查带偏（2026-10-10 实测踩到）。
    """
    old = bench.rd("g_spi_run", 1)[0]
    if old:
        bench.wr("g_spi_run", [0])
        time.sleep(0.05)
    return old


def resume_ping(old):
    if old:
        bench.wr("g_spi_run", [old])


def snap(pause=True):
    """抓全部诊断量。默认先暂停 ping 保证快照是原子的。"""
    old = pause_ping() if pause else 0
    d = {}
    for k in ("g_spi_init_rc", "g_spi_presc", "g_spi_hz", "g_spi_tx_n",
              "g_spi_ok", "g_spi_bad", "g_spi_float0", "g_spi_floatff",
              "g_spi_hwerr", "g_spi_last_code", "g_spi_run",
              "g_spi_irq_rising", "g_spi_irq_level"):
        d[k] = bench.rd(k, 1)[0]
    d["tx"] = rd_bytes("g_spi_last_tx", 8)
    d["rx"] = rd_bytes("g_spi_last_rx", 8)
    d["run"] = 0
    resume_ping(old)
    return d


def zero_counters():
    for k in ("g_spi_tx_n", "g_spi_ok", "g_spi_bad",
              "g_spi_float0", "g_spi_floatff", "g_spi_hwerr"):
        bench.wr(k, [0])


def health():
    h = {k: bench.rd(k, 1)[0] for k in ("g_boot_magic", "g_shell_rc")}
    if h["g_boot_magic"] != 0x594D4731:
        print("固件没在跑：%s（probe-rs download 后必须 reset）" % h)
        return False
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    l1 = bench.rd("g_loop_count", 1)[0]
    if l0 == l1:
        print("内核没在跑（g_loop_count 1 秒内没变：%d）" % l0)
        return False
    return True


def report(d):
    print("  init_rc=%-3d presc=/%-4d SCK=%s Hz" %
          (d["g_spi_init_rc"], d["g_spi_presc"], d["g_spi_hz"]))
    print("  事务 %-6d 通过 %-6d 不过 %-6d 全00 %-6d 全FF %-6d HAL错 %d" %
          (d["g_spi_tx_n"], d["g_spi_ok"], d["g_spi_bad"],
           d["g_spi_float0"], d["g_spi_floatff"], d["g_spi_hwerr"]))
    print("  上次 发: " + " ".join("%02x" % b for b in d["tx"]))
    print("  上次 收: " + " ".join("%02x" % b for b in d["rx"]))
    print("  上次判定: %s" % CODE_TEXT.get(d["g_spi_last_code"], "?"))
    print("  ready(PC0) 上升沿=%d 当前电平=%d" %
          (d["g_spi_irq_rising"], d["g_spi_irq_level"]))


def verdict(d):
    ok = d["g_spi_ok"]
    bad = d["g_spi_bad"]
    f0 = d["g_spi_float0"]
    ff = d["g_spi_floatff"]
    hw = d["g_spi_hwerr"]
    n = d["g_spi_tx_n"]

    print("\n---- 判据 ----")
    if n == 0:
        print("  [FAIL] 一次事务都没发（g_spi_tx_n=0）")
        print("         → g_spi_run 是否为 1？或写 g_spi_test=1 手动触发一次")
        return 1
    if hw:
        print("  [FAIL] HAL 传输报错 %d 次 → SPI 外设/时钟没起来，查 g_spi_init_rc" % hw)
        return 1
    if ok == 0 and (f0 or ff):
        print("  [FAIL] 通了但 MISO 上没东西（全00=%d 全FF=%d）" % (f0, ff))
        print("         → 线序/CS 脚对不对？对端 spi_link_start() 起来了吗？")
        print("           对端日志里应有 'VSPI(SPI3_HOST) slave ready'")
        return 1
    if ok == 0 and bad:
        print("  [FAIL] 收到东西但一次都没过（bad=%d，原因：%s）"
              % (bad, CODE_TEXT.get(d["g_spi_last_code"], "?")))
        return 1
    if bad:
        print("  [WARN] 通过 %d / 不过 %d —— 链路通但**不稳定**" % (ok, bad))
        print("         → 多半是速率太高；用 --sweep 找可用上限")
        return 1
    print("  [PASS] %d/%d 全通过，无一项失败" % (ok, n))
    return 0


def run_probe():
    """MOSI 线探针：PA7 静态驱动低/高，看对端 GPIO23 读到的电平跟不跟随。

    用来定论 XOR 恒为 0 到底是哪一种：
      lo=0 且 hi=1 → 线是通的 ⇒ 是"主机在发 0x00"这类软件问题
      不跟随/都是 -1 → 线没接或落错脚 ⇒ 去查 PA7 ↔ GPIO23
    """
    print("\n---- MOSI 线探针 ----")
    bench.wr("g_spi_probe_lo", [-1])
    bench.wr("g_spi_probe_hi", [-1])

    bench.wr("g_spi_probe", [1])
    for _ in range(40):
        time.sleep(0.2)
        if bench.rd("g_spi_probe", 1)[0] == 0:
            break
    lo = bench.rd("g_spi_probe_lo", 1)[0]

    bench.wr("g_spi_probe", [2])
    for _ in range(40):
        time.sleep(0.2)
        if bench.rd("g_spi_probe", 1)[0] == 0:
            break
    hi = bench.rd("g_spi_probe_hi", 1)[0]

    print("  驱动低 → 对端读到 %s" % (lo if lo >= 0 else "没读到(-1)"))
    print("  驱动高 → 对端读到 %s" % (hi if hi >= 0 else "没读到(-1)"))
    print()
    if lo == 0 and hi == 1:
        print("  [PASS] 电平跟随 ⇒ **MOSI 线是通的**（PA7→GPIO23 已连通）")
        print("         ⇒ XOR 恒 0 是**主机发送侧**的问题（实际在发 0x00），查 HAL TX")
        return 0
    print("  [FAIL] 电平不跟随 ⇒ **MOSI 线没接上 / 落错脚**")
    print("         ⇒ 查 STM32 PA7 是否真的接到 ESP32 GPIO23；")
    print("           特别注意别落在 GPIO21（那是本方案的 ready 输出脚，")
    print("           两个输出脚互灌会打架，也可能烧驱动）")
    return 1


def sweep():
    """从最慢扫到最快，找"稳定通过"的最高速率。"""
    print("\n---- 分频扫描（从慢到快，每档约 3 秒 / ~6 次事务）----")
    print("  %-8s %-12s %-6s %-6s %-6s %-6s %-6s %s"
          % ("分频", "SCK", "事务", "通过", "不过", "全00", "全FF", "结论"))
    best = None
    rows = []
    for p in PRESC_LIST:
        bench.wr("g_spi_presc", [p])
        time.sleep(0.2)
        zero_counters()
        time.sleep(3.0)
        d = snap()
        good = (d["g_spi_ok"] >= 3) and (d["g_spi_bad"] == 0)
        tag = "OK" if good else ("不过" if d["g_spi_bad"] else "悬空")
        rows.append((p, d, good))
        print("  /%-7d %-12s %-6d %-6d %-6d %-6d %-6d %s"
              % (p, d["g_spi_hz"], d["g_spi_tx_n"], d["g_spi_ok"],
                 d["g_spi_bad"], d["g_spi_float0"], d["g_spi_floatff"], tag))
        if good:
            best = (p, d["g_spi_hz"])

    print("\n---- 扫描结论 ----")
    if best is None:
        print("  [FAIL] 没有任何一档稳定通过 —— 先按最慢档(/256)排线序与从机")
        return 1
    print("  最快稳定档：/%d = %d Hz" % (best[0], best[1]))
    uart_bps = 921600
    print("  对比现状 UART 921600（8N1 有效约 %d B/s）：" % (uart_bps // 10))
    print("    本档字节率约 %d B/s ⇒ 约 **%.0f 倍**"
          % (best[1] // 8, (best[1] // 8) / float(uart_bps // 10)))
    return 0


def main():
    if not health():
        return 1

    if "--sweep" in sys.argv:
        return sweep()

    if "--probe" in sys.argv:
        return run_probe()

    watch = 0
    if "--watch" in sys.argv:
        watch = int(sys.argv[sys.argv.index("--watch") + 1])

    print("---- 当前状态 ----")
    d = snap()
    report(d)

    if watch:
        print("\n---- 观察 %d 秒 ----" % watch)
        t0 = d["g_spi_tx_n"]
        time.sleep(watch)
        d = snap()
        report(d)
        print("  %d 秒内新增事务 %d" % (watch, d["g_spi_tx_n"] - t0))

    return verdict(d)


if __name__ == "__main__":
    sys.exit(main())
