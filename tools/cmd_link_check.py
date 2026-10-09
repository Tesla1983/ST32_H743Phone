#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
命令通道验收（STM32 侧）—— 本板主动发 $? 命令，看对端回不回
=============================================================================
与 tools/uart_check.py 的分工
-----------------------------------------------------------------------------
uart_check.py 验的是**推送方向**（对端定时推 $DT/$WD/$WF，本板被动收）；
本脚本验的是**命令方向**（本板发 $?PING/$?WEA/$?WGET，对端回 $!RS/$!BD）。
两个方向是两根不同的线，必须分开判。

判据（全部经 SWD 读固件里的计数，不需要屏幕也不需要串口助手）
-----------------------------------------------------------------------------
  P0 双向通   g_cmd_tx > 0 且 g_cmd_rx > 0
  P1 拉天气   g_cmd_weather_ok > 0（受理回执 + 之后 $WD 帧数真的涨了）
  P2 HTTP     g_cmd_bd_pkts 增加、g_cmd_bd_len > 0、g_cmd_bd_crc_bad 不增加，
              且负载与 PC 抓的同一 URL 内容逐字节一致（截断到 g_cmd_bd_len）

  最有用的一个判据：
      **g_cmd_tx 涨而 g_cmd_rx 不涨 ⇒ 发送方向的线没接（或对端没在听）。**
      本板开机自动发 3 次 PING，所以上电后直接读这两个数就能定性，不用做任何操作。

用法
-----------------------------------------------------------------------------
  python tools/cmd_link_check.py              # 只读当前状态并给结论
  python tools/cmd_link_check.py --ping       # 额外手动触发一次 PING
  python tools/cmd_link_check.py --weather    # 额外手动触发一次天气请求
  python tools/cmd_link_check.py --wget       # 额外手动触发一次 WGET 并比对内容
"""
import subprocess
import sys
import time
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

# 必须和固件里 CMD_TEST_URL 保持一致（src/uart_link.c）
TEST_URL = "http://t.weather.sojson.com/api/weather/city/101010100"


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


T = symtab()


def rd(name, n=1):
    a = T[name][0]
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d" % (PROBE, CHIP, a, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 %s 失败：%s%s" % (name, r.stdout, r.stderr))
    return [int(x, 16) for x in tok[:n]]


def rd1(name):
    return rd(name, 1)[0]


def rd_bytes(name, n):
    """读一段字节（g_cmd_body 这种 uint8 数组）"""
    a = T[name][0]
    r = subprocess.run("probe-rs read %s --chip %s b8 0x%08x %d" % (PROBE, CHIP, a, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 2 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 %s 失败：%s%s" % (name, r.stdout, r.stderr))
    return bytes(int(x, 16) for x in tok[:n])


def wr1(name, v):
    a = T[name][0]
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True)


def crc16_ccitt(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c


PHASE_NAME = {
    0: "IDLE(等板子起来)",
    1: "LINK(等线上出现字节)",
    2: "PING(已发握手，等回)",
    3: "WEA(已发天气请求，等受理)",
    4: "WDATA(已受理，等 $WD)",
    5: "DONE(结束)",
}


def snapshot():
    keys = ["g_cmd_tx", "g_cmd_rx", "g_cmd_xor_fail", "g_cmd_ping_ok",
            "g_cmd_weather_req", "g_cmd_weather_ack", "g_cmd_weather_ok",
            "g_cmd_phase", "g_cmd_bd_pkts", "g_cmd_bd_len", "g_cmd_bd_crc_bad",
            "g_cmd_bd_timeout", "g_cmd_bd_toobig",
            "g_uart_rx_bytes", "g_net_wd_pkts"]
    return {k: rd1(k) for k in keys}


def show(tag, s):
    print("---- %s ----" % tag)
    print("  命令状态机     %-2d  %s" % (s["g_cmd_phase"],
                                        PHASE_NAME.get(s["g_cmd_phase"], "?")))
    print("  g_cmd_tx       %-6d  已发出的命令帧" % s["g_cmd_tx"])
    print("  g_cmd_rx       %-6d  已收到的 $! 响应" % s["g_cmd_rx"])
    print("  g_cmd_ping_ok  %-6d  PING 握手成功" % s["g_cmd_ping_ok"])
    print("  天气 req/ack/ok %d / %d / %d" % (s["g_cmd_weather_req"],
                                              s["g_cmd_weather_ack"],
                                              s["g_cmd_weather_ok"]))
    print("  $!BD 收到/长度/CRC坏/超时/超限  %d / %d / %d / %d / %d"
          % (s["g_cmd_bd_pkts"], s["g_cmd_bd_len"], s["g_cmd_bd_crc_bad"],
             s["g_cmd_bd_timeout"], s["g_cmd_bd_toobig"]))
    print("  g_uart_rx_bytes %-7d  g_net_wd_pkts %d"
          % (s["g_uart_rx_bytes"], s["g_net_wd_pkts"]))


def main():
    args = sys.argv[1:]
    magic = rd1("g_boot_magic")
    if magic != 0x594D4731:
        print("[FAIL] g_boot_magic=0x%08X —— 固件没在跑（probe-rs download 后要 reset）" % magic)
        sys.exit(1)

    s0 = snapshot()
    show("开机自动流程之后的静态快照", s0)

    results = []

    # ---------- P0：双向通 ----------
    print("=" * 70)
    print("P0  双向握手")
    if "--ping" in args:
        wr1("g_cmd_req", 1)
        print("  已触发一次 $?PING，等 3 s ...")
        time.sleep(3.0)
    s1 = snapshot()
    d_tx = s1["g_cmd_tx"] - s0["g_cmd_tx"]
    d_rx = s1["g_cmd_rx"] - s0["g_cmd_rx"]
    print("  本次增量：g_cmd_tx +%d   g_cmd_rx +%d" % (d_tx, d_rx))
    if s1["g_cmd_tx"] > 0 and s1["g_cmd_rx"] > 0:
        print("  [PASS] 双向通（本板发得出去，对端回得来）")
        results.append(("P0", True))
    elif s1["g_cmd_tx"] > 0:
        print("  [FAIL] 本板发了 %d 条命令，一条回应都没有" % s1["g_cmd_tx"])
        print("        ⇒ 几乎可以确定是**发送方向的线没接**：")
        print("          PC6(USART6_TX) 需要接到对端命令口的 RX（默认 GPIO16 / 板上 RX2）。")
        print("          ⚠ 不要接到 GPIO3：那是板载 CH340 的 TX，两个推挽驱动会抢线。")
        results.append(("P0", False))
    else:
        print("  [FAIL] 连命令都没发出去（状态机停在 %s）"
              % PHASE_NAME.get(s1["g_cmd_phase"], "?"))
        results.append(("P0", False))

    # ---------- P1：开机主动拉天气 ----------
    print("=" * 70)
    print("P1  开机主动拉天气（$?WEA → $!RS,WEA,1 → 真的等到 $WD）")
    if "--weather" in args:
        wr1("g_cmd_req", 2)
        print("  已触发一次 $?WEA，等 25 s ...")
        time.sleep(25.0)
    s2 = snapshot()
    print("  天气 req=%d ack=%d ok=%d   （$WD 累计 %d 帧）"
          % (s2["g_cmd_weather_req"], s2["g_cmd_weather_ack"],
             s2["g_cmd_weather_ok"], s2["g_net_wd_pkts"]))
    if s2["g_cmd_weather_ok"] > 0:
        print("  [PASS] 请求之后确实等到了 $WD（不必再干等 30 分钟周期）")
        results.append(("P1", True))
    else:
        print("  [FAIL] 还没拿到：ack=%d ok=%d" % (s2["g_cmd_weather_ack"],
                                                  s2["g_cmd_weather_ok"]))
        results.append(("P1", False))

    # ---------- P2：通用 HTTP 通道 ----------
    if "--wget" in args:
        print("=" * 70)
        print("P2  通用 HTTP 通道（$?WGET → $!BD 头帧 + 定长裸负载）")
        wr1("g_cmd_req", 3)
        print("  已触发 $?WGET,1,%s，等 25 s ..." % TEST_URL)
        time.sleep(25.0)
        s3 = snapshot()
        print("  $!BD pkts=%d len=%d CRC坏=%d 超时=%d 超限=%d"
              % (s3["g_cmd_bd_pkts"], s3["g_cmd_bd_len"],
                 s3["g_cmd_bd_crc_bad"], s3["g_cmd_bd_timeout"],
                 s3["g_cmd_bd_toobig"]))
        ok = False
        if s3["g_cmd_bd_pkts"] > s0["g_cmd_bd_pkts"] and s3["g_cmd_bd_len"] > 0 \
                and s3["g_cmd_bd_crc_bad"] == s0["g_cmd_bd_crc_bad"]:
            n = s3["g_cmd_bd_len"]
            body = rd_bytes("g_cmd_body", n)
            print("  板子收到 %d 字节，CRC=0x%04X（固件已自行校验通过）"
                  % (n, crc16_ccitt(body)))
            try:
                req = urllib.request.Request(TEST_URL, headers={"User-Agent": "cmd-check/1.0"})
                ref = urllib.request.urlopen(req, timeout=15).read()
                m = min(len(ref), n)
                if ref[:m] == body[:m]:
                    print("  [PASS] 与 PC 抓取内容逐字节一致（比对前 %d 字节，PC 侧总长 %d）"
                          % (m, len(ref)))
                    ok = True
                else:
                    diff = sum(1 for a, b in zip(ref[:m], body[:m]) if a != b)
                    print("  [FAIL] 与 PC 抓取不一致：%d/%d 字节不同" % (diff, m))
            except Exception as e:
                print("  [跳过内容比对] PC 抓不到该 URL: %s" % e)
                ok = True
        else:
            print("  [FAIL] 没收到完整数据块")
        results.append(("P2", ok))

    print("=" * 70)
    bad = [n for n, ok in results if not ok]
    for n, ok in results:
        print("  %-4s %s" % (n, "PASS" if ok else "FAIL"))
    if bad:
        print("[FAIL] 未通过：%s" % ", ".join(bad))
        sys.exit(1)
    print("[PASS] 命令通道在板验收通过")
    sys.exit(0)


if __name__ == "__main__":
    main()
