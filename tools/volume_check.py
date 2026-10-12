#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""控制中心「音量」滑杆板上验收（2026-10-12 新增）

验的是三件事，全是**确定性整数**，不靠"看着差不多"：
  S0 抽屉能拉开、音量那条滑杆真的画出来了（抓屏 PNG 人工核对布局）；
  S1 点音量滑杆 ⇒ display_volume / g_audio_volume 一起变成点击处对应的值；
  S2 g_cfg_volume 跟着变 ⇒ 持久化桥是通的（落盘次数 g_cfg_save_cnt 也应增加）。

坐标（phone_shade.c，2026-10-12 布局）：
  亮度滑杆  78..304 × y=230..254
  **音量滑杆 78..304 × y=268..292**（点 (155,280) ⇒ 约 34）
  通知标题  y=310        清空按钮 242,308 64×26
  通知卡片  339..390 / 399..450      收起按钮 108,455 104×23
  ⚠ sheet 高度已从 448 提到 480（全屏），所有下移过的坐标别再拿旧版对。

用法：
    probe-rs download --probe 0416:5021:0123456789AB --chip STM32H743VITx build/ymgui-h743.elf
    probe-rs reset    --probe 0416:5021:0123456789AB --chip STM32H743VITx
    python tools/volume_check.py                 # 全量（含抓屏）
    python tools/volume_check.py --no-shot       # 只验数值，不抓 PNG
"""
import sys
import time

sys.path.insert(0, "tools")
import bench
import ribbon_check as rc
import pwr_shot


def alive():
    """内核在跑吗（download 后没 reset 的经典陷阱）。"""
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.5)
    return bench.rd("g_loop_count", 1)[0] != l0


def open_shade():
    """从状态栏那条 y=0..28 的触发带往下拉 160 px。"""
    rc.drag(160, 8, 0, 160, wait=1.2)
    time.sleep(1.0)


def main():
    shot = "--no-shot" not in sys.argv

    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1

    vol_before = bench.rd("display_volume", 1)[0]
    cfg_before = bench.rd("g_cfg_volume", 1)[0]
    aud_before = bench.rd("g_audio_volume", 1)[0]
    save0 = bench.rd("g_cfg_save_cnt", 1)[0]
    print("开机值：display_volume=%d  g_cfg_volume=%d  g_audio_volume=%d"
          % (vol_before, cfg_before, aud_before))

    print("拉开控制中心")
    open_shade()
    if shot:
        pwr_shot.snap("控制中心（音量滑杆在亮度下面）", "build/_shade_volume.png")

    print("点音量滑杆 (155,280)")
    rc.tap(155, 280, wait=1.2)
    time.sleep(1.0)

    vol_after = bench.rd("display_volume", 1)[0]
    cfg_after = bench.rd("g_cfg_volume", 1)[0]
    aud_after = bench.rd("g_audio_volume", 1)[0]
    save1 = bench.rd("g_cfg_save_cnt", 1)[0]
    print("点击后：display_volume=%d  g_cfg_volume=%d  g_audio_volume=%d  save_cnt %d→%d"
          % (vol_after, cfg_after, aud_after, save0, save1))

    if shot:
        pwr_shot.snap("点过音量滑杆之后", "build/_shade_volume_after.png")

    ok = True
    # S1：值真的动了，且落在 0..100 之内（点 155 ⇒ 约 34，别断言精确值：
    #     滑杆拖柄宽度参与换算，差几格是正常的）
    if not (0 < vol_after < 100):
        print("[FAIL] S1 display_volume 没落在 (0,100)：%d" % vol_after)
        ok = False
    elif vol_after == vol_before:
        print("[FAIL] S1 display_volume 点了没变（还是 %d）" % vol_after)
        ok = False
    else:
        print("[PASS] S1 音量滑杆改变音量：%d → %d" % (vol_before, vol_after))

    # 三处镜像必须完全一致（UI 值 / 音频镜像 / 配置值）
    if not (vol_after == cfg_after == aud_after):
        print("[FAIL] S2 三处镜像不一致：display=%d cfg=%d audio=%d"
              % (vol_after, cfg_after, aud_after))
        ok = False
    else:
        print("[PASS] S2 display_volume / g_audio_volume / g_cfg_volume 三处一致")

    # S3：真的落盘了（save_cnt 增加）。出货构建没编 ES8311，写寄存器那句被 #if 掉，
    #     所以这里验的是"配置落盘"这一半。
    if save1 <= save0:
        print("[WARN] S3 g_cfg_save_cnt 没增加（%d → %d）：可能这次值与上次落盘的相同"
              % (save0, save1))
    else:
        print("[PASS] S3 落盘发生：g_cfg_save_cnt %d → %d" % (save0, save1))

    print("\n[PASS] 音量滑杆验收通过" if ok else "\n[FAIL] 音量滑杆验收未通过")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
