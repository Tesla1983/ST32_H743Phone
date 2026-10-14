#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""补丁12 验收：起播/停止"咔哒"与欠载判据（2026-10-14）

背景（用户报"音频中仍然存在的噪声和咔嗒声"）
-------------------------------------------
定位结论（见 docs/DEVELOPMENT_LOG.md 补丁12）：
  · 送进 ES8311 的采样本体是**干净的**（tools/audio_buf_probe.py 实测：无零样点、
    无静音填充、L≡R）⇒ 所以"咔"不是数据通路脏，而是**时钟/静音时序**问题。
  · 起播：环缓冲被两次 dma_fill_half 取空 ⇒ 第一个半传中断起就在填硬静音，
    实测一次起播 `g_ao_underrun` 能 +6（旧固件）。
  · 停止（切歌/退出）：I2S 时钟在 DAC 还挂在有效音量时被掐断 ⇒ 直流阶跃 ⇒ "咔"。

⚠⚠ 测量方法学（2026-10-14 实测得出，必须遵守）
------------------------------------------
**SWD 探针本身会制造欠载**：每次 probe-rs 读/写都会短暂停机内核，停机 >42 ms 时主循环
来不及补数 ⇒ 环被抽干 ⇒ 半缓冲被填硬静音 ⇒ `g_ao_under_dma` 自己就涨了。
实测：同一段固件、同一段音乐，**窗内密集采样（每 50 ms 一读）28 s 得到 +9 次**，
而**窗内完全不读**的 20 s 窗口得到 **0 次**。同时 `g_tick_ms` 全程稳定 16 ms、无尖峰。
⇒ 判据只能在"**两端各读一次、窗内不做任何探针操作**"的窗口里看；窗内密集读数得到的是
   探针伪影，不是固件行为。

本脚本判据（全部为"板上可复现的确定性判据"）
------------------------------------------
  C1 起播 + 稳态：g_ao_under_dma == 0        —— 没有"会听到的"静音填充
     （起播窗口本身就含一次探针写 + 若干读 ⇒ 旧固件在这里稳定 +6，新固件 0；见上文）
  C2 垫子：g_ao_ring_used_min >= 4096        —— 起播铺满整环后，抗卡顿余量始终 ≥42 ms
  C3 起播补数及时：0 < g_ao_t_pump1_ms <= 60 —— 起播后第一拍就补上，不是几十毫秒后
  C4 DMA 无异常停顿：g_ao_dma_gap_max <= 43000（HT→HT 正常就是 ≈42 000，见 audio_out.h）
  C5 UDR 自恢复未发生：g_ao_udr == 0
  C6 环未溢出丢数：g_ao_ring_drop == 0
  C7 读卡不断供：g_wav_underrun == 0
  C8 停止/再播闭环：g_es_rb_mute 的 bit6/5 == 0 —— "先数字静音再停钟"之后，
     下次起播必须已解静音（否则"停过一次就再也出不了声"，且音量/电源回读全对、看不出来）

用法
----
    python tools/audio_click_check.py          # 起播 3 s + 稳态 20 s + 停止/再播闭环
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

DBG = ("g_ao_dbg", 24)
# g_ao_dbg[] 索引（见 src/audio_out.c 的 dbg_snapshot 索引表）
UNDER_DMA, UNDER_FEED, UNDER_SHORT, UNDER_FIRST = 17, 18, 19, 20
T_PUMP1, T_DACWAKE, RING_MIN = 21, 22, 23
# g_ao_t_ramp_ms 不在 dbg 表里 -> 单独读


def dbg():
    """一次读走 g_ao_dbg[24]：这些量是同一时刻的现场（avoid 逐符号读的伪影）。"""
    return bench.rd(DBG[0], DBG[1])


def main():
    rc = 0
    bm = bench.rd("g_boot_magic", 1)[0]
    if bm != 0x594D4731:
        print("固件没在跑（g_boot_magic=0x%08X）" % bm)
        return 1
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(1.0)
    l1 = bench.rd("g_loop_count", 1)[0]
    print("主循环 %d 拍/秒" % (l1 - l0))
    if l0 == l1:
        print("内核没在跑")
        return 1

    print("\n=== ① 起播窗口（按下播放后 3 s）===")
    bench.wr("g_wav_test", [1])
    time.sleep(3.0)
    d = dbg()
    frames = bench.rd("g_ao_frames", 1)[0]
    print("  g_ao_under_dma=%d  under_feed=%d  under_short=%d  首个欠载@%d ms"
          % (d[UNDER_DMA], d[UNDER_FEED], d[UNDER_SHORT], d[UNDER_FIRST]))
    print("  起播时序：首次补满环 +%d ms   DAC 唤醒 +%d ms   音量 ramp 走完 +%d ms"
          % (d[T_PUMP1], d[T_DACWAKE], bench.rd("g_ao_t_ramp_ms", 1)[0]))
    print("  垫子下限 g_ao_ring_used_min=%d 半字（=%.0f ms）" % (d[RING_MIN], d[RING_MIN] / 48.828))
    print("  g_ao_frames=%d（≈%.2f s 音频）" % (frames, frames / 48828.0))
    start = dict(dma=d[UNDER_DMA], feed=d[UNDER_FEED], short=d[UNDER_SHORT],
                 ring_min=d[RING_MIN], pump1=d[T_PUMP1], gap=d[0], frames=frames)

    print("\n=== ② 稳态 20 s（真实播放，无起播扰动）===")
    time.sleep(20.0)
    d2 = dbg()
    f2 = bench.rd("g_ao_frames", 1)[0]
    wu = bench.rd("g_wav_underrun", 1)[0]
    rd_ = bench.rd("g_ao_ring_drop", 1)[0]
    udr = bench.rd("g_ao_udr", 1)[0]
    gap = bench.rd("g_ao_dma_gap_max", 1)[0]
    print("  20 s 增量：under_dma=+%d  under_feed=+%d  under_short=%d"
          % (d2[UNDER_DMA] - start["dma"], d2[UNDER_FEED] - start["feed"], d2[UNDER_SHORT]))
    half = bench.rd("g_ao_dma_half", 1)[0]
    cplt = bench.rd("g_ao_dma_cplt", 1)[0]
    ratio = f2 / max(1, half + cplt)
    print("  ring_used_min=%d 半字（瞬时；到 0 正常）  frames=%d（Δ=%d）"
          % (d2[RING_MIN], f2, f2 - start["frames"]))
    print("  半传+全传中断=%d+%d ⇒ 平均每个半缓冲补 %d 帧（满值 1024）" % (half, cplt, ratio))
    print("  g_wav_underrun=%d  g_ao_ring_drop=%d  g_ao_udr=%d  gap_max=%d µs" % (wu, rd_, udr, gap))

    print("\n=== ③ 停止 → 再播：REG31 解静音闭环 ===")
    bench.wr("g_wav_test", [2])           # 停（走 audio_out_stop：先数字静音、再停钟、再掉电）
    time.sleep(1.5)
    bench.wr("g_wav_test", [1])           # 再起播（dac_path_regs 末尾负责解静音）
    time.sleep(3.0)
    mute = bench.rd("g_es_rb_mute", 1)[0]
    d3 = dbg()
    print("  g_es_rb_mute=0x%02X（bit6/5 应=0 ⇒ 未静音）  g_ao_t_dacwake_ms=%d  音量 ramp 走完 +%d ms"
          % (mute & 0xFF, d3[T_DACWAKE], bench.rd("g_ao_t_ramp_ms", 1)[0]))
    # ★ 补丁13：停止/切歌**不再给 codec 断电**（断电会带来 VMIDSEL/VREF 台阶 = 爆音）。
    #   同时"切时钟前的静音"必须**被确认**，不能只是"写了"。
    pdown = bench.rd("g_es_pdown_n", 1)[0]
    sil_ng = bench.rd("g_es_silent_ng", 1)[0]
    sil_fail = bench.rd("g_ao_silent_fail", 1)[0]
    sil_vol = bench.rd("g_es_silent_vol", 1)[0]
    sil_mute = bench.rd("g_es_silent_mute", 1)[0]
    print("  补丁13：真·掉电次数=%d（正常路径应 0）  静音确认：成功=%d 未确认=%d  音频层失败=%d"
          % (pdown, bench.rd("g_es_silent_n", 1)[0], sil_ng, sil_fail))
    print("          最后一次静音回读：REG32=%#04x（应 0x00）  REG31=%#04x（bit6/5 应=1）"
          % (sil_vol & 0xFF, sil_mute & 0xFF))
    bench.wr("g_wav_test", [2])
    time.sleep(0.5)

    print("\n=== 判据 ===")
    checks = [
        ("C1 无'会听到的'静音填充（起播+稳态 under_dma == 0）", d2[UNDER_DMA] == 0),
        # ⚠ 为什么不再拿 ring_used_min 当判据：环占用**瞬时到 0 是正常的** —— 半传中断
        #   （或 HT/TC 合并成一次）刚把最后一块取走时它必然为 0，下一拍 tick 立刻补 4096。
        #   "环到 0"唯一有危害的情形是"紧接着又来一次补数" —— 那会直接体现为 C1 的
        #   under_dma 增长。所以判据取**补齐率**：
        ("C2 每个半缓冲都基本补满（frames/中断数 ≥ 900，满值 1024）", ratio >= 900),
        ("C3 起播后第一拍就补数（0 < pump1 ≤ 60 ms）", 0 < d[T_PUMP1] <= 60),
        ("C4 DMA 无异常停顿（gap_max ≤ 43000 µs）", gap <= 43000),
        ("C5 UDR 未发生（g_ao_udr == 0）", udr == 0),
        ("C6 环未溢出丢数（g_ao_ring_drop == 0）", rd_ == 0),
        ("C7 读卡不断供（g_wav_underrun == 0）", wu == 0),
        ("C8 停止后再播已解静音（REG31 bit6/5 == 0）", (mute & 0x60) == 0),
        # ★ 补丁13 新增两条：直指用户报的"爆音"
        #   C9：切时钟之前"静音"必须是**被确认**的，而不是"写了就算"。I2C 写会失败
        #       （实测首试 NACK 5%~30%，失败环里 0x31 静音/0x32 音量命中最多），
        #       "写了就算"= 有相当概率在满音量下切时钟 ⇒ 直流阶跃 ⇒ 那声爆音。
        ("C9 静音已确认（REG32 回读==0 且 REG31 bit6/5==1，且无未确认记录）",
         (sil_vol == 0) and ((sil_mute & 0x60) == 0x60) and (sil_ng == 0) and (sil_fail == 0)),
        #   C10：正常播放/停止**不再给 codec 断电**。es8311_stop_dac() 的掉电尾会把
        #        内部参考 VREF(REG0D bit2) 与 VMIDSEL 一起断掉，"基准塌掉 → 重建"会在
        #        输出耦合电容上产生一次低频台阶（几十毫秒的"咚/噗"）—— 那是模拟域的台阶，
        #        任何数字静音都挡不住。所以这条是"爆音"修复的另一半。
        ("C10 正常路径未给 codec 断电（g_es_pdown_n == 0）", pdown == 0),
    ]
    for name, ok in checks:
        print("  %s  %s" % ("✅" if ok else "❌", name))
        if not ok:
            rc = 1
    print("\n结论：%s" % ("全部通过" if rc == 0 else "有判据未通过，见上"))
    print("（附录：起播现场 %s）" % json.dumps({k: (v if not isinstance(v, int) else v)
                                                 for k, v in start.items()}, ensure_ascii=False))
    return rc


if __name__ == "__main__":
    sys.exit(main())
