#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ES8311 音频链路验收（方案 A 接线）：触发 g_audio_test，读 g_at_* 结果。

前提
----
固件必须带音频自检编入（**默认关闭**）：
    cmake -S . -B build -DYMGUI_AUDIO_TEST=ON
    cmake --build build --target ymgui-bin
然后 probe-rs download + **reset**（download 后内核 halt，不 reset 什么都不会跑）。

case 表
-------
    1  I2C 扫描（0x18/0x19）+ 读芯片 ID 0xFD/0xFE/0xFF  —— **不出声**，先跑这个
    2  全链路 · 五音上行 ×2 轮（244/488/733/977/1221 Hz，各 400 ms，≈4 s 纯音）
    3  交叉位拍 I2C 普查（SDA/SCL 对调扫描，排查"线接反"）
    4  全链路 · **单音 3 秒长音**（≈977 Hz 连续，听"出不出声"最快的一档）

用法
----
    python tools/audio_check.py          # case 1 然后 case 2
    python tools/audio_check.py 1        # 只跑 I2C 扫描（接了声卡/示波器前先这个）
    python tools/audio_check.py 4        # 放 3 秒长音
    python tools/audio_check.py 2 --amp 16000   # 指定提示音峰值幅度（默认 28000）
    python tools/audio_check.py 3        # 交叉位拍普查：SDA/SCL 对调扫描（排查"线接反"）

判据
----
case 1：g_at_rc=0 且 g_at_i2c_addr ∈ {0x18,0x19}，且 g_es_wr_ng/g_es_rd_ng=0。
        地址探到但 ID 全 0xFF/0x00 ⇒ 线可能通了但器件状态怪，看 g_es_wr_ok。
case 2：g_at_rc=0、g_at_i2s_rc=0、g_at_frames>0、g_at_hang=0。
        期望频率（给逻辑分析仪/示波器对照，由 I2SCFGR 回读算出）：
            MCLK ≈ 12.5 MHz（PC6）   SCK ≈ 3.125 MHz（PD3）
            LRCK ≈ 48.8 kHz（PB9）   DOUT ≈ 3.125 MHz 数据率（PB15 或 PC1）
        听到/看到声音即全通；无声先看 g_at_frames 是否在涨、再量 LRCK。
"""
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench  # noqa: E402  复用 rd/wr（从 ELF 取符号地址）


NEED = ("g_audio_test", "g_at_rc", "g_at_stage", "g_at_i2c_addr", "g_at_id0",
        "g_at_id1", "g_at_id2", "g_at_i2s_rc", "g_at_frames", "g_at_mclk_hz",
        "g_at_fs_hz", "g_at_sck_hz", "g_at_hang", "g_at_udr", "g_at_amp",
        "g_at_probe_us", "g_at_scan_n", "g_at_scan_first",
        "g_at_bb_norm", "g_at_bb_cross", "g_at_bb_first", "g_at_kclk",
        "g_es_wr_ok", "g_es_wr_ng", "g_es_rd_ok", "g_es_rd_ng")


def wait_stage(stage_done, timeout_s):
    """等 g_at_stage 到 100（自检结束）或超时。返回最后的 stage。"""
    t0 = time.time()
    last = 0
    while time.time() - t0 < timeout_s:
        last = bench.rd("g_at_stage", 1)[0]
        if last >= stage_done:
            return last
        time.sleep(0.1)
    return last


def run_case(case, timeout_s):
    bench.wr("g_audio_test", [case])
    st = wait_stage(100, timeout_s)
    if st < 100:
        print("  [FAIL] 超时：g_at_stage 停在 %d（超过 %.1f s 没跑完）" % (st, timeout_s))
        return False

    rc      = bench.rd("g_at_rc", 1)[0]
    addr    = bench.rd("g_at_i2c_addr", 1)[0]
    id0     = bench.rd("g_at_id0", 1)[0] & 0xFF
    id1     = bench.rd("g_at_id1", 1)[0] & 0xFF
    id2     = bench.rd("g_at_id2", 1)[0] & 0xFF
    wr_ok   = bench.rd("g_es_wr_ok", 1)[0]
    wr_ng   = bench.rd("g_es_wr_ng", 1)[0]
    rd_ok   = bench.rd("g_es_rd_ok", 1)[0]
    rd_ng   = bench.rd("g_es_rd_ng", 1)[0]
    i2s_rc  = bench.rd("g_at_i2s_rc", 1)[0]
    frames  = bench.rd("g_at_frames", 1)[0]
    mclk    = bench.rd("g_at_mclk_hz", 1)[0]
    fs      = bench.rd("g_at_fs_hz", 1)[0]
    sck     = bench.rd("g_at_sck_hz", 1)[0]
    hang    = bench.rd("g_at_hang", 1)[0]
    udr     = bench.rd("g_at_udr", 1)[0]
    probe_us = bench.rd("g_at_probe_us", 1)[0]
    scan_n   = bench.rd("g_at_scan_n", 1)[0]
    scan_1st = bench.rd("g_at_scan_first", 1)[0]
    bb_norm  = bench.rd("g_at_bb_norm", 1)[0]
    bb_cross = bench.rd("g_at_bb_cross", 1)[0]
    bb_1st   = bench.rd("g_at_bb_first", 1)[0]
    kclk     = bench.rd("g_at_kclk", 1)[0]

    print("  g_at_rc=%d  I2C 地址=0x%02X  ID(0xFD/FE/FF)=%02X %02X %02X" %
          (rc, addr, id0, id1, id2))
    print("  I2C 计数：写 OK/NG = %d/%d，读 OK/NG = %d/%d" % (wr_ok, wr_ng, rd_ok, rd_ng))
    if case == 3:
        print("  位拍普查：正常朝向(SDA=PB7,SCL=PB6) %d 个命中；"
              "交叉朝向(SDA=PB6,SCL=PB7) %d 个命中（第一个 0x%02X）"
              % (bb_norm, bb_cross, bb_1st))
        if bb_cross > 0:
            print("    ⇒ **只有交叉朝向有命中**：模块那头 SDA/SCL 实际是反的，把两根线对调！")
        elif bb_norm > 0:
            print("    ⇒ 正常朝向有命中（位拍能通而 HAL 不通？留意）")
        else:
            print("    ⇒ 两个朝向都扫不到：模块侧确实没有活着的 I2C 器件")
        ok = (bb_cross > 0 or bb_norm > 0)
        print("总结：%s" % ("交叉朝向命中，对调线即可 ✅" if ok else "两个朝向都无命中 ❌"))
        return ok
    print("  全地址段普查：%d 个器件应答（第一个 0x%02X）；ES8311 探测耗时 %d µs"
          % (scan_n, scan_1st, probe_us))
    if probe_us < 1000:
        print("    ⇒ 快速 NACK：I2C 外设在正常收发，只是没人 ACK")
    elif probe_us > 50000:
        print("    ⇒ 接近 6×20ms 超时：事务没跑完（内核时钟没跑/总线卡死），不是 NACK")
    if case == 2:
        print("  I2S：HAL rc=%d  帧数=%d  hang=%d  欠载次数=%d  内核时钟=%d Hz"
              % (i2s_rc, frames, hang, udr, kclk))
        print("  期望频率（I2SCFGR 回读）：MCLK=%d Hz  SCK=%d Hz  LRCK(fs)=%d Hz"
              % (mclk, sck, fs))
        if udr > 0:
            print("    ⇒ 欠载 %d 次（CPU 停顿 >82µs 缓冲深度所致，已自动清 UDR 继续）" % udr)

    ok = True
    if case == 1:
        if rc != 0:
            print("  [FAIL] I2C 阶段失败（rc=%d）：-1=总线初始化 -2=探测和真实写都无应答 -3=读 ID 失败" % rc)
            ok = False
        elif addr not in (0x18, 0x19):
            print("  [FAIL] 地址异常：0x%02X" % addr)
            ok = False
        else:
            print("  [OK] ES8311 在 0x%02X 应答，ID 已读出。" % addr)
            if wr_ng or rd_ng:
                print("  ⚠ 有失败的 I2C 事务（wr_ng=%d rd_ng=%d）——可能是时序/上拉问题。" % (wr_ng, rd_ng))
    else:
        if rc != 0:
            print("  [FAIL] 音频链路失败（rc=%d）：-1 总线 -2 无应答 -4 codec 寄存器 -5 I2S 格式"
                  " -6 I2S 初始化 -7 DAC 启动" % rc)
            ok = False
        elif hang or frames == 0:
            print("  [FAIL] 流没跑起来：hang=%d frames=%d（查 I2S 时钟/接线）" % (hang, frames))
            ok = False
        else:
            print("  [OK] 全链路跑通：%d 帧已送入 I2S。" % frames)
            print("  若音箱无声：用逻辑分析仪量 PB9(LRCK) 是否 ≈%d Hz、PC6(MCLK) ≈%d Hz；" % (fs, mclk))
            print("  时钟在而无声 ⇒ 查 ES8311 模拟侧（模块供电/喇叭线/0x32 音量）。")
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    amp = None
    if "--amp" in sys.argv:
        amp = int(args[sys.argv.index("--amp") + 1])
        args = [a for a in args if a != str(amp)]
    cases = [int(a) for a in args] or [1, 2]

    missing = [s for s in NEED if s not in bench.T]
    if missing:
        print("固件里没有音频自检符号（%s 等）。" % ", ".join(missing[:4]))
        print("  出货构建默认不含它。请先：")
        print("    cmake -S . -B build -DYMGUI_AUDIO_TEST=ON")
        print("    cmake --build build --target ymgui-bin")
        print("    probe-rs download --probe 0416:5021:0123456789AB --chip STM32H743VITx build/ymgui-h743.elf")
        print("    probe-rs reset --probe 0416:5021:0123456789AB --chip STM32H743VITx")
        return 1

    # 内核必须在跑（download 后 halt 的经典陷阱）
    l0 = bench.rd("g_loop_count", 1)[0]
    time.sleep(0.5)
    l1 = bench.rd("g_loop_count", 1)[0]
    if l0 == l1:
        print("内核没在跑（g_loop_count 0.5 s 内没变：%d）—— probe-rs download 后必须 reset。" % l0)
        return 1
    print("主循环在跑（%d 拍/0.5s）" % (l1 - l0))

    if amp is not None:
        if amp < 0 or amp > 32000:
            print("--amp 范围 0..32000（默认 8000）")
            return 1
        bench.wr("g_at_amp", [amp])
        print("提示音峰值幅度 g_at_amp = %d" % amp)

    allok = True
    names = {1: "I2C 扫描 + 读 ID", 2: "全链路 · 五音上行×2 轮", 3: "交叉位拍 I2C 普查",
             4: "全链路 · 单音 3 秒长音"}
    for c in cases:
        print("== case %d：%s ==" % (c, names.get(c, "全链路")))
        if not run_case(c, 6.0 if c == 1 else 12.0):
            allok = False
            break
        time.sleep(0.3)

    print()
    print("总结：%s" % ("全部通过 ✅" if allok else "有失败项，见上方判据 ❌"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
