#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""起播"防咔哒"时序验收：DAC 唤醒时机 + 音量 ramp。

【背景：起播那一声"咔哒"】
  DAC 通路上电是"从 power-down 醒来"，输出会从直流 0 跳到工作点；原来起播序列
  把 es8311_start_dac() 放在 I2S **之前**、且 REG32 一步写到目标音量 ⇒ 两个跳变叠加。
  现在的时序（见 src/audio_out.c 顶部「起播防咔哒时序」）：
    1) audio_out_start() 只起 I2S（时钟跑、FIFO 有数据）；
    2) 下一拍 tick  才 es8311_dac_start_muted()：通路接通、音量仍 0（静音）；
    3) 随后 8 拍把 REG32 从 0 分步升到 UI 音量（≈ 130 ms）。

【判据】
  R1  g_ao_dac_ready == 1     DAC 通路已被唤醒
  R2  g_ao_dac_ramp  == 0     音量 ramp 已走完（否则会停在半档）
  R3  g_ao_stage     == 7     起播走完了"防咔哒"那一步（7 = DAC 已唤醒）
  R4  g_ao_udr       == 0     起播无欠载（UDR 会让引擎停吃 FIFO ⇒ 可闻咔哒）
  R5  g_ao_frames     > 0     真的在出声
  R6  路径相关：DMA ⇒ g_ao_dma_rc == 0；TIM6 ⇒ g_ao_isr > 0
  R7  g_es_wr_ok 的增量 —— 起播确实发出了一串 I2C 写（**硬判下界 ≥12** + 打印实际值）。
        参考量级：完整 init 时无 ramp 45 / 有 ramp 53；**跳过 init**（s_codec_ready=1）时
        无 ramp 16 / 有 ramp 24（见下方 EST_* 常量）。⚠ 但**实测它本身会浮动**（同板连续
        3 次起播 = 24/27/36）—— 因为 5 处"读改写"读失败会少写、回读补写会多写、probe halt
        还有伪影，所以只做下界硬判、不给定值判据；ramp 走完与否由 R2+R5 保证。

用法
----
    python tools/audio_start_ramp_check.py           # DMA 路径（g_ao_use_dma=1）
    python tools/audio_start_ramp_check.py --tim6    # TIM6 滴灌路径（出货默认）
    python tools/audio_start_ramp_check.py --keep    # 验完不停播（留给耳朵复听）
"""
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench  # noqa: E402

# ---- I2C 写次数参考值（逐条数 es8311.c / audio_out.c 的写路径）----
#   es8311_init          29（2026-10-14 补丁11：+2 笔"I2C 噪声免疫前置" 0x44=0x08 ×2）
#   es8311_config_i2s     2
#   dac_path_regs        14（2026-10-14 补丁11：+2 笔 REG00=0x80 / REG01=0x3F 时钟管理恢复，
#                            对应 es8311_stop_dac() 的完整掉电尾 D9）
#   audio_out_set_volume  0 或 1（codec 未 init 过时返回 -1、不写）
#   ⚠ 每次起播是否跑完整 init 取决于 s_codec_ready：一旦 init 成功就不再 init（续播/二次起播）
#      ⇒ 增量只有 config_i2s + dac_path_regs（+ ramp）。两套参考值都要认。
#   ⚠ 回读体检的"确认期补写"（REG32/REG12）可能再加 0~1 笔，判据留 ±1 容差。
EST_NO_RAMP = 45    # 完整 init（29）+ config_i2s（2）+ dac_path_regs（14）
EST_RAMP    = 53    # 上一行 + 8 拍音量 ramp
EST_SKIP_NR = 16    # 跳过 init：config_i2s（2）+ dac_path_regs（14）
EST_SKIP_R  = 24    # 上一行 + 8 拍音量 ramp


def rd_many(names):
    return {n: bench.rd(n, 1)[0] for n in names}


def main():
    tim6 = "--tim6" in sys.argv
    keep = "--keep" in sys.argv
    use_dma = 0 if tim6 else 1
    path = "TIM6 滴灌" if tim6 else "I2S TX DMA"

    # 健康度：内核必须真的在跑（probe-rs download 后忘了 reset 就会假死）
    h = rd_many(["g_boot_magic", "g_loop_count"])
    if h["g_boot_magic"] != 0x594D4731:
        print("固件没在跑：g_boot_magic=0x%08X（download 后必须 reset）" % h["g_boot_magic"])
        return 1
    time.sleep(0.5)
    l1 = bench.rd("g_loop_count", 1)[0]
    if l1 == h["g_loop_count"]:
        print("内核没在跑（g_loop_count 没变）")
        return 1
    print("健康度 OK（主循环在跑）。路径 = %s\n" % path)

    # 先停播，保证从"未在播"这个干净起点开始（s_codec_ready 顺便归一）
    bench.wr("g_wav_test", [2])
    time.sleep(0.4)

    base = rd_many(["g_es_wr_ok", "g_wav_state", "g_ao_dac_ready", "g_ao_dac_ramp"])
    print("起播前基线：state=%d dac_ready=%d dac_ramp=%d wr_ok=%d"
          % (base["g_wav_state"], base["g_ao_dac_ready"],
             base["g_ao_dac_ramp"], base["g_es_wr_ok"]))

    # 选路径 + 触发起播（固定 /YMGUI/MUSIC/TOKYOHOT.WAV）
    bench.wr("g_ao_use_dma", [use_dma])
    bench.wr("g_wav_test", [1])

    # 起播一拍后再看（ramp 共 8 拍 ≈ 130 ms；等 0.4 s 足够走完）
    time.sleep(0.4)
    mid = rd_many(["g_ao_stage", "g_ao_dac_ready", "g_ao_dac_ramp", "g_wav_state"])
    print("起播 0.4 s：stage=%d dac_ready=%d dac_ramp=%d wav_state=%d"
          % (mid["g_ao_stage"], mid["g_ao_dac_ready"],
             mid["g_ao_dac_ramp"], mid["g_wav_state"]))

    time.sleep(1.2)
    s = rd_many(["g_ao_stage", "g_ao_dac_ready", "g_ao_dac_ramp", "g_ao_udr", "g_ao_frames",
                 "g_ao_underrun", "g_ao_ring_drop", "g_ao_dma_rc", "g_ao_dma_half",
                 "g_ao_dma_cplt", "g_ao_dma_err", "g_ao_isr", "g_es_wr_ok", "g_wav_state"])
    delta = s["g_es_wr_ok"] - base["g_es_wr_ok"]

    print("\n=== 起播 1.6 s 后 ===")
    print("  stage=%d  dac_ready=%d  dac_ramp=%d  wav_state=%d"
          % (s["g_ao_stage"], s["g_ao_dac_ready"], s["g_ao_dac_ramp"], s["g_wav_state"]))
    print("  frames=%d  udr=%d  underrun=%d  ring_drop=%d"
          % (s["g_ao_frames"], s["g_ao_udr"], s["g_ao_underrun"], s["g_ao_ring_drop"]))
    print("  dma_rc=%d  dma_half=%d  dma_cplt=%d  dma_err=%d  tim6_isr=%d"
          % (s["g_ao_dma_rc"], s["g_ao_dma_half"], s["g_ao_dma_cplt"],
             s["g_ao_dma_err"], s["g_ao_isr"]))
    print("  I2C 写增量 = %d 次（无 ramp 参考 %d，有 ramp 参考 %d）"
          % (delta, EST_NO_RAMP, EST_RAMP))

    ok = True
    def chk(c, msg):
        nonlocal ok
        print("  [%s] %s" % ("PASS" if c else "FAIL", msg))
        if not c:
            ok = False

    print()
    chk(s["g_ao_dac_ready"] == 1, "R1 DAC 通路已唤醒（g_ao_dac_ready=1）")
    chk(s["g_ao_dac_ramp"] == 0,  "R2 音量 ramp 已走完（g_ao_dac_ramp=0）")
    chk(s["g_ao_stage"] == 7,     "R3 起播阶段 = 7（DAC 唤醒那一步已执行）")
    if tim6:
        # ⚠ TIM6 路径的 FIFO 垫子只有 ~82 µs，而 probe-rs 每读一次就会把内核 halt
        #   ~1 s；halt 期间 ISR 填不上 FIFO ⇒ udr 必然涨。那是**探针伪影**，不是
        #   起播缺陷（出货默认路径在真机连续播放时的判据是"听着有没有卡顿"）。
        print("     [SKIP] R4 TIM6 路径不计 udr=%d —— probe-rs halt 会制造它" % s["g_ao_udr"])
    else:
        chk(s["g_ao_udr"] == 0,   "R4 起播无欠载（g_ao_udr=0）")
    chk(s["g_ao_frames"] > 0,     "R5 真的在出声（g_ao_frames>0）")
    if tim6:
        chk(s["g_ao_isr"] > 0,    "R6 TIM6 路径：心跳在跑（g_ao_isr>0）")
    else:
        chk(s["g_ao_dma_rc"] == 0, "R6 DMA 路径：起播返回码 0（g_ao_dma_rc=0）")
    # R7 判据（2026-10-14 修正）：写次数**不是确定值**，实测同板连续 3 次起播 = 24/27/36。
    #   三个来源让它浮动：① 是否跳过完整 init（±29）；② dac_path_regs/config_i2s 里 5 处
    #   "读改写"若读失败则该笔写被跳过（少 0~5 笔）；③ 回读体检"确认期补写"（多 0~6 笔）+
    #   probe-rs halt 伪影。⇒ 硬判只保**下界**（起播序列确实发了写、没早退），实际值打印出来
    #   供判读；偏离参考值时给一行提示而不误判失败。ramp 是否真走完由 R2（dac_ramp==0）+R5 兜。
    near = any(abs(delta - e) <= 2
               for e in (EST_RAMP, EST_NO_RAMP, EST_SKIP_R, EST_SKIP_NR))
    chk(delta >= 12, "R7 起播确实发出了 I2C 写（增量 %d ≥ 12；参考值见脚本头）" % delta)
    if not near:
        print("     （增量 %d 不在参考值 ±2 内 —— 条件读改写/回读补写/探针伪影都会让它浮动）" % delta)
    if abs(delta - EST_NO_RAMP) <= 2 or abs(delta - EST_SKIP_NR) <= 2:
        print("     ⚠ 增量≈%d（无 ramp 参考）⇒ 可能是**没有走 ramp**（8 次写没发生）"
              "——查 tick 里的 s_vol_ramp_left" % delta)

    if not keep:
        bench.wr("g_wav_test", [2])   # 停播，不吵人
        print("\n已停播。")
    else:
        print("\n（--keep：仍在播，供耳朵复听）")
    print("\n结论：%s" % ("全部 PASS" if ok else "有 FAIL，见上"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
