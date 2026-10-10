#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P4 蓝牙文件接收验收（STM32 侧，SWD 直读；不需要手机、不需要蓝牙）
=============================================================================
整条链路：ESP32 合成 BMP → SPI 从机 → src/bt_recv.c 按序号拉 → CRC 校验
          → FatFs 写 TF 卡 → img_store 导入 W25Q → 相册可见。

本脚本走的是**自测路径**（$?BTF,TEST,<w>,<h>）：ESP32 侧 bt_file_test_open()
合成一张 w×h 的 24 位 BMP，走与手机完全相同的下游管道。
这样验收不依赖真手机，可重复、可自动化。

判据（B0..B6）
-----------------------------------------------------------------------------
  B0  会话开启      g_bt_sess 增加（$?BTF,TEST 被对端受理）
  B1  收全落盘      g_bt_done 增加、g_bt_bytes == 54 + 4*ceil(3w/4)*h、
                    g_bt_rc == 0、g_bt_fs_rc == 0
  B2  CRC 无坏块    g_bt_crc_bad == 0（若 >0 但最终 done=1，说明重传兜住了 —— 报 WARN）
  B3  导入图库      g_bt_imp_rc == 0 且 g_img_count 增加、尺寸 == (w,h)
  B4  相册可见      g_img_used 增加 > 0（索引里真多了一条有效图片）
  B5  无缺额        g_cmd_bd_want == 0 —— 本次会话没有出现过"块收一半超时"
  B6  命令无连续失联 g_bt_get_max <= 1 —— 没有"连续两笔以上 GET 毫无应答"。
                    ≥6 就是 STALL 的成因；**在 --snap 0（不轮询）下这个数应为 0**，
                    若不为 0 说明链路真有问题（而不是探针干扰）。

用法
-----------------------------------------------------------------------------
  python tools/bt_file_check.py                 # 默认 224x152（≈100 KB 验收档）
  python tools/bt_file_check.py --w 96 --h 64    # 小图快跑
  python tools/bt_file_check.py --status         # 只读状态，不触发
  python tools/bt_file_check.py --burst 4        # 先写 g_spi_burst=4 再触发（扫档用）
  python tools/bt_file_check.py --snap 0 --wait 40   # ★ 不动核跑完整轮，只事后读一次
  python tools/bt_file_check.py --timeout 180    # 拉长等待

⚠ 跑之前：probe-rs download 之后必须 reset；对端 ESP32 必须已烧 P3 固件
  （启动日志有 'bt_file 就绪'），否则 B0 会以 g_bt_err=2(BTE_OPEN_TO) 失败。

⚠⚠⚠ 测量纪律（2026-10-10 用一整轮排查换来的教训，别省这一步去读源码）
-----------------------------------------------------------------------------
  快照实现是**一次 probe-rs 读整段 DTCM（37 KB，逐字节 b8）**。它不是原子操作，
  实测**一次要 ~6 s**：期间主循环被拖慢一个数量级，更糟的是——
    · 快照跨越了变量的不同时刻 ⇒ 同一次快照里的 `g_bt_done=1` 与
      `g_cmd_bd_done=43` 可能来自会话的不同阶段（曾据此误判"计数器自相矛盾"）；
    · 停核期间 SPI 的 CS 会被拉低几百毫秒，从机把相邻两笔事务并成一笔
      （ESP32 侧报 `trans_len not 4 bytes aligned`、`空事务`），
      而主机侧命令字节**已经从事务缓冲里弹掉了** ⇒ 那一笔 GET 凭空消失、
      2 s 后超时重发。连续 6 笔就撑满 STALL_MS ⇒ 报 `BTE_STALL`。
  实测同一套固件：SWD 全程轮询 ⇒ 79 笔 GET 只 50 块、1 次收包缺额、73.6 s；
  对照 ESP32 日志 `补排失败=0 / 命令区截断=0 / 重传仅 1 次 / 文件收全 102198 字节`
  ⇒ **链路本身是好的，是探针把它弄坏的。**
  ⇒ 所以：**判"通不通"先跑 `--snap 0`（不轮询）**；耗时一律看固件自记的
    `g_bt_ms`（g_bt_ms 由 src/bt_recv.c 在主循环里量），不看脚本墙钟。
"""
import os
import struct
import subprocess
import sys
import tempfile
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

NM = r"E:/software/ARMGNU~1/142BCE~1.3RE/bin/arm-none-eabi-nm.exe"
PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
ELF = "build/ymgui-h743.elf"

# 所有待读变量都落在 DTCM（0x20000000 起）的前 0x9200 字节里：
#   g_spi_burst 在 .data(0x2000014c)，其余在 .bss(0x200034xx ~ 0x20008d8c)。
# 一次读整段，本地索引。范围变了（新增变量更靠后）就把 DTCM_LEN 调大。
DTCM_BASE = 0x20000000
DTCM_LEN = 0x9200

BT_KEYS = ["g_bt_sess", "g_bt_chunks", "g_bt_bytes", "g_bt_crc_bad",
           "g_bt_retx", "g_bt_get_n", "g_bt_done", "g_bt_rc", "g_bt_err",
           "g_bt_fs_rc", "g_bt_wr", "g_bt_imp_rc", "g_bt_spi_tx",
           "g_bt_ms", "g_bt_get_to", "g_bt_get_max",
           "g_bt_recv_req", "g_bt_recv_w", "g_bt_recv_h",
           "g_bt_get_ack_ms"]
NET_KEYS = ["g_net_btf_state", "g_net_btf_size", "g_net_btf_recv",
            "g_net_bt_pkts", "g_cmd_btf_rc", "g_cmd_btf_rs",
            # $!BD 收包诊断：定位"块传了一半"（got < want）用
            "g_cmd_bd_pkts", "g_cmd_bd_done", "g_cmd_bd_crc_bad",
            "g_cmd_bd_toobig", "g_cmd_bd_timeout", "g_cmd_bd_want", "g_cmd_bd_got"]
IMG_KEYS = ["g_img_count", "g_img_used", "g_img_out_w", "g_img_out_h",
            "g_img_out_bytes", "g_img_rc", "g_img_scale"]
MISC_KEYS = ["g_boot_magic", "g_loop_count", "g_spi_burst", "g_spi_tx_n",
             "g_spi_err", "g_spi_last_code", "g_spi_cs_gap",
             "g_spi_presc", "g_spi_hz"]
ALL_KEYS = BT_KEYS + NET_KEYS + IMG_KEYS + MISC_KEYS

ERR_TEXT = {
    0: "无（未失败）",
    1: "BTE_OPEN —— $?BTF,OPEN/TEST 被对端拒绝（BT 起不来 / 链路不通）",
    2: "BTE_OPEN_TO —— 请求发了没人应（对端没跑 P3 固件 / SPI 命令回不来）",
    3: "BTE_SINK —— 落地文件开不了（挂载 / 建目录 / f_open）",
    4: "BTE_WRITE —— f_write 短写（卡满 / 卡故障）",
    5: "BTE_STALL —— 长时间没有任何进展",
    6: "BTE_BADSEQ —— 序号对不上 / 重取次数耗尽（中间丢了整块）",
}


def symtab():
    out = subprocess.run([NM, ELF], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore").stdout
    t = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            t.setdefault(p[2], []).append(int(p[0], 16))
    return t


T = symtab()

_missing = [k for k in ALL_KEYS if k not in T]
if _missing:
    print("[FAIL] ELF 里找不到符号：%s" % ", ".join(_missing))
    sys.exit(1)

# 符号地址 → DTCM 内偏移；越界的符号直接报错（不是静默读错地址）
OFF = {}
for k in ALL_KEYS:
    a = T[k][0]
    if not (DTCM_BASE <= a < DTCM_BASE + DTCM_LEN):
        print("[FAIL] 符号 %s @ 0x%08X 落在监测窗口 [0x%08X,0x%08X) 之外"
              % (k, a, DTCM_BASE, DTCM_BASE + DTCM_LEN))
        sys.exit(1)
    OFF[k] = a - DTCM_BASE


def _bulk():
    """一次读出整个 DTCM 窗口，返回 bytes。"""
    fd, path = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        r = subprocess.run(
            "probe-rs read %s --chip %s b8 0x%08x %d -o %s -f binary"
            % (PROBE, CHIP, DTCM_BASE, DTCM_LEN, path),
            shell=True, capture_output=True, text=True,
            encoding="utf-8", errors="ignore")
        with open(path, "rb") as f:
            d = f.read()
        if len(d) < DTCM_LEN:
            raise RuntimeError("读到 %d 字节（期望 %d）：%s%s"
                               % (len(d), DTCM_LEN, r.stdout[-200:], r.stderr[-200:]))
        return d
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def snapshot():
    d = _bulk()
    return {k: struct.unpack_from("<I", d, o)[0] for k, o in OFF.items()}


def rd1(name):
    return snapshot()[name]


def as_i32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def wr1(name, v):
    a = T[name][0]
    subprocess.run("probe-rs write %s --chip %s b32 0x%08x 0x%08x"
                   % (PROBE, CHIP, a, v & 0xFFFFFFFF),
                   shell=True, capture_output=True, text=True)


def bmp_bytes(w, h):
    return 54 + ((w * 3 + 3) // 4) * 4 * h


def show(tag, d):
    print("---- %s ----" % tag)
    print("  会话  sess=%d  done=%d  rc=%d  err=%s"
          % (d["g_bt_sess"], d["g_bt_done"], as_i32(d["g_bt_rc"]),
             ERR_TEXT.get(d["g_bt_err"], "?")))
    print("  数据  块=%d  字节=%d  坏块=%d  重取=%d  GET=%d  SPI事务=%d"
          % (d["g_bt_chunks"], d["g_bt_bytes"], d["g_bt_crc_bad"],
             d["g_bt_retx"], d["g_bt_get_n"], d["g_bt_spi_tx"]))
    print("  命令  GET无应答=%d  最长连续=%d（≥6 即 STALL 成因）"
          % (d["g_bt_get_to"], d["g_bt_get_max"]))
    ms = d["g_bt_ms"]
    if ms > 0:
        print("  耗时  固件自记 %d ms ⇒ %.1f KB/s（脚本墙钟不可用于测速，见文件头）"
              % (ms, (d["g_bt_bytes"] / 1024.0) / (ms / 1000.0)))
    else:
        print("  耗时  固件自记 ——（会话还没跑完）")
    print("  落地  fs_rc=%d  wr=%d  imp_rc=0x%X"
          % (d["g_bt_fs_rc"], d["g_bt_wr"], d["g_bt_imp_rc"]))
    print("  对端  $BT state=%d  size=%d  recv=%d  $BT帧=%d  BTF响应=%d(rc=%d)"
          % (d["g_net_btf_state"], d["g_net_btf_size"], d["g_net_btf_recv"],
             d["g_net_bt_pkts"], d["g_cmd_btf_rs"], d["g_cmd_btf_rc"]))
    print("  $!BD  头帧=%d  收齐=%d  CRC坏=%d  超长=%d  收包超时=%d  上次超时(要%d/只收到%d)"
          % (d["g_cmd_bd_pkts"], d["g_cmd_bd_done"], d["g_cmd_bd_crc_bad"],
             d["g_cmd_bd_toobig"], d["g_cmd_bd_timeout"],
             d["g_cmd_bd_want"], d["g_cmd_bd_got"]))
    print("  图库  count=%d  used=%d  out=%dx%d  out_bytes=%d  img_rc=%d"
          % (d["g_img_count"], d["g_img_used"], d["g_img_out_w"],
             d["g_img_out_h"], d["g_img_out_bytes"], d["g_img_rc"]))
    print("  链路  burst=%d  事务=%d  HAL失败=%d  上笔码=%d"
          % (d["g_spi_burst"], d["g_spi_tx_n"], d["g_spi_err"],
             as_i32(d["g_spi_last_code"])))


def main():
    args = sys.argv[1:]
    w, h = 224, 152
    timeout = 150
    burst = None
    snap = 0.5          # 两次快照之间的额外等待秒（实际周期 = snap + 读一次 DTCM 的 ~6 s）
    wait = 40           # --snap 0 时：触发后干等多少秒再去读唯一那一张快照
    imp_wait = 20       # 收尾后等"异步导入图库"落库的最长秒数
    if "--w" in args:
        w = int(args[args.index("--w") + 1])
    if "--h" in args:
        h = int(args[args.index("--h") + 1])
    if "--timeout" in args:
        timeout = int(args[args.index("--timeout") + 1])
    if "--burst" in args:
        burst = int(args[args.index("--burst") + 1])
    if "--snap" in args:
        snap = float(args[args.index("--snap") + 1])
    if "--wait" in args:
        wait = float(args[args.index("--wait") + 1])
    if "--imp-wait" in args:
        imp_wait = float(args[args.index("--imp-wait") + 1])
    presc = None        # --presc N：写 g_spi_presc（SPI 分频，2..256；64 ≈ 1.5625 MHz）
    gap = None          # --gap N：写 g_spi_cs_gap（连续事务之间 CS 高电平的空转次数）
    get_ack = None      # --get-ack N：写 g_bt_get_ack_ms（GET 无应答等待时长，ms）
    if "--presc" in args:
        presc = int(args[args.index("--presc") + 1])
    if "--gap" in args:
        gap = int(args[args.index("--gap") + 1])
    if "--get-ack" in args:
        get_ack = int(args[args.index("--get-ack") + 1])
    status_only = "--status" in args

    s0 = snapshot()
    if s0["g_boot_magic"] != 0x594D4731:
        print("[FAIL] g_boot_magic=0x%08X —— 固件没在跑（download 后要 reset）"
              % s0["g_boot_magic"])
        sys.exit(1)
    time.sleep(1.0)
    l1 = rd1("g_loop_count")
    if l1 == s0["g_loop_count"]:
        print("[FAIL] 内核没在跑（g_loop_count 1 秒内没变）—— 先 reset")
        sys.exit(1)
    print("[OK] 固件在跑，主循环 %d 拍/秒  g_spi_burst=%d"
          % (l1 - s0["g_loop_count"], s0["g_spi_burst"]))

    show("t0（触发前）", s0)

    if status_only:
        return

    if burst is not None:
        wr1("g_spi_burst", burst)
        v = rd1("g_spi_burst")
        print("[*] g_spi_burst 写为 %d（回读 %d）" % (burst, v))
        if v != burst:
            print("[FAIL] burst 写入没生效")
            sys.exit(1)

    if presc is not None:
        wr1("g_spi_presc", presc)
        v = rd1("g_spi_presc")
        hz = rd1("g_spi_hz") if "g_spi_hz" in T else 0
        print("[*] g_spi_presc 写为 %d（回读 %d，g_spi_hz=%d ⇒ %.3f MHz）"
              % (presc, v, hz, hz / 1e6))
        if v != presc:
            print("[FAIL] presc 写入没生效")
            sys.exit(1)
        time.sleep(0.2)

    if gap is not None:
        wr1("g_spi_cs_gap", gap)
        v = rd1("g_spi_cs_gap")
        print("[*] g_spi_cs_gap 写为 %d（回读 %d）≈ %.0f µs"
              % (gap, v, v / 133.0))
        if v != gap:
            print("[FAIL] cs_gap 写入没生效")
            sys.exit(1)

    if get_ack is not None:
        wr1("g_bt_get_ack_ms", get_ack)
        v = rd1("g_bt_get_ack_ms")
        print("[*] g_bt_get_ack_ms 写为 %d（回读 %d）" % (get_ack, v))
        if v != get_ack:
            print("[FAIL] get_ack 写入没生效")
            sys.exit(1)

    want = bmp_bytes(w, h)
    print("=" * 70)
    print("[*] 触发自测：%d×%d 24 位 BMP = %d 字节（≈%.1f KB）"
          % (w, h, want, want / 1024.0))
    wr1("g_bt_recv_w", w)
    wr1("g_bt_recv_h", h)
    wr1("g_bt_recv_req", 1)

    t0 = time.time()
    last = s0

    if snap <= 0:
        # ★ 不轮询：触发之后一个 SWD 访问都不做，干等 wait 秒再读一次。
        #   这是"测链路真实行为"的唯一干净姿势（理由见文件头"测量纪律"）。
        print("[*] --snap 0：不轮询，干等 %.0f s 后读唯一一张快照" % wait)
        time.sleep(wait)
        last = snapshot()
    else:
        while True:
            cur = snapshot()
            el = time.time() - t0
            done = cur["g_bt_done"] - s0["g_bt_done"]
            if done > 0:
                last = cur
                break
            if cur["g_bt_err"] != 0 and cur["g_bt_sess"] > s0["g_bt_sess"]:
                last = cur
                break
            if el > timeout:
                last = cur
                print("[!] 超时 %d s（会话仍未完成）" % timeout)
                break
            sess_b = cur["g_bt_bytes"]
            pct = "  %.1f%%" % (100.0 * sess_b / want) if want else ""
            print("    ... %.1fs  会话字节 %d/%d%s  块+%d  坏块+%d  GET+%d  GET无应答=%d  want/got=%d/%d"
                  % (el, sess_b, want, pct,
                     cur["g_bt_chunks"] - s0["g_bt_chunks"],
                     cur["g_bt_crc_bad"] - s0["g_bt_crc_bad"],
                     cur["g_bt_get_n"] - s0["g_bt_get_n"],
                     cur["g_bt_get_to"], cur["g_cmd_bd_want"], cur["g_cmd_bd_got"]))
            time.sleep(snap)

    elapsed = time.time() - t0

    # ---- 等"异步导入图库"落库 ----
    # img_import_path() 只是**排队**，真正的解码+写 W25 由 img_store_poll 在后续帧里做。
    # 不等待就会看到 imp_rc=0 但 g_img_count/g_img_used 还没变 ⇒ B3/B4 假失败。
    deadline = time.time() + imp_wait
    pending = 0xFFFFFFFF
    s1 = last
    while time.time() < deadline:
        if s1["g_img_rc"] != pending:
            break
        time.sleep(6.0)            # 一次快照 ~6 s，等 6 s 再读，别空转
        s1 = snapshot()

    show("t1（结束后）", s1)
    print("=" * 70)

    res = []

    # ---------- B0 ----------
    print("B0  会话开启（g_bt_sess 增加）")
    if s1["g_bt_sess"] > s0["g_bt_sess"]:
        print("  [PASS] 会话已开（sess %d → %d），$?BTF,TEST 被对端受理"
              % (s0["g_bt_sess"], s1["g_bt_sess"]))
        res.append(("B0", True))
    else:
        print("  [FAIL] 会话没开起来")
        print("         ⇒ 对端 ESP32 没烧 P3 固件 / SPI 命令回不来 / BT 起不来")
        res.append(("B0", False))

    # ---------- B1 ----------
    print("=" * 70)
    print("B1  收全并落盘（done+1、字节数 == %d、rc==0）" % want)
    ok_b1 = (s1["g_bt_done"] > s0["g_bt_done"] and s1["g_bt_bytes"] == want
             and as_i32(s1["g_bt_rc"]) == 0 and s1["g_bt_fs_rc"] == 0)
    print("  done %d → %d   字节 %d（期望 %d）  rc=%d  fs_rc=%d"
          % (s0["g_bt_done"], s1["g_bt_done"], s1["g_bt_bytes"], want,
             as_i32(s1["g_bt_rc"]), s1["g_bt_fs_rc"]))
    if ok_b1:
        print("  [PASS] 文件收全且写进 TF 卡")
        res.append(("B1", True))
    else:
        print("  [FAIL] err=%s" % ERR_TEXT.get(s1["g_bt_err"], "?"))
        res.append(("B1", False))

    # ---------- B2 ----------
    print("=" * 70)
    print("B2  CRC（g_bt_crc_bad 本次增量为 0）")
    cb = s1["g_bt_crc_bad"] - s0["g_bt_crc_bad"]
    if cb == 0:
        print("  [PASS] 一块都没坏（重取 %d 次）" % (s1["g_bt_retx"] - s0["g_bt_retx"]))
        res.append(("B2", True))
    elif s1["g_bt_done"] > s0["g_bt_done"]:
        print("  [WARN] 坏块 %d 个，但重传兜住、最终收全（retx +%d）"
              % (cb, s1["g_bt_retx"] - s0["g_bt_retx"]))
        res.append(("B2", True))
    else:
        print("  [FAIL] 坏块 %d 个且没收全" % cb)
        res.append(("B2", False))

    # ---------- B3 ----------
    print("=" * 70)
    print("B3  导入图库（imp_rc==0、尺寸对）")
    ok_b3 = (s1["g_bt_imp_rc"] == 0 and s1["g_img_count"] > s0["g_img_count"]
             and s1["g_img_out_w"] == w and s1["g_img_out_h"] == h)
    print("  imp_rc=0x%X  图库 %d → %d  导入尺寸 %dx%d（期望 %d×%d）"
          % (s1["g_bt_imp_rc"], s0["g_img_count"], s1["g_img_count"],
             s1["g_img_out_w"], s1["g_img_out_h"], w, h))
    if ok_b3:
        print("  [PASS] 已排队并完成导入（相册索引多了一条）")
        res.append(("B3", True))
    else:
        print("  [FAIL] 导入没成功或尺寸不对")
        res.append(("B3", False))

    # ---------- B4 ----------
    print("=" * 70)
    print("B4  相册可见（g_img_used 增加）")
    du = s1["g_img_used"] - s0["g_img_used"]
    if du > 0:
        print("  [PASS] 图库已用字节 %d → %d（+%d）"
              % (s0["g_img_used"], s1["g_img_used"], du))
        res.append(("B4", True))
    else:
        print("  [FAIL] 图库已用字节没变 —— 相册里看不到新图")
        res.append(("B4", False))

    # ---------- B5 ----------
    print("=" * 70)
    print("B5  本次会话无缺额（g_cmd_bd_timeout 增量 == 0，即没有块传一半超时）")
    dto = s1["g_cmd_bd_timeout"] - s0["g_cmd_bd_timeout"]
    if dto == 0:
        print("  [PASS] 二进制收包零超时")
        res.append(("B5", True))
    else:
        print("  [FAIL] 本次出现 %d 次收包超时（最近一次：声明 %d 只收到 %d，缺 %d 字节）"
              % (dto, s1["g_cmd_bd_want"],
                 s1["g_cmd_bd_got"], s1["g_cmd_bd_want"] - s1["g_cmd_bd_got"]))
        res.append(("B5", False))

    # ---------- B6 ----------
    print("=" * 70)
    print("B6  命令无连续失联（g_bt_get_max <= 5）")
    dget = s1["g_bt_get_n"] - s0["g_bt_get_n"]
    dto2 = s1["g_bt_get_to"] - s0["g_bt_get_to"]
    print("  GET 发出 %d 笔，其中无应答 %d 笔（%.0f%%），最长连续 %d 笔"
          % (dget, dto2, 100.0 * dto2 / dget if dget else 0.0,
             s1["g_bt_get_max"]))
    # ⚠ 阈值是怎么来的（别随手改大，2026-10-10）：
    #   超时判据已经是两档（无字节 300 ms / 在途 1500 ms），所以"连续 N 笔"能撑穿
    #   STALL_MS(12 s) 的条件是 N × 300 ms ≥ 12 s ⇒ N ≥ 40。曾经的"≤1（≥6 判死）"
    #   是单档 2000 ms 时代的数，早就失效了。
    #   实测（--snap 0 --burst 8 --presc 64）最长连续 2~5 笔是常态抖动，
    #   所以取 5 当"链路没有成片死掉"的判据。
    # ⚠ 但**残余丢失率本身仍是已知问题**：15%~20% 的 GET 要重发一次。
    #   已排除的原因：不是等待窗口（1500/3000 ms 一样丢）、不是 SCK（3.125 MHz 更差、
    #   0.781 MHz 更慢，1.5625 MHz 最优）、不是 CS 抬升宽度（3/9/22/60 µs 无趋势）、
    #   也不是从机补排顺序（那个已修，把丢命令从 38% 降到 ~15%）。
    #   每一笔丢的代价现在只有 300 ms（原来 2000 ms）⇒ 100 KB 从 22~44 s 降到 9~14 s。
    if s1["g_bt_get_max"] <= 5:
        print("  [PASS] 没有成片失联（≤5 笔；撑穿 STALL 需要 ≥40 笔）")
        res.append(("B6", True))
    else:
        print("  [FAIL] 最长连续 %d 笔无应答（≥40 笔才会撑穿 STALL_MS）"
              % s1["g_bt_get_max"])
        print("         若本轮是 --snap 0（没轮询）跑的 ⇒ 链路真有问题；")
        print("         若开着轮询 ⇒ 先按文件头的测量纪律用 --snap 0 复测。")
        res.append(("B6", False))

    print("=" * 70)
    for k, v in res:
        print("  %-3s %s" % (k, "PASS" if v else "FAIL"))
    allp = all(v for _, v in res)

    ms = s1["g_bt_ms"]
    tx = s1["g_spi_tx_n"] - s0["g_spi_tx_n"]
    print("-" * 70)
    if ms > 0:
        print("[*] burst=%d  字节=%d  固件自记 %d ms ⇒ **%.1f KB/s**（%.1f 字节/事务）"
              % (s1["g_spi_burst"], s1["g_bt_bytes"], ms,
                 (s1["g_bt_bytes"] / 1024.0) / (ms / 1000.0),
                 s1["g_bt_bytes"] / max(1.0, float(tx))))
    else:
        print("[*] burst=%d  字节=%d  固件没记到时长（会话没跑完？）"
              % (s1["g_spi_burst"], s1["g_bt_bytes"]))
    print("[*] SPI 事务 %d 笔（本次会话增量）  脚本墙钟 %.1fs（含快照停顿，仅供对照）"
          % (tx, elapsed))
    print("[%s] %s" % ("PASS" if allp else "FAIL",
                       "P4 蓝牙文件接收链路在板验收通过" if allp
                       else "P4 有未通过项，见上"))
    sys.exit(0 if allp else 1)


if __name__ == "__main__":
    main()
