#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""TF 卡 WAV 播放链路（音乐 app 真正走的那条）板上验收（2026-10-13）

与 audio_out_check.py 的区别：那条验的是**内置正弦**（不碰卡）；这条验的是
**真·音乐 app 路径** —— FatFs 打开 /YMGUI/MUSIC/TOKYOHOT.WAV、解析 WAV 头、
feeder 从 TF 卡读数据、喂进同一个 audio_out 引擎（codec + I2S2 + TIM6 滴灌）。

判据：
  B1  文件能打开 + 头能解析 + 起播成功：g_wav_rc == 0
  B2  正在播：g_wav_state == 1 且 g_ao_playing == 1
  B3  卡在供给：g_wav_pos 在涨（文件被读）
  B4  音频在流：g_ao_frames 在涨
  B5  卡跟得上：g_wav_read_ms < 42（半缓冲 42 ms，读一次不能超时）
  B6  采样率/声道对得上：g_wav_rate == 48828 且 g_wav_channels == 1

⚠ 只验数据通路；听不听得见由耳朵判定。WAV 必须已拷进 TF 卡
   /YMGUI/MUSIC/TOKYOHOT.WAV（用户自己拔卡拷贝）。文件不在 ⇒ B1 报 -3/-2。

用法：
    python tools/wav_play_check.py
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

# g_wav_test..g_wav_state 在 .bss 连续（见 nm 校验），一次读全：
#   [0]test [1]read_ms [2]underrun [3]pos [4]total [5]channels
#   [6]bits [7]rate [8]state
WV_BASE = "g_wav_test"
WV_N = 9

# g_ao_stage..g_ao_codec 连续 11 字：索引 7=frames 9=playing 10=codec
AO_BASE = "g_ao_stage"
AO_N = 11


def rd_wav():
    w = bench.rd(WV_BASE, WV_N)
    return {
        "test": w[0], "read_ms": w[1], "underrun": w[2], "pos": w[3],
        "total": w[4], "channels": w[5], "bits": w[6], "rate": w[7],
        "state": w[8],
    }


def rd_ao():
    w = bench.rd(AO_BASE, AO_N)
    return {"frames": w[7], "playing": w[9], "codec": w[10]}


def alive():
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.5)
    return bench.rd("g_loop_count", 1)[0] != l0


def main():
    ok = True

    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1

    print("起播：写 g_wav_test = 1（/YMGUI/MUSIC/TOKYOHOT.WAV，走音乐 app 真实路径）")
    bench.wr("g_wav_test", [1])
    time.sleep(1.0)

    wv = rd_wav()
    rc = bench.rd("g_ao_rc", 1)[0]
    ao = rd_ao()
    print("  起播后 ~1 s：g_wav_rc=%d  state=%d  rate=%d  ch=%d  bits=%d  "
          "total=%d  pos=%d  read_ms=%d  ao_playing=%d"
          % (rc, wv["state"], wv["rate"], wv["channels"], wv["bits"],
             wv["total"], wv["pos"], wv["read_ms"], ao["playing"]))

    if rc != 0:
        print("[FAIL] B1 起播失败：g_wav_rc=%d（0=OK，-2=卡没挂载，-3=打不开文件，"
              "-11..-20=WAV 头解析失败，-22=audio_out 起播失败）。"
              " 多半是 TF 卡里没有 /YMGUI/MUSIC/TOKYOHOT.WAV —— 请确认已拷入。"
              % rc)
        return 1
    print("[PASS] B1 文件打开 + WAV 头解析 + 起播成功（g_wav_rc=0）")

    if wv["state"] != 1 or ao["playing"] != 1:
        print("[FAIL] B2 没在播：g_wav_state=%d  g_ao_playing=%d"
              % (wv["state"], ao["playing"]))
        ok = False
    else:
        print("[PASS] B2 正在播（g_wav_state=1, g_ao_playing=1）")

    if wv["rate"] != 48828 or wv["channels"] != 1:
        print("[WARN] B6 采样率/声道与 I2S(48828/单声道) 不符：rate=%d ch=%d"
              % (wv["rate"], wv["channels"]))
    else:
        print("[PASS] B6 采样率/声道对得上（%d Hz / %d ch，无需重采样）"
              % (wv["rate"], wv["channels"]))

    pos0, frames0 = wv["pos"], ao["frames"]

    time.sleep(1.5)
    wv2 = rd_wav()
    ao2 = rd_ao()
    print("  再采一拍：pos %d→%d   ao_frames %d→%d   read_ms=%d   underrun=%d"
          % (pos0, wv2["pos"], frames0, ao2["frames"], wv2["read_ms"], wv2["underrun"]))

    if wv2["pos"] <= pos0:
        print("[FAIL] B3 卡没在供给：g_wav_pos 不涨（%d→%d）" % (pos0, wv2["pos"]))
        ok = False
    else:
        print("[PASS] B3 TF 卡在读数据（pos +%d 字节）" % (wv2["pos"] - pos0))

    if ao2["frames"] <= frames0:
        print("[FAIL] B4 音频没在流：g_ao_frames 不涨（%d→%d）"
              % (frames0, ao2["frames"]))
        ok = False
    else:
        print("[PASS] B4 音频在流（frames +%d）" % (ao2["frames"] - frames0))

    if wv2["read_ms"] >= 42:
        print("[WARN] B5 单次补数耗时 %d ms ≥ 42 ms（半缓冲预算），可能卡顿"
              % wv2["read_ms"])
    else:
        print("[PASS] B5 卡跟得上（单次补数 %d ms < 42 ms）" % wv2["read_ms"])

    # 收尾：停播（g_wav_test=2）
    bench.wr("g_wav_test", [2])
    time.sleep(0.3)
    wv3 = rd_wav()
    if wv3["state"] != 0:
        print("[WARN] 停播后 g_wav_state=%d（应=0）" % wv3["state"])
    else:
        print("[PASS] 停播干净（g_wav_state=0）")

    print("\n[PASS] TF 卡 WAV 播放链路验收通过（听不听得见由耳朵判定）"
          if ok else "\n[FAIL] TF 卡 WAV 播放链路验收未通过")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
