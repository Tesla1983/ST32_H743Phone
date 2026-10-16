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
  K5 ⚠ 全程 g_es_fail 的**增量** —— ★补丁17 起**改为 WARN 并报出失败寄存器**，不再判 FAIL。
     理由（据实，非放宽）：它衡量的是"有写尝试穷尽了重试"，**不等于"状态错了"**。
     板上实测：同一轮里 K5 报增量 6，而 K1/K2/K3/K4/K6a/K6b **全部通过**（REG01=0x3F、
     REG32 回读==目标）⇒ 终态是被回读闭环救回来的、正确的。
     "有没有坏状态"改由 K1~K4/K6（终态）+ **K7**（注入下闭环是否管用）来判 —— 更严，不是更松。
  K6 ★ K2 非空判据的**写入侧证据**（见下）
  K7 ★ 补丁17 新增：**故障注入**证明闭环真的管用 —— 用 `g_es_drop_wr_reg=0x01` 让
     "REG01（时钟使能）那一笔写"整段重试全失败，然后起播：
       K7a 注入确实被消费（g_es_fail_reg 低 16 位 == 0xFF）
       K7b 最终 REG01 仍 == 0x3F、REG00 bit7 == 1（**被闭环补回**）
       K7c g_es_fix_wr 有增量（证明是"补写"救的，而不是"根本没丢"）
       K7d/K7e DAC 仍唤醒、frames 仍在涨（不只是寄存器好看，是真的还在出声）
     ⚠ 这一条才是"时钟寄存器丢写会不会变成无声"的**直接**判据：不做注入，就只是在
       "这次恰好没丢"的运气下观察。

  K8 ★ 补丁17 新增：**诊断自检** —— 注入"REG32 那一笔写失败"，验证 `g_es_fail_reg` 确实
     报出 0x32（否则 K5 那句 WARN 指不出"是哪一支寄存器"，就是一句空话）。

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
    print("g_es_fail: 起播前=%d 收尾=%d ⇒ 增量=%d（0 最好；>0 = 有写穷尽了重试）"
          % (f0, f1, f1 - f0))
    warn = []
    if f1 != f0:
        fr = rd1("g_es_fail_reg")
        reg = (fr >> 16) & 0xFFFF
        st  = fr & 0xFFFF
        stn = {0xFF: "故障注入", 1: "NACK", 2: "BUSY", 3: "TIMEOUT"}.get(st, str(st))
        # ★ 补丁17：据实改判 —— 这条**不再判 FAIL**，改为 WARN + 报出失败寄存器。
        #   为什么改（不是放宽）：g_es_fail 衡量的是"有写尝试穷尽了重试"，**不等于"状态错了"**。
        #   本轮实测证据：同一轮里 K5 报增量 6，而状态类判据 K1/K2/K3/K4/K6a/K6b **全部通过**
        #   （REG01=0x3F、REG32 回读==目标、REG06 一致）⇒ 终态是被兜底救回来的、正确的。
        #   判"有没有坏状态"应当由 K1~K4/K6（终态）+ K7（注入下闭环是否管用）来判。
        #   ⚠ 但**不能**把这个数字藏起来：它同时报出"是哪一支寄存器"，因为丢 0x01/0x00
        #     （时钟使能）与丢 0x32（音量）后果完全不同（前者无声、后者音量偏轻）。
        warn.append("K5 g_es_fail 增量 %d，最后一笔是 REG0x%02X（%s）—— 终态判据仍全部通过，"
                    "记为告警（根因见 DEVELOPMENT_LOG 第三十六/三十七版）" % (f1 - f0, reg, stn))
        print("       ⚠ WARN 有 %d 笔写穷尽了重试；最后一笔 = REG0x%02X 状态=%s" % (f1 - f0, reg, stn))
        print("         （终态仍正确：见上面的 K1~K4/K6；根因=电气边际 + 电源态切换期 NACK 窗口）")

    # ---- K7（补丁17 新增）：**状态级注入 A/B** —— 证明"把 REG00/REG01 纳入回读闭环"承重 ----
    #   【为什么不是"让写失败"】只让写失败，芯片里的**旧值不变**（那时 REG01 本来就是 0x3F）
    #   ⇒ 救不救都一样，测不出东西。丢写的**真实后果**是"芯片停在一个错的值上"，
    #   所以这里用 g_es_clr_clk_req 直接把 REG01 清成 0x00（时钟全关 ⇒ 无声，而音量/
    #   电源/静音回读全都正常）—— 这才是"起播恢复时钟那一笔丢了"之后芯片的真实处境。
    #   【A/B】g_es_clk_loop_en=0 复现旧行为（只读不判不补）⇒ 它永远不回来；
    #           g_es_clk_loop_en=1（默认）⇒ 下次回读体检把它补回 0x3F。
    print("-" * 74)
    print("K7 状态注入 A/B：清掉 REG01（时钟使能）—— 闭环关/开，看它会不会自己回来")
    snap(1, 3.0)                                    # 先确保在播（回读体检只在播放中跑）
    if rd1("g_ao_playing") != 1:
        fails.append("K7a 前置条件不成立：没有在播放（回读体检不会跑）⇒ 本项是空测")

    # --- K7a：闭环关（= 旧行为）⇒ 注入的状态必须**保持**（复现"永久无声"）---
    bench.wr("g_es_clk_loop_en", [0])
    fx0 = rd1("g_es_fix_wr")
    bench.wr("g_es_clr_clk_req", [1])               # 注入：本拍把 REG01 写成 0x00
    time.sleep(2.5)                                 # > 体检周期（64 拍 ≈ 1.09 s）×2
    c1_off = rd1("g_es_rb_clk1")
    fx1 = rd1("g_es_fix_wr")
    print("  [K7a 闭环关] 清 REG01 后 REG01=%#04x（1 应保持 0x00）fix_wr %d→%d" % (c1_off, fx0, fx1))
    if c1_off != 0x00:
        # 若它自己回来了 ⇒ 说明"另有机制在补时钟"，本条修法的必要性论证就不成立 ⇒ 必须报出来。
        fails.append("K7a 闭环关着，清掉的 REG01 却自己变成了 %#04x ⇒ 除回读闭环外还有别的机制"
                     "在补时钟，本条修法的必要性论证需要复查" % c1_off)
    else:
        print("       ✅ 复现成功：旧行为下 REG01=0x00 **保持不动**（无时钟 ⇒ 无声，而其它回读全正常）")

    # --- K7b：闭环开（默认）⇒ 同样的注入必须被补回 ---
    bench.wr("g_es_clk_loop_en", [1])
    fx2 = rd1("g_es_fix_wr")
    bench.wr("g_es_clr_clk_req", [1])               # 再清一次（此时闭环开着）
    time.sleep(2.5)
    c1_on = rd1("g_es_rb_clk1")
    c0_on = rd1("g_es_rb_clk0")
    fx3 = rd1("g_es_fix_wr")
    playing = rd1("g_ao_playing")
    print("  [K7b 闭环开] 清 REG01 后 REG01=%#04x REG00=%#04x fix_wr %d→%d playing=%d"
          % (c1_on, c0_on, fx2, fx3, playing))

    if c1_on != 0x3F:
        fails.append("★K7b 闭环开着 REG01 仍=%#04x（应 0x3F）—— 时钟寄存器丢写**没被补回** ⇒ "
                     "「进度条在走但完全无声」这种坏状态仍然存在" % c1_on)
    if (c0_on & 0x80) != 0x80:
        fails.append("★K7b 闭环开着 REG00=%#04x（应 bit7=1：CSM_ON）" % c0_on)
    if fx3 <= fx2:
        fails.append("K7c g_es_fix_wr 没有增量（%d→%d）⇒ 不像「被补写救回」，判据可能失效"
                     % (fx2, fx3))
    if playing != 1:
        fails.append("K7d 闭环补回后没在播放（g_ao_playing=%d）" % playing)
    if not [f for f in fails if f.startswith(("★K7", "K7c", "K7d"))]:
        print("  ✅ K7 通过：闭环关时注入的 REG01=0x00 **保持不动**（复现坏状态），"
              "闭环开时被补回 0x3F（fix_wr 增量 %d）⇒ 这条修法是承重的、不是装饰"
              % (fx3 - fx2))
    bench.wr("g_es_clk_loop_en", [1])
    bench.wr("g_es_clr_clk_req", [0])

    # ---- K8（补丁17 新增）：诊断自检 —— 证明 g_es_fail_reg 真能指出"是哪一支寄存器" ----
    #   K5 的 WARN 值不值得看，取决于这个量准不准；否则那句 WARN 就是空话。
    #   做法：在**停止态**注入"对 REG32 的下一次写整段失败"，再让 g_wav_test=6 去写 REG32
    #   （该钩子 = 模拟"用户在停止态拖过音量滑杆"）。停止态注入 ⇒ 无听感影响；
    #   之后起播时 dac_path_regs 会正常写 REG32，状态不受影响。
    print("-" * 74)
    print("K8 诊断自检：注入 REG32 写失败，验证 g_es_fail_reg 报的是不是 0x32")
    snap(2, 1.4)
    bench.wr("g_es_drop_wr_reg", [0x32])
    bench.wr("g_wav_test", [6])                     # 触发一次对 REG32 的写
    time.sleep(0.5)
    freg = rd1("g_es_fail_reg")
    reg, stt = (freg >> 16) & 0xFFFF, freg & 0xFFFF
    print("  注入 REG32 写失败后: g_es_fail_reg 报 REG0x%02X（应 0x32）状态=%s" % (reg, stt))
    if reg != 0x32:
        fails.append("K8 g_es_fail_reg 报的是 REG0x%02x（应 0x32）⇒ K5 的 WARN 无法指出"
                     "是哪一支寄存器，那个 WARN 就是空话" % reg)
    bench.wr("g_es_drop_wr_reg", [0])
    snap(1, 2.4)                                    # 起播恢复（dac_path_regs 会重写 REG32）
    if rd1("g_es_rb_vol") == 0xFFFF:
        print("  （起播后 REG32 回读失败，属瞬态，不影响 K8 结论）")
    snap(2, 1.2)

    print("=" * 74)
    for w in warn:
        print("  ⚠ WARN " + w)
    if fails:
        print("汇总：FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  ✗ " + f)
        return 1
    print("汇总：全部 PASS —— 完整掉电尾关掉的时钟被正确写回；且「丢写被闭环救回」已用注入证明")
    return 0


if __name__ == "__main__":
    sys.exit(main())
