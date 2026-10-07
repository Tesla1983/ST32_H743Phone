#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""校验 T() 的实参是否都存在于翻译表里 —— 纯静态检查，不需要硬件。

【为什么需要这个检查】
  `PhoneLang_Tr()` 查不到 key 时**返回中文原文**（刻意兜底：宁可显示中文也不要空白）。
  好处是健壮，坏处是：**调用点写错一个字，英文模式下就静默显示中文** ——
  不报错、不崩溃、运行时验证也很难发现（除非逐个目视）。
  本脚本把源码里所有 `T(...)` 里出现的**字符串字面量**抽出来，与翻译表 key 做集合比对，
  一次把这类拼写不一致全抓出来。

【实现要点：为什么不用简单正则】
  初版用 T\("..."\) 抓字面量、再用 T\([^()]*\? "A" : "B"\) 抓三目，
  结果**漏掉了 `T(PhoneHost_GetWifi() ? "A" : "B")`** —— 因为 `[^()]*` 不允许括号，
  而条件是函数调用。表现为"三目项 0 个"、且把 `"未连接"` 误报成"表中未引用"。
  ⇒ 改用**词法扫描**：正确跟踪字符串与注释，再用括号配对取出每个 T(...) 的完整实参，
  然后在实参里提取所有字符串字面量。这样对任何写法都成立：
      T("字面量")                              → 1 个 key
      T(cond() ? "A" : "B")                    → 2 个 key
      T(PhoneApps_Get(id)->title)              → 0 个（动态，另行核对）
      snprintf(buf, n, T("%s\n模块：%s"), ...)  → 1 个 key（模板）

用法
----
    python tools/check_lang_keys.py
退出码 0 = 完全一致；1 = 有缺失（真 bug，英文模式会静默显示中文）；2 = 仅有冗余条目
"""
import glob
import os
import re
import sys

PHONE_SHELL = "third_party/YMGUI/project_Demo/phone_shell"


def scan_T_args(text):
    """词法扫描，取出所有 T(...) 的实参文本（正确处理字符串与注释）。"""
    args = []
    i, n = 0, len(text)

    while i < n:
        c = text[i]

        # 行注释
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        # 块注释
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # 字符串（跳过，避免字符串里的 T( 被误认）
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        # 字符字面量
        if c == "'":
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
            continue

        # 识别独立的标识符 T(  （前面不能是标识符字符，避免匹配到 xxxT(）
        if (c == "T" and i + 1 < n and text[i + 1] == "("
                and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"))):
            start = i + 2
            depth = 1
            j = start
            while j < n and depth > 0:
                ch = text[j]
                if ch == '"':                       # 跳过字符串
                    j += 1
                    while j < n:
                        if text[j] == "\\":
                            j += 2
                            continue
                        if text[j] == '"':
                            j += 1
                            break
                        j += 1
                    continue
                if ch == "/" and j + 1 < n and text[j + 1] == "/":
                    k = text.find("\n", j)
                    j = n if k < 0 else k + 1
                    continue
                if ch == "/" and j + 1 < n and text[j + 1] == "*":
                    k = text.find("*/", j + 2)
                    j = n if k < 0 else k + 2
                    continue
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            args.append(text[start:j])
            i = j + 1
            continue

        i += 1
    return args


def unescape(s):
    return (s.replace("\\n", "\n").replace("\\t", "\t")
             .replace('\\"', '"').replace("\\\\", "\\"))


def literals_in(arg_text):
    """实参里出现的所有字符串字面量（已还原转义）。"""
    out = []
    i, n = 0, len(arg_text)
    while i < n:
        if arg_text[i] == '"':
            i += 1
            buf = []
            while i < n:
                if arg_text[i] == "\\" and i + 1 < n:
                    buf.append(arg_text[i:i + 2])
                    i += 2
                    continue
                if arg_text[i] == '"':
                    i += 1
                    break
                buf.append(arg_text[i])
                i += 1
            out.append(unescape("".join(buf)))
            continue
        i += 1
    return out


def collect_calls():
    """返回 (字面量key→来源文件列表, 动态实参个数, 含字面量的T调用次数, T调用总次数)"""
    found = {}
    dynamic = 0
    lit_calls = 0
    total_calls = 0
    files = (sorted(glob.glob(os.path.join(PHONE_SHELL, "*.c")))
             + sorted(glob.glob(os.path.join(PHONE_SHELL, "apps/*.c")))
             + sorted(glob.glob("src/*.c")))
    for fn in files:
        try:
            txt = open(fn, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for arg in scan_T_args(txt):
            total_calls += 1
            lits = literals_in(arg)
            if not lits:
                dynamic += 1
                continue
            lit_calls += 1
            for k in lits:
                found.setdefault(k, []).append(os.path.basename(fn))
    return found, dynamic, lit_calls, total_calls


def collect_dict():
    path = os.path.join(PHONE_SHELL, "phone_lang.c")
    txt = open(path, encoding="utf-8", errors="ignore").read()
    body = txt.split("s_dict[] =")[1].split("\n};")[0] if "s_dict[] =" in txt else txt
    entries = {}
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', body):
        entries[unescape(m.group(1))] = unescape(m.group(2))
    return entries


def main():
    calls, dynamic, lit_calls, total_calls = collect_calls()
    table = collect_dict()

    # 16 个应用 title 通过动态项 T(PhoneApps_Get(id)->title) 取用，
    # 静态分析无法把"动态实参"关联到具体取值 ⇒ 先把它们收集起来，
    # 后面既要用它们做 ① 的存在性核对，也要用来把它们从"未引用"里剔除（避免误报）。
    titles = []
    for fn in sorted(glob.glob(os.path.join(PHONE_SHELL, "apps/*.c"))):
        txt = open(fn, encoding="utf-8", errors="ignore").read()
        for m in re.finditer(r'\.title\s*=\s*"([^"]+)"', txt):
            titles.append(m.group(1))
    dynamic_covered = set(titles) & set(table)

    print("T() 调用总次数           = %d" % total_calls)
    print("  · 含字面量的调用       = %d（共 %d 种不同的 key）" % (lit_calls, len(calls)))
    print("  · 动态实参调用         = %d（如 T(PhoneApps_Get(id)->title)）" % dynamic)
    print("翻译表条目数             = %d" % len(table))
    print("")

    missing = sorted(k for k in calls if k not in table)
    unused = sorted(k for k in table if k not in calls and k not in dynamic_covered)
    rc = 0

    if missing:
        print("!! 源码用了但表里没有（英文模式下会静默显示中文）：")
        for k in missing:
            print("   %-46s ← %s" % ('"%s"' % k.replace("\n", "\\n"),
                                      ", ".join(sorted(set(calls[k])))))
        rc = 1
    else:
        print("[OK ] T() 里所有字面量 key 都能在表里查到")

    print("")
    if unused:
        print("提示：表里有但未引用（改名后忘了删 / 为动态项预留）：")
        for k in unused:
            print("   %s" % ('"%s"' % k.replace("\n", "\\n")))
        if rc == 0:
            rc = 2
    else:
        print("[OK ] 表里没有死条目（动态项覆盖的 %d 个 title 已排除）" % len(dynamic_covered))

    # ---- 动态项核对 ----
    print("\n---- 动态项核对 ----")
    dyn_ok = True
    bad_t = [t for t in titles if t not in table]
    print("① 应用 title %d 个（经 T(PhoneApps_Get(id)->title) 取用）" % len(titles))
    if bad_t:
        print("   !! 不在表里：%s" % bad_t)
        dyn_ok = False
    else:
        print("   [OK ] 全部在表里")
    print("② 其余动态项：T(PhoneHost_GetWifi() ? \"A\" : \"B\") 形式的两支已按字面量收进上面比对")

    if not dyn_ok and rc == 0:
        rc = 1

    # ---- 自检：扫描器计数必须自洽 ----
    print("\n③ 扫描器自检：%d(字面量调用) + %d(动态调用) = %d，总调用 %d → %s"
          % (lit_calls, dynamic, lit_calls + dynamic, total_calls,
             "一致" if lit_calls + dynamic == total_calls else "不一致!!"))

    print("")
    if rc == 0:
        print("[PASS] 翻译表与调用点完全一致")
    elif rc == 1:
        print("[FAIL] 存在 key 缺失 —— 英文模式下会静默显示中文，必须修")
    else:
        print("[PASS*] 无缺失；仅有未引用条目（不阻塞，见提示）")
    return rc


if __name__ == "__main__":
    sys.exit(main())
