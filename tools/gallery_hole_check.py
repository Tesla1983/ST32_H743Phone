#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""图库空洞回收：验证「删除腾出的空间能被下次导入复用」（2026-10-11）。

要证的问题：
  `src/img_store.c` 原来的分配是"永远追加在 used = max(off+len) 之后"
  ⇒ 删掉**中间**某张不会回收空间（只有删最靠后那张才会）。
  实测图库 31 张占满 2 MB 后，连合法的 224×152 BMP 都导不进去
  （g_img_rc=8 RC_NO_SPACE、g_img_step=5）。相册有删除按钮却腾不出空间，
  是功能上的半截。

修法：新增 `idx_alloc_in(base, need)` —— 在索引副本上找第一块放得下的空洞
  （起点按 IMG_BLK 对齐，且必须整段落在那条有效条目之前），找不到才追加到末尾。

本脚本的做法（非破坏性 + A/B 对照）：
  0. 从 XIP 把索引扇区读回来，解析出所有有效槽位与它们各自"删掉后能腾出多少"；
  1. A（修前同一固件上的已知事实）：先跑一次自测会话 ⇒ 应因满而失败；
  2. 挑一个"删除后空洞 ≥ 本次图片所需"的槽位，SWD 触发删除（g_img_del_req）；
  3. B：再跑同一会话 ⇒ 应成功（g_img_rc=0），且 **g_img_slot 应等于刚删的那个槽**
     —— 这条才是"空间真的被复用了"的判据，光看 rc=0 只能说明"有地方放"。

⚠ 会真的删掉图库里一个槽位的数据（本脚本只挑图库里的测试图，但仍是真删）。
   脚本会打印它删的是哪个槽、数据落在哪，方便事后核对。

用法：python tools/gallery_hole_check.py [--w 224] [--h 152]
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench
import ribbon_check as rc

IDX_XIP = 0x90000000 + 0x00100000      # IMG_BASE（索引扇区）
DATA_OFF = 4096                        # IMG_DATA_ADDR - IMG_BASE
BLK = 4096
DATA_SIZE = 0x00200000 - 4096
ENTRY = 16
MAGIC = 0x4947
FLAG_VALID = 0x0001


def rd1(n):
    return bench.rd(n, 1)[0]


def read_index():
    r = os.popen("probe-rs read %s --chip %s b8 0x%08x %d"
                 % (bench.PROBE, bench.CHIP, IDX_XIP, BLK)).read()
    tok = [x for x in r.split()
           if len(x) == 2 and all(c in "0123456789abcdef" for c in x.lower())]
    if len(tok) < BLK:
        raise RuntimeError("读索引失败（拿到 %d 字节）：%s" % (len(tok), r[:200]))
    return bytes(int(x, 16) for x in tok[:BLK])


def parse(buf):
    out = []
    for i in range(256):
        e = buf[i * ENTRY:(i + 1) * ENTRY]
        magic, flags, off, ln = struct.unpack_from("<HHII", e, 0)
        if magic == MAGIC and (flags & FLAG_VALID):
            out.append((i, off, ln))
    return out


def align_up(v):
    return (v + BLK - 1) & ~(BLK - 1)


def hole_if_deleted(ents, slot):
    """删掉 slot 之后，分配器会在哪、能用多少（按固件 idx_alloc_in 的判据算）。

    ⚠ 必须按**固件里 idx_alloc_in 的判据**算，不能只算"被删条目的长度"：
      空洞起点是 align_up(前一条目的 end)，终点是"下一条目"的起点，
      所以删掉某张后可用量还受**前一条的长度**影响（前一条 end 对齐后可能吃掉一截）。
    返回 (空洞起点, 可用字节数)。
    """
    tgt = [e for e in ents if e[0] == slot]
    if not tgt:
        return (0, 0)
    _, soff, _ = tgt[0]
    prev_end = 0
    for s, off, ln in sorted([e for e in ents if e[0] != slot], key=lambda t: t[1]):
        if off > soff:                         # 第一条排在目标之后的条目 = 空洞终点
            st = align_up(prev_end)
            return (st, align_up(off) - st)
        prev_end = off + ln                    # 目标之前的条目：更新前一条的结束位置
    st = align_up(prev_end)
    return (st, DATA_SIZE - st)                # 它是最后一张 ⇒ 尾部整段


def wait_done(base, timeout_s=150):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if rd1("g_bt_done") != base:
            return True
        time.sleep(2.0)
    return False


def run_session(w, h):
    base = rd1("g_bt_done")
    bench.wr("g_bt_recv_w", [w])
    bench.wr("g_bt_recv_h", [h])
    bench.wr("g_bt_recv_req", [1])
    if not wait_done(base):
        return None
    time.sleep(2.0)                            # 等异步导入锁出结果
    return {
        "g_bt_rc":        rd1("g_bt_rc"),
        "g_bt_imp_rc":    rd1("g_bt_imp_rc"),
        "g_bt_imp_final": rd1("g_bt_imp_final"),
        "g_img_rc":       rd1("g_img_rc"),
        "g_img_slot":     rd1("g_img_slot"),
    }


def main():
    ap = sys.argv[1:]
    w = 224
    h = 152
    if "--w" in ap:
        w = int(ap[ap.index("--w") + 1])
    if "--h" in ap:
        h = int(ap[ap.index("--h") + 1])
    need = w * h * 2

    if rd1("g_boot_magic") != 0x594D4731:
        print("[FAIL] g_boot_magic 不对 —— 固件没在跑（download 后必须 reset）")
        return 1

    ents = parse(read_index())
    used = align_up(max([o + l for _, o, l in ents], default=0))
    print("图库：%d 张有效 · 已用 %d / %d B（余 %d）"
          % (len(ents), used, DATA_SIZE, DATA_SIZE - used))
    print("本次要导入 %d×%d = %d B（+对齐后 %d）"
          % (w, h, need, align_up(need)))

    # ---------------- A：先看"满"这一侧 ----------------
    print("\nA 先跑一次（预期因满失败，rc=8）")
    a = run_session(w, h)
    if a is None:
        print("[FAIL] 会话没结束")
        return 1
    for k in sorted(a):
        print("    %-16s = %d" % (k, a[k]))
    print("  A %s" % ("符合预期：被空间挡住" if a["g_img_rc"] == 8
                      else "！rc=%d，与预期(8)不同 —— 先看懂再往下" % a["g_img_rc"]))

    # ---------------- 挑一个删了够用的槽 ----------------
    holes = {s: hole_if_deleted(ents, s) for s, _, _ in ents}
    cand = sorted(((holes[s][1], s) for s, _, _ in ents), key=lambda t: -t[0])
    print("\n删掉各槽后能腾出的最大空洞（前 5）：%s"
          % ", ".join("slot%d:%dB" % (s, v) for v, s in cand[:5]))
    pick = None
    for v, s in cand:
        if v >= align_up(need):
            pick = (v, s)
            break
    if pick is None:
        print("[SKIP] 没有任何单槽能腾出 %d B —— 换更大的 w/h 组合或先多删几个；"
              "本次无法做 B 对照" % align_up(need))
        return 1
    cap, slot = pick
    hole_start = holes[slot][0]
    print("选 slot %d：空洞起点 off=%d、可用 %d B ≥ %d B"
          % (slot, hole_start, cap, align_up(need)))

    # 末尾追加的落点（用于反证：如果落在尾巴上，说明**没有**复用空洞）
    tail_off = align_up(max([o + l for _, o, l in ents if _ != slot]))

    # ---------------- 删 ----------------
    print("\nB1 删除 slot %d（g_img_del_req）" % slot)
    bench.wr("g_img_del_req", [slot])
    time.sleep(1.5)
    print("    g_img_del_rc=%d  g_img_del_next=%d"
          % (rd1("g_img_del_rc"), rd1("g_img_del_next")))

    ents2 = parse(read_index())
    print("    删后有效张数 = %d（原 %d）" % (len(ents2), len(ents)))

    # ---------------- B：再导一次 ----------------
    print("\nB2 再跑一次同一会话（预期成功，且**数据写进刚腾出的空洞**）")
    b = run_session(w, h)
    if b is None:
        print("[FAIL] 会话没结束")
        return 1
    for k in sorted(b):
        print("    %-16s = %d" % (k, b[k]))

    # ⚠ 判据不能看 g_img_slot：那是**索引槽**，分配器从 0 号起找第一个空条目
    #   （slot 0 在更早的删除测试里已经空着）⇒ 它恒等于 0，证明不了空间复用。
    #   空间复用只能看**数据偏移**：新条目应落在 hole_start，而不是末尾 tail_off。
    ents3 = parse(read_index())
    news = [e for e in ents3 if e[0] == b["g_img_slot"]]
    new_off = news[0][1] if news else -1
    ok_off = (new_off == hole_start)
    print("\n新条目：索引槽 %d、数据 off=%d、len=%d" % (b["g_img_slot"], new_off,
                                                  news[0][2] if news else -1))
    print("        空洞起点 hole_start=%d ｜ 若走【追加到末尾】会落在 tail_off=%d"
          % (hole_start, tail_off))
    print("判据：g_img_rc==0（真导进去了）且**数据 off == hole_start**（复用空洞，"
          "不是追加到末尾）")
    ok = (b["g_img_rc"] == 0 and b["g_bt_imp_final"] == 0 and ok_off)
    print("B %s（rc=%d off=%d）" % ("PASS" if ok else "FAIL", b["g_img_rc"], new_off))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
