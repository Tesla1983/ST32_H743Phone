#!/usr/bin/env python3
"""单字词典"删 entries[] 索引 + 按最大桶跨度分配候选数组"的等价性 / 上界校验。

背景（2026-10-07 改动，见 ime_candidates.inc 的 CHAR_AT 与 imeCharMatchCap）：

  CHARS_ROM 分支原本 GY_malloc1(count × 12 B) 建一份完整索引放在 heap1
  （pinyin_gb2312：7291 × 12 = 87 492 B），而 payload 本身已经在片上 flash ——
  索引只是 (py, ch, frequency) 的搬运冗余。现改成按序号现场解记录。
  同时 s_char_matches 的容量从 S_CHAR_COUNT(7291) 改成 imeCharMatchCap()(614)。

这两步都**不能靠"看起来没问题"验收**，所以本脚本在宿主机上用同一个 .bin
跑确定性整数断言（不需要开发板）：

  A1  charEntryAt(i) 与旧 entries[] 构造逐条相等（7291 条，0 失配）
  A2  imeCharMatchCap() == 最大桶跨度
  A3  全枚举 26 个单字母 + 676 个两字母查询，实际 match 数均不越 cap
  A4  loadCharDictionary 的桶归属校验 0 违规

⚠ A3 是"按桶上界分配"这条推导的安全网：
  resetCharSearch 一次只扫一个桶，所以 match ≤ 桶跨度 ≤ cap。
  一旦将来重生成词典导致这个上界失效，本脚本会红，而不是等到板上越界写。

上板实测（2026-10-07，`python tools/heap_peak_check.py`）：
  heap1 peak 71 064 B（69.4 KB，33%），与上面按字节算的 109 288 B 节省完全吻合；
  ime_probe 成功 2/2；A2 gram/ramp 均 0；g_fault.magic 0；FLASH +24 B。

用法：python tools/ime_char_index_check.py
退出码：0 = 全通过；1 = 有断言失败。
"""

import os
import string
import struct
import sys

BIN = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "third_party", "YMGUI", "project_Demo", "chinese_ime", "pinyin_gb2312.bin",
)
MAGIC = b"YMIMEC1\x00"


def main():
    with open(BIN, "rb") as f:
        d = f.read()
    count, blob_size, hs, rs, bc = struct.unpack_from("<5I", d, 8)
    assert d[:8] == MAGIC, "magic 不是 YMIMEC1"
    assert bc == 26 and rs == 12 and hs == 136, (bc, rs, hs)

    buckets = [struct.unpack_from("<I", d, 28 + 4 * i)[0] for i in range(bc + 1)]
    blob_base = hs + count * rs

    def cstr(base):
        n = 0
        while base + n < len(d) and d[base + n] != 0:
            n += 1
        return d[base:base + n].decode("utf-8", "replace")

    def read_le32(off):
        return struct.unpack_from("<I", d, off)[0]

    # ---- 旧路径：loadCharDictionary 里建的 entries[] ----
    old = []
    for i in range(count):
        rec = hs + i * rs
        py, ch = read_le32(rec), read_le32(rec + 4)
        old.append((cstr(blob_base + py), cstr(blob_base + ch), read_le32(rec + 8)))

    # ---- 新路径：charEntryAt(i)（s_char_data = d, s_char_blob = d + hs + count*rs）----
    def entry_at(i):
        rec = hs + i * rs
        py, ch = read_le32(rec), read_le32(rec + 4)
        return (cstr(blob_base + py), cstr(blob_base + ch), read_le32(rec + 8))

    fails = []

    # A1
    mismatch = sum(1 for i in range(count) if entry_at(i) != old[i])
    if mismatch:
        fails.append(f"A1 charEntryAt 与 entries[] 失配 {mismatch}/{count}")

    # A2
    cap = max(buckets[i + 1] - buckets[i] for i in range(bc))
    big = chr(ord("a") + max(range(bc), key=lambda i: buckets[i + 1] - buckets[i]))
    if cap != 614 or count != 7291:
        fails.append(f"A2 cap={cap}（桶 '{big}'）count={count} —— 与注释里的 614/7291 不符，"
                     "若是有意换词典请同步更新 ime_candidates.inc 的注释")

    # A3
    def match_count(query):
        b = ord(query[0]) - ord("a")
        if not (0 <= b < 26):
            return 0
        seen = []
        for i in range(buckets[b], buckets[b + 1]):
            e = entry_at(i)
            if not e[0].startswith(query):
                continue
            dup = -1
            for j, idx in enumerate(seen):
                if entry_at(idx)[1] == e[1]:
                    dup = j
                    break
            if dup >= 0:
                if e[2] > entry_at(seen[dup])[2]:
                    seen[dup] = i
            else:
                seen.append(i)
        return len(seen)

    worst1 = max(((match_count(c), c) for c in string.ascii_lowercase))
    worst2 = max(
        ((match_count(a + b), a + b) for a in string.ascii_lowercase for b in string.ascii_lowercase)
    )
    if worst1[0] > cap or worst2[0] > cap:
        fails.append(f"A3 match 数越界 cap={cap}：单字母 {worst1} 两字母 {worst2}")

    # A4
    bad = 0
    for i in range(count):
        b = ord(entry_at(i)[0][0]) - ord("a")
        if not (0 <= b < 26 and buckets[b] <= i < buckets[b + 1]):
            bad += 1
    if bad:
        fails.append(f"A4 桶归属违规 {bad}/{count}")

    print(f"词典 {os.path.basename(BIN)}：count={count} record={rs}B header={hs}B")
    print(f"A1 charEntryAt vs entries[] : {count} 条，失配 {mismatch}")
    print(f"A2 imeCharMatchCap()        : {cap}（最大桶 '{big}'），旧容量 {count}")
    print(f"A3 match 上界               : 单字母 {worst1[0]} ('{worst1[1]}') · "
          f"两字母 {worst2[0]} ('{worst2[1]}') · cap {cap} · 越界=False")
    print(f"A4 桶归属校验               : 违规 {bad}")
    print()
    print(f"heap1 预估：87 492B(entries) + 29 164B(旧 s_char_matches) "
          f"→ {cap * 12}B(新) = 省 {87492 + 29164 - cap * 12} B")
    if fails:
        for f_ in fails:
            print("FAIL " + f_)
        return 1
    print("全部断言通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
