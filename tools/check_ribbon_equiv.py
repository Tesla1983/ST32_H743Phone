#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P0-1（壁纸 ribbon 合并）的逐位等价性证明：**纯软件 A/B**，不需要上板。

做法
----
把 wallpaper() 的 ribbon 段在主机上按**两种算法各跑一遍完整一帧**：

  旧：for x in 0..319: 每次填 1 px 宽 × (H-crest) 高      （逐列碎填充）
  新：crest 相同的相邻列合成一段，一次填；与本 band 不相交的整段跳过

两种算法都按库里 Draw_Fill 的真实语义执行：
  · 先与 surface.clip 求交（clip = 当前 band），再与 buf_area 求交；
  · 全空的直接不算一次调用（与 YMGUI_DIAG_FILLPX 的计数口径一致）；
  · 对每个落进交集的像素，把 (layer, color, opa) 按**调用顺序**追加进该像素的写入序列。

然后逐像素比对两个序列。序列相同 ⇒ 每个像素被混合的**次数、颜色、opa、顺序**全部相同
⇒ 混合结果逐位相同（alpha 混合是逐像素独立的运算，与邻像素无关）。

为什么不用上板抓屏比对
----------------------
抓屏只能证明"这次画出来的跟上次看起来一样"，而帧里有动画/时间文本在变。
这个脚本证明的是**算法层面**的等价，覆盖全部 153600 个像素 × 全部 15 个 band，
比抽一帧比对更彻底。
"""
import sys
from collections import defaultdict

W, H = 320, 480
BAND = 32
NBANDS = H // BAND

LAYERS = (          # (name, color, opa, base, xc, div)  与 phone_shell.c 的三层一一对应
    ("L1", (241, 173, 164), 175, 239,  90,  620),
    ("L2", (86, 76, 133),   240, 315, 340,  750),
    ("L3", (44, 48, 88),    220, 379,   0, 1300),
)


def crest(x, base, xc, div):
    d = x - xc
    return base + (d * d) // div      # C 的整数除法（d*d ≥ 0，不存在负数截断问题）


def intersect(a, b):
    """库的 GY_Rect_Intersect（半开区间 [x, x+w)）"""
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[0] + a[2], b[0] + b[2]), min(a[1] + a[3], b[1] + b[3])
    if x2 <= x1 or y2 <= y1:
        return None
    return (x1, y1, x2 - x1, y2 - y1)


def fill(writes, calls, area, clip, buf_area, layer, color, opa):
    """按 Draw_Fill 的语义裁剪并写入；返回是否真的写了（非空的才算一次调用）"""
    r = intersect(area, clip)
    if r is None:
        return
    r = intersect(r, buf_area)
    if r is None:
        return
    calls[0] += 1
    x0, y0, w, h = r
    for y in range(y0, y0 + h):                  # Draw_Fill 内层：y 外、x 内
        for x in range(x0, x0 + w):
            writes[(x, y)].append((layer, color, opa))


def run(legacy):
    """跑完整一帧（15 个 band），legacy=True 走旧算法"""
    writes = defaultdict(list)
    calls = [0]
    for k in range(NBANDS):
        band = (0, k * BAND, W, BAND)            # surface.clip == s.buf_area（库里的常态）
        for name, color, opa, base, xc, div in LAYERS:
            if legacy:
                for x in range(W):               # 旧：逐列 1 px 宽
                    c = crest(x, base, xc, div)
                    fill(writes, calls, (x, c, 1, H - c), band, band, name, color, opa)
            else:
                x = 0                            # 新：crest 相同的连续列合并 + 整段跳空
                while x < W:
                    c = crest(x, base, xc, div)
                    x2 = x + 1
                    while x2 < W and crest(x2, base, xc, div) == c:
                        x2 += 1
                    top = band[1]                # clip 上/下边界换算到 area 局部行
                    bot = band[1] + band[3]
                    if c < bot and H > top:      # 与 phone_shell.c 的跳空条件一致
                        fill(writes, calls, (x, c, x2 - x, H - c), band, band,
                             name, color, opa)
                    x = x2
    return writes, calls[0]


def main():
    old, c_old = run(True)
    new, c_new = run(False)

    print("=== P0-1 ribbon 合并：逐位等价性 A/B ===")
    print("  旧算法：非空填充调用 %d 次/帧" % c_old)
    print("  新算法：非空填充调用 %d 次/帧   （省 %d 次 = %.1f%%）"
          % (c_new, c_old - c_new, 100.0 * (c_old - c_new) / c_old))

    allpx = set(old) | set(new)
    print("  覆盖像素数：旧 %d / 新 %d / 并集 %d" % (len(old), len(new), len(allpx)))

    diff = 0
    first = None
    for p in sorted(allpx):
        if list(old[p]) != list(new[p]):
            diff += 1
            if first is None:
                first = p
    if diff:
        print("X 不等价！%d 个像素的写入序列不同，首个 %s" % (diff, first))
        print("   旧：%s" % (list(old[first]),))
        print("   新：%s" % (list(new[first]),))
        return 1
    print("✅ 全部 %d 个像素的 (层, 颜色, opa, 顺序) 写入序列完全一致 ⇒ 逐位等价" % len(allpx))
    return 0


if __name__ == "__main__":
    sys.exit(main())
