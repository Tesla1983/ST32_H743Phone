#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""无线电开关验收：STM32 控 ESP32 的 WiFi / 蓝牙开关（2026-10-10）
=============================================================================
验的是「设置页拨开关 → STM32 发 $?RADIO → ESP32 真的切无线电 → 回 $RD →
STM32 开关回填」这条闭环。全部经 probe-rs 直读 RAM，**不需要屏幕、不需要串口助手**。

判据（三个，缺一不可）
-----------------------------------------------------------------------------
  R0  链路活着      g_net_rd_pkts > 0
                    ⇒ 说明对端的 $RD 状态帧真的到了本板
                    （为 0 = 本板没问或问了没回，见下面「历史踩坑」）
  R1  蓝牙开关      写 g_cmd_req=4(BT ON) ⇒ g_net_radio_bt 变 1
                    写 g_cmd_req=5(BT OFF) ⇒ g_net_radio_bt 变 0
  R2  WiFi 开关     写 g_cmd_req=7(WiFi OFF) ⇒ g_net_radio_wifi 变 0
                    写 g_cmd_req=8(WiFi ON)  ⇒ g_net_radio_wifi 变 1（务必恢复）

  每一步都**同时**要求 g_net_rd_pkts 涨（对端确实回了新一帧 $RD），
  只涨 g_cmd_tx 不算 —— 命令发出去不等于对端执行了。

触发机制
-----------------------------------------------------------------------------
  g_cmd_req 是 src/uart_link.c 里 `cmd_tick()` 的「脚本手工触发」口：
  写 1/2/3 = PING/WEA/WGET，写 4/5 = BT ON/OFF，6 = 查 $RD，7/8 = WiFi OFF/ON。
  发完固件自动清 0。本脚本写值后轮询等结果。

用法
-----------------------------------------------------------------------------
  python tools/radio_check.py            # 只验 R0 + 蓝牙（不动 WiFi，最安全）
  python tools/radio_check.py --wifi     # 加验 WiFi（会短暂断网，脚本末尾自动恢复）
  python tools/radio_check.py --wait 6   # 每步等待秒数（默认 4）

⚠ 跑之前：probe-rs download 之后**必须 reset**，否则内核 halt、什么都不会涨。

历史踩坑（都是本轮实测抓出来的真 bug，改这块代码前先看）
-----------------------------------------------------------------------------
1. **`$?` 的 `?` 必须自己写进 type**：uart_link_cmd() 不会替你补。
   当初写成 "RADIO"/"RD" ⇒ 线上是 `$RADIO,...`，对端比的是 "?RADIO"
   ⇒ 永远匹配不上、开关静默失效，而 g_cmd_tx 照样涨（看起来"发出去了"）。
2. **本板必须自己问 $?RD**：不能指望对端开机广播那一帧 —— 对端比本板先起来，
   那一帧本板还没开始轮询 SPI ⇒ g_net_rd_pkts 恒 0。
   故开机序列加了 CMD_PH_RADIO 相位 + CMD_PH_DONE 里 30 s 周期重查。
3. **判据不能看 g_cmd_rx**：`$?RD` 只让对端回 $RD 状态帧，**不回** $!RS，
   所以这一相里 g_cmd_rx 恒不变，拿它当判据必定误判超时。
"""
import argparse
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

KEYS = ["g_net_rd_pkts", "g_net_radio_wifi", "g_net_radio_bt",
        "g_cmd_req", "g_cmd_tx", "g_cmd_rx"]

# g_cmd_req 的取值（与 src/uart_link.c cmd_tick() 保持一致）
REQ_BT_ON, REQ_BT_OFF, REQ_QUERY = 4, 5, 6
REQ_WIFI_OFF, REQ_WIFI_ON = 7, 8


def symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    t = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            t.setdefault(p[2], []).append(int(p[0], 16))
    return t


def read32(addr, n=1):
    r = subprocess.run(
        f'probe-rs read {PROBE} --chip {CHIP} b32 0x{addr:08x} {n}',
        capture_output=True, text=True, encoding="utf-8", errors="ignore",
        shell=True)
    vals = []
    for tok in r.stdout.split():
        try:
            vals.append(int(tok, 16))
        except ValueError:
            pass
    return vals


def write32(addr, val):
    subprocess.run(
        f'probe-rs write {PROBE} --chip {CHIP} b32 0x{addr:08x} {val}',
        capture_output=True, text=True, encoding="utf-8", errors="ignore",
        shell=True)


class Ctx:
    def __init__(self):
        self.t = symtab()
        self.addr = {}
        missing = []
        for k in KEYS:
            if k not in self.t:
                missing.append(k)
            else:
                self.addr[k] = self.t[k][0]
        if missing:
            print("FAIL 符号缺失（固件不是最新版？）: " + ", ".join(missing))
            sys.exit(2)

    def rd(self, key):
        v = read32(self.addr[key], 1)
        return v[0] if v else None

    def snap(self):
        return {k: self.rd(k) for k in
                ("g_net_rd_pkts", "g_net_radio_wifi", "g_net_radio_bt")}


def fire(ctx, req, wait_s, label, expect_key, expect_val):
    """发一次命令，等 $RD 回来，判目标字段是否变成期望值。"""
    # 已经是期望态了：对端不会再推新帧（状态没变），直接算过。
    # ⚠ 不处理会误报 FAIL —— 实测"BT 本来就是开的"那次就是这样。
    if ctx.rd(expect_key) == expect_val:
        print(f"  PASS {label}: 本就是 {expect_key}={expect_val}（无需翻转）")
        return True
    p0 = ctx.rd("g_net_rd_pkts")
    write32(ctx.addr["g_cmd_req"], req)
    t0 = time.time()
    got = None
    while time.time() - t0 < wait_s:
        time.sleep(0.4)
        if ctx.rd("g_net_rd_pkts") > p0:
            time.sleep(0.3)
            got = ctx.snap()
            break
    if got is None:
        print(f"  FAIL {label}: 等 {wait_s}s 没等到新的 $RD"
              f"（g_net_rd_pkts 停在 {p0}）—— 对端没执行或帧没回来")
        return False
    val = got[expect_key]
    ok = (val == expect_val)
    print(f"  {'PASS' if ok else 'FAIL'} {label}: "
          f"{expect_key}={val}（期望 {expect_val}）"
          f"  $RD 帧数 {p0}→{got['g_net_rd_pkts']}"
          f"  wifi={got['g_net_radio_wifi']} bt={got['g_net_radio_bt']}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wifi", action="store_true",
                    help="加验 WiFi 开关（会短暂断网，脚本末尾自动恢复）")
    ap.add_argument("--wait", type=float, default=4.0, help="每步等待秒数")
    a = ap.parse_args()

    ctx = Ctx()
    print("=== 无线电开关验收（STM32 → ESP32）===")
    s0 = ctx.snap()
    print(f"起始: $RD 帧数={s0['g_net_rd_pkts']} "
          f"wifi={s0['g_net_radio_wifi']} bt={s0['g_net_radio_bt']} "
          f"(cmd_tx={ctx.rd('g_cmd_tx')} cmd_rx={ctx.rd('g_cmd_rx')})")

    results = []

    # R0：链路活着 —— $RD 真的到过
    r0 = s0["g_net_rd_pkts"] > 0
    print(f"{'PASS' if r0 else 'FAIL'} R0 链路: g_net_rd_pkts="
          f"{s0['g_net_rd_pkts']}（>0 才说明 $RD 到过本板）")
    results.append(r0)
    if not r0:
        print("  ⇒ 先跑 --wait 更长，或检查：固件是否已 reset？"
              "开机序列 CMD_PH_RADIO 是否已走到？")

    # R1：蓝牙开关（不影响联网，安全）
    print("R1 蓝牙开关:")
    results.append(fire(ctx, REQ_BT_ON, a.wait, "BT ON", "g_net_radio_bt", 1))
    results.append(fire(ctx, REQ_BT_OFF, a.wait, "BT OFF", "g_net_radio_bt", 0))

    # R2：WiFi 开关（可选，末尾必恢复）
    if a.wifi:
        print("R2 WiFi 开关（注意：OFF 期间天气/时间会停止更新）:")
        results.append(fire(ctx, REQ_WIFI_OFF, a.wait,
                            "WiFi OFF", "g_net_radio_wifi", 0))
        results.append(fire(ctx, REQ_WIFI_ON, a.wait + 2,
                            "WiFi ON(恢复)", "g_net_radio_wifi", 1))

    ok = all(results)
    print("=== " + ("ALL PASS" if ok else "FAILED") +
          f"（{sum(results)}/{len(results)}）===")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
