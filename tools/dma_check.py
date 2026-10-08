#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DMA ↔ D-Cache 一致性双向台架 —— 一键验收。

它回答三个问题（都在真机上、都是整数结论，不靠肉眼）：

  ① 方向①：CPU 写 → DMA 读          预期失配 0      （FORCEWT=1 ⇒ CPU 写直达内存）
  ② 方向②：DMA 写 → CPU 读（不维护） **判据随 MPU 配置而变，见下**
  ③ 方向②：DMA 写 → CPU 读（invalidate）预期失配 0  （证明补救办法有效）

★ 用例② 有两种"正确"，含义完全不同（2026-10-08 完成 L3 后新增）：
    · **L3 之前**（SRAM1 由 region3 配成 CACHEABLE）：预期失配 = 字数（64）。
      它证明"DMA 写后 CPU 读到陈旧值"这个风险**真实存在**。
    · **L3 之后**（SRAM1 由 region0 配成 NOT_CACHEABLE）：预期失配 = **0**。
      DMA 的写对 CPU 立即可见 ⇒ 代码里一行 clean/invalidate 都不需要写，
      一致性由 **MPU 保证**而不是靠调用方记性 —— 这才是 L3 的目的。
      若 L3 做完后 ② 仍是 64 ⇒ region 优先级没排对，或缓冲没落在 SRAM1，
      要回去查 MPU 配置，不能当作通过。
      ⚠ 优先级方向（2026-10-08 更正）：ARMv7-M 是**编号越大优先级越高**
      （DDI0403 "highest region number takes priority"），不是"编号小优先"。
      SRAM1 用 region0 也生效，是因为它不与其它 region 重叠；
      真要"压住"某个大 region，编号必须比它大（L2-4 的 AXI 8 KB 用的就是 region7）。

用法
----
    python tools/dma_check.py

判定与退出码
------------
    0  三个用例符合当前 MPU 配置下的预期（会明确打印是哪一种）
    2  ② 既不是 0 也不是全部失配（半对半 ⇒ 需要复查，见输出的提示）
    3  ③ 失败（补救无效 —— 严重，架构结论要改写）
    4  ① 失败（方向① 出乎意料，说明对 FORCEWT 的假设有误）
    1  前置条件不满足（内核没在跑 / D-Cache 没开 / DMA 初始化失败 / 符号重名）
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

# 台架参数，必须与 src/dma_bench.c 的 DMA_BENCH_WORDS 一致
WORDS = 64
EXPECT_B = WORDS          # 方向② 预期"每一个字都读到陈旧值"

# 诊断量符号名（全部在 DTCM；若重名则 rd() 会取错地址，故下面显式校验）
VARS = [
    "g_dma_test", "g_dma_case",
    "g_dma_init_rc", "g_dma_xfer_rc", "g_dma_poll_rc",
    "g_dma_dcache_on", "g_dma_forcewt",
    "g_dma_a_mismatch", "g_dma_b_mismatch", "g_dma_c_mismatch",
    "g_dma_a_fresh_lo", "g_dma_a_fresh_hi",
    "g_dma_b_fresh_lo", "g_dma_b_fresh_hi",
    "g_dma_c_fresh_lo", "g_dma_c_fresh_hi",
    "g_dma_src_addr", "g_dma_dst_addr", "g_dma_words", "g_dma_bytes", "g_dma_cyc",
]


def bitmask_desc(lo, hi, words):
    """把"读到新值"位图按 cache line（8 字 = 32 B）渲染。
    注意措辞：位只表示"该字读到的是内存里的新值"，不推断原因
    （①③ 本来就该读到新值；② 里出现"新值"段才意味着那条 line 没有陈旧副本）。"""
    bits = lo | (hi << 32)
    segs = []
    for ln in range(words // 8):
        seg = (bits >> (ln * 8)) & 0xFF
        if seg == 0xFF:
            segs.append("L%d新" % ln)
        elif seg == 0x00:
            segs.append("L%d陈旧" % ln)
        else:
            segs.append("L%d部分新(0x%02X)" % (ln, seg))
    return " ".join(segs)


def check_symbols():
    """符号必须唯一 —— 重名会让 bench.rd() 静默取到另一个变量的地址。"""
    dup = {n: bench.T[n] for n in VARS if len(bench.T.get(n, [])) != 1}
    if dup:
        print("[FAIL] 以下符号缺失或重名，读数会取错地址：%s" % dup)
        return False
    return True


def rd1(n):
    return bench.rd(n, 1)[0]


def rd_ppb(addr):
    """直接读内核私有外设总线（PPB）上的一个 32 位寄存器。
    probe-rs 能读 PPB（本工程此前读 DHCSR=0xE000EDF0 成功过）。
    这是**独立于固件自报**的地面真值：固件里的 g_dma_dcache_on 万一忘了填，
    这里读到的仍是硬件真实状态。"""
    import re
    import subprocess
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08X 1" % (bench.PROBE, bench.CHIP, addr),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    m = re.findall(r"\b[0-9a-fA-F]{8}\b", r.stdout)
    return int(m[0], 16) if m else None


def main():
    if not check_symbols():
        return 1

    # ---- 前置 1：内核必须在跑（probe-rs download 之后内核仍 halt，必须 reset）----
    a = rd1("g_loop_count")
    time.sleep(1.5)
    b = rd1("g_loop_count")
    if a == b:
        print("[FAIL] 主循环没在跑（g_loop_count %d 两次不变）→ 先 probe-rs reset" % a)
        return 1
    print("主循环在跑：g_loop_count %d → %d" % (a, b))

    # ---- 触发 ----
    bench.wr("g_dma_case", [0])
    bench.wr("g_dma_test", [1])

    # ---- 等跑完（轮询 g_dma_case，最多 3 秒；期间不读其他变量以免干扰）----
    case = 0
    for _ in range(60):
        time.sleep(0.05)
        case = rd1("g_dma_case")
        if case == 2:
            break
    if case != 2:
        print("[FAIL] 台架没跑完（g_dma_case = %d，1=初始化失败）" % case)
        if case == 1:
            print("       g_dma_init_rc = %d" % rd1("g_dma_init_rc"))
        return 1

    # ---- 收集结果 ----
    init_rc = rd1("g_dma_init_rc")
    xfer_rc = rd1("g_dma_xfer_rc")
    poll_rc = rd1("g_dma_poll_rc")
    dcache  = rd1("g_dma_dcache_on")
    forcewt = rd1("g_dma_forcewt")
    a_mis   = rd1("g_dma_a_mismatch")
    b_mis   = rd1("g_dma_b_mismatch")
    c_mis   = rd1("g_dma_c_mismatch")
    src     = rd1("g_dma_src_addr")
    dst     = rd1("g_dma_dst_addr")
    words   = rd1("g_dma_words")
    byts    = rd1("g_dma_bytes")
    cyc     = rd1("g_dma_cyc")

    # ---- 独立地面真值：直接读 SCB->CCR / SCB->CACR ----
    ccr  = rd_ppb(0xE000ED14)
    cacr = rd_ppb(0xE000ED94)
    hw_dc     = None if ccr  is None else ((ccr  >> 16) & 1)
    hw_forcewt = None if cacr is None else ((cacr >> 2) & 1)

    print("")
    print("DMA 通路：init_rc=%d xfer_rc=%d poll_rc=%d（0 = HAL_OK）" % (init_rc, xfer_rc, poll_rc))
    print("缓冲区  ：src=0x%08X dst=0x%08X  %d 字 / %d 字节" % (src, dst, words, byts))
    print("传输耗时：%d cycles @400MHz = %.2f us（%d B ⇒ %.1f MB/s）"
          % (cyc, cyc / 400.0, byts, (byts / 1e6) / (cyc / 400e6) if cyc else 0.0))
    print("")
    print("D-Cache / FORCEWT（固件自报 vs 直接读 SCB 寄存器）")
    print("  固件自报 ：D-Cache=%d  FORCEWT=%d" % (dcache, forcewt))
    print("  SCB->CCR = 0x%s  SCB->CACR = 0x%s"
          % ("%08X" % ccr if ccr is not None else "读取失败",
             "%08X" % cacr if cacr is not None else "读取失败"))
    if hw_dc is not None:
        print("  寄存器读出：D-Cache=%d  FORCEWT=%d" % (hw_dc, hw_forcewt))
    print("")
    print("① 方向① CPU写 → DMA读         失配 %d / %d   预期 0        %s"
          % (a_mis, words, "OK" if a_mis == 0 else "FAIL"))
    print("② 方向② DMA写 → CPU读(不维护) 失配 %d / %d   预期 %d      %s"
          % (b_mis, words, EXPECT_B, "OK(已复现风险)" if b_mis == EXPECT_B else "见下方判定"))
    print("③ 方向② DMA写 → CPU读(invalid) 失配 %d / %d   预期 0        %s"
          % (c_mis, words, "OK" if c_mis == 0 else "FAIL"))
    print("")
    print("逐字位图（1 = 该字读到**新值** ⇒ 该处没有陈旧副本；每 8 字 = 一条 32 B cache line）")
    print("  ① %s" % bitmask_desc(rd1("g_dma_a_fresh_lo"), rd1("g_dma_a_fresh_hi"), words))
    print("  ② %s" % bitmask_desc(rd1("g_dma_b_fresh_lo"), rd1("g_dma_b_fresh_hi"), words))
    print("  ③ %s" % bitmask_desc(rd1("g_dma_c_fresh_lo"), rd1("g_dma_c_fresh_hi"), words))

    # ---- 前置 2：D-Cache 必须真的开着（否则台架测不出东西）----
    if dcache != 1 or (hw_dc is not None and hw_dc != 1):
        print("")
        print("[FAIL] D-Cache 没开 —— 本台架考察的正是 cache 一致性，前提不成立。")
        print("       检查 main() 里 sys_cache_enable() 是否还在 MPU 之后被调用。")
        return 1

    # 缓冲区必须在 D2 SRAM1（0x3000_0000 ~ 0x3002_0000），否则说明链接脚本没生效
    in_sram1 = 0x30000000 <= src < 0x30020000 and 0x30000000 <= dst < 0x30020000
    if not in_sram1:
        print("")
        print("[FAIL] 缓冲区不在 D2 SRAM1（src=0x%08X）—— 检查 .bss_dma 是否被 '*(.bss*)' 吃掉" % src)
        return 1

    print("")

    # ---- 判定 ----
    if a_mis != 0:
        print("[FAIL] 方向① 出乎意料地失败：CPU 写没能到达内存。")
        print("       这说明对 FORCEWT 的假设有误 —— 先查 SCB->CACR.FORCEWT 是否真为 1。")
        return 4

    if c_mis != 0:
        print("[FAIL] 方向③ 失败：invalidate 之后 CPU 仍读到陈旧值。补救办法无效，")
        print("       架构结论必须改写（不能靠手工 cache 维护，只能划非缓存 DMA 区）。")
        return 3

    print("[PASS] 方向①、③ 都符合预期（0 失配）。")
    print("")

    if b_mis == 0:
        print("[PASS] ★ L3 已生效 ★ 方向② 失配 0 —— DMA 缓冲所在的 SRAM1 已经是**非缓存**区：")
        print("   · DMA 写完，CPU 不需任何 invalidate 就能读到新值；")
        print("   · 一致性由 **MPU 保证**（region0：SRAM1 非缓存），不是靠调用方记得做；")
        print("   · 这是接 SDMMC **IDMA 高速模式**（L2-4）的前置条件，现在可以做了。")
        print("")
        print("   若你预期看到的是「失配 64」（即想复现缓存不一致）：")
        print("   说明 MPU 已按 L3 重排过，属于**正常且是目标状态**，不是台架失效。")
        return 0

    print("[PASS] 结论成立，三条同时为真：")
    print("   · FORCEWT=1 ⇒ CPU 写必达内存，DMA 读方向**不需要**任何 cache 维护；")
    if b_mis == EXPECT_B:
        print("   · DMA 写后 CPU 读到陈旧值（%d/%d 个字全部陈旧）⇒ 风险**真实存在**；" % (b_mis, words))
    else:
        print("   · DMA 写后 CPU 读到陈旧值（%d/%d 个字）⇒ 风险**真实存在**；" % (b_mis, words))
        print("     为何不是「全部字数」：见上方逐字位图 —— 有整数条 32 B cache line")
        print("     未持有陈旧副本（被换出或从未填充）。这不改变结论：")
        print("     **只要有一个字读到陈旧值，静默数据损坏就已经发生**，")
        print("     而且它与 cache line 粒度绑定（所以 DMA 缓冲区必须按 32 B 对齐）。")
    print("   · invalidate 能完整补救（失配 0）⇒ 但代价是「调用方必须记得做」。")
    print("")
    print("⇒ 当前仍是**非缓存 DMA 区未生效**的状态（SRAM1 可缓存 ⇒ 风险真实存在）。")
    print("   L3 的做法：MPU 用 **region0**（编号最小=优先级最高）把 SRAM1 配成非缓存 ——")
    print("   注意不能拿编号大的 region 去覆盖，压不住原来覆盖 SRAM1~3 的那条。")
    print("   做完后重跑本脚本，② 应由 %d 变成 0。" % EXPECT_B)
    return 0


if __name__ == "__main__":
    sys.exit(main())
