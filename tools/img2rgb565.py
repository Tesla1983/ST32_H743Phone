#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把常见图片转成 **RGB565 裸数据**（可直接灌进 W25Q128 供 XIP 直读）。

为什么需要它
------------
板上**没有外部 RAM**（原理图无 SDRAM/PSRAM 专用脚，FMC 只接 LCD），
而相册面板 284×230 的全尺寸 RGB565 需要 130 640 B，超过 heap1 的 115 KB 余量。
但 `YMGUI_DrawImg.h:12` 里 `GYimg.data` 是 **`const GYpx*`** —— 只读指针，
所以可以指向 QSPI 的 XIP 地址 `0x90000000 + offset`，**解码/存放 RAM 开销为 0**。
本工具就是这条路的配套：在 PC 侧把图片转成固件能直接 blit 的裸像素。

输出格式
--------
默认**纯裸数据**，行优先、stride = w，每个像素 2 字节：

    值   = ((R>>3) << 11) | ((G>>2) << 5) | (B>>3)
    字节 = 小端（低字节在前）—— MCU 是 Cortex-M7 小端，读出来就是正确的 uint16_t

加 `--hdr` 时前面多 8 字节头：`"RGB5"` + u16 w + u16 h（小端）。

用法
----
    python tools/img2rgb565.py photo.jpg                 # 原尺寸输出 photo.rgb565
    python tools/img2rgb565.py a.png b.jpg --max 180     # 最长边压到 180（保持比例）
    python tools/img2rgb565.py a.jpg --w 240 --h 180     # 强制尺寸（会变形）
    python tools/img2rgb565.py a.png --fit 240x180       # 保持比例居中裁剪后缩放
    python tools/img2rgb565.py --bg '#000' a.png --hdr   # 透明处填黑 + 带头

判据（脚本自己会打，整数结论）
------------------------------
    ① 输出字节数 == w * h * 2（+8 若带头）
    ② 纯色探针：红/绿/蓝/白/黑 五个像素值 == F800 / 07E0 / 001F / FFFF / 0000
       —— 用 `--selftest` 跑，不依赖任何输入图片
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("需要 Pillow：pip install pillow\n")
    raise SystemExit(2)

MAGIC = b"RGB5"


def to_rgb565(img):
    """PIL RGB 图 -> RGB565 小端裸数据（bytes）。"""
    w, h = img.size
    px = img.convert("RGB").tobytes()          # 每像素 3 字节 R,G,B
    out = bytearray(w * h * 2)
    o = 0
    for i in range(0, len(px), 3):
        r = px[i]
        g = px[i + 1]
        b = px[i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[o] = v & 0xFF                      # 小端：低字节在前
        out[o + 1] = (v >> 8) & 0xFF
        o += 2
    return bytes(out)


def scale_fit(img, tw, th):
    """保持比例：等比放大到覆盖目标框后居中裁剪，再缩到目标尺寸（不留黑边）。"""
    w, h = img.size
    scale = max(tw / float(w), th / float(h))
    nw, nh = int(round(w * scale)), int(round(h * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    left = (nw - tw) // 2
    top = (nh - th) // 2
    return img.crop((left, top, left + tw, top + th))


def scale_max(img, m):
    """最长边压到 m，保持比例（不足时**不放大**）。"""
    w, h = img.size
    s = m / float(max(w, h))
    if s >= 1.0:
        return img
    return img.resize((int(round(w * s)), int(round(h * s))), Image.LANCZOS)


def flatten(img, bg):
    """带 alpha 的图贴到纯色背景上。返回 RGB 图。"""
    if img.mode not in ("RGBA", "LA", "P"):
        return img
    im = img.convert("RGBA")
    base = Image.new("RGBA", im.size, bg + (255,))
    base.alpha_composite(im)
    return base.convert("RGB")


def parse_bg(s):
    s = s.lstrip("#")
    if len(s) == 3:
        s = "".join(c * 2 for c in s)
    if len(s) != 6:
        raise argparse.ArgumentTypeError("背景色要写成 #RRGGBB 或 #RGB，例如 #000 / #ffffff")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def selftest():
    """用构造出来的纯色图验证编码正确性 —— 不依赖任何外部图片。"""
    probes = [((255, 0, 0), 0xF800), ((0, 255, 0), 0x07E0), ((0, 0, 255), 0x001F),
              ((255, 255, 255), 0xFFFF), ((0, 0, 0), 0x0000)]
    ok = True
    for (rgb, expect) in probes:
        im = Image.new("RGB", (1, 1), rgb)
        got = struct.unpack("<H", to_rgb565(im))[0]
        if got != expect:
            ok = False
            print("  [FAIL] %s -> 0x%04X（期望 0x%04X）" % (rgb, got, expect))
        else:
            print("  [OK]   %s -> 0x%04X" % (rgb, got))

    # 尺寸/长度：3×2 图，验证行优先与字节数
    im = Image.new("RGB", (3, 2), (255, 0, 0))
    d = to_rgb565(im)
    if len(d) != 3 * 2 * 2:
        ok = False
        print("  [FAIL] 3×2 图字节数 %d（期望 12）" % len(d))
    else:
        print("  [OK]   3×2 图字节数 12 == w*h*2")

    print("[%s] 纯色探针自检" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="图片 -> RGB565 裸数据（供 W25Q128 XIP 直读）")
    ap.add_argument("images", nargs="*", help="输入图片（jpg/png/bmp/...）")
    ap.add_argument("--w", type=int, default=0, help="输出宽（与 --h 同用，会变形）")
    ap.add_argument("--h", type=int, default=0, help="输出高")
    ap.add_argument("--max", type=int, default=0, help="最长边压到此值（保持比例，不放大）")
    ap.add_argument("--fit", default="", help="保持比例居中裁剪到 WxH，例如 240x180")
    ap.add_argument("--bg", type=parse_bg, default=(255, 255, 255), help="透明处填充色，默认 #fff")
    ap.add_argument("--hdr", action="store_true", help="输出前加 8 字节 RGB5 头")
    ap.add_argument("--outdir", default="", help="输出目录（默认与输入同目录）")
    ap.add_argument("--selftest", action="store_true", help="只跑编码正确性自检，不需要图片")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.images:
        ap.error("至少要给一张图片，或用 --selftest 跑自检")

    if args.fit:
        try:
            fw, fh = (int(x) for x in args.fit.lower().split("x"))
        except ValueError:
            ap.error("--fit 要写成 宽x高，例如 240x180")
    else:
        fw = fh = 0

    rc = 0
    for path in args.images:
        if not os.path.isfile(path):
            print("[FAIL] 找不到 %s" % path)
            rc = 1
            continue

        im = Image.open(path)
        im = flatten(im, args.bg)

        if fw and fh:
            im = scale_fit(im, fw, fh)
        elif args.w and args.h:
            im = im.resize((args.w, args.h), Image.LANCZOS)
        elif args.max:
            im = scale_max(im, args.max)

        w, h = im.size
        data = to_rgb565(im)
        blob = (MAGIC + struct.pack("<HH", w, h) + data) if args.hdr else data

        expect = w * h * 2 + (8 if args.hdr else 0)
        if len(blob) != expect:
            print("[FAIL] %s 字节数 %d（期望 %d）" % (path, len(blob), expect))
            rc = 1
            continue

        stem = os.path.splitext(os.path.basename(path))[0]
        outdir = args.outdir or os.path.dirname(path) or "."
        out = os.path.join(outdir, stem + ".rgb565")
        with open(out, "wb") as f:
            f.write(blob)

        head = "带头 8B，" if args.hdr else ""
        print("[OK] %-24s %4d×%-4d  %s%7d B  -> %s"
              % (os.path.basename(path), w, h, head, len(blob), out))
        print("     灌库：python tools/qspi_provision.py --bin %s --offset <QSPI偏移>" % out)

    return rc


if __name__ == "__main__":
    sys.exit(main())
