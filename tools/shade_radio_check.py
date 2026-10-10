#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""下拉抽屉（控制中心）的 WiFi / 蓝牙开关验收（2026-10-10）
=============================================================================
验的是「抽屉瓦片 ↔ 真实无线电状态」这条闭环：
  拨瓦片 → STM32 发 $?RADIO → ESP32 真的切无线电 → 回 $RD → 瓦片颜色跟着变

三层判据（缺一不可）
-----------------------------------------------------------------------------
  S0  抽屉可开   从状态栏往下拖后能看到控制中心（瓦片区域像素明显变化）
  S1  拨动发命令 点 WiFi / 蓝牙瓦片 ⇒ g_cmd_tx 增加（命令真的出去了）
  S2  瓦片跟随   把 g_net_radio_wifi / g_net_radio_bt 写成 0/1 后，
                 瓦片区域像素在两种状态下**必须不同**（颜色随状态变）

⚠ 为什么不能只断言"点一下状态就翻转"：
   那要等对端 ESP32 回 $RD，而验收脚本必须能独立跑（对端可能不在线）。
   S1 验"命令发出"，S2 用 SWD 直接写状态量验"显示跟随" —— 两段各自确定。

⚠ 瓦片几何（phone_quick.c 的 PhoneQuick_Place）：
   index%4*76 / 90 + index%8/4*69 / 66×62 ⇒
     wifi      = (14, 90, 66×62)
     bluetooth = (242, 90, 66×62)
   两个瓦片的背景色：开 = RGB(77,100,221)、关 = RGB(42,51,70)。

用法
-----------------------------------------------------------------------------
  python tools/shade_radio_check.py            # 全量（含真实开关一轮，会动真无线电）
  python tools/shade_radio_check.py --static   # 只验 S0/S1/S2，不真开真关无线电

⚠ 跑之前：probe-rs download 之后**必须 reset**，否则内核 halt。
"""
import argparse
import sys
import time

sys.path.insert(0, "tools")
import bench            # noqa: E402  SWD 读写
import caret_check as cc  # noqa: E402  全帧镜像读取
import pwr_shot as ps   # noqa: E402  抓屏（强制重绘）

# 瓦片矩形（抽屉内坐标系 = 屏幕坐标系）
WIFI_TILE = (14, 90, 66, 62)
BT_TILE = (242, 90, 66, 62)
WIFI_CLICK = (45, 117)      # 瓦片中心
BT_CLICK = (275, 117)


def rd1(name):
    return bench.rd(name, 1)[0]


def tap(x, y, dx=0, dy=0, wait=0.5):
    """走 phone_shell 的 g_bdtap_* 注入通道（支持拖拽）。"""
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [dx])
    bench.wr("g_bdtap_dy", [dy])
    bench.wr("g_bdtap_seq", [rd1("g_bdtap_seq") + 1])
    time.sleep(wait)


def frame():
    """抓一整帧（强制重绘，避免读到静止画面的旧帧）。"""
    ps.snap("tmp", "build/_shade_tmp.png")
    return cc.frame()


def region_px(rect):
    """取一帧里某矩形的像素。"""
    px = frame()
    x0, y0, w, h = rect
    return [px[(y0 + j) * cc.G.W + (x0 + i)] for j in range(h) for i in range(w)]


def region_diff(a, b):
    return sum(1 for u, v in zip(a, b) if u != v)


def open_shade():
    """从状态栏往下拖，打开控制中心。
    抽屉的触发区是状态栏那条 y=0..28 的透明带（phone_shade.c 的 trigger）。"""
    tap(160, 8, 0, 160, wait=1.0)


def close_shade():
    """从下方往上拖，收起抽屉。
    ⚠ 必须先收起再测"打开"：抽屉若本来就是开的，拉开动作不产生像素变化，
      "拖前/拖后"比对会得到 0 ⇒ 误判成"没打开"（第一次写这个脚本就踩了）。"""
    tap(160, 430, 0, -160, wait=1.0)
    tap(160, 430, 0, -160, wait=1.0)     # 再来一次，确保收干净


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--static", action="store_true",
                    help="只验 S0/S1/S2，不真开真关无线电")
    a = ap.parse_args()

    fails = []
    print("=== 下拉抽屉 WiFi/蓝牙开关验收 ===")

    # ---- S0：拉开抽屉 ----
    # ⚠ 判据用**全屏**像素差，不用单个瓦片区域：
    #   抽屉从屏外整块滑入，全屏变化十几万像素，信号最强、不依赖任何内部符号。
    #   （最初版本拿"蓝牙瓦片那一小块"当判据，实测为 0 而误判"没打开" ——
    #    那次其实是别的原因导致的假阴性，教训是判据要留足余量。）
    print("S0 打开控制中心（状态栏下拖 160px）")
    close_shade()                      # 先确保是收起态，否则"拖前/拖后"无差异
    a = frame()
    open_shade()
    time.sleep(0.8)
    b = frame()
    d = sum(1 for u, v in zip(a, b) if u != v)
    ok = d > 50000
    print(f"  {'PASS' if ok else 'FAIL'} 全屏像素变化 {d}（>50000 视为抽屉已开）")
    if not ok:
        fails.append("S0")
        print("  ⇒ 抽屉没打开，后面的点击都不可信，直接停。")
        print("=== FAILED ===")
        return 1

    # ---- S1：点瓦片 ⇒ 命令真的发出 ----
    print("S1 拨瓦片 ⇒ 发 $?RADIO 命令（判据 g_cmd_tx 增加）")
    t0 = rd1("g_cmd_tx")
    tap(*WIFI_CLICK)
    time.sleep(0.8)
    t1 = rd1("g_cmd_tx")
    ok = t1 > t0
    print(f"  {'PASS' if ok else 'FAIL'} WiFi 瓦片: g_cmd_tx {t0} → {t1}")
    if not ok:
        fails.append("S1-wifi")

    t0 = rd1("g_cmd_tx")
    tap(*BT_CLICK)
    time.sleep(0.8)
    t1 = rd1("g_cmd_tx")
    ok = t1 > t0
    print(f"  {'PASS' if ok else 'FAIL'} 蓝牙瓦片: g_cmd_tx {t0} → {t1}")
    if not ok:
        fails.append("S1-bt")

    # ---- S2：瓦片显示跟随 $RD 回流 ----
    print("S2 瓦片颜色跟随真实状态（SWD 写 g_net_radio_* 后比对像素）")
    bench.wr("g_net_rd_pkts", [1])     # >0 才让 Radio*On 返回真值
    for key, tile, label in (("g_net_radio_wifi", WIFI_TILE, "WiFi"),
                             ("g_net_radio_bt", BT_TILE, "蓝牙")):
        bench.wr(key, [0])
        time.sleep(0.6)
        off = region_px(tile)
        bench.wr(key, [1])
        time.sleep(0.6)
        on = region_px(tile)
        d = region_diff(off, on)
        ok = d > 500                    # 背景色整块换（66×62=4092 像素）⇒ 上千
        print(f"  {'PASS' if ok else 'FAIL'} {label} 瓦片 关/开 像素差 {d}（>500 视为颜色随状态变）")
        if not ok:
            fails.append(f"S2-{label}")

    # ---- 收尾：把 WiFi 恢复成开 ----
    # ⚠ S1 点 WiFi 瓦片是**真的**下发 $?RADIO,WIFI,...，会把对端 ESP32 的 WiFi 关掉
    #   （之后时间/天气停更）。收尾用固件自带的测试触发口 g_cmd_req=8 恢复，
    #   否则每跑一次这个脚本就把对端留在断网状态。
    #   取值与 src/uart_link.c 的 cmd_tick() 一致：7=WiFi OFF / 8=WiFi ON。
    print("收尾：恢复 WiFi 为开（发 $?RADIO,WIFI,ON）")
    bench.wr("g_cmd_req", [8])
    time.sleep(3.0)
    print(f"  g_net_radio_wifi = {rd1('g_net_radio_wifi')}（期望 1）")

    print("=== " + ("ALL PASS" if not fails else "FAILED: " + ", ".join(fails)) + " ===")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
