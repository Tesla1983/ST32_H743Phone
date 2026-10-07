#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ime_probe.py —— 在板上跑一遍「拼音 → 候选 → 上屏」并读回结果。

    python tools/ime_probe.py nihao zhongguo shijie

做的事情：用 SWD 把拼音串写进 g_ime_pinyin，置 g_ime_test = 1，
等 phone_shell 的输入法自测状态机跑到结束，再把候选词和上屏后的文本读回来。
不需要碰屏，不需要重新烧录。

每一步的结果都是确定的整数/字符串，可以直接抄进验收表。
"""
import re
import struct
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
ELF = "build/ymgui-h743.elf"


def symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    tab = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                tab[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return tab


def rd32(addr, words):
    cmd = f"probe-rs read {PROBE} --chip {CHIP} b32 0x{addr:08x} {words}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    toks = [t for t in r.stdout.split()
            if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    if len(toks) < words:
        raise RuntimeError(f"读 0x{addr:08x} 失败: {(r.stderr or r.stdout).strip()}")
    return [int(t, 16) for t in toks[:words]]


def wr32(addr, words):
    # probe-rs 的 VALUES 只认十进制或带 0x 前缀的十六进制，这里统一走十进制
    values = " ".join(str(w) for w in words)
    cmd = f"probe-rs write {PROBE} --chip {CHIP} b32 0x{addr:08x} {values}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    if r.returncode != 0:
        raise RuntimeError(f"写 0x{addr:08x} 失败: {(r.stderr or r.stdout).strip()}")


def read_bytes(addr, nbytes):
    words = (nbytes + 3) // 4
    vals = rd32(addr, words)
    return b"".join(struct.pack("<I", v) for v in vals)[:nbytes]


def cstr(buf):
    return buf.split(b"\x00")[0].decode("utf-8", errors="replace")


def main():
    words = sys.argv[1:]
    if not words:
        print(__doc__)
        return 1

    tab = symtab()
    A_PY = tab["g_ime_pinyin"]
    A_TEST = tab["g_ime_test"]
    A_STEP = tab["g_ime_step"]
    A_RC = tab["g_ime_rc"]
    A_CAND = tab["PhoneIME_BoardCand"]
    A_CNT = tab["PhoneIME_BoardCandCount"]
    A_TEXT = tab["PhoneIME_BoardText"]

    ok = 0
    for py in words:
        payload = py.encode("utf-8")[:15]
        # 逐 4 字节写；不足补 0（含结尾 \0）
        buf = payload + b"\x00" * (4 - len(payload) % 4)
        wr32(A_PY, [int.from_bytes(buf[i:i + 4], "little")
                    for i in range(0, len(buf), 4)])
        wr32(A_TEST, [1])

        rc, step = 99, 0
        for _ in range(40):
            time.sleep(0.5)
            step = rd32(A_STEP, 1)[0]
            rc = rd32(A_RC, 1)[0]
            if step >= 6:
                break

        cnt = rd32(A_CNT, 1)[0]
        cand_raw = read_bytes(A_CAND, 24 * 6)
        cand = [cstr(cand_raw[i * 24:(i + 1) * 24]) for i in range(6)]
        cand = [c for c in cand if c][:6]
        text = cstr(read_bytes(A_TEXT, 192))

        tag = "OK  " if rc == 0 else "FAIL"
        print(f"[{tag}] '{py}'  step={step} rc={rc}  候选({cnt})={' '.join(cand)}")
        print(f"         上屏后文本 = {text!r}")
        if rc == 0:
            ok += 1

    print(f"\n成功 {ok}/{len(words)}")
    return 0 if ok == len(words) else 1


if __name__ == "__main__":
    sys.exit(main())
