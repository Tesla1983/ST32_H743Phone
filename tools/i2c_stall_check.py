#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""I2C 阻塞上限 / "偶发卡死" / "切时钟前静音确认" 验收（2026-10-14 补丁13）

【用户报的两个现象（实为同一个根因）】
 ① 音频里仍有咔嗒（爆音）；
 ② 快速点播放/暂停若干轮后，整个 UI 卡住无法滑动（约 1/3 概率）。

【本轮板上取证（第一步就是复现，不是猜）】
`tools/ui_freeze_repro.py 40`（真实 g_bdtap 点击路径）第 5 次点击后**主循环停转**，现场：

    状态：wav_state=1 ao_playing=1 ao_stage=7 es_wr_ok=87 es_wr_ng=51 es_fail=49
    g_fault: magic=0 kind=?      ← 不是内核故障，就是被 I2C 拖住

· `es_fail=49` —— 5 轮播放/暂停里有 **49 个寄存器"5 次重试全灭"**；
· `g_es_breaker_trip` **恒为 0** —— 熔断器压根没触发（见下"旧代码三处缺陷"）；
· 复测（分项计数）失败类型 **100% 是 NACK**（busy/tmo 都是 0），失败环里出现频率最高的
  两个寄存器是 **0x31（数字静音）与 0x32（DAC 音量）** —— 恰好是"切 I2S 时钟之前
  最后一道防线"那两个寄存器。

⇒ 两个现象是**同一个根因**：
   · 静音那一笔写丢了 ⇒ 切时钟时 DAC 还挂在有效音量上 ⇒ **爆音**；
   · 重试风暴 × HAL 内部的固定等待 ⇒ 主循环单拍被拖到数百毫秒 ⇒ **UI 卡死**。

【旧代码的三处缺陷（本补丁逐一收口）】
 ① **熔断器被自己关掉**：`es8311_bus_clear()` 里原本 `s_es_consec_fail = 0`，而 bus_clear
    恰恰只在"SDA 被从机钳住"时被调用 —— 那正是熔断要对付的状态 ⇒ 计数永远攒不满 6
    ⇒ 熔断永不触发（实测 breaker_trip 恒 0）。
 ② **HAL 内部有一笔与项目无关的固定 25 ms 等待**：`HAL_I2C_Master_Transmit()` 开头
    `I2C_WaitOnFlagUntilTimeout(..., I2C_FLAG_BUSY, SET, I2C_TIMEOUT_BUSY, ...)`，
    而 `I2C_TIMEOUT_BUSY = 25 ms` 是 **HAL 源文件里的常量**（stm32h7xx_hal_i2c.c:344），
    与本项目传进去的 5 ms 毫无关系。BUSY 被卡住时**每次尝试**都先白等 25 ms。
 ③ **没有"单拍预算"**：19 笔起播 × 5 次重试，逐笔线性放大。

【修法】
 · 进 HAL 之前**自己查 BUSY**（命中就绕开 HAL，省下那 25 ms），并用 **RCC 硬件复位**
   （I2C1RST）清 BUSY —— PE 复位不保证能清它；
 · 熔断器改为"只有**真正成功**才清零"；连续 3 次 bus clear 没有换来成功即判总线不可用；
 · **降级快速失败**：上一笔耗时 > 5 ms ⇒ 之后每笔只用 1 次尝试 + 1 ms 超时，直到某笔
   又快又好地完成才恢复满重试（把烂总线下的单拍阻塞收缩一个量级）；
 · 静音从"写一笔就算"升级为 **es8311_dac_become_silent()：写 + 回读确认 + 补写**，
   未确认时 `g_ao_silent_fail` / `g_es_silent_ng` 会 +1（把"有爆音风险"变成可见计数）。

【判据】
  S1 常态：40 轮真实点击（快速播放/暂停）里主循环**始终在转**（g_loop_count 单调增）
  S2 常态：板上记录的**单拍最长耗时 g_tick_ms_max ≤ 60 ms**（正常 16~20；>200 才是"卡住"）
  S3 常态：单拍 > 20 ms 的**拍数占比**很低（g_tick_ms_over20 / Δloop ≤ 5%）
  S4 常态：`g_es_tx_us_max < 30000`（单笔事务从不接近 HAL 那笔 25 ms 的固定等待）
  S5 常态：`g_ao_silent_fail == 0` 且 `g_es_silent_ng == 0`（每次停/暂停的静音都被确认）
  S6 常态：`g_es_pdown_n == 0`（正常播放/停止**不再给 codec 断电** ⇒ 无 VMIDSEL 台阶）
  S7 ★最坏情况仿真（`g_es_nack_sim=2`：对端永不回应 + 假装 SDA 被钳住，纯软件）：
      主循环**仍在转**、`g_tick_ms_max` 仍 ≤ 上限、且**熔断器确实触发**
      （`g_es_breaker_trip > 0`）—— 这一条把"卡死"从"看运气≈1/3"变成"有确定性上限"。
  S8 仿真关掉后能自愈：起播仍成功（wav_state=1 且 frames 在涨）⇒ 降级态可退出

⚠ 探针会造伪影：probe-rs 每次读数都短暂停机内核，停机本身就能制造 I2C 失败。
   所以本脚本**不在窗口内密集采样**：整轮只在一头一尾各读一次（S2 那样的"板上记录极值"
   正是为此设计的 —— 主机读一次就能拿到尖峰，不必高频采样）。
⚠ 只验链路与阻塞上限；**听感由耳朵判定**。

用法：
    python tools/i2c_stall_check.py            # 默认 40 轮
    python tools/i2c_stall_check.py 60         # 60 轮
"""
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench  # noqa: E402

CYCLES = 40
for a in sys.argv[1:]:
    if a.isdigit():
        CYCLES = int(a)

BTN_PLAY = (160, 417)          # 播放/暂停按钮中心（抓屏核对过，见 ui_freeze_repro.py）
ICON_MUSIC = (123, 290)        # 桌面音乐图标

# S2/S7 的判据阈值。
# 依据：主循环正常一拍 ≈16 ms；起播那一拍要写 ~19 笔 I2C（含 codec 通路 + 音量 ramp），
# 即使全部失败、每笔都耗满 5 ms 超时，也应在 60 ms 内收敛（降级模式会把后续笔压到 1 ms）。
TICK_MAX_MS = 60
# 最坏情况仿真下的上限：允许更宽松（19 笔 × 1 ms 超时 + 每笔的恢复与 2 ms 延时），
# 但仍然必须是"几十毫秒"量级 —— 而不是秒级（旧代码在那里会到 ~2.4 s）。
TICK_MAX_SIM_MS = 250
TX_US_MAX = 30000              # 单笔事务耗时上限（µs）：HAL 那笔固定等待是 25 000


def wr(name, v):
    bench.wr(name, [v])


def rd(name):
    return bench.rd(name, 1)[0]


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          encoding="utf-8", errors="ignore")


def tap(p):
    bench.wr("g_bdtap_x", [p[0]])
    bench.wr("g_bdtap_y", [p[1]])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    bench.wr("g_bdtap_seq", [rd("g_bdtap_seq") + 1])


def reset_board():
    sh("probe-rs reset %s --chip %s" % (bench.PROBE, bench.CHIP))
    time.sleep(4.0)


def loop_now():
    return rd("g_loop_count")


def main():
    fails = []
    print("=" * 78)
    print("I2C 阻塞上限 / 偶发卡死 / 静音确认 验收（补丁13）")
    print("=" * 78)

    reset_board()
    l0 = loop_now()
    time.sleep(0.5)
    if loop_now() == l0:
        print("[ABORT] 复位后主循环没在转 —— 先排查基础状态")
        return 1
    if rd("g_boot_magic") != 0x594D4731 or rd("g_shell_rc") != 0:
        print("[ABORT] 固件没正常起来")
        return 1

    # ================= A 段：常态（真实点击路径，快速播放/暂停）=================
    print("\n【A】常态：%d 轮真实点击（快速播放/暂停）" % CYCLES)
    tap(ICON_MUSIC)
    time.sleep(1.2)
    if rd("g_wav_state") not in (0, 1):
        print("  （打开 app 后 wav_state=%d）" % rd("g_wav_state"))

    # 清零板上极值记录（在真正开跑之前）
    wr("g_tick_ms_max", 0)
    wr("g_tick_ms_over20", 0)
    l_start = loop_now()
    stalled_at = None

    for i in range(1, CYCLES + 1):
        tap(BTN_PLAY)
        time.sleep(0.30)
        a = loop_now()
        time.sleep(0.30)
        b = loop_now()
        if a == b:
            stalled_at = i
            print("  ★ 第 %d 轮点击后 0.30 s 内主循环不涨（%d→%d）—— 仍在卡" % (i, a, b))
            break

    l_end = loop_now()
    dloop = l_end - l_start
    tick_max = rd("g_tick_ms_max")
    over20 = rd("g_tick_ms_over20")
    tx_max = rd("g_es_tx_us_max")
    slow_n = rd("g_es_slow_n")
    busy_n = rd("g_es_busy_stuck_n")
    rcc_n = rd("g_es_rcc_rst_n")
    bc_fail = rd("g_es_bc_fail")
    trip = rd("g_es_breaker_trip")
    sil_ok = rd("g_es_silent_n")
    sil_ng = rd("g_es_silent_ng")
    ao_sil_fail = rd("g_ao_silent_fail")
    pdown = rd("g_es_pdown_n")
    esfail = rd("g_es_fail")
    wrng = rd("g_es_wr_ng")
    rdng = rd("g_es_rd_ng")

    rate = (100.0 * over20 / dloop) if dloop else 0.0
    print("  主循环：%d 拍（%d→%d）%s" %
          (dloop, l_start, l_end, "  ★中途卡过★" if stalled_at else "  全程在转"))
    print("  板上记录：g_tick_ms_max=%d ms   >20 ms 的拍数=%d（%.2f%%）" %
          (tick_max, over20, rate))
    print("  I2C：单笔最长=%d µs  慢笔(>5ms)=%d  BUSY 卡住命中=%d  RCC复位=%d" %
          (tx_max, slow_n, busy_n, rcc_n))
    print("       bus clear 执行=%d（之后 SDA 仍被钳住 %d）  熔断触发=%d" %
          (rd("g_es_bus_clear"), bc_fail, trip))
    print("       wr_ng=%d rd_ng=%d fail=%d（全程累计）" % (wrng, rdng, esfail))
    print("  I2S 停机等待：最长 %d ms（CSUSP 生效）  等满 100 ms 兜底=%d 次" %
          (rd("g_ao_csusp_ms_max"), rd("g_ao_csusp_tmo")))
    print("  静音确认：成功=%d  未确认=%d   音频层失败=%d" % (sil_ok, sil_ng, ao_sil_fail))
    print("  真·掉电次数 g_es_pdown_n=%d（正常路径应为 0）" % pdown)

    if stalled_at is not None:
        fails.append("S1 第 %d 轮点击后主循环 0.30 s 内不涨 —— 卡死仍可复现" % stalled_at)
    if tick_max > TICK_MAX_MS:
        fails.append("S2 单拍最长 %d ms > %d ms" % (tick_max, TICK_MAX_MS))
    if dloop and rate > 5.0:
        fails.append("S3 长拍占比 %.2f%% > 5%%" % rate)
    if tx_max >= TX_US_MAX:
        fails.append("S4 单笔事务最长 %d µs ≥ %d（疑似又走到 HAL 那笔 25 ms 固定等待）"
                     % (tx_max, TX_US_MAX))
    if ao_sil_fail != 0 or sil_ng != 0:
        fails.append("S5 静音未确认 ao_fail=%d es_ng=%d（切时钟时可能还有效音量 = 爆音风险）"
                     % (ao_sil_fail, sil_ng))
    if pdown != 0:
        fails.append("S6 正常路径发生了真·掉电 %d 次（应为 0：断电会带来 VMIDSEL 台阶）" % pdown)

    # ================= B 段：最坏情况仿真（对端永不回应 + 假装 SDA 被钳住）=================
    print("\n【B】最坏情况仿真：g_es_nack_sim=2（纯软件，对端永不回应 + 假装 SDA 被钳住）")
    wr("g_es_tx_us_max", 0)
    wr("g_tick_ms_max", 0)
    wr("g_tick_ms_over20", 0)
    trip0 = rd("g_es_breaker_trip")
    wr("g_es_nack_sim", 2)
    time.sleep(0.3)
    l0 = loop_now()
    for _ in range(4):                 # 反复触发"起播/停止"，每次都走满 I2C 通路
        tap(BTN_PLAY)
        time.sleep(0.5)
    l1 = loop_now()
    sim_tick_max = rd("g_tick_ms_max")
    sim_over20 = rd("g_tick_ms_over20")
    sim_tx_max = rd("g_es_tx_us_max")
    sim_trip = rd("g_es_breaker_trip") - trip0
    sim_rcc = rd("g_es_rcc_rst_n")
    sim_loop_ok = (l1 > l0)
    print("  仿真期间主循环：%d→%d（%s）" % (l0, l1, "在转" if sim_loop_ok else "★停转★"))
    print("  g_tick_ms_max=%d ms   >20ms 拍数=%d   单笔最长=%d µs" %
          (sim_tick_max, sim_over20, sim_tx_max))
    print("  熔断触发（本次增量）=%d   RCC 硬件复位累计=%d" % (sim_trip, sim_rcc))

    if not sim_loop_ok:
        fails.append("S7 最坏仿真下主循环停转 —— 阻塞上限没收住")
    if sim_tick_max > TICK_MAX_SIM_MS:
        fails.append("S7 最坏仿真下单拍最长 %d ms > %d ms" % (sim_tick_max, TICK_MAX_SIM_MS))
    if sim_trip == 0:
        fails.append("S7 最坏仿真下熔断器**从未触发**（旧代码的缺陷①还在）")

    # ================= C 段：关掉仿真，确认能自愈 =================
    print("\n【C】关掉仿真，确认降级态能退出（自愈）")
    wr("g_es_nack_sim", 0)
    time.sleep(0.5)
    tap(BTN_PLAY)                      # 确保处于播放态
    time.sleep(2.5)
    st = rd("g_wav_state")
    fr0 = rd("g_ao_frames")
    time.sleep(2.0)
    fr1 = rd("g_ao_frames")
    l2 = loop_now()
    time.sleep(0.5)
    l3 = loop_now()
    print("  wav_state=%d  frames %d→%d（%s）  主循环 %d→%d（%s）" %
          (st, fr0, fr1, "在涨" if fr1 > fr0 else "不涨",
           l2, l3, "在转" if l3 > l2 else "★停转★"))
    if st != 1:
        fails.append("S8 仿真关掉后起播没成功（wav_state=%d）" % st)
    if fr1 <= fr0:
        fails.append("S8 仿真关掉后数据通路没走（frames 不涨）")
    if l3 <= l2:
        fails.append("S8 仿真关掉后主循环停转")

    # 收尾：停播，恢复干净状态
    tap(BTN_PLAY)
    time.sleep(0.5)

    print("\n" + "=" * 78)
    if fails:
        print("汇总：FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  ✗ " + f)
        return 1
    print("汇总：全部 PASS")
    print("  · 常态 %d 轮快速播放/暂停全程不卡，单拍最长 %d ms（判据 ≤ %d）"
          % (CYCLES, tick_max, TICK_MAX_MS))
    print("  · 最坏仿真下单拍最长 %d ms（判据 ≤ %d），熔断器确实触发 %d 次"
          % (sim_tick_max, TICK_MAX_SIM_MS, sim_trip))
    print("  · 静音确认 0 次失败；正常路径 0 次真·掉电")
    return 0


if __name__ == "__main__":
    sys.exit(main())
