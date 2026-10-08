#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ESP32 → STM32H743 上行链路验收（USART6 / PC6-TX / PC7-RX，115200）。

回答三个问题，顺序不能反：

  1. **解析对不对**（不依赖硬件）       → 自检模式，喂三行实测样本
  2. **线上有没有字节**（波特率/接线）   → 等 15 s 看 g_uart_rx_bytes 涨不涨
  3. **帧有没有解析出来**               → g_net_dt_pkts / g_net_wd_pkts

这么分是为了**定位快**：
  自检挂 = 固件解析代码有问题（与线无关）；
  自检过 + rx_bytes 不动 = 接线/波特率/对端没发；
  rx_bytes 在涨但 dt_pkts 不涨 = 协议对不上（看 g_uart_line 原文）。

用法
----
    python tools/uart_check.py            # 全套（自检 + 线上等待 15 s）
    python tools/uart_check.py --wait 30  # 线上多等一会（天气帧 30 min 一次，等不到是正常的）
    python tools/uart_check.py --self     # 只跑自检，不碰线上数据

判据
----
  g_uart_rc        = 0          初始化成功
  自检             dt/wd/wf 各 +1，xor_fail 不增加
  g_uart_rx_bytes  在涨         线上真有字节
  g_uart_err_fe    = 0          帧错误 0 ⇒ 波特率对
  g_net_dt_pkts    > 0          至少收到一帧时间（10 s 一帧，等 15 s 够）
"""
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench  # noqa: E402  提供 PROBE / CHIP / rd / wr / T

PROBE = bench.PROBE
CHIP = bench.CHIP
rd = bench.rd
wr = bench.wr


def rdu8(name, n):
    """读 uint8 数组（probe-rs 最小粒度是 32 位，读 ceil(n/4) 个字再拆字节）。"""
    words = (n + 3) // 4
    vals = rd(name, words)
    out = bytearray()
    for v in vals:
        out += v.to_bytes(4, "little")
    return bytes(out[:n])


def show(title):
    print("\n---- %s ----" % title)


def snapshot():
    return {
        "rc": rd("g_uart_rc", 1)[0],
        "baud": rd("g_uart_baud", 1)[0],
        "bytes": rd("g_uart_rx_bytes", 1)[0],
        "frames": rd("g_uart_frames", 1)[0],
        "drop": rd("g_uart_rx_drop", 1)[0],
        "fe": rd("g_uart_err_fe", 1)[0],
        "ne": rd("g_uart_err_ne", 1)[0],
        "ore": rd("g_uart_err_ore", 1)[0],
        "pe": rd("g_uart_err_pe", 1)[0],
        "dt": rd("g_net_dt_pkts", 1)[0],
        "wd": rd("g_net_wd_pkts", 1)[0],
        "wf": rd("g_net_wf_pkts", 1)[0],
        "xor": rd("g_net_xor_fail", 1)[0],
        "bad": rd("g_net_bad_pkts", 1)[0],
        "idle": rd("g_uart_idle_ms", 1)[0],
    }


def main():
    args = sys.argv[1:]
    wait_s = 15
    if "--wait" in args:
        wait_s = int(args[args.index("--wait") + 1])
    self_only = "--self" in args

    # 内核有没有在跑（download 后不 reset 的话这里会先报出来）
    c0 = rd("g_loop_count", 1)[0]
    time.sleep(0.3)
    if rd("g_loop_count", 1)[0] == c0:
        print("固件没在跑（g_loop_count 不动）—— probe-rs download 后必须 reset")
        return 1

    fail = []

    # ---------- 1. 自检：解析对不对（与线无关）----------
    show("1. 解析自检（喂三行实测样本，结果落在 g_uart_st_* 影子变量里）")
    wr("g_uart_selftest", [1])
    time.sleep(0.6)

    # ⚠ 读的是 g_uart_st_*（影子），不是 g_net_*：脚本等了 0.6 s 才读，
    #   主变量已经被线上真实帧覆盖了。
    d_dt = rd("g_uart_st_dt", 1)[0]
    d_wd = rd("g_uart_st_wd", 1)[0]
    d_wf = rd("g_uart_st_wf", 1)[0]
    d_xor = rd("g_uart_st_xor", 1)[0]

    print("  $DT +%d   $WD +%d   $WF +%d   XOR 失败 +%d" % (d_dt, d_wd, d_wf, d_xor))
    print("  epoch=%d  %02d-%02d %02d:%02d:%02d wday=%d"
          % (rd("g_uart_st_epoch", 1)[0], rd("g_uart_st_mon", 1)[0],
             rd("g_uart_st_mday", 1)[0], rd("g_uart_st_hh", 1)[0],
             rd("g_uart_st_mm", 1)[0], rd("g_uart_st_ss", 1)[0],
             rd("g_uart_st_wday", 1)[0]))
    print("  温度×10=%d  code=%d   （样本：20.0℃ / 晴）"
          % (rd("g_uart_st_temp", 1)[0], rd("g_uart_st_code", 1)[0]))

    # 期望全部来自对端日志里的原样样本 $DT,1791469600,2026-10-08,22:26:40,4*28
    #                                $WD,200,47,65,111,0,晴*EB
    #                                $WF,1,192.168.1.2,-76*08
    exp = [("$DT 解析 +1", d_dt, 1), ("$WD 解析 +1", d_wd, 1), ("$WF 解析 +1", d_wf, 1),
           ("XOR 失败 +0", d_xor, 0),
           ("小时 = 22", rd("g_uart_st_hh", 1)[0], 22),
           ("分钟 = 26", rd("g_uart_st_mm", 1)[0], 26),
           ("秒   = 40", rd("g_uart_st_ss", 1)[0], 40),
           ("月   = 10", rd("g_uart_st_mon", 1)[0], 10),
           ("日   = 8", rd("g_uart_st_mday", 1)[0], 8),
           ("wday = 4(周四)", rd("g_uart_st_wday", 1)[0], 4),
           ("温度×10 = 200", rd("g_uart_st_temp", 1)[0], 200),
           ("code = 0(晴)", rd("g_uart_st_code", 1)[0], 0)]
    for name, got, want in exp:
        if got != want:
            fail.append("自检：%s（实到 %d，应为 %d）" % (name, got, want))

    print("  自检：%s" % ("PASS" if not fail else "FAIL"))

    if self_only:
        return 1 if fail else 0

    # ---------- 2. 线上有没有字节 ----------
    show("2. 线上接收（等 %d s，时间帧 10 s 一帧）" % wait_s)
    b0 = snapshot()
    print("  g_uart_rc=%d  baud=%d" % (b0["rc"], b0["baud"]))
    if b0["rc"] != 0:
        fail.append("uart_link_init 失败：g_uart_rc=%d" % b0["rc"])

    time.sleep(wait_s)
    b1 = snapshot()

    print("  rx_bytes %d → %d（+%d）" % (b0["bytes"], b1["bytes"], b1["bytes"] - b0["bytes"]))
    print("  frames   %d → %d（+%d）" % (b0["frames"], b1["frames"], b1["frames"] - b0["frames"]))
    print("  $DT %d → %d   $WD %d → %d   $WF %d → %d"
          % (b0["dt"], b1["dt"], b0["wd"], b1["wd"], b0["wf"], b1["wf"]))
    print("  错误：FE=%d NE=%d ORE=%d PE=%d   丢字节=%d   idle=%d ms"
          % (b1["fe"], b1["ne"], b1["ore"], b1["pe"], b1["drop"], b1["idle"]))

    if b1["bytes"] == b0["bytes"]:
        fail.append("线上 %d s 内一个字节都没收到（接线 / 波特率 / 对端没发）" % wait_s)
    if b1["fe"] != b0["fe"]:
        fail.append("出现帧错误 FE +%d —— 波特率不对的典型症状" % (b1["fe"] - b0["fe"]))
    if b1["dt"] == b0["dt"]:
        fail.append("没收到 $DT 帧（rx_bytes 动了但解析不出，看下面原文）")

    # ---------- 3. 最后一帧原文（排障第一手证据）----------
    show("3. 最后一帧原文")
    ln = rd("g_uart_line_len", 1)[0]
    raw = rdu8("g_uart_line", min(ln if 0 < ln <= 200 else 200, 200))
    print("  长度 %d：%r" % (ln, raw.split(b"\x00")[0]))
    raw0 = rdu8("g_uart_raw", 16)
    print("  头 16 字节：%s" % " ".join("%02X" % b for b in raw0))

    show("结论")
    if fail:
        for f in fail:
            print("  FAIL: %s" % f)
        return 1
    print("  PASS：链路通、波特率对、帧解析正确")
    return 0


if __name__ == "__main__":
    sys.exit(main())
