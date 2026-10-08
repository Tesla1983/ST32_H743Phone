#!/usr/bin/env python3
"""从目标板的全帧镜像缓冲读出 YMGUI 渲染结果，还原成 PNG。

与早期 Rust 参照工程的抓屏脚本同一思路，但这里是 YMGUI 移植工程：
flush_cb 每推一条 band 就把它累积进 AXI SRAM 起始的 320x480 全帧缓冲
（链接脚本里的 _frame_buf），所以读这一块即可得到"YMGUI 画出来的整屏"。

用法：
    python grab_ymgui.py [输出.png]

本工具**自己打开整帧镜像并强制重绘一帧**（见 enable_mirror_and_redraw），
调用方不必先把设备设成某个状态；反过来，若照旧只读 _frame_buf，在
"镜像默认关 + 画面静止"的出货配置下只会抓到开机旧值（全黑）。

不依赖第三方库（PNG 用 zlib + struct 手写）。
"""
import struct
import subprocess
import sys
import time
import zlib

PROBE = r"E:\.cargo\bin\probe-rs.exe"
CHIP = "STM32H743VITx"
# 本机常同时插着两个探针（FireDAP + ESP JTAG），不指定会进交互式选择而报错
PROBE_SEL = ["--probe", "0416:5021:0123456789AB"]

# 写变量要符号地址，故需要 nm（与 bench.py 同一份工具链路径）
NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
ELF = "build/ymgui-h743.elf"

W, H = 320, 480
FRAME_BUF = 0x2400_0000          # 链接脚本 _frame_buf（AXI SRAM 起始）


def sym(name):
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise RuntimeError("符号表里没有 " + name)


def wr32(name, val):
    r = subprocess.run([PROBE, "write", *PROBE_SEL, "--chip", CHIP, "b32",
                        hex(sym(name)), hex(val & 0xFFFFFFFF)],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        raise RuntimeError("写 %s 失败：%s%s" % (name, r.stdout, r.stderr))


def enable_mirror_and_redraw():
    """开整帧镜像并造一次整屏重绘 —— 读 _frame_buf 之前必须先做。

    ⚠ 两个都不能省（2026-10-04 实际踩过，抓出来是 526 字节纯黑 PNG）：
      1) 固件默认 YMGUI_PORT_FRAME_MIRROR=OFF（步骤 2 为省 2.54 ms/帧从出货构建
         摘掉了），镜像不开时 phone_flush 根本不往 _frame_buf 写；
      2) 画面静止时 YMGUI_Refresh 因"无脏区"直接早返回、根本不 flush，
         所以即使开了镜像也要 g_force_redraw 造一帧，否则缓冲里仍是开机旧值。
    本函数让抓屏工具自给自足，不必依赖调用方记得先开镜像。
    """
    wr32("g_frame_mirror", 1)
    wr32("g_force_redraw", 1)
    time.sleep(0.8)              # 等主循环跑满一帧（含 15 条 band 的 flush）


def rd(addr, words):
    out = subprocess.run(
        [PROBE, "read", *PROBE_SEL, "--chip", CHIP, "b32", hex(addr), str(words)],
        capture_output=True, text=True, timeout=600,
    )
    txt = (out.stdout or "").strip().splitlines()[-1] if (out.stdout or "").strip() else ""
    return [int(x, 16) for x in txt.split()]


def png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", hdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    nwords = (W * H + 1) // 2
    enable_mirror_and_redraw()   # ⚠ 见该函数注释：不开镜像/不强制重绘就只会抓到黑屏
    print(f"读取全帧缓冲 0x{FRAME_BUF:X}，{nwords} 字 ...")
    data = rd(FRAME_BUF, nwords)
    print(f"读到 {len(data)} 字")
    if len(data) < nwords:
        print("!! 读取字数不足")
        return 1

    # 一个 32 位字装两个像素，**低 16 位是前一个**（小端）
    px = []
    for word in data:
        px.append(word & 0xFFFF)
        if len(px) < W * H:
            px.append((word >> 16) & 0xFFFF)
    px = px[: W * H]

    uniq = {}
    for v in px:
        uniq[v] = uniq.get(v, 0) + 1
    top = sorted(uniq.items(), key=lambda kv: -kv[1])[:10]
    print("出现最多的颜色:", " ".join(f"0x{v:04X}x{c}" for v, c in top))
    print(f"不同颜色数: {len(uniq)}")

    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            v = px[y * W + x]
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            row += bytes((r, g, b))
        rows.append(bytes(row))

    out = sys.argv[1] if len(sys.argv) > 1 else "ymgui.png"
    png(out, W, H, rows)
    print("已写出", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
