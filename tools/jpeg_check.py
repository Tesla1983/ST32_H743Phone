#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""相册 JPEG 路径验收：卡上 .jpg → H743 硬件 JPEG 解码 → W25Q128 → XIP 显示。

为什么要这个脚本
----------------
"相册只认 BMP" 一直是 README §10 的未做项，卡点是**内存**：
  284×230 的 RGB565 全帧 = 130 640 B，整帧留在 RAM 里太贵。
解法是**分块解码 + 边转边写**（见 src/img_store.c 的 import_jpeg）：
  硬件每次吐一个 MCU 行（4:2:0 时 = 宽×16 的 YUV，共 24×宽 字节）→
  立刻转 RGB565 → 攒满 4 KB 写 W25Q128 → 最后从 XIP 显示（零 RAM）。
  峰值 RAM ≈ 输入 4 KB + YUV 块 7.7 KB，而不是 130 KB。

本脚本回答三件事，全部是整数判据：
  ① 导入成功：g_img_rc == 0、g_img_out_bytes == w*h*2
  ② 内容正确：XIP 读回的 RGB565 与「主机侧 Pillow 解同一张 JPEG」逐像素比对。
     ⚠ JPEG 是有损压缩，IDCT 实现不同会有 ±1~2 的差异 ⇒ 判据是
       **平均每通道误差** 与 **超差像素数**（阈值可 --tol 调），不是"全等"。
  ③ 尺寸限制生效：给一张超限的大图，必须被拒绝（RC_TOO_WIDE = 7）

用法
----
    python tools/jpeg_check.py              # 240×180（16 对齐） + 284×230（相册尺寸）
    python tools/jpeg_check.py --tol 12      # 放宽单通道容差
"""
import os
import struct
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench           # noqa: E402
import img_check as ic  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

CHUNK = ic.CHUNK if hasattr(ic, "CHUNK") else 4096
TMP_JPG = os.path.join(os.environ.get("TEMP", "."), "jpeg_check_src.jpg")
TMP_XIP = os.path.join(os.environ.get("TEMP", "."), "jpeg_check_xip.bin")
TMP_IDX = os.path.join(os.environ.get("TEMP", "."), "jpeg_check_idx.bin")
CARD = "/YMGUI/PIC/selftest.jpg"


def rd1(name):
    return bench.rd(name, 1)[0]


def make_jpeg(w, h):
    """造一张上下左右都不对称的 JPEG（行序/列序写错必然被比对抓出来）。"""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        r = 255 - (y * 200 // max(1, h - 1))
        b = 30 + (y * 200 // max(1, h - 1))
        g = (y * 255 // max(1, h - 1))
        d.line([(0, y), (w, y)], fill=(r, g, b))
    for x in range(0, w, 8):
        d.line([(x, 0), (x, h)], fill=(x % 256, 200, 255 - (x % 256)))
    d.rectangle([0, 0, max(2, w // 4), max(2, h // 4)], fill=(255, 0, 0))
    d.rectangle([w - max(2, w // 4), h - max(2, h // 4), w - 1, h - 1], fill=(0, 0, 255))
    im.save(TMP_JPG, "JPEG", quality=90)
    with open(TMP_JPG, "rb") as f:
        return f.read()


def put_file(path, blob):
    ic.wr8(bench.T["g_img_path"][0], path.encode("ascii") + b"\0")
    bench.wr("g_img_mk_total", [0])
    for i in range(0, len(blob), CHUNK):
        part = blob[i:i + CHUNK]
        ic.wr8(bench.T["s_blk"][0] if "s_blk" in bench.T else 0x30001A00, part)
        bench.wr("g_img_mk_len", [len(part)])
        bench.wr("g_img_mk_rc", [0xAAAAAAAA])
        bench.wr("g_img_mk", [1 if i == 0 else 2])
        if not ic.wait_done("g_img_mk_rc", 60):
            print("  [FAIL] 写第 %d 片超时" % (i // CHUNK))
            return False
        if rd1("g_img_mk_rc") != 0:
            print("  [FAIL] 写第 %d 片失败" % (i // CHUNK))
            return False
    return rd1("g_img_mk_total") == len(blob)


def run_import(path):
    ic.wr8(bench.T["g_img_path"][0], path.encode("ascii") + b"\0")
    bench.wr("g_img_test", [1])
    t0 = time.time()
    while time.time() - t0 < 10:
        if rd1("g_img_test") == 0:
            break
        time.sleep(0.05)
    t0 = time.time()
    while time.time() - t0 < 180:
        if rd1("g_img_busy") == 0:
            return True
        time.sleep(0.2)
    print("  [FAIL] g_img_busy 不归零")
    return False


def flat_mask(px_list, w, h, thr=8):
    """标出"局部平坦"的像素：3x3 邻域内参考值变化都 <= thr。

    为什么分这个：固件的色度上采样是**最近邻**，而 libjpeg 默认是 fancy（三角形滤波）
    插值 —— 两者只在**颜色突变处**才有差别。平坦区若误差很小、超差像素全在边缘，
    就说明解码与色彩是对的，剩下的差异是上采样方式不同（可接受），而不是错位。
    """
    flat = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            c = px_list[i]
            ok = 1
            for dy in (-1, 0, 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                for dx in (-1, 0, 1):
                    p = px_list[yy * w + (x + dx)]
                    if (abs(p[0] - c[0]) > thr or abs(p[1] - c[1]) > thr
                            or abs(p[2] - c[2]) > thr):
                        ok = 0
                        break
                if not ok:
                    break
            flat[i] = ok
    return flat


def compare(w, h, tol):
    """XIP 读回 vs 主机侧 Pillow 解码，统计误差（并单列平坦区）。"""
    ref = Image.open(TMP_JPG).convert("RGB")
    px_list = list(ref.getdata())

    with open(TMP_XIP, "rb") as f:
        raw = f.read(w * h * 2)

    flat = flat_mask(px_list, w, h)
    bad = 0
    total = 0
    flat_total = 0
    flat_n = 0
    flat_bad = 0
    for i in range(w * h):
        v = struct.unpack_from("<H", raw, i * 2)[0]
        dr = ((v >> 11) & 0x1F) << 3
        dg = ((v >> 5) & 0x3F) << 2
        db = (v & 0x1F) << 3
        c = px_list[i]
        er = abs(dr - c[0])
        eg = abs(dg - c[1])
        eb = abs(db - c[2])
        total += er + eg + eb
        if max(er, eg, eb) > tol:
            bad += 1
        if flat[i]:
            flat_total += er + eg + eb
            flat_n += 1
            if max(er, eg, eb) > tol:
                flat_bad += 1
    n = w * h
    favg = (flat_total / (flat_n * 3.0)) if flat_n else 0.0
    return total / (n * 3.0), bad, n, favg, flat_n, flat_bad


def one_case(tag, w, h, tol, expect_ok=True):
    print("=" * 74)
    print("%s：%d×%d" % (tag, w, h))
    print("=" * 74)
    blob = make_jpeg(w, h)
    print("  源 JPEG %d 字节" % len(blob))
    if not put_file(CARD, blob):
        return False
    if not run_import(CARD):
        return False

    rc = rd1("g_img_rc")
    ow = rd1("g_img_out_w")
    oh = rd1("g_img_out_h")
    ob = rd1("g_img_out_bytes")
    print("  g_img_rc=%d  out=%dx%d  bytes=%d（期望 %d）  step=%d  fs_rc=%d  qspi_rc=%d"
          % (rc, ow, oh, ob, w * h * 2, rd1("g_img_step"),
             rd1("g_img_fs_rc"), rd1("g_img_qspi_rc")))

    if not expect_ok:
        ok = (rc == 7)   # RC_TOO_WIDE
        print("  %s（期望被拒：rc=7）" % ("OK" if ok else "FAIL"))
        return ok

    if rc != 0 or ob != w * h * 2 or ow != w or oh != h:
        print("  [FAIL] 导入判据没达到")
        return False

    # XIP 读回：先读索引条目拿数据偏移，再读那一块像素
    slot = rd1("g_img_slot")
    idx_addr = 0x90000000 + 0x00100000 + slot * 16      # IMG_IDX_ADDR + slot*16
    ic.read_mem_to_file(idx_addr, 16, TMP_IDX)
    with open(TMP_IDX, "rb") as f:
        e = f.read(16)
    magic, flags, off = struct.unpack_from("<HHI", e, 0)
    print("  索引 slot=%d magic=0x%04X flags=%d off=%d" % (slot, magic, flags, off))
    data_addr = 0x90000000 + 0x00100000 + 4096          # IMG_DATA_ADDR
    ic.read_mem_to_file(data_addr + off, ob, TMP_XIP)
    avg, bad, n, favg, fn, fbad = compare(w, h, tol)
    print("  XIP 读回 %d 像素：平均每通道误差 %.2f，超差像素 %d / %d（容差 %d）"
          % (n, avg, bad, n, tol))
    print("    其中**平坦区** %d 像素：平均误差 %.2f，超差 %d"
          % (fn, favg, fbad))
    # 判据分两层：
    #   · 平坦区平均误差 < 3  ⇒ 解码与色彩转换本身是对的（JPEG 有损 + RGB565 量化
    #     之后，跨解码器比较也就这个量级）；
    #   · 全图超差比例 < 5%  ⇒ 剩下的差异只出现在颜色突变处，那是"最近邻 vs
    #     libjpeg fancy 上采样"的固有差别，不是错位（错位会让平坦区同样出错）。
    ok = (favg < 3.0) and (bad * 100 // n < 5) and (avg < 12.0)
    print("  %s" % ("OK" if ok else "FAIL"))
    return ok


def main():
    tol = 24
    args = sys.argv[1:]
    if "--tol" in args:
        tol = int(args[args.index("--tol") + 1])

    need = ["g_img_test", "g_img_rc", "g_img_path", "g_img_out_w", "g_img_out_h",
            "g_img_out_bytes", "g_img_busy", "g_img_slot", "g_img_step",
            "g_img_fs_rc", "g_img_qspi_rc", "g_img_mk", "g_img_mk_len",
            "g_img_mk_rc", "g_img_mk_total"]
    missing = [n for n in need if n not in bench.T]
    if missing:
        print("[FAIL] 固件里找不到：%s" % ", ".join(missing))
        return 1

    ok = True
    ok = one_case("① 16 对齐尺寸", 240, 180, tol) and ok
    ok = one_case("② 相册面板尺寸（非 16 对齐）", 284, 230, tol) and ok
    ok = one_case("③ 超限大图必须被拒绝", 640, 480, tol, expect_ok=False) and ok

    print("=" * 74)
    if ok:
        print("[PASS] JPEG 硬件解码 → W25Q128 → XIP 通路全部通过")
    else:
        print("[FAIL] 上面有判据没达到")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
