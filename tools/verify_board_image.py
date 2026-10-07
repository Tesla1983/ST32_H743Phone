#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把机内 flash 的内容读回来，和本地/CI 构建出的 .bin 做逐字节比对。

为什么要有这个脚本：
  CI 能不能"编出跟板上一样的固件"，最终只能靠**把板上的字节读回来**说话，
  不能靠"两边 cmake 都成功了"这种间接证据。

用法：
  python tools/verify_board_image.py build/ymgui-h743.bin
  python tools/verify_board_image.py <bin> --length 65536      # 只比前 64 KB（快）
  python tools/verify_board_image.py <bin> --sampled 4096      # 抽样：头/尾/中段各一块

判据：
  完全一致 ⇒ 板上跑的就是这份固件。
  不一致   ⇒ 打印**第一个不同的偏移**、不同字节数，并按 4 KB 分块列出差异块，
             便于判断是"整体不同"（版本不对）还是"局部不同"（某几个区域重编产差异）。

注意：读 flash 是一件跑几分钟的事（CMSIS-DAP 上一整片 1.5 MB），故支持 --sampled 快查。
"""

import subprocess
import sys
import os

PROBE = "--probe 0416:5021:0123456789AB"  # FireDAP CMSIS-DAP（本机还有 DAPLink / ESP JTAG）
CHIP = "STM32H743VITx"
FLASH_BASE = 0x08000000


def read_words(addr, n):
    """用 probe-rs 读 n 个 32 位字（宽度 b32 最快）。返回 uint32 列表。"""
    cmd = "probe-rs read %s --chip %s b32 0x%08x %d" % (PROBE, CHIP, addr, n)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 8 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < n:
        raise RuntimeError("读 0x%08X 失败（要 %d 字，得 %d 字）：%s%s"
                           % (addr, n, len(tok), r.stdout[:400], r.stderr[:400]))
    return [int(x, 16) for x in tok[:n]]


def read_bytes(addr, nbytes):
    """按 32 位字读，再摊平成小端字节；末尾不足 4 字节按实际长度截断。"""
    nwords = (nbytes + 3) // 4
    words = read_words(addr, nwords)
    buf = bytearray()
    for w in words:
        buf += bytes([w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF])
    return bytes(buf[:nbytes])


def diff_blocks(a, b, base, block=4096):
    """返回 (第一个不同的绝对偏移, 不同字节数, 差异分块列表)。"""
    first = None
    nd = 0
    blocks = []
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            if first is None:
                first = base + i
            nd += 1
            blk = (base + i) // block
            if not blocks or blocks[-1] != blk:
                blocks.append(blk)
    if len(a) != len(b):
        if first is None:
            first = base + n
        nd += abs(len(a) - len(b))
    return first, nd, blocks


def main():
    args = [x for x in sys.argv[1:]]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    path = args[0]

    def opt(name, default):
        if name in args:
            return int(args[args.index(name) + 1])
        return default

    length = opt("--length", 0)
    sampled = opt("--sampled", 0)

    if not os.path.isfile(path):
        print("找不到文件：%s" % path)
        return 2
    disk = open(path, "rb").read()
    total = length or len(disk)

    if sampled:
        # 抽样：头、1/4、1/2、3/4、尾各一块，快查用
        step = min(sampled, max(1, (total - sampled) // 4))
        segs = [0, (total - sampled) // 4, (total - sampled) // 2,
                3 * (total - sampled) // 4, total - sampled]
        ok = True
        for off in segs:
            off = max(0, min(off, total - sampled))
            board = read_bytes(FLASH_BASE + off, sampled)
            part = disk[off:off + sampled]
            first, nd, _ = diff_blocks(board, part, FLASH_BASE + off)
            if nd == 0:
                print("[OK ] 0x%08X..0x%08X 一致（%d 字节）"
                      % (FLASH_BASE + off, FLASH_BASE + off + sampled, sampled))
            else:
                ok = False
                print("[BAD] 0x%08X..0x%08X 有 %d 字节不同，首个差异在 0x%08X"
                      % (FLASH_BASE + off, FLASH_BASE + off + sampled, nd, first))
        print("[%s] 抽样比对：%s vs %s" % ("PASS" if ok else "FAIL", path, "机内 flash"))
        return 0 if ok else 1

    print("读取机内 flash 0x%08X 起 %d 字节（约 %d 秒）..." % (FLASH_BASE, total, total // 12000))
    board = read_bytes(FLASH_BASE, total)
    print("读回完毕，比对...")
    first, nd, blocks = diff_blocks(board, disk, FLASH_BASE)

    if nd == 0:
        print("[PASS] 机内 flash 与 %s 完全一致（%d 字节）" % (path, len(disk)))
        return 0

    print("[FAIL] 机内 flash 与 %s 不一致" % path)
    print("       文件 %d 字节 / 读回 %d 字节" % (len(disk), len(board)))
    print("       不同字节数：%d" % nd)
    print("       首个差异偏移：0x%08X" % first)
    print("       有差异的 4 KB 块：%s" % ", ".join("0x%06X" % (b * 4096) for b in blocks[:20]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
