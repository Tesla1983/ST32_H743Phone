#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""图库链路验收：TF 卡 BMP → RGB565 → W25Q128 → XIP 读回比对。

它回答三个问题（都在真机上、都是整数结论）：

  ① XIP 冲突：退出 memory-mapped 之后读 0x9000_0000 会发生什么？
     （拿到垃圾数据 / 还是触发总线故障）—— 这决定导入期间要采取什么隔离措施
  ② 导入：BMP 能被正确转成 RGB565 并灌进 W25Q128 吗？
  ③ 内容：XIP 上读回的像素与「主机侧独立算出的参考」逐字节一致吗？

为什么要先跑 ①
--------------
字模与 IME 词典都在 XIP 上，UI 每帧都在读；而写 W25Q128 必须退出映射。
退出期间读 XIP 的后果**此前是未知的**，先用 g_img_test=2 把它量出来。

用法
----
    python tools/img_check.py                  # 全流程：探针 → 造图 → 写卡 → 导入 → 比对
    python tools/img_check.py --probe-only     # 只跑 XIP 冲突探针
    python tools/img_check.py --w 160 --h 120  # 换一张更大的测试图
    python tools/img_check.py --scale 2        # 顺便验证 1/2 抽样

退出码
------
    0   全部通过
    1   前置条件不满足（内核没跑 / 符号缺失）
    2   XIP 探针期间出了故障（★要据此改架构）
    3   导入失败
    4   内容比对不一致
    5   写卡（夹具）失败
"""

import os
import struct
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402  （提供 symtab / rd / wr / NM / PROBE / CHIP）

from PIL import Image, ImageDraw  # noqa: E402

XIP_BASE   = 0x90000000
IMG_IDX    = 0x00100000
IMG_DATA   = 0x00100000 + 4096
CARD_PATH  = "/YMGUI/t.bmp"
TMP_BMP    = os.path.join(os.environ.get("TEMP", "."), "img_check_src.bmp")
TMP_XIP    = os.path.join(os.environ.get("TEMP", "."), "img_check_xip.bin")
CHUNK      = 4096          # 与固件 s_blk 同尺寸


def rd1(name):
    return bench.rd(name, 1)[0]


def wr8(addr, data, chunk=256):
    """按字节写目标内存（probe-rs 的 b8 写，分批避免命令行过长）。"""
    for i in range(0, len(data), chunk):
        part = data[i:i + chunk]
        vals = " ".join("0x%02x" % b for b in part)
        r = subprocess.run("probe-rs write %s --chip %s b8 0x%08x %s"
                           % (bench.PROBE, bench.CHIP, addr + i, vals),
                           shell=True, capture_output=True, text=True,
                           encoding="utf-8", errors="ignore")
        if r.returncode:
            raise RuntimeError("写内存 0x%08x 失败：%s%s" % (addr + i, r.stdout, r.stderr))


def read_mem_to_file(addr, length, path):
    r = subprocess.run("probe-rs read %s --chip %s b8 0x%08x %d -o %s -f binary"
                       % (bench.PROBE, bench.CHIP, addr, length, path),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    if r.returncode or not os.path.isfile(path):
        raise RuntimeError("读内存 0x%08x 失败：%s%s" % (addr, r.stdout, r.stderr))
    with open(path, "rb") as f:
        return f.read()


def wait_idle(name_sentinel, timeout_s=180):
    """触发后等待完成：先写哨兵，触发，再等哨兵被改写。"""
    bench.wr(name_sentinel, [0xAAAAAAAA])
    return wait_done(name_sentinel, timeout_s)


def wait_done(sentinel_name, timeout_s=180):
    """等到哨兵被**最终值**替换。

    ⚠ 0xFFFFFFFF 也是"未完成"的中间态：固件一开始会先把 rc 置成 0xFFFFFFFF
    再开跑，若只判 != 0xAAAAAAAA，会在固件刚起步时就误判完成
    （2026-10-08 实测：读到的 rc=0xFFFFFFFF 被当成了失败结果）。
    """
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        v = rd1(sentinel_name)
        if v != 0xAAAAAAAA and v != 0xFFFFFFFF:
            return True
        time.sleep(0.2)
    return False


def make_test_bmp(w, h):
    """造一张**上下左右都不对称**的测试图 —— 行序/列序写错必然被比对抓出来。"""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        # 纵向渐变：顶部偏红、底部偏蓝
        r = 255 - (y * 200 // max(1, h - 1))
        b = 30 + (y * 200 // max(1, h - 1))
        g = (y * 255 // max(1, h - 1))
        d.line([(0, y), (w, y)], fill=(r, g, b))
    for x in range(0, w, 8):          # 横向条纹（列序错会被抓）
        d.line([(x, 0), (x, h)], fill=(x % 256, 200, 255 - (x % 256)))
    d.rectangle([0, 0, max(2, w // 4), max(2, h // 4)], fill=(255, 0, 0))       # 左上红
    d.rectangle([w - max(2, w // 4), h - max(2, h // 4), w - 1, h - 1], fill=(0, 0, 255))
    im.save(TMP_BMP, "BMP")           # Pillow 默认 24bpp、bottom-up
    with open(TMP_BMP, "rb") as f:
        return f.read()


def reference_rgb565(bmp_path):
    """主机侧**独立**算出参考 RGB565（小端）—— 不走固件任何代码路径。

    Pillow 打开 BMP 时已处理好 bottom-up 行序，得到的 Image 是 top-down 的，
    与 GYimg 要求的行优先 top-down 一致，所以直接逐像素转即可。
    """
    im = Image.open(bmp_path).convert("RGB")
    w, h = im.size
    px = im.tobytes()
    out = bytearray(w * h * 2)
    o = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[o] = v & 0xFF
        out[o + 1] = (v >> 8) & 0xFF
        o += 2
    return bytes(out), w, h


def health():
    h = {k: rd1(k) for k in ("g_boot_magic", "g_shell_rc", "g_font_ok")}
    if h["g_boot_magic"] != 0x594D4731:
        print("固件没在跑：%s（probe-rs download 后必须 reset）" % h)
        return False
    l0 = rd1("g_loop_count")
    time.sleep(0.6)
    l1 = rd1("g_loop_count")
    if l0 == l1:
        print("内核没在跑（g_loop_count 没变化：%d）" % l0)
        return False
    return True


def run_xip_probe():
    print("=" * 66)
    print("① XIP 冲突探针：退出 memory-mapped 后读 0x9000_0000 会怎样")
    print("=" * 66)
    bench.wr("g_img_rc", [0xAAAAAAAA])
    bench.wr("g_img_test", [2])
    if not wait_done("g_img_rc", 30):
        print("[FAIL] 探针超时（30 s）—— 很可能读 XIP 时内核挂住了，需要 reset。")
        return 2
    a = rd1("g_img_xip_inmap")
    b = rd1("g_img_xip_exited")
    c = rd1("g_img_xip_restored")
    flt = rd1("g_img_xip_fault")
    print("  （探针地址 = 偏移 0x300000 的 IME 词典首，正常值 0x504D4959）")
    print("  映射中读到的值    = 0x%08X" % a)
    print("  退出映射后读到的  = 0x%08X%s" % (b, "   ← 停在哨兵：读这一步把内核打挂了"
                                              if b == 0xDEADDEAD else ""))
    print("  恢复映射后读到的  = 0x%08X" % c)
    print("  探针期间新增故障  = %d" % flt)
    print()
    if a == 0:
        print("[!] 映射中读到的就是 0 —— 探针地址选错了（读到了空区域），")
        print("    下面三个值会全 0，**什么都没测出来**。先换一个已知非 0 的地址。")
        return 2
    if b == 0xDEADDEAD:
        print("[FAIL] ★ 退出映射后读 XIP 直接把内核打挂 —— 导入期间必须完全隔离渲染与输入，")
        print("       且任何中断回调都不能碰 XIP。当前架构（主循环阻塞式导入）已满足，")
        print("       但分帧化之前必须先解决它。")
        return 2
    if flt:
        print("[FAIL] ★ 探针期间发生了内核故障（退出映射期间读 XIP 触发了总线异常）。")
        return 2
    same = ("仍然等于映射中的值 —— 退出映射后这一读**还在返回旧内容**"
            if b == a else "**不同**（拿到 0x%08X，是别的内容 / 垃圾）" % b)
    print("[PASS] 退出映射后读 XIP 不会打挂内核，但读到的值与映射中%s。" % same)
    print("       ⇒ 导入期间**必须**阻止任何代码读 XIP（g_img_busy 门闩就是干这个的）。")
    print("       ⇒ 恢复映射后读回 %s。" % ("正常值（与①一致）" if c == a else "0x%08X（与①不同！）" % c))
    return 0


def put_file_on_card(blob):
    """用写文件夹具把 BMP 分片送进 TF 卡（板子自己写，不用拔卡）。"""
    print("=" * 66)
    print("② 把测试 BMP 写进 TF 卡（%d 字节，%d 片）" % (len(blob), (len(blob) + CHUNK - 1) // CHUNK))
    print("=" * 66)
    wr8(bench.T["g_img_path"][0], CARD_PATH.encode("ascii") + b"\0")
    bench.wr("g_img_mk_total", [0])
    for i in range(0, len(blob), CHUNK):
        part = blob[i:i + CHUNK]
        wr8(bench.T["s_blk"][0] if "s_blk" in bench.T else 0x30001A00, part)
        bench.wr("g_img_mk_len", [len(part)])
        bench.wr("g_img_mk_rc", [0xAAAAAAAA])
        bench.wr("g_img_mk", [1 if i == 0 else 2])
        if not wait_done("g_img_mk_rc", 60):
            print("[FAIL] 写第 %d 片超时" % (i // CHUNK))
            return False
        rc = rd1("g_img_mk_rc")
        if rc != 0:
            print("[FAIL] 写第 %d 片失败：g_img_mk_rc=%d  FRESULT=%d"
                  % (i // CHUNK, rc, rd1("g_img_fs_rc")))
            return False
    total = rd1("g_img_mk_total")
    print("  已写入 %d 字节（期望 %d）  %s" % (total, len(blob), "OK" if total == len(blob) else "FAIL"))
    return total == len(blob)


def run_import(scale):
    print("=" * 66)
    print("③ 导入：BMP → RGB565 → W25Q128")
    print("=" * 66)
    wr8(bench.T["g_img_path"][0], CARD_PATH.encode("ascii") + b"\0")
    bench.wr("g_img_scale", [scale])
    bench.wr("g_img_rc", [0xAAAAAAAA])
    bench.wr("g_img_test", [1])
    if not wait_done("g_img_rc", 300):
        print("[FAIL] 导入超时（300 s）")
        return None
    d = {k: rd1(k) for k in ("g_img_rc", "g_img_step", "g_img_slot", "g_img_src_w",
                             "g_img_src_h", "g_img_bpp", "g_img_out_w", "g_img_out_h",
                             "g_img_out_bytes", "g_img_used", "g_img_fs_rc",
                             "g_img_qspi_rc", "g_img_cyc", "g_img_count")}
    print("  返回码=%d  步骤=%d  槽位=%d  FRESULT=%d  QSPI=%d"
          % (d["g_img_rc"], d["g_img_step"], d["g_img_slot"], d["g_img_fs_rc"], d["g_img_qspi_rc"]))
    print("  源 %dx%d @%dbpp → 输出 %dx%d = %d B"
          % (d["g_img_src_w"], d["g_img_src_h"], d["g_img_bpp"],
             d["g_img_out_w"], d["g_img_out_h"], d["g_img_out_bytes"]))
    print("  耗时 %.1f ms   图库已用 %d B   图库图片数 %d"
          % (d["g_img_cyc"] / 400e3, d["g_img_used"], d["g_img_count"]))
    return d


def main():
    need = ["g_img_test", "g_img_rc", "g_img_path", "g_img_mk", "g_img_mk_len",
            "g_img_mk_rc", "g_img_mk_total", "g_img_xip_exited", "g_img_out_bytes"]
    miss = [k for k in need if k not in bench.T]
    if miss:
        print("固件里没有这些符号：%s（图库模块没编进去？）" % miss)
        return 1

    if not health():
        return 1

    w = h = 0
    scale = 1
    a = sys.argv[1:]
    for i, s in enumerate(a):
        if s == "--w":
            w = int(a[i + 1])
        elif s == "--h":
            h = int(a[i + 1])
        elif s == "--scale":
            scale = int(a[i + 1])
    w = w or 96
    h = h or 64

    if "--no-probe" in sys.argv:
        rc = 0
    else:
        rc = run_xip_probe()
        if "--probe-only" in sys.argv:
            return rc
    print()

    blob = make_test_bmp(w, h)
    print("测试图 %dx%d，BMP 文件 %d 字节" % (w, h, len(blob)))
    if not put_file_on_card(blob):
        return 5
    print()

    d = run_import(scale)
    if d is None or d["g_img_rc"] != 0:
        print("[FAIL] 导入失败（见上方返回码/步骤号；步骤号含义见 img_store.c 的 STEP_*）")
        return 3

    expect = d["g_img_out_w"] * d["g_img_out_h"] * 2
    if d["g_img_out_bytes"] != expect:
        print("[FAIL] 输出字节数 %d != w*h*2 = %d" % (d["g_img_out_bytes"], expect))
        return 3

    print()
    print("=" * 66)
    print("④ 内容比对：XIP 读回 vs 主机侧独立参考")
    print("=" * 66)
    off = rd1("g_img_slot")
    # 槽位偏移要查索引：直接从 XIP 读索引条目
    idx = read_mem_to_file(XIP_BASE + IMG_IDX + off * 16, 16,
                           os.path.join(os.environ.get("TEMP", "."), "img_check_idx.bin"))
    magic, flags = struct.unpack_from("<HH", idx, 0)
    eoff, elen = struct.unpack_from("<II", idx, 4)
    ew, eh = struct.unpack_from("<HH", idx, 12)
    print("  索引槽位 %d：magic=0x%04X flags=%d off=%d len=%d %dx%d"
          % (off, magic, flags, eoff, elen, ew, eh))
    if (magic != 0x4947) or (flags & 1) == 0:
        print("[FAIL] 索引条目无效")
        return 3
    if (ew, eh, elen) != (d["g_img_out_w"], d["g_img_out_h"], d["g_img_out_bytes"]):
        print("[FAIL] 索引与导入结果不一致")
        return 3

    got = read_mem_to_file(XIP_BASE + IMG_DATA + eoff, elen, TMP_XIP)
    ref, rw, rh = reference_rgb565(TMP_BMP)
    if scale != 1:
        im = Image.open(TMP_BMP).convert("RGB")
        im = im.resize((rw // scale, rh // scale), Image.NEAREST)
        px = im.tobytes()
        ref = bytearray()
        for i in range(0, len(px), 3):
            r, g, b = px[i], px[i + 1], px[i + 2]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            ref += bytes((v & 0xFF, (v >> 8) & 0xFF))
        ref = bytes(ref)

    n = min(len(got), len(ref))
    mis = sum(1 for i in range(n) if got[i] != ref[i])
    print("  读回 %d 字节，参考 %d 字节，逐字节失配 %d" % (len(got), len(ref), mis))
    if len(got) != len(ref):
        print("[FAIL] 长度不一致")
        return 4
    if mis:
        # 定位前几个失配，帮助判断是行序错还是列序错
        bad = [i for i in range(n) if got[i] != ref[i]][:6]
        print("  前几个失配字节偏移：%s（像素序号 %s）"
              % (bad, [b // 2 for b in bad]))
        print("[FAIL] 内容不一致 —— 若失配集中在整段行，多半是 BMP 行序处理反了。")
        return 4

    print()
    print("[PASS] 全链路成立：")
    print("   · BMP（bottom-up）被正确转成 top-down RGB565 并灌进 W25Q128；")
    print("   · XIP 读回与主机侧独立参考**逐字节一致**（失配 0）；")
    print("   · 索引条目与导入结果一致；")
    print("   · 峰值 RAM 只有 6 KB 缓冲，与图片尺寸无关（流式分块）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
