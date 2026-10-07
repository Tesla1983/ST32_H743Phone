#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 build/phrases_xip.bin（1.27 MB）灌进 W25Q128 的 IME 区（QSPI 偏移 0x300000）。

【为什么需要这个脚本】
词典 1.27 MB > 内部 flash 剩余 508 KB ⇒ 不能像字模那样"objcopy 内嵌 + 开机自灌"
（font_provision.c 就是那条路）；也 > 任何一块 RAM ⇒ 不能一次性送进去。
所以走**分块**：host --SWD--> SRAM1 暂存区（120 KB）--固件--> W25Q128，共 11 块。

【⚠ 数据通道为什么是 gdb 而不是 `probe-rs write`】
`probe-rs write b32` 把**每个 32 位字当一个命令行参数**传 —— 16 KB（4096 个值）
实测直接 WinError 206（命令行 >32 KB），8 KB 也一样超。Windows 上这条路不通。
所以数据块改走 `probe-rs gdb` 起的 RSP server + arm-none-eabi-gdb 的 `restore`
（走 RSP 协议，不受命令行长度限制）。

【⚠ 又为什么全程不碰 probe-rs CLI】
实测：`probe-rs` CLI 与 `probe-rs gdb` server **不能同时占用探针** —— CLI 一开，
server 那条连接立刻 "Target disconnected: error while reading"。所以本脚本在
server 存活期间**一条 probe-rs 命令都不发**，连状态字也用 gdb 的 `x` 读。
代价：每块都要把 server 起起停停（~10 s/块），所以暂存区做到 120 KB 把块数压到 11。

【前置条件】
    cmake -S . -B build -DYMGUI_IME_PHRASES_XIP=ON -DYMGUI_QSPI_PROVISION=ON
    cmake --build build
    probe-rs download --probe <P> --chip STM32H743VITx build/ymgui-h743.elf
    probe-rs reset    --probe <P> --chip STM32H743VITx
    python tools/qspi_provision.py

【判据】
    ① 每块 qspi_write 返回 0（最后读 g_prov_state == 2、g_prov_rc == 0）；
    ② 灌完后固件**从 XIP 读回**整段算 CRC32，与 host 侧 zlib.crc32 逐字节相等。
       走 XIP 而不是 indirect 读是刻意的：要验的是"运行时那条读路径通不通"。
    ③ 最后 probe-rs reset，让 PhoneIME_Init 重新 loadPhraseDictionary。

用法
----
    python tools/qspi_provision.py                  # 灌 + 复核
    python tools/qspi_provision.py --verify-only    # 只做 CRC 复核（不写）
    python tools/qspi_provision.py --start 614400   # 从断点续灌
"""

import argparse
import os
import re
import subprocess
import sys
import time
import zlib

ELF = "build/ymgui-h743.elf"
BIN = "build/phrases_xip.bin"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
GDB = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-gdb.exe"
GDB_PORT = 1337          # probe-rs gdb 的默认端口（**不是**常见的 3333）
SERVER_UP_S = 7.0        # server 起来到能接受连接的等待

QSPI_IME_OFF = 0x300000  # 词典在 W25Q128 里的偏移（计划书 §6.4 IME 区）

_SYMTAB = {}


def nm(name):
    if not _SYMTAB:
        out = subprocess.run([NM, ELF], capture_output=True, text=True,
                             encoding="utf-8", errors="ignore").stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3:
                try:
                    _SYMTAB[p[2]] = int(p[0], 16)
                except ValueError:
                    pass
    if name not in _SYMTAB:
        raise SystemExit("ELF 里找不到符号 %s（忘了开 YMGUI_QSPI_PROVISION？）" % name)
    return _SYMTAB[name]


def kill_server():
    subprocess.run("taskkill /F /IM probe-rs.exe", shell=True,
                   capture_output=True, text=True, encoding="utf-8", errors="ignore")
    time.sleep(1.0)


RESUME = ["detach"]
"""⚠ 让目标恢复运行靠的是**杀掉 gdb server**，不是 detach（实测 detach 不会恢复）。
所以每块发完之后必须先 kill server，再用 probe-rs 轮询 g_prov_state 等到 2，
然后才起下一块 —— 只靠"两次会话之间那 1 秒"是不够的：120 KB 要擦 30 个扇区，
固件根本来不及写完（第一轮实测 g_prov_chunks 恒为 0 就是这个原因）。"""


def gdb(ex_cmds, retries=2):
    """起 server → 跑一串 gdb -ex → 停 server。返回 gdb 的 stdout。
    调用方只需要给"要做什么"，恢复运行由本函数统一补在最后。"""
    for attempt in range(retries + 1):
        kill_server()
        srv = subprocess.Popen(
            "probe-rs gdb %s --chip %s" % (PROBE, CHIP), shell=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(SERVER_UP_S)
            cmd = [GDB, "-batch"]
            for e in list(ex_cmds) + RESUME:
                cmd += ["-ex", e]
            cmd.append(ELF)
            r = subprocess.run(cmd, capture_output=True, text=True,
                               encoding="utf-8", errors="ignore",
                               timeout=300)
            out = (r.stdout or "") + (r.stderr or "")
            if "Remote communication error" not in out and "detached" in out:
                return out
            last = out
        finally:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except Exception:
                pass
            kill_server()
    raise SystemExit("gdb 连不上（%d 次都失败）。\n最后一次输出：\n%s"
                     % (retries + 1, last.strip()[:600]))


def pr_wr32(name, val):
    """probe-rs 写。⚠ 只能在 **gdb server 没在跑** 时用（两者不能同时占探针）。"""
    a = nm(name)
    r = subprocess.run("probe-rs write %s --chip %s b32 0x%08x %d" % (PROBE, CHIP, a, val),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    if r.returncode != 0:
        raise SystemExit("写 %s 失败：%s" % (name, (r.stderr or r.stdout).strip()))


def pr_rd32(name):
    a = nm(name)
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1" % (PROBE, CHIP, a),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    for t in r.stdout.split():
        if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower()):
            return int(t, 16)
    raise SystemExit("读 %s 失败：%s" % (name, (r.stderr or r.stdout).strip()))


def wait_block(timeout=90.0):
    """等到这一块写完：state 2 = 成功，3 = 失败。"""
    t0 = time.time()
    st = 0
    while time.time() - t0 < timeout:
        st = pr_rd32("g_prov_state")
        if st in (2, 3):
            return st
        time.sleep(0.5)
    print("  [超时] state=%d" % st)
    return st


def gdb_read_u32(sym):
    out = gdb(["set confirm off",
               "target remote localhost:%d" % GDB_PORT,
               "x/1xw 0x%08x" % nm(sym)])
    m = re.search(r"0x[0-9a-f]+[^:]*:\s*0x([0-9a-f]{8})", out)
    if not m:
        raise SystemExit("读 %s 失败：%s" % (sym, out.strip()[:300]))
    return int(m.group(1), 16)


def gdb_write_u32(sym, val):
    gdb(["set confirm off",
         "target remote localhost:%d" % GDB_PORT,
         "set *(unsigned int*)0x%08x = %d" % (nm(sym), val)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=BIN)
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=QSPI_IME_OFF)
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--start", type=int, default=0, help="从载荷的第几个字节开始（断点续灌）")
    ap.add_argument("--max", type=int, default=0, help="本批最多灌多少字节（0 = 灌到尾）")
    a = ap.parse_args()

    if not os.path.isfile(a.bin):
        raise SystemExit("缺 %s —— 先跑 python tools/gen_ime_dict.py" % a.bin)

    data = open(a.bin, "rb").read()
    print("载荷 %s：%d B = %.1f KB" % (os.path.basename(a.bin), len(data), len(data) / 1024.0))

    enabled = gdb_read_u32("g_prov_enabled")
    if enabled != 1:
        raise SystemExit("固件没编入灌库支持（g_prov_enabled=%d）—— 请用 "
                         "-DYMGUI_QSPI_PROVISION=ON 重新编译并烧录" % enabled)
    buf_addr = gdb_read_u32("g_prov_buf_addr")
    buf_size = gdb_read_u32("g_prov_buf_size")
    print("灌库支持：已编入；暂存区 0x%08x / %d B" % (buf_addr, buf_size))

    if not a.verify_only:
        # 计数器清零：上一轮（或中途失败重来）的残留会让报表读起来像成功了
        for s_ in ("g_prov_chunks", "g_prov_done", "g_prov_erase", "g_prov_state"):
            pr_wr32(s_, 0)
        t0 = time.time()
        done = a.start
        stop = len(data) if a.max <= 0 else min(len(data), a.start + a.max)
        print("本批：文件偏移 %d → %d（共 %d B），每块 %d B"
              % (done, stop, len(data), min(buf_size, len(data))))
        while done < stop:
            n = min(buf_size, len(data) - done, stop - done)
            gdb(["set confirm off",
                 "target remote localhost:%d" % GDB_PORT,
                 "restore %s binary 0x%x %d %d" % (a.bin, buf_addr - done, done, done + n),
                 "set *(unsigned int*)0x%08x = %d" % (nm("g_prov_len"), n),
                 "set *(unsigned int*)0x%08x = %d" % (nm("g_prov_addr"), a.offset + done),
                 "set *(unsigned int*)0x%08x = 1" % nm("g_prov_cmd")])
            st = wait_block()
            if st != 2:
                raise SystemExit("第 %d 块失败：state=%d rc=%d"
                                 % (done // buf_size, st, pr_rd32("g_prov_rc")))
            done += n
            print("  已下发 %6d / %d B（%.0f%%）  %.0f s"
                  % (done, len(data), done * 100.0 / len(data), time.time() - t0))
        print("下发完成：%.0f s" % (time.time() - t0))

    print("== 收尾状态 ==")
    for s_ in ("g_prov_state", "g_prov_rc", "g_prov_chunks", "g_prov_done", "g_prov_erase"):
        print("  %-16s = %d" % (s_, pr_rd32(s_)))

    print("== 端到端复核：固件从 XIP 读回算 CRC32 ==")
    pr_wr32("g_prov_crc", 0)
    pr_wr32("g_prov_addr", a.offset)
    pr_wr32("g_prov_len", len(data))
    for s_ in ("g_prov_chunks", "g_prov_done", "g_prov_erase", "g_prov_state"):
        pass
    pr_wr32("g_prov_verify", 1)
    for _ in range(40):
        time.sleep(0.5)
        crc = pr_rd32("g_prov_crc")
        if crc != 0:
            break
    want = zlib.crc32(data) & 0xFFFFFFFF
    print("  板上 XIP CRC = 0x%08x" % crc)
    print("  host   CRC32 = 0x%08x" % want)
    if crc != want:
        print("  [FAIL] 不一致 —— XIP 读回的内容与写入的不符")
        return 1
    print("  [OK] 一致（%d 字节走 XIP 路径读回）" % len(data))

    print()
    print("下一步：probe-rs reset 让 PhoneIME_Init 重新加载词典，")
    print("        然后跑 python tools/ime_bench.py 量按键耗时。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
