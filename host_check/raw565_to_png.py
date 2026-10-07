#!/usr/bin/env python3
"""把无头宿主导出的原始 RGB565 帧转成 PNG。

用法:  python raw565_to_png.py <输入.raw565> <输出.png> [宽 高]

为什么要自己写：本机没有 Pillow，而 PNG 的 zlib 流用 Python 标准库的 zlib
就能生成，不必依赖任何第三方包。

数据格式与板端 ymgui_port.c 的全帧镜像缓冲**一致**（都是小端 uint16 RGB565），
所以宿主截图和板端抓屏图是可以直接逐像素对比的。
"""

import struct
import sys
import zlib


def png_write(path, width, height, rgb_rows):
    """rgb_rows: 每行 bytes，长度 = width*3"""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + row for row in rgb_rows)      # 每行前置 filter=0
    out = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 480

    data = open(src, "rb").read()
    need = w * h * 2
    if len(data) < need:
        print(f"数据不足: {len(data)} < {need}")
        return 1

    px = struct.unpack_from("<%dH" % (w * h), data, 0)
    rows = []
    for y in range(h):
        row = bytearray(w * 3)
        base = y * w
        for x in range(w):
            v = px[base + x]
            r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row[x * 3 + 0] = (r5 << 3) | (r5 >> 2)
            row[x * 3 + 1] = (g6 << 2) | (g6 >> 4)
            row[x * 3 + 2] = (b5 << 3) | (b5 >> 2)
        rows.append(bytes(row))

    png_write(dst, w, h, rows)

    # 顺带打一个颜色直方图，方便和板端抓屏图对比是否同一幅界面
    from collections import Counter
    top = Counter(px).most_common(8)
    print(f"已写出 {dst}  ({w}x{h})；不同颜色数 {len(set(px))}")
    print("  最多颜色:", " ".join(f"0x{k:04X}x{v}" for k, v in top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
