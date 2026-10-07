#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""按行列出指定文件里"含中文的字符串字面量"，附带行号，用于整理翻译表。

用法：python tools/list_cn_strings.py <文件> [<文件> ...]
"""
import re
import sys

STR = re.compile(r'"([^"\n]*)"')


def has_han(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def main():
    for fn in sys.argv[1:]:
        try:
            lines = open(fn, encoding="utf-8", errors="ignore").read().splitlines()
        except OSError as e:
            print("!! %s: %s" % (fn, e))
            continue
        print("=" * 60)
        print(fn)
        print("=" * 60)
        for i, line in enumerate(lines, 1):
            code = re.sub(r"//.*$", "", line)
            found = [m.group(1) for m in STR.finditer(code) if has_han(m.group(1))]
            if found:
                for s in found:
                    print("%5d  %s" % (i, s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
