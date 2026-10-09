#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""读对端（ESP32）的 USB 控制台日志，专抓命令口心跳
=============================================================================
为什么需要它
-----------------------------------------------------------------------------
命令通道排障时，最想问的一句话是：

    「对端那个 RX 脚，到底有没有收到字节？」

STM32 侧只能看到 `g_cmd_rx` 不涨，但那**区分不了**：
  ① 线根本没通（对端一个字节都没收到）
  ② 对端收到了但没回 / 回了但 STM32 没解析出来

对端 `main/uplink_cmd.c` 里有一个每 10 s 一次的心跳日志，长这样：

    I (xxxxx) uplink_cmd: [扫描] 921600 bps: 期间收到 11 字节, 行 1 | 24 3F 50 49 4E 47 2A 32 46 0D 0A

它同时给出**字节数**和**原始 hex** —— 一眼就能区分上面两种情况。
本工具就是把这些行从日志流里挑出来。

前置：日志口必须在 UART0
-----------------------------------------------------------------------------
对端 `LOG_PORT` 默认 = `OTHER_PORT`(UART2)，TX = GPIO17 —— 那根线没人接，
日志就白白消失了。必须改成 UART0（= 板载 CH340 / COM10）才读得到：

    sdkconfig / sdkconfig.defaults 两处都要改（defaults 不合入已存在的 sdkconfig）：
      # CONFIG_UPLINK_LOG_DEST_OTHER is not set
      CONFIG_UPLINK_LOG_DEST_UART0=y
    改完必须 **build 且 flash** —— 只 build 不烧等于没改。

    ⚠ 排障结束后请改回 `CONFIG_UPLINK_LOG_DEST_OTHER=y`，别让日志混进帧流。

用法
-----------------------------------------------------------------------------
    python tools/esp_log_watch.py                  # 听 40 s
    python tools/esp_log_watch.py --seconds 90     # 听 90 s
    python tools/esp_log_watch.py --port COM10 --all   # 打印全部日志

判据
-----------------------------------------------------------------------------
  - 心跳里「期间收到 N 字节」N=0，hex 全空
        ⇒ GPIO22 上什么都没有 ⇒ PB9→GPIO22 这根线没接上 / 脚位找错
  - N>0 且 hex 以 `24 3F 50` 开头（= `$?P`，即 `$?PING`）
        ⇒ 线通了，且波特率被正确识别 ⇒ 问题在对端响应或 STM32 解析那一侧
  - N>0 但 hex 是乱码 / 以别的字节开头
        ⇒ 线通了但**波特率不对**，双方采样点错位（重点查两边 BRR/时钟源）
"""

import argparse
import re
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


# 心跳行： [扫描] 921600 bps: 期间收到 11 字节, 行 1 | 24 3F 50 ...
HEARTBEAT = re.compile(
    r"\[扫描\]\s*(\d+)\s*bps:\s*期间收到\s*(\d+)\s*字节,\s*行\s*(\d+)\s*\|\s*(.*)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--all", action="store_true", help="打印全部日志行（默认只打心跳+关键行）")
    a = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("[FAIL] 缺 pyserial：pip install pyserial")
        return 1

    print("=" * 74)
    print("对端控制台日志监听  %s @ %d" % (a.port, a.baud))
    print("=" * 74)
    print("⚠ 前提：对端 LOG_PORT 必须已切到 UART0（见本文件顶部说明），否则读不到。")
    print("  监听 %.0f 秒 ...\n" % a.seconds)
    sys.stdout.flush()

    try:
        s = serial.Serial(a.port, a.baud, timeout=0.5)
    except Exception as e:
        print("[FAIL] 打不开 %s：%s %s" % (a.port, type(e).__name__, e))
        return 1

    beats, buf = [], b""
    t0 = time.time()
    try:
        while time.time() - t0 < a.seconds:
            d = s.read(4096)
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode("utf-8", "replace").rstrip()
                m = HEARTBEAT.search(t)
                if m:
                    beats.append((m.group(1), int(m.group(2)),
                                  int(m.group(3)), m.group(4).strip()))
                    print("  [心跳] %s bps  期间收到 %d 字节  行 %d" %
                          (m.group(1), int(m.group(2)), int(m.group(3))))
                    print("         hex: %s" % m.group(4).strip())
                elif a.all:
                    print("  %s" % t)
                elif any(k in t for k in ("uplink", "命令", "cmd", "CMD",
                                          "GPIO", "baud", "波特")):
                    print("  %s" % t)
    finally:
        s.close()

    print("\n" + "=" * 74)
    if not beats:
        print("一个心跳都没收到（%.0f 秒内）。可能原因：" % a.seconds)
        print("  ① 日志口没切到 UART0（最常见，改完要 build+flash）")
        print("  ② 对端没在运行 / 复位了")
        print("  ③ 波特率不对（控制台固定 115200）")
        return 1

    total = sum(b[1] for b in beats)
    print("共 %d 次心跳，累计收到 %d 字节" % (len(beats), total))
    non_zero = [b for b in beats if b[1] > 0]
    if not non_zero:
        print("\n  ⇒ **GPIO22 上一个字节都没收到**")
        print("    PB9 → 对端 RX 这根线没接上，或脚位找错。")
        print("    下一步：pb9 发帧的同时用万用表量对端那一脚有没有电平跳变。")
        return 1

    hexs = non_zero[-1][3].replace(" ", "")
    print("\n  ⇒ GPIO22 收到过字节。最近一次：%d 字节，hex=%s" %
          (non_zero[-1][1], non_zero[-1][3]))
    if hexs.upper().startswith("243F50"):
        print("    以 `24 3F 50` = `$?P` 开头 ⇒ **线通了，且波特率被正确识别**")
        print("    ⇒ 断点转移到『对端有没有回 $!RS』或『STM32 有没有解析出来』")
    else:
        print("    不是 `$?P` 开头 ⇒ 线虽通但**波特率不对**（采样点错位，收到的是噪声）")
        print("    ⇒ 查两边 BRR/时钟源：STM32 侧应为 917431（UART4 挂 APB1=100 MHz）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
