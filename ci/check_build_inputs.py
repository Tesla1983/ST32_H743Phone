#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建输入完整性闸门：确认那两个"objcopy 进固件"的 .bin 真的在仓库里、且大小对得上。

为什么单独设一道：
  这两个 .bin 是**被 .gitignore 排除规则的反向例外**救回来的（根目录的 `*.bin`
  规则会把它们一起吃掉）。一旦有人改 .gitignore 或者从别的地方 clone，
  最容易发生的不是编译报错（CMake 确实会报，但错误信息很晦涩），
  而是"某台机器上少一步 by-product 导致的莫名失败"。所以这里用**显式、确定性**的判据把这条约定钉住。

判据（确定性整数，不用"看起来差不多"）：
  - 文件必须存在
  - 字节数必须精确等于下表的期望值（字库 982016、IME 词典 116350）

用法：
  python ci/check_build_inputs.py
"""

import os
import sys

EXPECTED = [
    # (相对仓库根的路径, 精确字节数, 说明)
    ("third_party/YMGUI/tools/gb2312_glyphs.bin", 982016,
     "GB2312 字模载荷，objcopy 进 .rodata.gb2312"),
    ("third_party/YMGUI/project_Demo/chinese_ime/pinyin_gb2312.bin", 116350,
     "拼音单字词典，objcopy 进 .rodata.ime_chars"),
]


def main():
    bad = 0
    print("构建输入完整性检查：%d 项" % len(EXPECTED))
    for rel, want, note in EXPECTED:
        if not os.path.isfile(rel):
            bad += 1
            print("  [MISS] %s —— %s" % (rel, note))
            continue
        size = os.path.getsize(rel)
        flag = "OK  " if size == want else "BAD "
        if size != want:
            bad += 1
        print("  [%s] %s  %d 字节（期望 %d）  %s" % (flag, rel, size, want, note))

    if bad:
        print()
        print("[FAIL] %d/%d 项有问题。" % (bad, len(EXPECTED)))
        print("       这两个 .bin 必须入库：根目录 .gitignore 里 `*.bin` 的反向例外负责这件事，")
        print("       详见 third_party/YMGUI/VENDOR_NOTE.md。")
        return 1

    print()
    print("[PASS] %d/%d 项齐全且大小精确一致。" % (len(EXPECTED), len(EXPECTED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
