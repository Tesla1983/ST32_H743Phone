#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""补丁15 取证：把"咔"量成数字，并在 TIM6 / DMA 两条路径上对照（2026-10-14）

【为什么又要一轮取证】前几轮修"咔"用的都是**间接代理量**：
    g_ao_under_short（有没有填硬零）/ g_ao_silent_*（静音确认成没成）/
    g_ao_dma_gap_max（DMA 停顿多长）—— 每个只覆盖一类成因，而用户复听仍报"咔"。
本轮改为**直接量数据出口**（见 src/audio_out.c 补丁15 注释）：

    g_ao_seam_max        DMA：本次半缓冲首样本 − 上次半缓冲末样本（跨块接缝最大跳变）
    g_ao_seam_n          上面 > 30000 的次数
    g_ao_tz_run_max      TIM6：送进 TXDR 的最长连续零（半字）
    g_ao_feed_short_max  feeder 单次短读的最大缺口（帧）
                          ⚠ 1024/2048 意味着"整块被合成成零"，旧代码会直接播出 21~42 ms 硬零

【三个互斥假设，本脚本用来区分它们】
    H1 数据出口合成零块（pump_frames 的 memset / dma_fill_half 的硬零）
       ⇒ g_ao_under_short 或 g_ao_feed_short_max 出现"整块"级别
    H2 半缓冲接缝被破坏（环被覆盖 / 数据被跳过）
       ⇒ g_ao_seam_n 在稳态播放中增长
    H3 模拟/寄存器时序（数字静音硬切、软 ramp 未走完就掐钟）
       ⇒ 上面三个量都干净，但用户仍听到咔

【判读铁律（本项目既有）】SWD 探针每次读写都会**停机内核**，停机 > 42 ms 主循环就来不及
补数 ⇒ 环被抽干 ⇒ 我自己的读数就能造出欠载。所以：**窗内一次都不读**，只在相位边界
做一次批读（g_ao_dbg[24] + g_ao_dbg3[16]，两次读 ≈ 1 s，其间不再做别的操作）。

用法
----
    python tools/click_source_probe.py            # 两条路径各跑一遍完整动作序列
    python tools/click_source_probe.py --dma 1    # 只跑 DMA
    python tools/click_source_probe.py --dma 0    # 只跑 TIM6
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

# g_ao_dbg[] 索引（见 src/audio_out.c 的 dbg_snapshot 索引表）
D_UNDERRUN, D_UNDER_DMA, D_UNDER_FEED, D_UNDER_SHORT = 16, 17, 18, 19
D_UNDER_FIRST, D_PUMP1, D_DACWAKE, D_RING_MIN = 20, 21, 22, 23

# g_ao_dbg3[] 索引（补丁15）
SEAM_MAX, SEAM_N, TZ_RUN_MAX, FEED_SHORT_MAX = 0, 1, 2, 3
STOP_CALLS, STOP_DONE = 7, 8

EXTRA = ("g_ao_use_dma", "g_ao_udr", "g_ao_dma_half", "g_ao_dma_cplt",
         "g_wav_underrun", "g_wav_read_ms", "g_es_silent_n", "g_es_silent_ng",
         "g_ao_silent_fail", "g_ao_frames", "g_ao_pause_calls", "g_ao_resume_calls",
         # 补丁15b：起播时序表 + ISR 计数（用来算"平均帧/中断"，也用来盯"停→立刻起播"
         # 这条幂等路径有没有把上一段的帧/时间原点带进来）
         "g_ao_isr", "g_ao_t_cstart_ms", "g_ao_t_pump1_ms",
         "g_ao_t_dacwake_ms", "g_ao_t_ramp_ms")


def snap():
    """一个相位的全部读数：2 次批读 + 一批单符号读。"""
    d = bench.rd("g_ao_dbg", 24)
    d3 = bench.rd("g_ao_dbg3", 16)
    s = {}
    for k in EXTRA:
        if k in bench.T:
            s[k] = bench.rd(k, 1)[0]
    return d, d3, s


def show(tag, d, d3, s):
    print("  [%s] under_dma=%d under_feed=%d under_short=%d 首个欠载@%s ms  ring_min=%s"
          % (tag, d[D_UNDER_DMA], d[D_UNDER_FEED], d[D_UNDER_SHORT],
             ("-" if d[D_UNDER_FIRST] == 0 else str(d[D_UNDER_FIRST])),
             ("-" if d[D_RING_MIN] == 0xFFFFFFFF else str(d[D_RING_MIN]))))
    print("        seam_max=%d seam_n=%d tz_run_max=%d feed_short_max=%d"
          % (d3[SEAM_MAX], d3[SEAM_N], d3[TZ_RUN_MAX], d3[FEED_SHORT_MAX]))
    print("        frames=%d udr=%d dma_half=%d dma_cplt=%d wav_underrun=%d read_ms=%d"
          % (s.get("g_ao_frames", -1), s.get("g_ao_udr", -1), s.get("g_ao_dma_half", -1),
             s.get("g_ao_dma_cplt", -1), s.get("g_wav_underrun", -1), s.get("g_wav_read_ms", -1)))
    print("        es_silent_n=%d es_silent_ng=%d ao_silent_fail=%d pause=%d resume=%d"
          % (s.get("g_es_silent_n", -1), s.get("g_es_silent_ng", -1),
             s.get("g_ao_silent_fail", -1), s.get("g_ao_pause_calls", -1),
             s.get("g_ao_resume_calls", -1)))
    # ★ 补丁15b：起播时序表 + 平均帧/中断。
    #   判据：ramp/dacwake/pump1 应当是"百毫秒以内"，若出现几万（≈开机时长）说明
    #   诊断量的时间原点（g_ao_t_cstart_ms）没被这次起播重新标定 —— 补丁15b 已修，
    #   本行就是它的回归哨兵。平均帧/中断在 DMA 路径应 ≈1024（2048 半字 / 2）。
    isr = s.get("g_ao_isr", 0)
    fr  = s.get("g_ao_frames", 0)
    avg = ("-" if isr == 0 else "%.1f" % (fr / isr))
    print("        时序: pump1=%s dacwake=%s ramp=%s ms  |  帧=%d 中断=%d 帧/中断=%s"
          % (s.get("g_ao_t_pump1_ms", -1), s.get("g_ao_t_dacwake_ms", -1),
             s.get("g_ao_t_ramp_ms", -1), fr, isr, avg))
    print("        停: 发起=%d 收尾完成=%d（补丁15：两者应相等 ⇒ 淡出没被卡住）"
          % (d3[STOP_CALLS], d3[STOP_DONE]))


def verdict(tag, d, d3):
    """返回 (是否干净, 理由列表)。

    ★★ 补丁15 起判据口径变更（有据，不是放宽）★★
      · `under_short > 0` **不再是"会听到咔"的判据**：补丁15 已把 dma_fill_half 的缺口
        从"填硬 0"改成"**冻结最后一个有效样本 + 缺口后 ramp-in 接回真实数据**"
        （15c 之前是"线性衰减到 0"，但衰减只是把阶跃推到了缺口尾部 ⇒ 15c 换成冻结）⇒
        缺口本身**不再产生阶跃**（两端都 C0 连续）。
        现在真正直接量"阶跃"的是 `seam_max / seam_n`。
      · `tz_run_max` 只作参考信息：TIM6 是逐半字流，缺数据时 ISR 什么都不写
        （H7 的 I2S 会重复上一采样 ⇒ 电平不变 ⇒ 无阶跃），所以它**不可能**合成零块；
        实测 11172 个连续零 = **WAV 曲目本身的开头静音**（116 ms），不是故障。
      · `feed_short_max` 只在**稳态相位**里才算异常：在"停止"相位里它等于 2048，那是
        **读到文件尾**（feeder_cb 里 avail==0 直接 return 0 ⇒ miss = 2048 帧），属正常
        收尾；补丁15 起这条路径也不再合成零块（只把真正读到的放进环）。
    """
    bad = []
    steady = ("起播" in tag) or ("续播" in tag) or ("静窗" in tag) or ("连点" in tag)
    if d3[SEAM_N] > 0:
        bad.append("★ H2 跨半缓冲接缝出现 %d 次 >30000 的人工断口（seam_max=%d）"
                   % (d3[SEAM_N], d3[SEAM_MAX]))
    if steady and d3[FEED_SHORT_MAX] >= 512:
        bad.append("H1 稳态相位 feeder 短读 %d 帧（播放中读到尾/短读 ⇒ 环会饿）"
                   % d3[FEED_SHORT_MAX])
    return (len(bad) == 0), bad


def run(use_dma, secs):
    bench.wr("g_ao_use_dma", [use_dma])
    time.sleep(0.3)
    mode = "DMA" if use_dma else "TIM6"
    print("\n" + "=" * 78)
    print("【%s 路径】g_ao_use_dma=%d" % (mode, use_dma))
    print("=" * 78)

    # 干净起点：先停、复位计数（g_wav_test=2 = 停播）
    bench.wr("g_wav_test", [2])
    time.sleep(0.6)

    phases = []

    # ① 起播
    bench.wr("g_wav_test", [1])
    t0 = time.time()
    time.sleep(secs)
    phases.append(("① 起播+稳态%ds" % secs, snap()))

    # ①b 静窗基线：3 s 内**一次都不读**（不制造停机伪影）⇒ 这一段若 under_short 不涨，
    #     说明"稳态播放本身"不产生缺口；后面相位里涨的那些就都来自"动作/探针干扰"。
    time.sleep(3.0)
    phases.append(("①b 静窗3s(零读取)", snap()))

    # ② 暂停（动作后只等 0.8 s 就快照，抓动作瞬态）
    bench.wr("g_wav_test", [3])
    time.sleep(0.8)
    phases.append(("② 暂停后 0.8s", snap()))

    # ③ 续播
    bench.wr("g_wav_test", [4])
    time.sleep(secs)
    phases.append(("③ 续播+稳态%ds" % secs, snap()))

    # ④ 停止
    bench.wr("g_wav_test", [2])
    time.sleep(0.8)
    phases.append(("④ 停止后 0.8s", snap()))

    # ⑤ 再起播（验"停过一次之后还能不能干净地起"）
    bench.wr("g_wav_test", [1])
    time.sleep(secs)
    phases.append(("⑤ 再起播+稳态%ds" % secs, snap()))

    # ⑥ 连点：播放→暂停→播放→暂停（模拟用户快速连点）
    for _ in range(4):
        bench.wr("g_wav_test", [3])
        time.sleep(0.5)
        bench.wr("g_wav_test", [4])
        time.sleep(0.5)
    time.sleep(1.5)
    phases.append(("⑥ 连点 4×(暂停0.5s→续播0.5s)", snap()))

    bench.wr("g_wav_test", [2])
    time.sleep(0.5)

    for tag, (d, d3, s) in phases:
        show(tag, d, d3, s)

    print("\n  ---- 逐相位判读（只看该相位是否有新异常）----")
    ok_all = True
    for tag, (d, d3, s) in phases:
        ok, bad = verdict(tag, d, d3)
        if ok:
            print("    %-28s 干净" % tag)
        else:
            ok_all = False
            for b in bad:
                print("    %-28s ⚠ %s" % (tag, b))

    # 紧凑总表：跨相位看 seam_max（**这是"咔"最直接的量度**：有无人工断口/阶跃）
    print("\n  ---- 紧凑总表（seam_max=跨半缓冲接缝最大跳变；32767=满幅）----")
    print("    %-28s %8s %6s %8s %8s %8s" % ("相位", "seam_max", "seam_n",
                                              "under_sh", "feed_sh", "ring_min"))
    for tag, (d, d3, s) in phases:
        print("    %-28s %8d %6d %8d %8d %8s"
              % (tag, d3[SEAM_MAX], d3[SEAM_N], d[D_UNDER_SHORT], d3[FEED_SHORT_MAX],
                 ("-" if d[D_RING_MIN] == 0xFFFFFFFF else str(d[D_RING_MIN]))))
    print("  ⇒ %s 路径：%s" % (mode, "全部相位干净" if ok_all else "存在异常（见上）"))
    return ok_all


def main():
    if bench.rd("g_boot_magic", 1)[0] != 0x594D4731:
        print("固件没在跑")
        return 1
    if "g_ao_dbg3" not in bench.T:
        print("本固件没有补丁15 的诊断量（g_ao_dbg3）—— 请先烧补丁15 的构建。")
        return 1

    secs = 6.0
    if "--secs" in sys.argv:
        secs = float(sys.argv[sys.argv.index("--secs") + 1])
    want = None
    if "--dma" in sys.argv:
        want = int(sys.argv[sys.argv.index("--dma") + 1])

    r = {}
    for m in ([1, 0] if want is None else [want]):
        r[m] = run(m, secs)

    print("\n" + "=" * 78)
    for m in r:
        print("  %-4s 路径：%s" % ("DMA" if m else "TIM6", "干净" if r[m] else "有异常"))
    print("=" * 78)
    # 对照结论：只有当"两条都干净"或"一条干净一条不干净"时才是有信息量的结果
    if 1 in r and 0 in r:
        if r[0] and not r[1]:
            print("  ⇒ 不对称成立：TIM6 干净、DMA 不干净（与用户听感一致）")
        elif r[1] and r[0]:
            print("  ⇒ 两条都干净：本动作序列下取证不到异常（咔来自更细的窗口或模拟域）")
        elif r[1] and not r[0]:
            print("  ⇒ 反过来了：DMA 干净、TIM6 不干净")
        else:
            print("  ⇒ 两条都有异常 ⇒ 成因在两条路径共用的部分（feeder / pump / es8311 时序）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
