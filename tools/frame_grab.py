#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""抓屏工具：把板上的镜像帧缓冲读回宿主机，存 PNG 并可选导出 ASCII art。

为什么需要它
------------
改字模（cell_w 8→9，修字母 W 被裁）这类改动，**A2 逐像素自检只能证明"面板里的内容
与 YMGUI 渲染结果一致"**，证明不了"渲染出来的 W 真的不再是残缺的"——两条都走同一条
渲染路径，一起错也会一起对。要真判据只能把**真实像素**读回来肉眼看。

帧缓冲在 AXI SRAM：`_frame_buf = 0x24000000`，320×480×2 B（RGB565），共 307 200 B。
`g_frame_mirror = 1` 时 phone_flush 会把整帧拷进去（见 src/main.c）。

用法
----
    python tools/frame_grab.py                    # 抓屏 -> build/frame.png
    python tools/frame_grab.py --ascii 0 0 320 64 # 再把某块区域打成 ASCII art
    python tools/frame_grab.py --out x.png

注意
----
* 抓屏前必须先写 `g_frame_mirror = 1`，再**等 ≥ 2 拍渲染**（约 0.3 s）才读，
  否则读到的是上一帧或半帧（src/main.c:147 明确写了这个顺序）。
* probe-rs 一次读 76 800 个 32 位字，输出很大但可控（几秒）。
"""

import argparse
import os
import struct
import subprocess
import sys
import time

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"

# _frame_buf 的地址：linker/stm32h743vit6.ld 里 `= ORIGIN(AXI)`。
# 别硬编码成别的值；改链接脚本后要重新 `arm-none-eabi-nm build/ymgui-h743.elf | grep _frame_buf`。
FB_ADDR = 0x24000000
W, H = 320, 480
BPP = 2
WORDS = (W * H * BPP) // 4          # 76 800 个 32 位字

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd, timeout=180):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def nm_sym(name):
    """从 ELF 取符号地址，失败返回 None（此时调用方应该用已知常量兜底）。"""
    elf = os.path.join(REPO, "build", "ymgui-h743.elf")
    if not os.path.exists(elf):
        return None
    r = sh("arm-none-eabi-nm %s | grep -w %s" % (elf, name))
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def grab(mirror_addr):
    """写 g_frame_mirror=1 → 等两拍 → 读整帧，返回 bytes（长度 W*H*2）。"""
    if mirror_addr is not None:
        r = sh("probe-rs write %s --chip %s b32 0x%08x 0x00000001" % (PROBE, CHIP, mirror_addr))
        if r.returncode:
            print("[WARN] 写 g_frame_mirror 失败：%s%s" % (r.stdout, r.stderr))
        else:
            print("  已置 g_frame_mirror = 1")
    time.sleep(0.6)                              # ≥ 2 拍；0.3s 是最低位，取 0.6 留余量

    print("  读取 0x%08x 处 %d 个 32 位字（%.0f KB），约需 45 秒..." % (FB_ADDR, WORDS, WORDS * 4 / 1024.0))
    # 走 -o/-f binary 直接落盘：
    #   ① 比解析 76 800 个十六进制 token 快且稳；
    #   ② 首次尝试用 stdout 解析时曾把 --probe 的值当成 <WIDTH>（参数顺序错），
    #      落盘版不受输出文本格式影响。
    # ⚠ 格式名是 binary（不是 bin），写错会报 "a similar value exists: 'binary'"。
    tmp = os.path.join(REPO, "build", "frame.bin")
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    r = sh("probe-rs read %s --chip %s b32 0x%08x %d -o %s -f binary"
           % (PROBE, CHIP, FB_ADDR, WORDS, tmp), timeout=600)
    if r.returncode:
        raise RuntimeError("读帧缓冲失败：%s%s" % (r.stdout, r.stderr))
    raw = open(tmp, "rb").read()
    if len(raw) != W * H * 2:
        raise RuntimeError("帧长度 %d ≠ 期望 %d" % (len(raw), W * H * 2))
    return raw


def to_png(raw, path):
    """RGB565 小端 → PNG。没有 PIL 就退回写 PPM（也能看）。"""
    try:
        from PIL import Image
    except ImportError:
        # PPM 兜底：P6 + 24bit RGB
        with open(path.replace(".png", ".ppm"), "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (W, H))
            for i in range(0, W * H * 2, 2):
                px = raw[i] | (raw[i + 1] << 8)
                r5, g6, b5 = (px >> 11) & 0x1F, (px >> 5) & 0x3F, px & 0x1F
                f.write(bytes(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))))
        print("  未装 PIL，已写 PPM：%s" % path.replace(".png", ".ppm"))
        return
    img = Image.new("RGB", (W, H))
    buf = bytearray(W * H * 3)
    for i in range(0, W * H * 2, 2):
        px = raw[i] | (raw[i + 1] << 8)
        r5, g6, b5 = (px >> 11) & 0x1F, (px >> 5) & 0x3F, px & 0x1F
        j = (i // 2) * 3
        buf[j] = (r5 << 3) | (r5 >> 2)
        buf[j + 1] = (g6 << 2) | (g6 >> 4)
        buf[j + 2] = (b5 << 3) | (b5 >> 2)
    img.frombytes(bytes(buf))
    img.save(path)
    print("  已写 PNG：%s（%d×%d）" % (path, W, H))


def ascii_art(raw, x0, y0, w, h):
    """把一块区域打成 ASCII art（越亮=越接近背景色）。

    判据是形状而不是颜色：文字通常在深色/浅色背景上，这里按"与左上角像素的差异"
    分档，能同时适应深底浅字和浅底深字。
    """
    RAMP = " .:-=+*#%@"
    base = None
    lines = []
    for y in range(y0, min(y0 + h, H)):
        row = []
        for x in range(x0, min(x0 + w, W)):
            i = (y * W + x) * 2
            px = raw[i] | (raw[i + 1] << 8)
            r5, g6, b5 = (px >> 11) & 0x1F, (px >> 5) & 0x3F, px & 0x1F
            lum = (r5 * 8 + g6 * 4 + b5 * 8) // 3      # 0..255 粗亮度
            if base is None:
                base = lum
            d = min(abs(lum - base), 255)
            row.append(RAMP[min(d * len(RAMP) // 128, len(RAMP) - 1)])
        lines.append("".join(row))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/frame.png")
    ap.add_argument("--ascii", nargs=4, type=int, default=None,
                    metavar=("X", "Y", "W", "H"),
                    help="额外把这块区域打成 ASCII art")
    ap.add_argument("--no-mirror", action="store_true",
                    help="不写 g_frame_mirror（它已经是 1 时用它省一步）")
    args = ap.parse_args()

    addr = None if args.no_mirror else nm_sym("g_frame_mirror")
    if addr is None and not args.no_mirror:
        addr = 0x20001078      # ELF 不在时的兜底（2026-10-07 实测值）
        print("  ELF 里没找到 g_frame_mirror，用兜底地址 0x%08x" % addr)

    os.chdir(REPO)
    raw = grab(addr)
    out = args.out
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    # 缩放到 2 倍便于肉眼查看（320×480 在屏幕上偏小）
    try:
        from PIL import Image
        to_png(raw, out)
        im = Image.open(out)
        im.resize((W * 2, H * 2), Image.NEAREST).save(out.replace(".png", "_2x.png"))
        print("  已写放大版：%s" % out.replace(".png", "_2x.png"))
    except Exception as e:                       # PIL 缺失时 to_png 已退回 PPM
        to_png(raw, out)

    if args.ascii:
        x0, y0, w, h = args.ascii
        print("\n=== 区域 (%d,%d) %d×%d ===" % (x0, y0, w, h))
        for line in ascii_art(raw, x0, y0, w, h):
            print("  |%s|" % line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
