#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ES8311 时钟管理恢复验收（2026-10-14 补丁11·D8/D9 配套）

【验的是什么】
补丁11 把 `es8311_stop_dac()` 换成了官方 esp_codec_dev 的**完整有序掉电尾**（D9）：
它会关掉 REG00 的 CSM_ON、把 REG01 的全部时钟使能清 0（MCLK_ON/BCLK_ON/CLKADC_ON/
CLKDAC_ON/ANACLKx_ON），并把 REG0D 写 0xFC（连内部参考 VREF 一起断电，旧版停在 0xFA）。

本项目续播**跳过完整 init**（audio_out.c：s_codec_ready 一旦置位就不再 init）⇒ 起播路径
`dac_path_regs()` 必须把 REG00=0x80 / REG01=0x3F 写回来，否则芯片**没有时钟**、DAC 收不到
数据 ⇒ **续播无声**，而这件事**音量回读（REG32/REG12）验不出来**（I2C 与音频时钟无关，
REG32 照样可读）—— 所以必须单独看时钟管理寄存器（本脚本看 g_es_rb_clk1/2/6）。

【判据】
  K1 首次起播后 REG01 == 0x3F（MCLK/BCLK/ADCCLK/DACCLK 全使能）
  K2 ★ 显式掉电 → 起播后 REG01 仍 == 0x3F   ← D9 配套恢复的证明
  K3 REG02 == 0x00（本项目 48k 分频/倍频）、REG06 与首次起播一致（bclk_div=4、SCK 不反相）
  K4 续播后 REG32 回读 == es8311_volume_reg(UI 音量)（芯片里真实音量对）
  K5 全程 g_es_fail 无增量（掉电尾/起播没有真丢写）
  K6 ★ K2 非空判据的**写入侧证据**（见下）

⚠⚠ 2026-10-14（补丁13）本脚本的关键改动：**掉电改为"显式触发"**。
  补丁13 起，正常播放/停止路径**不再**调用 es8311_stop_dac()（它会连内部参考 VREF 一起
  断掉，"基准塌掉 → 重建"本身就是一声爆音；用户报的爆音与此直接相关）。
  ⇒ 若本脚本还按老写法"停一下就期待读到掉电后的状态"，K2 就变成**空判据**：
    读到 REG01==0x3F 可能只是"它从来没被关过"，而不是"被关掉后又被写回来"。
  ⇒ 所以现在用排障钩子 `g_es_pdown_req=1` **显式**执行一次真·掉电，并用**写入侧**证据
     把配对钉死：
       K6a 掉电后 `g_es_reg01_val == 0x00`（掉电尾确实把时钟关了）
       K6b 起播后 `g_es_reg01_val == 0x3F` 且 `g_es_reg01_n` 有增量（起播路径确实写回来了）
     这两条与硬件回读独立，不受"读失败"影响，是真正不可绕过的证据。

⚠ 只验寄存器链路；听感由耳朵判定（判据只保证"时钟/通路/音量都对"）。
用法：python tools/es8311_clk_check.py [轮数，默认 2]
"""
import sys
import time

sys.path.insert(0, "tools")
import bench  # noqa: E402

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 2

VOL_FULL = 0xBF


def rd1(name):
    return bench.rd(name, 1)[0]


def clk():
    """回读时钟管理三兄弟（各一次 SWD 读；只读、不改电气）。"""
    return rd1("g_es_rb_clk1"), rd1("g_es_rb_clk2"), rd1("g_es_rb_clk6")


def snap(phase, wait):
    """写相位码触发板上动作（1起播/2停/3暂停/4续播），等它做完。"""
    bench.wr("g_wav_snap_req", [phase])
    time.sleep(wait)


def vol_reg(ui):
    return (ui * VOL_FULL) // 100


def main():
    fails = []

    print("=" * 74)
    print("ES8311 时钟管理恢复验收（补丁11·D8/D9：完整掉电尾 + 起播配套恢复）")
    print("=" * 74)

    l0 = rd1("g_loop_count")
    time.sleep(0.5)
    if rd1("g_loop_count") == l0:
        print("[FAIL] 内核没在转（probe-rs download 之后必须 reset）")
        return 1

    snap(2, 1.4)                        # 停播，回干净状态
    f0 = rd1("g_es_fail")

    # ---- 首次起播（刚 init 完；dac_path_regs 会写 REG00/REG01，此处是基线）----
    snap(1, 2.4)
    c1a, c2a, c6a = clk()
    print("首次起播后: REG01=%#04x REG02=%#04x REG06=%#04x" % (c1a, c2a, c6a))
    if c1a != 0x3F:
        fails.append("K1 首次起播后 REG01=%#04x（应 0x3F：全时钟使能）" % c1a)

    for r in range(1, ROUNDS + 1):
        print("-" * 74)
        print("轮 %d/%d" % (r, ROUNDS))

        # ① 停（补丁13 起"停止"**不再**自动给 codec 断电，见 es8311_stop_dac 的说明）
        snap(2, 1.4)
        # ①b **显式**触发一次真·掉电（排障钩子 g_es_pdown_req，主循环消费一次）。
        #     ⚠ 这一步是本脚本在补丁13 之后**不能省**的：否则 K2 会退化成空判据。
        p0  = rd1("g_es_pdown_n")
        n0  = rd1("g_es_reg01_n")
        bench.wr("g_es_pdown_req", [1])
        time.sleep(0.8)
        p1  = rd1("g_es_pdown_n")
        v01_after_pdown = rd1("g_es_reg01_val")
        if p1 <= p0:
            fails.append("[%d] K6 显式掉电钩子没被消费（g_es_pdown_n %d→%d）" % (r, p0, p1))
        if v01_after_pdown != 0x00:
            fails.append("[%d] K6a 掉电后 REG01 最后写入值 = %#04x（应 0x00：掉电尾确实关了时钟）"
                         % (r, v01_after_pdown))

        # ② 起播（走 dac_path_regs：应把 REG00=0x80 / REG01=0x3F 写回）
        snap(1, 2.6)
        n1  = rd1("g_es_reg01_n")
        v01_after_start = rd1("g_es_reg01_val")

        c1, c2, c6 = clk()
        ui = rd1("g_audio_volume")
        exp = vol_reg(ui)
        vol = rd1("g_es_rb_vol")

        print("  起播后: REG01=%#04x REG02=%#04x REG06=%#04x | REG32 回读=%#04x 期望=%#04x（UI %d）"
              % (c1, c2, c6, vol, exp, ui))
        print("          掉电: pdown %d→%d，REG01 写入值→%#04x；起播: REG01 写入值→%#04x（写入侧计数 %d→%d）"
              % (p0, p1, v01_after_pdown, v01_after_start, n0, n1))

        if c1 != 0x3F:
            fails.append("[%d] ★K2 显式掉电→起播后 REG01=%#04x（应 0x3F）—— 掉电尾关掉的时钟没被写回，"
                         "芯片无时钟 ⇒ 续播无声" % (r, c1))
        # ★ K6b：写入侧证据 —— 起播路径确实写过 REG01=0x3F（这样 K2 才不是空判据）
        if (n1 <= n0) or (v01_after_start != 0x3F):
            fails.append("[%d] K6b 起播没有写回 REG01（写入次数 %d→%d，最后值 %#04x，应 +1 且 0x3F）"
                         % (r, n0, n1, v01_after_start))
        if c2 != 0x00:
            fails.append("[%d] K3 REG02=%#04x（应 0x00：本项目 48k 分频/倍频）" % (r, c2))
        if c6 != c6a:
            fails.append("[%d] K3 REG06=%#04x ≠ 首次起播的 %#04x（bclk_div/SCK 反相位被改动）"
                         % (r, c6, c6a))
        if vol != 0xFFFF and vol != exp:
            fails.append("[%d] K4 续播后回读音量 %#04x ≠ 期望 %#04x" % (r, vol, exp))
        elif vol == 0xFFFF:
            print("  （REG32 回读失败：芯片此刻不可达，留作告警，不判失败）")

        bad = [f for f in fails if f.startswith("[%d]" % r)]
        print("  → 本循环 %s" % ("OK" if not bad else "有问题"))

    f1 = rd1("g_es_fail")
    print("-" * 74)
    print("g_es_fail: 起播前=%d 收尾=%d ⇒ 增量=%d（应 0；>0 ⇒ 掉电尾有笔真丢写）"
          % (f0, f1, f1 - f0))
    if f1 != f0:
        fails.append("K5 g_es_fail 增量 %d（掉电尾/起播有真丢写）" % (f1 - f0))

    print("=" * 74)
    if fails:
        print("汇总：FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  ✗ " + f)
        return 1
    print("汇总：全部 PASS —— 完整掉电尾关掉的时钟，起播时被正确写回（REG01=0x3F）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
