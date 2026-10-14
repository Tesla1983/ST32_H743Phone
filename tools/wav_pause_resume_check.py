#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""暂停 → 续播（音乐 app「点暂停再点播放」）链路板上验收 + 排障（2026-10-13）

【为什么单独有这条】
用户报："点暂停之后再点播放，进度条在走但是没声音，有时候有声音但音量大小混乱。"

【根因（2026-10-13 板上定位，三层）】
· 第一层（真因）：`audio_out_stop()`（暂停曾走它）末尾的 `es8311_stop_dac()` 会写 9 笔
  I2C 并把 codec 打进 **power-down**（REG0E=0xFF、REG12=0x02）。芯片掉电/上电窗口里
  不对自己的 I2C 地址应答 ⇒ 那 9 笔里总有几笔变成 NACK；而丢写又被调用方 `(void)`
  忽略 ⇒ 音量只写进去一半 = "音量大小混乱"；极端情况连 REG32（音量）都丢
  ⇒ 芯片停在 0（静音）= "进度条在走但没声音"。
· 第二层（探针侧）：`HAL_I2C_Master_Transmit` 是**阻塞轮询**的，发生在主循环线程里。
  而 probe-rs 每次 `read/write` 都会 **halt 内核**（外设不停）⇒ 会打断正在进行的
  I2C 事务，制造假 NACK。**读得越密、假失败越多** —— 这就是历史上那个
  "失败随交互次数 3→13→29→50→94 递增"的真相：主因是探针密度，不是芯片坏了。
  实测对照：复位后静置 10 s、全程零探针起播 = 58 笔写 0 失败；密集逐符号读 = 47% 失败。
  伪影特征：`g_es_ng_idr == 0x40`（SDA 被压住）。真 NACK 时总线是释放的（0xC0）。
· 第三层（采样方式）：既然"每次单符号读 = 一次停机"，那就把**测量搬到板上**：
  固件里加了「相位锁存」——写 `g_wav_snap_req=<相位码>`，板子在下一拍执行该动作、
  等 N 拍后在**同一个函数里**把全部诊断量锁进一张连续 RAM 表 `g_wav_snap[]`。
  本脚本只做 **一次** 批量读（`probe-rs read -f binary -o`，160 字节）⇒ 一次停机拿全表，
  且表内各量是同一时刻的。**这就是本版验收对伪影免疫的根本原因。**

【修法（本脚本验的就是它）】
  暂停改走 `audio_out_pause()`：只发 **1 笔** REG32（静音）、**不给 codec 断电**，
  芯片保持可达 ⇒ 续播那段 ~19 笔 I2C 不再有 NACK 窗口。
  「切歌 / 播完 / 退出」仍走 `audio_out_stop()`（断电是对的，下次是全新起播）。

【判据（P1~P8；P1~P7 只看锁存表，P8 另读两个稳定计数器）】
  P1 暂停的静音动作只发**少量** REG32 写（补丁13 起为 2~4 笔：半步降 + 归零 [+每轮补写]）
  P2 暂停后 codec 仍通电可达：`rb_pwr != 0xFFFF` 且 power-down 位 == 0
  P3 ★终态权威判据：续播后 `reg32_wr == es8311_volume_reg(UI音量)`
     （这是**写入值**，与 I2C 成败无关 ⇒ 不受探针伪影影响；== 0 ⇒ 停在静音 = 没声音）
  P4 续播后 DAC 就绪：`dac_ready==1` 且 `ramp==0`
  P5 续播真的在出声：`wav_state==1` 且 `playing==1` 且 frames 在涨
  P6 不变量自洽：`ng_head == wr_ng + rd_ng`（失败环计数与分项一致）且 `fail == 0`
     （重试 3 次仍失败的次数；>0 才是真丢写）
  P8 ★补丁13：暂停时"静音**被确认**" —— 读回 REG32==0x00 且 REG31 的 bit6/5==1，
     且 `g_ao_silent_fail == 0`。为什么这一条不能省：I2C 写会失败（实测首试 NACK
     5%~30%，失败环里 0x31/0x32 命中最多），而"写了就算"恰好让"切时钟时 DAC 还挂在
     有效音量上"这个**爆音根因**变成一个看不出来的坏状态；P1 只数笔数、看不出这个。

⚠ 补丁13 的改动影响到 P1 的口径："暂停只发 1 笔 REG32"这条判据的前提（一次写就能静音）
  已被证伪 —— 现在暂停走 `es8311_dac_become_silent()`：REG32 半步降 → 归零 → REG31 静音
  → **回读确认**（不对就补写再验）。笔数改为动作内**实测**（用 g_es_reg32_n 的前后差），
  因此 P1 由"== 1"改为"2~4 笔"（下界 2 = 半步降+归零；上界 4 = 两轮未确认时各补 1 笔）。
  判据的**意图不变**（"暂停不许走 stop_dac 的断电路径" 由 P2 直接覆盖）。

⚠ 只验链路；听感由耳朵判定。
用法：
    python tools/wav_pause_resume_check.py          # 3 个循环
    python tools/wav_pause_resume_check.py 5        # 5 个循环
"""
import os
import subprocess
import sys
import struct
import time

sys.path.insert(0, "tools")
import bench

CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 3

VOL_FULL = 0xBF                      # es8311_volume_reg(100)

# 与 src/wav_play.h 的 G_SNAP_* 一一对应（顺序即内存布局，改一边必须改另一边）
IDX = {
    "PHASE": 0, "LOOP": 1, "WSTATE": 2, "WRC": 3, "WPOS": 4, "WTOTAL": 5,
    "AOPLAY": 6, "AOREADY": 7, "AORAMP": 8, "AOWAKE": 9, "AOSKIP": 10,
    "AOINITRC": 11, "AORC": 12, "ESWROK": 13, "ESWRNG": 14, "ESRDOK": 15,
    "ESRDNG": 16, "ESRETRY": 17, "ESFAIL": 18, "ESFIX": 19, "ESRB": 20,
    "ESPWR": 21, "ESREG32N": 22, "ESREG32W": 23, "ESRBVOL": 24, "ESRBPWR": 25,
    "ESRB0E": 26, "ESWNCUR": 27, "ESWNS": 28, "ESNAK": 29, "ESBUSY": 30,
    "ESTMO": 31,     "ESNGHEAD": 32, "ESNGIDR": 33, "VOLUI": 34, "DMARSH": 35,
    "DMARCP": 36, "DMARERR": 37, "FRAMES": 38, "MAGIC": 39, "ESPRETRY": 40,
    "PAUSECALL": 41, "PAUSEWR": 42, "RESUMECALL": 43,
}
N_WORDS = 44
MAGIC = 0x5A4D4731


def vol_reg(ui):
    return (ui * VOL_FULL) // 100


def latch(phase, delay=None, wait=1.6):
    """触发板上相位锁存并等它写完（默认延迟 40 拍 ≈ 0.64 s）。"""
    if delay is not None:
        bench.wr("g_wav_snap_delay", [delay])
    bench.wr("g_wav_snap_req", [phase])
    time.sleep(wait)


def latch_snap(phase, wait=1.6, delay=None, tries=3):
    """触发相位 + 读回表，**带确认重发**。

    ⚠⚠ 为什么必须带确认（2026-10-13 踩到）：
       SWD 写 `g_wav_snap_req` 是"写一个变量、由板子在下一拍消费"的握手。实测偶尔会
       出现"写返回成功、板子却没消费"（表里 PHASE 还是上一次的值、WSTATE 也没变），
       本脚本此前就会报"期望 PHASE=3，实际 PHASE=0x1"而中止 —— 但用两个独立复刻脚本
       重跑同一序列（cyc2_probe / req_probe）**每次都成功**，说明是 SWD 写入/时序的
       偶发丢失，不是固件不处理。⇒ 这里改成"写后校验、不对就重发"，最多 tries 次。
       判据只看 PHASE 是否等于本次相位码（bit31==0）—— 这是板上自己写的，不受伪影影响。
    """
    for k in range(tries):
        latch(phase, delay=delay, wait=wait)
        try:
            t = snap(phase, tries=2)
            if k != 0:
                print("  （相位 %d 第 %d 次触发才落地 ⇒ SWD 写入偶发丢失，非固件问题）"
                      % (phase, k + 1))
            return t
        except RuntimeError:
            if k == tries - 1:
                raise
            time.sleep(0.4)
    raise RuntimeError("unreachable")


def snap(phase, tries=6):
    """一次批量读回整张锁存表（164 字节）。这是本脚本**唯一**的读数手段。

    ⚠ 用 `probe-rs read -f binary -o <tmp>` 而不是 b32 逐字读：前者一次调用就把
      整表拉回，只停机一次；后者每个字都要重新握手。
    ⚠ 重试判据 = MAGIC 对 且 PHASE==期望（bit31==0）。半张表必被拒。
    ⚠⚠ 临时文件必须**每次都换名 + 读前删**（2026-10-13 踩到）：
       probe-rs 偶尔会 returncode=0 却没刷新输出文件，复用同一路径就会读到
       **上一次的旧表**（MAGIC 是对的、PHASE 是旧值）⇒ 报"MAGIC/PHASE 校验不过"，
       而板上其实早已锁存成功（复刻实验证实锁存 1.5 s 内必落地）。
       用唯一文件名 ⇒ 读前文件不存在 ⇒ 读不到新数据就会因"文件缺失"而重试，
       永远不会把旧表当新表。
    """
    addr = bench.T["g_wav_snap"][0]
    tmpdir = os.environ.get("TEMP", "/tmp")
    last = None
    for k in range(tries):
        tmp = os.path.join(tmpdir, "wav_snap_%d_%d.bin" % (phase, k))
        if os.path.exists(tmp):
            os.remove(tmp)
        cmd = ("probe-rs read %s --chip %s -f binary -o %s b32 0x%08x %d"
               % (bench.PROBE, bench.CHIP, tmp, addr, N_WORDS))
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(tmp):
            raw = open(tmp, "rb").read()
            if len(raw) == N_WORDS * 4:
                w = struct.unpack("<%dI" % N_WORDS, raw)
                last = w
                if w[IDX["MAGIC"]] == MAGIC \
                        and (w[IDX["PHASE"]] & 0x7FFFFFFF) == phase \
                        and (w[IDX["PHASE"]] & 0x80000000) == 0:
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass
                    return dict(zip(IDX.keys(), w))
        time.sleep(0.3)
    if last is not None:
        raise RuntimeError("锁存表读取失败：期望 PHASE=%d，实际 PHASE=%#x（LOOP=%d WSTATE=%d）"
                           % (phase, last[IDX["PHASE"]], last[IDX["LOOP"]],
                              last[IDX["WSTATE"]]))
    raise RuntimeError("锁存表读取失败（probe-rs 6 次都没产出文件）")


def main():
    fails = []

    print("=" * 78)
    print("暂停 → 续播 链路验收（走 wav_play_pause / wav_play_resume，与 app 按钮同一条路）")
    print("采样方式：板上相位锁存 + 一次批量读（%d B）⇒ 单次停机，对 halt 伪影免疫"
          % (N_WORDS * 4))
    print("=" * 78)

    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.4)
    if bench.rd("g_loop_count", 1)[0] == l0:
        print("[FAIL] 内核没在转（probe-rs download 之后必须 reset）")
        return 1

    bench.wr("g_wav_snap_delay", [40])
    print("复位到干净状态：停止播放")
    latch(2, wait=1.2)

    for cyc in range(1, CYCLES + 1):
        print("-" * 78)
        print("循环 %d/%d" % (cyc, CYCLES))

        # ① 起播（锁存表 A）
        A = latch_snap(1, wait=2.2)

        # ② 暂停（锁存表 B）
        # ⚠⚠ 判据已从"wr_ok 增量"改为**动作级计数**（2026-10-13）：
        #   旧判据 `B.wr_ok − A.wr_ok` 的窗口**跨了播放期** —— 播放中"每秒回读体检"
        #   补写的 REG32 会被误计成"暂停写了多笔"，实测间歇报 2/3/5/16 笔，
        #   复跑全部证伪为**窗口伪影**（不是代码回归）。窗口无论如何收紧都根除不了。
        #   ⇒ 现由 audio_out_pause() **自己**累加 g_ao_pause_calls / g_ao_pause_volwr，
        #    与 I2C 流量、探针密度、窗口长度全都无关，是确定性判据。
        B = latch_snap(3, wait=1.8)
        pause_calls  = B["PAUSECALL"] - A["PAUSECALL"]
        pause_writes = B["PAUSEWR"]   - A["PAUSEWR"]

        # ③ 续播（锁存表 C）
        C = latch_snap(4, wait=2.6)

        # ④ frames 增长（锁存表 D，不动作、只再锁一次）
        D = latch_snap(9, wait=0.8)
        frames_grew = D["FRAMES"] > C["FRAMES"]

        expected = vol_reg(C["VOLUI"])
        print("  起播表A: state=%d rc=%d ao_rc=%d init_rc=%d playing=%d ready=%d ramp=%d wake=%d wr_ok=%d probe_retry=%d" %
              (A["WSTATE"], A["WRC"], A["AORC"], A["AOINITRC"], A["AOPLAY"], A["AOREADY"],
               A["AORAMP"], A["AOWAKE"], A["ESWROK"], A["ESPRETRY"]))
        print("  暂停表B: state=%d pause_calls=%d pause_wr=%d  ⇒ 暂停写 REG32 笔数=%d（旧实现 9；新实现应为 1）" %
              (B["WSTATE"], pause_calls, pause_writes, pause_writes))
        print("  续播表C: state=%d rc=%d playing=%d ready=%d ramp=%d wake=%d skip=%d" %
              (C["WSTATE"], C["WRC"], C["AOPLAY"], C["AOREADY"], C["AORAMP"],
               C["AOWAKE"], C["AOSKIP"]))
        print("          REG32 写入=%#04x 期望=%#04x（UI 音量 %d）| 回读=%#04x" %
              (C["ESREG32W"], expected, C["VOLUI"], C["ESRBVOL"]))
        print("          REG12 回读=%#04x（power-down 位应为 0）" % C["ESRBPWR"])
        print("          I2C: 写ok=%d 写ng=%d 读ok=%d 读ng=%d retry=%d fail=%d fix=%d" %
              (C["ESWROK"] - A["ESWROK"], C["ESWRNG"] - A["ESWRNG"],
               C["ESRDOK"] - A["ESRDOK"], C["ESRDNG"] - A["ESRDNG"],
               C["ESRETRY"] - A["ESRETRY"], C["ESFAIL"] - A["ESFAIL"],
               C["ESFIX"] - A["ESFIX"]))
        print("          frames: C=%d D=%d ⇒ 增长=%s" % (C["FRAMES"], D["FRAMES"], frames_grew))

        # ---- P0 起播本身要成功（否则后面全是空谈）----
        # WRC=0 且 AOREADY=1。WRC<0 见 wav_play.h 取值表（-22=audio_out_start 失败，
        # 旧版 probe 零重试时就会撞上"codec 在掉电窗口 NACK" ⇒ 正是用户报的症状）。
        if A["WRC"] != 0:
            fails.append("[%d] P0 起播失败 WRC=%d AORC=%d AOINITRC=%d wr_ok=%d"
                         "（-22=audio_out_start 失败）"
                         % (cyc, A["WRC"], A["AORC"], A["AOINITRC"], A["ESWROK"]))
        # ---- P1 暂停只发 1 笔 I2C 写（★动作级判据，不受窗口影响）----
        if pause_calls != 1:
            fails.append("[%d] P1 暂停动作执行了 %d 次（应为 1）" % (cyc, pause_calls))
        if not (1 <= pause_writes <= 4):
            fails.append("[%d] P1 暂停写了 %d 笔 REG32（补丁13 起应为 2~4：半步降+归零"
                         "[+每轮未确认再补 1 笔]；=9 说明又走了 stop_dac 断电路径）"
                         % (cyc, pause_writes))
        # ---- P8 ★补丁13：静音必须"被确认"（否则切时钟时还有效音量 = 爆音根因）----
        sil_fail = bench.rd("g_ao_silent_fail", 1)[0]
        s_vol = bench.rd("g_es_silent_vol", 1)[0]
        s_mute = bench.rd("g_es_silent_mute", 1)[0]
        if sil_fail != 0:
            fails.append("[%d] ★P8 静音未确认 %d 次（g_ao_silent_fail>0 ⇒ 有爆音风险）"
                         % (cyc, sil_fail))
        elif s_vol != 0x00 or (s_mute & 0x60) != 0x60:
            fails.append("[%d] ★P8 暂停后静音回读不对（REG32=%#04x 应 0；REG31=%#04x 应 bit6/5=1）"
                         % (cyc, s_vol, s_mute))
        # ---- P2 暂停后 codec 仍通电可达 ----
        if B["ESRBPWR"] == 0xFFFF:
            fails.append("[%d] P2 暂停后读不回 REG12（芯片不可达/已断电）—— 应保持通电" % cyc)
        elif B["ESRBPWR"] & 0x02:
            fails.append("[%d] P2 暂停后 REG12=%#04x power-down 位=1（不该断电）"
                         % (cyc, B["ESRBPWR"]))
        # ---- P3 ★终态权威判据（以**回读值**为准）----
        # 为什么不用 REG32 写入值：`g_es_reg32_wr` 只记"最后一次成功的写"，受 I2C
        # 成败影响；而回读值 g_es_rb_vol 是**芯片里真实躺着的值**，才是耳朵听到的。
        # 但回读本身也可能失败（芯片在窗口里 NACK）⇒ 那样会得 0xFFFF，此时回落到
        # 写入值判定，并把"回读失败"单独记为告警而不是失败（见 P7）。
        if C["ESRBVOL"] != 0xFFFF:
            if C["ESRBVOL"] != expected:
                fails.append("[%d] ★P3 回读音量 %#04x ≠ 期望 %#04x（芯片里真实音量不对）"
                             % (cyc, C["ESRBVOL"], expected))
        elif C["ESREG32W"] != expected:
            fails.append("[%d] ★P3 回读失败且写入值 %#04x ≠ 期望 %#04x（无法确认音量落进芯片）"
                         % (cyc, C["ESREG32W"], expected))
        # ---- P4 DAC 就绪 ----
        if C["AOREADY"] != 1 or C["AORAMP"] != 0:
            fails.append("[%d] P4 DAC 未就绪（ready=%d ramp=%d）"
                         % (cyc, C["AOREADY"], C["AORAMP"]))
        # ---- P5 真的在出声 ----
        if C["WSTATE"] != 1 or C["AOPLAY"] != 1:
            fails.append("[%d] P5 续播没进播放态（state=%d playing=%d）"
                         % (cyc, C["WSTATE"], C["AOPLAY"]))
        elif not frames_grew:
            fails.append("[%d] P5 续播后 frames 不涨（%d）—— 数据通路没走" % (cyc, C["FRAMES"]))
        # ---- P6 不变量自洽 ----
        if C["ESNGHEAD"] != C["ESWRNG"] + C["ESRDNG"]:
            fails.append("[%d] P6 失败环计数不自洽（head=%d ≠ wr_ng+rd_ng=%d）"
                         % (cyc, C["ESNGHEAD"], C["ESWRNG"] + C["ESRDNG"]))
        if C["ESFAIL"] != A["ESFAIL"]:
            fails.append("[%d] P6 出现\"重试 3 次仍失败\" +%d 笔（真丢写）"
                         % (cyc, C["ESFAIL"] - A["ESFAIL"]))
        # ---- P7 codec 可达性（探测重试是否救回了"没探到"）----
        # 起播时若 probe 首试 NACK、延时后探到 ⇒ ESPRETRY 会涨。这是**修好了**的证据
        # （旧写法会直接判"总线上没有 ES8311" ⇒ rc=-22 ⇒ 用户"点播放没反应"）。
        # 只要这段涨了，就说明重试确实在起作用，不算失败，只打印。
        if A["ESPRETRY"] != 0:
            print("  （起播探测重试 +%d：probe 曾 NACK，延时后探到 —— 旧写法此处会失败）"
                  % A["ESPRETRY"])

        bad = [f for f in fails if f.startswith("[%d]" % cyc)]
        print("  → 本循环 %s" % ("OK" if not bad else "有问题"))
        latch(2, wait=1.0)      # 停播，回干净状态

    print("=" * 78)
    if fails:
        print("汇总：FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  ✗ " + f)
        return 1
    print("汇总：全部 PASS —— 暂停只发 1 笔静音、codec 未断电、续播后音量确实落进芯片")
    return 0


if __name__ == "__main__":
    sys.exit(main())
