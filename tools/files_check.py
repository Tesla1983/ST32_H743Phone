#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""文件管理器「照片 / 笔记」接 TF 卡真实文件 —— 一键验收。

what / why
----------
文件管理器原来四个分类全是硬编码假文案（apps/files.c 只有 103 行、一次 f_open
都没有）。2026-10-09 按"只做分类存储"的范围接了真实目录：
    /YMGUI/PIC   照片（*.bmp，点一个就导入图库）
    /YMGUI/NOTE  笔记（*.txt，笔记 app 读写，UTF-8 原样）
    /YMGUI/REC、/YMGUI/DOWN 未建 —— 板上无音频硬件、浏览器无联网下载，保持占位。

本脚本回答三件事，全部是**整数判据**，不靠肉眼：
  ① 目录扫描：往卡里丢一个 .bmp 后，扫描条目数 +1（证明列的是真实目录，不是常量）
  ② 照片导入：从 /YMGUI/PIC 里挑一张灌进 W25Q128，g_img_rc == 0
  ③ 笔记往返：新建一条含中文的 .txt 再读回，逐字节失配 0（证明 UTF-8 原样读写）

用法
----
    python tools/files_check.py

判据
----
    g_scan_n    扫描条目数（每次写卡后必须 +1）
    g_img_rc    导入返回码（0 = 成功）
    g_note_rc   笔记自检返回码（0 = 成功）
    g_note_cmp  笔记往返比对的失配字节数（0 = 完全一致）
"""
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, "tools")
import bench          # noqa: E402
import img_check as ic  # noqa: E402

PIC_DIR  = "/YMGUI/PIC"
PIC_FILE = "/YMGUI/PIC/selftest.bmp"
CHUNK    = ic.CHUNK if hasattr(ic, "CHUNK") else 4096

NEED = ["g_img_test", "g_img_rc", "g_img_path", "g_scan_n", "g_scan_cyc",
        "g_scan_fs_rc", "g_note_rc", "g_note_bytes", "g_note_cmp",
        "g_img_mk", "g_img_mk_len", "g_img_mk_rc", "g_img_mk_total",
        "g_img_count", "g_img_out_bytes"]


def rd1(name):
    """读一个 32 位变量。⚠ bench.rd 的第一个参数是**符号名**不是地址
    （地址要用 bench.wr8 那一路；这里踩过一次：传地址进去 KeyError）。"""
    return bench.rd(name, 1)[0]


def wr_path(p):
    ic.wr8(bench.T["g_img_path"][0], p.encode("ascii") + b"\0")


def run_mode(mode, timeout=180):
    """触发一次 img_store_poll 并等它跑完。

    ⚠ 判据**不能**用 g_img_rc：真实失败时它可能被置成 0xFFFFFFFF，而
      wait_done 恰恰把 0xFFFFFFFF 当"还没跑完"，于是永远等下去（2026-10-09
      第一次跑就卡在这里 180 s）。
      ⇒ 正解：g_img_test 被主循环取走（归 0）+ g_img_busy 归 0 = 这一轮结束了，
        成败再去读 g_img_rc。"""
    bench.wr("g_img_test", [mode])
    t0 = time.time()
    while time.time() - t0 < 10:
        if rd1("g_img_test") == 0:
            break
        time.sleep(0.05)
    else:
        print("  [FAIL] g_img_test 没被主循环取走（固件在跑吗？）")
        return False
    t0 = time.time()
    while time.time() - t0 < timeout:
        if rd1("g_img_busy") == 0:
            return True
        time.sleep(0.2)
    print("  [FAIL] g_img_busy 迟迟不归零（超时 %d s）" % timeout)
    return False


def put_file(path, blob):
    """把 blob 分片写进卡上的 path（复用 img_check 的写文件夹具，只是路径不同）。"""
    wr_path(path)
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
        rc = rd1("g_img_mk_rc")
        if rc != 0:
            print("  [FAIL] 写第 %d 片失败：g_img_mk_rc=%d" % (i // CHUNK, rc))
            return False
    total = rd1("g_img_mk_total")
    print("  已写入 %d 字节（期望 %d）  %s" % (total, len(blob), "OK" if total == len(blob) else "FAIL"))
    return total == len(blob)


def main():
    missing = [n for n in NEED if n not in bench.T]
    if missing:
        print("[FAIL] 固件里找不到这些符号：%s" % ", ".join(missing))
        print("       先确认烧的是最新固件，且 build/ymgui-h743.elf 是最新的。")
        return 1

    ok = True

    # ① 建目录 + 扫一次（基线）
    print("=" * 66)
    print("① 建 /YMGUI/PIC 与 /YMGUI/NOTE，扫照片目录（基线）")
    print("=" * 66)
    if not run_mode(5):
        print("  [FAIL] mode 5 超时")
        return 1
    n0 = rd1("g_scan_n")
    print("  扫描条目数 g_scan_n = %d（FRESULT=%d）" % (n0, rd1("g_scan_fs_rc")))

    # ② 往照片目录丢一张 BMP，条目数必须 +1
    print("=" * 66)
    print("② 往 %s 写一张测试 BMP（%d×%d）" % (PIC_DIR, 96, 64))
    print("=" * 66)
    blob = ic.make_test_bmp(96, 64)
    if not put_file(PIC_FILE, blob):
        return 1
    if not run_mode(5):
        print("  [FAIL] mode 5 超时")
        return 1
    n1 = rd1("g_scan_n")
    print("  扫描条目数 g_scan_n = %d（期望 %d）  %s"
          % (n1, n0 + 1, "OK" if n1 == n0 + 1 else "FAIL"))
    print("  扫描耗时 g_scan_cyc = %d 周期（约 %.1f ms @400 MHz）"
          % (rd1("g_scan_cyc"), rd1("g_scan_cyc") / 400000.0))
    ok = ok and (n1 == n0 + 1)

    # ③ 从照片目录导入一张（走的就是 UI 点击那条路径的底层）
    print("=" * 66)
    print("③ 从照片目录导入 %s" % PIC_FILE)
    print("=" * 66)
    wr_path(PIC_FILE)
    bench.wr("g_img_scale", [1])
    if not run_mode(1):
        print("  [FAIL] 导入超时")
        return 1
    rc = rd1("g_img_rc")
    print("  g_img_rc = %d（0 = 成功）  输出 %d 字节  图库总数 %d"
          % (rc, rd1("g_img_out_bytes"), rd1("g_img_count")))
    ok = ok and (rc == 0)

    # ④ 笔记往返（含中文，验证 UTF-8 原样读写）
    print("=" * 66)
    print("④ 笔记往返自检：新建含中文的 .txt → 读回 → 逐字节比对")
    print("=" * 66)
    if not run_mode(6):
        print("  [FAIL] mode 6 超时")
        return 1
    nrc, ncmp, nbytes = rd1("g_note_rc"), rd1("g_note_cmp"), rd1("g_note_bytes")
    print("  g_note_rc = %d（0 = 成功）  失配 %d 字节  长度 %d 字节" % (nrc, ncmp, nbytes))
    ok = ok and (nrc == 0) and (ncmp == 0) and (nbytes > 0)

    print("=" * 66)
    if ok:
        print("[PASS] 照片目录扫描 + 导入 + 笔记往返 全部通过")
    else:
        print("[FAIL] 上面有判据没达到，见逐项输出")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
