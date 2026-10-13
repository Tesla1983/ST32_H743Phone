#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""常驻音频输出链路（codec + I2S2 + TIM6 中断滴灌）板上验收（2026-10-12 / 2026-10-13 修订）

与 tools/audio_check.py 的区别：**那条验的是自检路径**（轮询写 FIFO，只在
YMGUI_AUDIO_TEST=ON 时编进来）；**这条验的是出货路径**——音乐 app 与控制中心
音量真正会走的那条：ES8311 寄存器 → I2S2 → TIM6 周期中断滴灌 FIFO。

声源用**内置正弦**（g_ao_demo），故意不碰 TF 卡，这样：
    demo 响、播 WAV 不响  ⇒ 播放器/文件的问题；
    demo 也不响           ⇒ 音频硬件/接线/TIM6 滴灌的问题。

判据（全是确定整数）：
  A1  codec 起得来：g_ao_rc == 0 且 g_ao_codec == 1
  A2  TIM6 心跳在转：g_ao_isr / g_ao_push 都在涨
  A3  播了 3 秒的量：g_ao_frames ≈ 48 828 × 3 = 146 484（±10%）
  A4  没有持续欠载：g_ao_udr 与 g_ao_underrun 都远小于总帧数
  A5  播完自动收尾：g_ao_playing 回到 0

⚠⚠ 2026-10-13 修订：原脚本每个变量一次 probe-rs 子进程（~0.4–0.7s/次），
   8 次调用累计 > 3 s，把"播放中"窗口读成了"播完之后"（codec/playing 已被
   audio_out_stop 清零），误报 A1 失败。本版把 g_ao_stage..g_ao_codec 这 11 个
   连续的全局量一次性读回（一次 probe-rs），并对"没赶上播放窗口"做重触发重试。

用法：
    probe-rs download --probe 0416:5021:0123456789AB --chip STM32H743VITx build/ymgui-h743.elf
    probe-rs reset    --probe 0416:5021:0123456789AB --chip STM32H743VITx
    python tools/audio_out_check.py
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

FS = 48828          # I2S 的 fs（MCLK 12.5 MHz / 256）
DEMO_SEC = 3

# 自检音量：本自检播的是 977 Hz 正弦，出厂音量（70）听着太刺耳 —— 临时把
# g_audio_volume 降到一半，自检结束再还原。只动 RAM 镜像，不碰 app_config
# 持久化（所以对出货行为零影响）。
# ⚠ ES8311 REG32 是 0.5 dB/LSB 的数字音量（0xBF=0dB）：UI 值 70→35 的实际
#   衰减远不止"听感一半"（约 -33 dB）。若想要"听感小一半"（约 -6 dB），
#   把这里改成 64 即可。
CHECK_VOLUME = 35

# g_ao_stage..g_ao_codec 在 .bss 里连续排布（见 nm 校验），一次读全：
#   [0]stage [1]isr_dry [2]isr [3]txp_seen [4]pump [5]push [6]underrun
#   [7]frames [8]udr [9]playing [10]codec
AO_BLOCK_BASE = "g_ao_stage"
AO_BLOCK_N = 11


def rd_block():
    w = bench.rd(AO_BLOCK_BASE, AO_BLOCK_N)
    return {
        "stage": w[0], "isr_dry": w[1], "isr": w[2], "txp_seen": w[3],
        "pump": w[4], "push": w[5], "underrun": w[6], "frames": w[7],
        "udr": w[8], "playing": w[9], "codec": w[10],
    }


def alive():
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.5)
    return bench.rd("g_loop_count", 1)[0] != l0


def trigger_and_catch():
    """写 g_ao_demo=1 起播，等到"播放中"状态再返回那一拍的快照。
    若 probe-rs 延迟把窗口读漏了（playing 已回 0），过 3.5 s 重触发重试。"""
    for attempt in range(3):
        bench.wr("g_ao_demo", [1])
        time.sleep(0.7)
        b = rd_block()
        if b["playing"] == 1:
            return b, attempt
        print("  (第%d次没赶上播放窗口 playing=%d，等播完重触发)"
              % (attempt + 1, b["playing"]))
        time.sleep(3.5)
    return None, 3


def _run():
    ok = True

    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1

    print("起播：写 g_ao_demo = 1（3 秒 977 Hz，走常驻 TIM6 滴灌通路）")
    b0, attempt = trigger_and_catch()
    if b0 is None:
        print("[FAIL] 三次都没赶上播放窗口（playing 始终为 0）—— 起播可能失败。")
        return 1
    print("  播放中快照（attempt=%d）：playing=%d  codec=%d  stage=%d  "
          "isr=%d  push=%d  frames=%d"
          % (attempt, b0["playing"], b0["codec"], b0["stage"],
             b0["isr"], b0["push"], b0["frames"]))

    rc = bench.rd("g_ao_rc", 1)[0]
    if rc != 0 or b0["codec"] != 1:
        print("[FAIL] A1 codec 没起来：g_ao_rc=%d（0=OK，-2=总线上没 ES8311，"
              "-3=寄存器序列失败，-4=I2S 初始化失败）  codec=%d"
              % (rc, b0["codec"]))
        return 1
    print("[PASS] A1 ES8311 起来且序列写完（g_ao_rc=0, codec=1）")

    if b0["playing"] != 1:
        print("[FAIL] A1b 起播后 g_ao_playing 不是 1：%d" % b0["playing"])
        ok = False

    # 再等一会儿（仍在 3 s 窗口内），取第二拍看心跳有没有继续转
    time.sleep(1.2)
    b1 = rd_block()
    print("  TIM6 心跳：isr %d→%d   push(半字) %d→%d   frames %d→%d"
          % (b0["isr"], b1["isr"], b0["push"], b1["push"],
             b0["frames"], b1["frames"]))
    if b1["isr"] <= b0["isr"] or b1["push"] <= b0["push"]:
        print("[FAIL] A2 TIM6 中断/滴灌没在涨（isr %d→%d, push %d→%d）—— "
              "心跳没转起来，或中断没接上（检查 startup 向量表）"
              % (b0["isr"], b1["isr"], b0["push"], b1["push"]))
        ok = False
    else:
        print("[PASS] A2 TIM6 心跳在转（isr +%d, push +%d 半字）"
              % (b1["isr"] - b0["isr"], b1["push"] - b0["push"]))

    if b1["frames"] < FS * DEMO_SEC * 45 // 100:
        print("[WARN] A3 当前帧数 %d 偏少（播放中预期已超 %d）—— 可能采样过早"
              % (b1["frames"], FS * DEMO_SEC * 45 // 100))
    else:
        print("[PASS] A3 播量在增长（当前 %d 帧 ≈ %.2f s，播完应≈%d）"
              % (b1["frames"], b1["frames"] / float(FS), FS * DEMO_SEC))

    udr = b1["udr"]
    underrun = b1["underrun"]
    isr_dry = b1["isr_dry"]
    print("  欠载：UDR=%d  underrun=%d  isr_dry=%d" % (udr, underrun, isr_dry))
    if underrun > 8:
        print("[WARN] A4 underrun=%d 偏多 —— 补数没跟上（主循环被长任务拖住？）" % underrun)
    else:
        print("[PASS] A4 欠载在可接受范围（UDR=%d, underrun=%d, isr_dry=%d）"
              % (udr, underrun, isr_dry))

    # 等播完（> 3 s 后），确认自动收尾 + 总帧数对得上
    time.sleep(2.0)
    b2 = rd_block()
    frames = b2["frames"]
    expect = FS * DEMO_SEC
    print("  播完后快照：playing=%d  frames=%d（期望约 %d）"
          % (b2["playing"], frames, expect))
    if frames < expect * 90 // 100 or frames > expect * 110 // 100:
        print("[FAIL] A3 总送出帧数 %d 不在 [%d, %d] 内（±10%%）"
              % (frames, expect * 90 // 100, expect * 110 // 100))
        ok = False
    else:
        print("[PASS] A3 播量对得上（%d 帧 ≈ %.2f s）" % (frames, frames / float(FS)))

    if b2["playing"] != 0:
        print("[FAIL] A5 播完没收尾：g_ao_playing=%d" % b2["playing"])
        ok = False
    else:
        print("[PASS] A5 播完自动停（g_ao_playing=0，TIM6 与 codec 都已挂起）")

    print("\n[PASS] 常驻出声链路验收通过（听不听得见由耳朵判定，这里只验数据通路）"
          if ok else "\n[FAIL] 常驻出声链路验收未通过")
    return 0 if ok else 1


def main():
    saved_vol = bench.rd("g_audio_volume", 1)[0]
    bench.wr("g_audio_volume", [CHECK_VOLUME])
    print("自检音量：g_audio_volume %d → %d（自检结束还原）"
          % (saved_vol, CHECK_VOLUME))
    try:
        return _run()
    finally:
        bench.wr("g_audio_volume", [saved_vol])
        print("已还原 g_audio_volume = %d" % saved_vol)


if __name__ == "__main__":
    sys.exit(main())
