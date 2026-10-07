#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""统计 phone_shell 里"含中文的字符串字面量"数量，用于估算 UI 中英切换的工作量。

只统计字面量（双引号内的内容），不统计注释 —— 因为注释不需要翻译。
"""
import glob
import re
import sys

STR = re.compile(r'"([^"\n]*)"')


def has_han(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def main():
    files = sorted(glob.glob("*.c") + glob.glob("apps/*.c") + glob.glob("*.h"))
    rows = []
    total = 0
    for f in files:
        try:
            txt = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        # 先去掉行注释与块注释，避免把注释里的中文算进来
        txt = re.sub(r"//[^\n]*", "", txt)
        txt = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
        n = sum(1 for m in STR.finditer(txt) if has_han(m.group(1)))
        if n:
            rows.append((n, f))
            total += n
    rows.sort(reverse=True)
    for n, f in rows:
        print("%5d  %s" % (n, f))
    print("-" * 40)
    print("%5d  合计（含中文的字符串字面量）" % total)
    print("%5d  文件数" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
