#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把卡上被自检改写过的扇区**还原回去**（对卡做到无损）。

【为什么需要它】
2026-10-08 第二轮自检出现过：多块读失败 → 恢复步骤**也失败** ⇒
卡尾部那 8 个扇区被写成了测试图案，原内容没写回去。
内存里的备份（s_bak）在复位后会丢，所以当时先用
    probe-rs read ... b8 0x30000200 12288 -o build/sd_bss_sd.bin -f binary
把整段 .bss_sd 抢救到了主机。本脚本就是把那份备份**写回卡**。

用法
----
    python tools/sd_restore.py                     # 用默认参数还原
    python tools/sd_restore.py --file 备份.bin --block 0 --sector 61067264

流程
----
    1. 读固件自报的 g_sd_bak_addr（**不猜段内布局**——实测三个缓冲的排列
       顺序与源码书写顺序并不一致，猜地址会写错地方）；
    2. 把 4 KB 原内容分批写进板上的 s_bak；
    3. 写 g_sd_sector，再写 g_sd_test=3 触发"回填写回"；
    4. 读 g_sd_restore_mis —— **0 即还原成功**。
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench

BAK_FILE = "build/sd_bss_sd.bin"
DEF_SECTOR = 61067264          # 第二轮自检用的扇区（= 总块数 - 2048）
BLOCK_BYTES = 8 * 512          # SD_BENCH_NSEC × 512


def wr_words(addr, words, chunk=64):
    """按 32 位字写入内存，分批（命令行太长会被 shell 掐掉）。"""
    for i in range(0, len(words), chunk):
        part = words[i:i + chunk]
        a = addr + i * 4
        vals = " ".join("0x%08x" % (w & 0xFFFFFFFF) for w in part)
        r = subprocess.run("probe-rs write %s --chip %s b32 0x%08x %s"
                           % (bench.PROBE, bench.CHIP, a, vals),
                           shell=True, capture_output=True, text=True,
                           encoding="utf-8", errors="ignore")
        if r.returncode:
            raise RuntimeError("写 0x%08X 失败：%s%s" % (a, r.stdout, r.stderr))


def rd1(n):
    return bench.rd(n, 1)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=BAK_FILE, help="板上 .bss_sd 段的完整副本")
    ap.add_argument("--block", type=int, default=0,
                    help="副本里的第几个 4 KB 块是备份（默认 0）")
    ap.add_argument("--sector", type=int, default=DEF_SECTOR, help="要还原的起始扇区")
    args = ap.parse_args()

    if not os.path.exists(args.file):
        print("[FAIL] 备份文件不存在：%s" % args.file)
        return 1

    # ---- 前置：内核必须在跑 ----
    a = rd1("g_loop_count")
    time.sleep(1.0)
    b = rd1("g_loop_count")
    if a == b:
        print("[FAIL] 主循环没在跑（g_loop_count %d 两次不变）→ 先 probe-rs reset" % a)
        return 1

    data = open(args.file, "rb").read()
    off = args.block * BLOCK_BYTES
    if off + BLOCK_BYTES > len(data):
        print("[FAIL] 备份文件只有 %d 字节，取不出第 %d 个 %d 字节块"
              % (len(data), args.block, BLOCK_BYTES))
        return 1
    payload = data[off:off + BLOCK_BYTES]

    bak_addr = rd1("g_sd_bak_addr")
    if bak_addr == 0:
        print("[FAIL] g_sd_bak_addr = 0 —— 卡没初始化成功，地址没自报")
        return 1
    print("板上备份缓冲地址：0x%08X（固件自报）" % bak_addr)
    print("还原目标：扇区 %d 起 8 个扇区（%d 字节），数据源 %s 块 %d"
          % (args.sector, BLOCK_BYTES, args.file, args.block))
    print("  前 16 字节：%s" % payload[:16].hex())

    # ---- 1) 回填 s_bak ----
    words = list(struct.unpack("<%dI" % (BLOCK_BYTES // 4), payload))
    wr_words(bak_addr, words)

    # ---- 2) 设扇区并触发 ----
    bench.wr("g_sd_state", [0])
    bench.wr("g_sd_sector", [args.sector])
    bench.wr("g_sd_test", [3])

    state = 0
    for _ in range(150):
        time.sleep(0.1)
        state = rd1("g_sd_state")
        if state != 0:
            break
    if state != 2:
        print("[FAIL] 没跑完（g_sd_state = %d，1 = 卡初始化失败）" % state)
        return 1

    rc  = rd1("g_sd_restore_rc")
    mis = rd1("g_sd_restore_mis")
    hal = rd1("g_sd_hal_rc")
    err = rd1("g_sd_errcode")
    sta = rd1("g_sd_sta")
    rtry = rd1("g_sd_retry")
    print("")
    print("写回 rc=%d（0=OK）  读回比对失配 = %d 字节  判据 0" % (rc, mis))
    print("HAL 返回码=%d  ErrorCode=0x%08X  SDMMC->STA=0x%08X  重试 %d 次"
          % (hal, err, sta, rtry))

    if mis != 0 or rc != 0:
        print("")
        print("[FAIL] 还原没成功 —— 扇区 %d 起的 8 个扇区内容仍可能是测试图案。" % args.sector)
        return 2

    print("")
    print("[PASS] 卡内容已还原：8 个扇区与备份逐字节一致（失配 0）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
