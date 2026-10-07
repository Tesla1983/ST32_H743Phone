#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
可复现性闸门：拿构建产物和参考哈希比对。

为什么单独写成脚本而不是 workflow 里一行 shell：
  目标是**确定性判据**，并且要求输出足够 self-explanatory —— 红色的那一行
  必须直接告诉值班的人"是哪一个文件不一样、差在哪、下一步该看什么"，
  而不是只看到 "exit code 1"。

判据：
  - 每个参考项都必须对上，任一对不上 ⇒ 退出码 1
  - 缺失文件（构建没产出）⇒ 退出码 1，且明确报"缺失"而不是"哈希不同"
  两者病因完全不同，不要混为一谈。

用法：
  python ci/check_firmware_hash.py <build_dir> ci/firmware_reference.sha256
"""

import hashlib
import os
import sys


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_reference(ref_path):
    items = []
    with open(ref_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            items.append((parts[0], parts[-1]))
    return items


def main():
    if len(sys.argv) != 3:
        print("用法: python ci/check_firmware_hash.py <build_dir> <reference.sha256>")
        return 2
    build_dir, ref_path = sys.argv[1], sys.argv[2]

    if not os.path.isfile(ref_path):
        print("[FAIL] 找不到参考哈希文件：%s" % ref_path)
        return 1

    items = load_reference(ref_path)
    if not items:
        print("[FAIL] 参考哈希文件里没有任何条目：%s" % ref_path)
        return 1

    bad = 0
    print("可复现性检查：%d 项" % len(items))
    for want, name in items:
        path = os.path.join(build_dir, name)
        if not os.path.isfile(path):
            bad += 1
            print("  [MISS] %s —— %s 不存在（构建失败或产物目录不对）" % (name, path))
            continue
        got = sha256_of(path)
        size = os.path.getsize(path)
        if got == want:
            print("  [OK  ] %s  sha256=%s  size=%d" % (name, got, size))
        else:
            bad += 1
            print("  [DIFF] %s  size=%d" % (name, size))
            print("         期望 %s" % want)
            print("         实得 %s" % got)

    if bad:
        print()
        print("[FAIL] %d/%d 项与参考不一致。" % (bad, len(items)))
        print("       下一步：若没人改构建参数，优先怀疑工具链版本漂移（查 TOOLCHAIN_VERSION）；")
        print("               若恰刚升级了工具链或有意为之的功能改动，更新 ci/firmware_reference.sha256")
        print("               并在 commit message 里写清原因，规则见 ci/README.md。")
        return 1

    print()
    print("[PASS] %d/%d 项字节级一致 —— CI 产物与板上固件相同。" % (len(items), len(items)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
