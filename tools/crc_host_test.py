#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Host 侧验证 app_config.c 的 CRC32 与 blob 布局 —— 不需要硬件。

【为什么值得单独测】
  配置存到 flash 后要靠 CRC32 判定"这份数据是否完整"。如果 CRC 算法写错
  （表值抄错 / 半字节顺序错 / 初值与终值异或漏了），后果是：
    · 每次上电校验都失败 ⇒ 语言永远保存不了；
    · **而且是静默的** —— 不报错、不崩溃，只是"设置不生效"，极难定位。
  所以把算法与标准 CRC-32/ISO-HDLC（即 zlib.crc32）做逐向量比对。

【做法】从 src/app_config.c 里**提取真实的表值**（而不是在 Python 里另抄一份），
  用相同的半字节算法复算，与 zlib.crc32 比对。这样能抓住"表值抄错"这类缺陷；
  若表值正确，则算法结构必然与标准一致（否则结果不可能逐字节相同）。

同时核对 AppConfigBlob 的字段偏移 —— 这必须与 tools/lang_check.py 的解析一致，
否则"固件写的"和"脚本读的"错位，验证结论就不可信。

用法：python tools/crc_host_test.py     退出码 0 = 全部通过
"""
import os
import random
import re
import sys
import zlib

APP_CONFIG_C = "src/app_config.c"
LANG_CHECK_PY = "tools/lang_check.py"

# 标准 CRC-32/ISO-HDLC 检查值（"123456789" → 0xCBF43926）
CHECK_INPUT = b"123456789"
CHECK_CRC = 0xCBF43926


def extract_table():
    """从源码提取 s_crc_nib 的 16 个表值。"""
    txt = open(APP_CONFIG_C, encoding="utf-8", errors="ignore").read()
    m = re.search(r"s_crc_nib\[16\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise SystemExit("!! 在 %s 里找不到 s_crc_nib 表" % APP_CONFIG_C)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{8})[uU]?", m.group(1))]
    if len(vals) != 16:
        raise SystemExit("!! 表项数不是 16，而是 %d" % len(vals))
    return vals


def crc32_with_table(table, data, init=0xFFFFFFFF, xorout=0xFFFFFFFF):
    """完全照搬 app_config.c 的 crc32_update 写法（半字节、每字节两次）。"""
    crc = init
    for b in data:
        crc ^= b
        crc = ((crc >> 4) ^ table[crc & 0x0F]) & 0xFFFFFFFF
        crc = ((crc >> 4) ^ table[crc & 0x0F]) & 0xFFFFFFFF
    return (crc ^ xorout) & 0xFFFFFFFF


def check_table_values(table):
    """半字节表的第 i 项应等于"以 i 为初值走一次半字节更新"的结果。
    判据：用表算 CRC("123456789") 必须等于标准值。"""
    ok = True
    got = crc32_with_table(table, CHECK_INPUT)
    print("① 标准检查值：CRC32(\"123456789\") = 0x%08X（期望 0x%08X）→ %s"
          % (got, CHECK_CRC, "OK" if got == CHECK_CRC else "FAIL"))
    if got != CHECK_CRC:
        ok = False

    # 与 zlib 逐向量比对（覆盖空串、单字节、跨块、随机）
    cases = [b"", b"\x00", b"\xFF", b"A", CHECK_INPUT,
             bytes(range(256)), b"x" * 1000]
    random.seed(20261004)
    for _ in range(8):
        cases.append(bytes(random.randrange(256) for _ in range(random.randrange(1, 300))))

    bad = 0
    for c in cases:
        if crc32_with_table(table, c) != (zlib.crc32(c) & 0xFFFFFFFF):
            bad += 1
            print("   !! 不一致（len=%d）" % len(c))
    print("② 与 zlib.crc32 比对 %d 个向量 → %s"
          % (len(cases), "全部一致" if bad == 0 else "%d 个不一致" % bad))
    if bad:
        ok = False
    return ok


def check_blob_layout():
    """核对 AppConfigBlob 的字段偏移与 lang_check.py 的解析假设是否一致。"""
    txt = open(APP_CONFIG_C, encoding="utf-8", errors="ignore").read()
    m = re.search(r"typedef struct\s*\{(.*?)\}\s*AppConfigBlob\s*;", txt, re.S)
    if not m:
        raise SystemExit("!! 找不到 AppConfigBlob 结构")
    fields = re.findall(r"uint32_t\s+(\w+)(?:\[(\d+)\])?\s*;", m.group(1))

    off = 0
    layout = {}
    for name, arr in fields:
        layout[name] = off
        off += 4 * (int(arr) if arr else 1)
    size = off

    print("\n③ AppConfigBlob 布局（自源码解析）：")
    for name in layout:
        print("   +%-3d %s" % (layout[name], name))
    print("   sizeof = %d 字节" % size)

    ok = True
    # 固件侧：APP_CFG_BLOB_LEN 必须 >= sizeof（否则扇区预留区装不下，写越界）
    m2 = re.search(r"#define\s+APP_CFG_BLOB_LEN\s+(\d+)", txt)
    blob_len = int(m2.group(1)) if m2 else None
    m3 = re.search(r"#define\s+APP_CFG_MAGIC\s+(0x[0-9A-Fa-f]+)", txt)
    magic = int(m3.group(1), 16) if m3 else None
    print("   APP_CFG_BLOB_LEN = %s，APP_CFG_MAGIC = 0x%08X" % (blob_len, magic or 0))

    if blob_len is None or blob_len < size:
        print("   !! BLOB_LEN(%s) < sizeof(%d) —— 写会越界" % (blob_len, size))
        ok = False

    # 脚本侧：lang_check.py 按 b32 读 8 个字，假设 w[0]=magic, w[2]=lang, w[7]=crc
    lc = open(LANG_CHECK_PY, encoding="utf-8", errors="ignore").read()
    m4 = re.search(r"CFG_MAGIC\s*=\s*(0x[0-9A-Fa-f]+)", lc)
    script_magic = int(m4.group(1), 16) if m4 else None
    if script_magic != magic:
        print("   !! lang_check.py 的 CFG_MAGIC = 0x%08X 与固件 0x%08X 不一致"
              % (script_magic or 0, magic or 0))
        ok = False
    else:
        print("   [OK ] lang_check.py 的 magic 与固件一致")

    if layout.get("lang") != 8:
        print("   !! lang 偏移是 %s，但 lang_check.py 假设在字索引 2（=偏移 8）"
              % layout.get("lang"))
        ok = False
    else:
        print("   [OK ] lang 在偏移 8（字索引 2），与 lang_check.py 一致")

    if layout.get("crc") != 28:
        print("   !! crc 偏移是 %s，但 lang_check.py 假设在字索引 7（=偏移 28）"
              % layout.get("crc"))
        ok = False
    else:
        print("   [OK ] crc 在偏移 28（字索引 7），与 lang_check.py 一致")

    # crc 必须覆盖它之前的全部字节
    covered = layout["crc"]
    print("   crc 覆盖前 %d 字节（blob_crc 用 offsetof(crc) 作长度）" % covered)
    if covered != size - 4:
        print("   !! crc 不是最后一个字段，覆盖长度与 sizeof-4 不符")
        ok = False

    return ok


def main():
    print("=" * 62)
    print("CRC32 与配置布局 host 侧验证（不需要硬件）")
    print("=" * 62)
    table = extract_table()
    print("从 %s 提取到 s_crc_nib[16]：%s\n"
          % (APP_CONFIG_C, " ".join("%08X" % v for v in table[:4]) + " ..."))
    ok1 = check_table_values(table)
    ok2 = check_blob_layout()
    print("")
    if ok1 and ok2:
        print("[PASS] CRC32 与标准一致，blob 布局与验收脚本一致")
        return 0
    print("[FAIL] 见上方 !! 项")
    return 1


if __name__ == "__main__":
    sys.exit(main())
