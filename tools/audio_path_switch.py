#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""切换出音路径（I2S TX DMA / TIM6 滴灌）——给"用耳朵 A/B"用（2026-10-14 补丁15）

背景
----
用户问过："为什么昨天用 TIM6 就没有爆音呢？后面走 DMA 路线就一直有爆音？"
并明确授权："如果修复后仍无法消除，则改用 TIM6 替代当前方案。"

补丁15 的结论是：那声"咔"来自**两条路径共用**的静音时序（es8311_dac_become_silent()
里 REG31 数字静音 = 即时把 ΣΔ 调制器输入钳 0 ⇒ 模拟输出一次阶跃），**与出音方式无关**
—— 所以"只换 TIM6"并不能消掉它，补丁15 修的是共享根因（见 docs/DEVELOPMENT_LOG.md）。

本脚本让**耳朵**来做最终判定：一条命令切一条路径，听同一段音乐、同样操作，
哪条路径下那几声"咔"没了/更轻。

⚠ 注意
  · `g_ao_use_dma` **不落盘** ⇒ 复位/断电后回到默认（DMA）。重烧固件也会回到默认。
  · 只影响**下一次起播**（当前正在播的那一段不会换路）—— 切换后请先点"停止"再"播放"。
  · 出音接线与 codec 配置两条路径完全相同，不存在"换路后没声音"的额外风险；
    唯一的差别是 CPU 中断频率：DMA ≈ 95 次/秒，TIM6 = 50 000 次/秒（见 --bench）。

用法
----
    python tools/audio_path_switch.py dma     # 切到 DMA（默认路径）
    python tools/audio_path_switch.py tim6    # 切到 TIM6 滴灌
    python tools/audio_path_switch.py status  # 只看当前值
    python tools/audio_path_switch.py bench   # 各跑 5 s 量主循环帧率（两条路径的开销差）
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402


def status():
    return bench.rd("g_ao_use_dma", 1)[0]


def loop_rate(secs=5.0):
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(secs)
    l1 = bench.rd("g_loop_count", 1)[0]
    return (l1 - l0) / secs


def main():
    if bench.rd("g_boot_magic", 1)[0] != 0x594D4731:
        print("固件没在跑（probe-rs download 之后必须 reset）")
        return 1
    arg = (sys.argv[1] if len(sys.argv) > 1 else "status").lower()

    if arg == "status":
        v = status()
        print("g_ao_use_dma=%d ⇒ 出音走 %s" % (v, "I2S TX DMA（默认）" if v else "TIM6 滴灌"))
        return 0

    if arg == "bench":
        for name, v in (("DMA", 1), ("TIM6", 0)):
            bench.wr("g_ao_use_dma", [v])
            time.sleep(0.5)
            bench.wr("g_wav_test", [2])
            time.sleep(0.6)
            bench.wr("g_wav_test", [1])
            time.sleep(1.5)                      # 等起播/音量 ramp 走完再量
            print("  %-4s：主循环 %.1f 拍/秒（播放中）" % (name, loop_rate(5.0)))
            bench.wr("g_wav_test", [2])
            time.sleep(0.6)
        bench.wr("g_ao_use_dma", [1])
        print("  （已恢复 g_ao_use_dma=1）")
        return 0

    if arg in ("dma", "tim6"):
        v = 1 if arg == "dma" else 0
        bench.wr("g_ao_use_dma", [v])
        time.sleep(0.3)
        print("已切到 %s（g_ao_use_dma=%d）" % ("DMA" if v else "TIM6", status()))
        print("⚠ 只对**下一次起播**生效：请在音乐 app 里先点暂停/停止、再点播放。")
        print("⚠ 复位或重新上电后回到默认（DMA）。")
        return 0

    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
