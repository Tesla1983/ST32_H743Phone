#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 phrases_rime.bin（旧格式 v2）重排成 **XIP 友好** 的 v3 格式 phrases_xip.bin。

【为什么要有这个文件】
方案 B（全量词组词典上外部 flash XIP）的性能约束不是容量、而是**外部 flash 的读时序**。
2026-10-07 在板上实测（tools/micro.py case 20~30，YMGUI_XIP_BENCH）：

  · 新起一段**不连续**读 ≈ 510 ns
  · 段内每多读 1 字节 ≈ 25 ns（≈40 MB/s，预取窗口 < 16 B）

也就是说 **XIP 的成本单位是"一段连续读"，不是"一条 load 指令"**。
旧格式每条记录是"16 B 记录区 + 三个字符串各自散在 blob 区"，扫一条要新起 3 段
（记录 / 拼音串 / 词串）⇒ 实测 1.97 µs/条。v3 把每桶写成**一整块连续区**、
条目自描述且首尾相接 ⇒ 整桶 = 1 段，实测模型下 ~31 B × 25 ns ≈ 0.78 µs/条。

【v3 布局】
  header（256 B）
    +0   magic "YMIMEP3\0"
    +8   u32 count          总条目数
    +12  u32 header_size    256
    +16  u32 bucket_count   26
    +20  u32 file_size      整个文件字节数
    +24  u32 entry_hdr      8（每条目的定长头）
    +28  u32 scan_cap_hint  生成器建议的每桶扫描上限（固件可不采纳）
    +32  u32 bucket_off[27] 各桶在文件内的**字节**偏移（末项 = file_size）
    +140 u32 bucket_n[26]   各桶条目数
    +244 u32 reserved[3]    0
  bucket block（桶 i 占 [bucket_off[i], bucket_off[i+1])），条目首尾相接：
    u32 hdr    = py_len | (wd_len << 8)     （均不含结尾 NUL）
    u32 freq
    char py[py_len + 1]   含 NUL
    char wd[wd_len + 1]   含 NUL
    pad 到 4 字节边界
  桶内条目按 **frequency 降序**（配合固件侧 IME_PHRASE_SCAN_CAP 使用：
  截断扫描时丢掉的永远是长尾低频词）。

【为什么丢掉 initials】
旧格式每条有 py / initials / word 三个串。全项目检索确认 `initials`
在 ime_candidates.inc 里**只被赋值、从未被读取**（phraseMatches 只用 p->py）。
v3 不再存它 —— 省 ~4 B/条（全量约 190 KB XIP 读带宽），且零功能变化。

用法
----
    python tools/gen_ime_dict.py                 # 默认输出到 build/phrases_xip.bin
    python tools/gen_ime_dict.py -o out.bin

校验：python tools/ime_phrase_xip_check.py
"""

import argparse
import os
import struct
import sys

MAGIC = b"YMIMEP3\x00"
HEADER_SIZE = 256
ENTRY_HDR = 8
BUCKETS = 26
SCAN_CAP_HINT = 512


def load_v2(path):
    """解析旧格式，返回 [(py, ini, wd, freq)] 与桶边界（条目下标）。"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != b"YMIMEP2\x00":
        raise SystemExit("不是 v2 词组词典：magic 不匹配（%r）" % d[:8])
    count, blob_size, hs, rs, bc = struct.unpack_from("<5I", d, 8)
    if bc != BUCKETS or rs != 16:
        raise SystemExit("桶数/记录尺寸不符预期：bc=%d rs=%d" % (bc, rs))
    bk = [struct.unpack_from("<I", d, 28 + 4 * i)[0] for i in range(bc + 1)]
    blob = hs + count * rs

    def cstr(base):
        n = 0
        while d[base + n] != 0:
            n += 1
        return d[base:base + n]

    ents = []
    for i in range(count):
        r = hs + i * rs
        py, ini, wd, fr = struct.unpack_from("<4I", d, r)
        ents.append((cstr(blob + py), cstr(blob + ini), cstr(blob + wd), fr))
    return ents, bk


def build(ents, bk, scan_cap_hint=SCAN_CAP_HINT):
    """按桶重排（桶内词频降序）并序列化成 v3。"""
    blocks = []
    offs = [0] * (BUCKETS + 1)
    ns = [0] * BUCKETS
    cur = HEADER_SIZE
    for i in range(BUCKETS):
        sub = ents[bk[i]:bk[i + 1]]
        # 桶内按词频降序；同频按 py 再按 wd，保证输出稳定可复现
        sub = sorted(sub, key=lambda e: (-e[3], e[0], e[2]))
        buf = bytearray()
        for py, _ini, wd, fr in sub:
            if len(py) > 255 or len(wd) > 255:
                raise SystemExit("串太长，放不进 u8 长度字段：%r / %r" % (py, wd))
            buf += struct.pack("<II", len(py) | (len(wd) << 8), fr)
            buf += py + b"\0" + wd + b"\0"
            pad = (-(len(buf))) & 3
            buf += b"\0" * pad
        offs[i] = cur
        ns[i] = len(sub)
        cur += len(buf)
        blocks.append(bytes(buf))
    offs[BUCKETS] = cur

    hdr = bytearray(HEADER_SIZE)
    hdr[0:8] = MAGIC
    struct.pack_into("<6I", hdr, 8, len(ents), HEADER_SIZE, BUCKETS, cur,
                     ENTRY_HDR, scan_cap_hint)
    for i in range(BUCKETS + 1):
        struct.pack_into("<I", hdr, 32 + 4 * i, offs[i])
    for i in range(BUCKETS):
        struct.pack_into("<I", hdr, 140 + 4 * i, ns[i])
    return bytes(hdr) + b"".join(blocks)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", default=os.path.join(
        root, "third_party/YMGUI/project_Demo/chinese_ime/phrases_rime.bin"))
    ap.add_argument("-o", default=os.path.join(root, "build/phrases_xip.bin"))
    ap.add_argument("--scan-cap", type=int, default=SCAN_CAP_HINT)
    a = ap.parse_args()

    ents, bk = load_v2(a.i)
    out = build(ents, bk, a.scan_cap)
    d = os.path.dirname(a.o)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(a.o, "wb") as f:
        f.write(out)

    old = os.path.getsize(a.i)
    print("输入 %s  %d 条 / %d B" % (os.path.basename(a.i), len(ents), old))
    print("输出 %s  %d B = %.1f KB" % (os.path.basename(a.o), len(out), len(out) / 1024.0))
    print("  相对旧文件 %+.1f KB（%+.1f%%）" %
          ((len(out) - old) / 1024.0, (len(out) - old) / old * 100.0))
    print("  每桶条目数 min/max = %d / %d" % (min(bk[i + 1] - bk[i] for i in range(26)),
                                             max(bk[i + 1] - bk[i] for i in range(26))))
    print("  扫描上限 hint = %d ⇒ 单次扫桶最多读 %d B/位置" %
          (a.scan_cap, a.scan_cap * 31))
    return 0


if __name__ == "__main__":
    sys.exit(main())
