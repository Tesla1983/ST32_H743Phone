#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""phrases_xip.bin（v3）的宿主机确定性校验 —— **不需要板**。

只做整数/字节级断言，不做"看起来像"的判断。五条：

  A1  v3 逐条解码结果与 v2 原始 47280 条**按桶逐一相等**（含顺序，桶内已按词频降序）
  A2  头部自洽：magic / bucket_off 单调 / off[0]==header_size / off[26]==file_size
      / sum(bucket_n)==count
  A3  每个桶块内条目首尾相接、4 字节对齐、每个串都在正确位置以 NUL 结尾、末条目
      正好停在桶块末尾（不多一字节、不少一字节）
  A4  桶归属：每条 py[0]-'a' 必须等于它所在桶号（固件 loadPhraseDictionary 同款校验）
  A5  扫描成本模型：给定 IME_PHRASE_SCAN_CAP，算出最坏单次扫桶读多少字节，
      并据此给出 ns 估算（用板上实测的 25 ns/字节 + 510 ns 起段）

用法：python tools/ime_phrase_xip_check.py
退出码：0 = 全通过；1 = 有断言失败。
"""

import os
import struct
import sys

BUCKETS = 26
HEADER_SIZE = 256
ENTRY_HDR = 8
SCAN_CAP = 512          # 固件侧 IME_PHRASE_SCAN_CAP，改这里要和 .inc 保持一致
NS_PER_BYTE = 25.0      # 板上实测：XIP 段内顺序读 25.1 ns/字节（micro.py case 27）
NS_NEW_SEG = 510.0      # 板上实测：新起一段不连续读（micro.py case 20/21/22）

fail = []


def check(cond, msg):
    print(("  [OK]   " if cond else "  [FAIL] ") + msg)
    if not cond:
        fail.append(msg)
    return cond


def load_v2(path):
    with open(path, "rb") as f:
        d = f.read()
    count, blob_size, hs, rs, bc = struct.unpack_from("<5I", d, 8)
    bk = [struct.unpack_from("<I", d, 28 + 4 * i)[0] for i in range(bc + 1)]
    blob = hs + count * rs

    def cstr(base):
        n = 0
        while d[base + n] != 0:
            n += 1
        return d[base:base + n]

    ents = []
    for i in range(count):
        py, ini, wd, fr = struct.unpack_from("<4I", d, hs + i * rs)
        ents.append((cstr(blob + py), cstr(blob + ini), cstr(blob + wd), fr))
    return ents, bk


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    v2 = os.path.join(root, "third_party/YMGUI/project_Demo/chinese_ime/phrases_rime.bin")
    v3 = os.path.join(root, "build/phrases_xip.bin")
    for p in (v2, v3):
        if not os.path.isfile(p):
            print("缺文件：%s（先跑 tools/gen_ime_dict.py）" % p)
            return 1

    src, sbk = load_v2(v2)
    d = open(v3, "rb").read()

    print("== A2 头部自洽 ==")
    check(d[:8] == b"YMIMEP3\x00", "magic = YMIMEP3")
    count, hs, bc, fsize, ehdr, cap = struct.unpack_from("<6I", d, 8)
    check(hs == HEADER_SIZE, "header_size = %d" % hs)
    check(bc == BUCKETS, "bucket_count = %d" % bc)
    check(ehdr == ENTRY_HDR, "entry_hdr = %d" % ehdr)
    check(fsize == len(d), "file_size %d == 实际 %d" % (fsize, len(d)))
    off = [struct.unpack_from("<I", d, 32 + 4 * i)[0] for i in range(BUCKETS + 1)]
    n = [struct.unpack_from("<I", d, 140 + 4 * i)[0] for i in range(BUCKETS)]
    check(off[0] == hs, "bucket_off[0] == header_size")
    check(off[BUCKETS] == fsize, "bucket_off[26] == file_size")
    check(all(off[i] <= off[i + 1] for i in range(BUCKETS)), "bucket_off 单调不减")
    check(all((off[i] & 3) == 0 for i in range(BUCKETS + 1)), "bucket_off 4 字节对齐")
    check(sum(n) == count, "sum(bucket_n) %d == count %d" % (sum(n), count))
    check(count == len(src), "count %d == v2 条数 %d" % (count, len(src)))

    print("== A3 桶块内条目首尾相接 / A4 桶归属 / A1 内容等价 ==")
    bad_align = bad_term = bad_fit = bad_bucket = bad_order = mismatch = 0
    worst_scan = 0
    worst_bucket = None
    for bi in range(BUCKETS):
        p, end = off[bi], off[bi + 1]
        got = []
        bytes_read = 0
        k = 0
        while p < end:
            if (p & 3) != 0:
                bad_align += 1
                break
            if p + ENTRY_HDR > end:
                bad_fit += 1
                break
            hdr, fr = struct.unpack_from("<II", d, p)
            py_len = hdr & 0xFF
            wd_len = (hdr >> 8) & 0xFF
            total = ENTRY_HDR + py_len + 1 + wd_len + 1
            if p + ((total + 3) & ~3) > end:
                bad_fit += 1
                break
            py = d[p + ENTRY_HDR:p + ENTRY_HDR + py_len]
            wd = d[p + ENTRY_HDR + py_len + 1:p + ENTRY_HDR + py_len + 1 + wd_len]
            if d[p + ENTRY_HDR + py_len] != 0 or d[p + ENTRY_HDR + py_len + 1 + wd_len] != 0:
                bad_term += 1
                break
            if not (py and 97 <= py[0] <= 122 and py[0] - 97 == bi):
                bad_bucket += 1
            got.append((py, wd, fr))
            if k < SCAN_CAP:
                bytes_read += (total + 3) & ~3
            k += 1
            p += (total + 3) & ~3
        if p != end and not (bad_align or bad_term or bad_fit):
            bad_fit += 1
        if bytes_read > worst_scan:
            worst_scan, worst_bucket = bytes_read, chr(97 + bi)
        # 与 v2 同桶比对（v2 同桶已按 (-freq, py, wd) 排序后再比）
        want = sorted([(e[0], e[2], e[3]) for e in src[sbk[bi]:sbk[bi + 1]]],
                      key=lambda t: (-t[2], t[0], t[1]))
        if len(got) != len(want):
            mismatch += len(want) - len(got)
        else:
            for a, b in zip(got, want):
                if a != b:
                    mismatch += 1
            if any(got[i][2] < got[i + 1][2] for i in range(len(got) - 1)):
                bad_order += 1

    check(bad_align == 0, "条目起始 4 字节对齐（违例 %d）" % bad_align)
    check(bad_term == 0, "py/wd 都在声明长度处 NUL 结尾（违例 %d）" % bad_term)
    check(bad_fit == 0, "末条目正好停在桶块末尾（违例 %d）" % bad_fit)
    check(bad_bucket == 0, "桶归属 py[0]-'a'==桶号（违例 %d）" % bad_bucket)
    check(bad_order == 0, "桶内按 frequency 降序（违例桶 %d）" % bad_order)
    check(mismatch == 0, "v3 与 v2 逐条（py/word/freq）比对失配 %d 条" % mismatch)

    print("== A5 扫描成本模型（cap=%d）==" % SCAN_CAP)
    ns = NS_NEW_SEG + worst_scan * NS_PER_BYTE
    print("  最坏单次扫桶：%d B（桶 '%s'）" % (worst_scan, worst_bucket))
    print("  ⇒ 新起 1 段 + 段内顺序读 = %.0f ns + %d × %.0f ns = **%.2f ms**" %
          (NS_NEW_SEG, worst_scan, NS_PER_BYTE, ns / 1e6))
    for qlen in (4, 6, 8):
        print("  ⇒ %d 字拼音（buildComposite 每字扫一次桶）≈ **%.2f ms**" %
              (qlen, ns * qlen / 1e6))
    check(ns * 8 / 1e6 < 6.0, "8 字查询的词组扫描 < 6 ms（判据：不超过一帧 14.7 ms 的一半）")
    check(len(d) <= 3 * 1024 * 1024, "文件 %d B 装得进 IME 保留区 3 MB" % len(d))

    print()
    if fail:
        print("失败 %d 项：" % len(fail))
        for m in fail:
            print("   - " + m)
        return 1
    print("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
