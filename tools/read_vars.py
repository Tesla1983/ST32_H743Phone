#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
read_vars.py —— 通过 probe-rs 从运行中的目标板上读一组全局诊断变量。

用法：
    python tools/read_vars.py g_boot_magic g_shell_rc g_fault ...

原理：先用 arm-none-eabi-nm 从 ELF 取到符号地址，把相邻（间距 <= GAP）的变量
合并成一次 probe-rs read（连接一次 probe 要几秒，逐符号读太慢），最后按符号
偏移切回各自的 32 位值。

注意：probe-rs read 只能读 RAM。放在 FLASH 里的 const 读不到，脚本会明确报错
提示，不会假装成功。
"""
import subprocess
import sys

ELF = "build/ymgui-h743.elf"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
GAP = 64          # 相邻变量合并读的最大间距（字节）
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"


def load_symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    tab = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                tab[parts[2]] = (int(parts[0], 16), parts[1])
            except ValueError:
                pass
    return tab


def addr_of(tab, name):
    if name not in tab:
        raise KeyError(name)
    addr, typ = tab[name]
    if typ not in ("b", "B", "d", "D", "s", "S", "n"):
        raise KeyError(f"{name} 类型 '{typ}' 不是 RAM 变量（可能是 FLASH 里的 const）")
    return addr


def main():
    names = sys.argv[1:]
    if not names:
        print(__doc__)
        return 1

    tab = load_symtab()
    items = []
    for n in names:
        try:
            items.append((addr_of(tab, n), n))
        except KeyError as e:
            print(f"[SKIP] {e}")
    items.sort()

    groups = []
    for addr, name in items:
        if groups and addr - groups[-1]["end"] <= GAP:
            groups[-1]["end"] = addr
            groups[-1]["names"].append(name)
        else:
            groups.append({"start": addr, "end": addr, "names": [name]})

    for g in groups:
        start = g["start"] & ~3
        words = (g["end"] + 4 - start + 3) // 4
        cmd = f"probe-rs read {PROBE} --chip {CHIP} b32 0x{start:08x} {words}"
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                           encoding="utf-8", errors="ignore")
        hexwords = [t for t in r.stdout.split()
                    if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
        if len(hexwords) < words:
            print(f"[FAIL] 读 0x{start:08x} 失败: "
                  f"{(r.stderr or r.stdout).strip()}")
            continue
        for name in g["names"]:
            addr = addr_of(tab, name)
            val = int(hexwords[(addr - start) // 4], 16)
            print(f"{name:20s} @0x{addr:08x} = 0x{val:08x} ({val})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
