#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""TF 卡 **IDMA** 通路一键验收 —— 路线图 L2-4 的判据。

前置：L3 必须已完成（SRAM1 用 MPU 配成非缓存 DMA 区），
      判据是 `python tools/dma_check.py` 用例② 失配从 64 变成 0。
      没做 L3 就跑 IDMA，会得到"不报错、不 HardFault"的静默数据损坏 ——
      这个风险 dma_check.py 已经在真机上复现过，不要跳过。

本脚本在一轮里给出三组整数结论：

  A. 轮询基线（mode 1）    失配 0，并记下写/读耗时
  B. IDMA    （mode 4）    失配 0，并记下写/读耗时  ⇒ 与 A 比出加速比
  C. IDMA 缓冲区"非缓存"的**独立证据**  g_sd_dma_stale == 0
     做法：固件先把读缓冲填成哨兵 0x5A 并真读一遍（让它在 cache 里有副本），
     再让 IDMA 从背后改写这块缓冲，最后 CPU 直接读。
     若这块缓冲仍是 cacheable 且没人 invalidate，CPU 会整片读到哨兵（stale 接近 4096）；
     非缓存区下必须一个哨兵字节都读不到。
     （缓冲在 **AXI SRAM 尾部 8 KB**，不是 SRAM1 —— 原因见下面的"IDMA 可达性"一节。）

用法
----
    python tools/sd_dma_check.py            # 轮询基线 + IDMA @25MHz
    python tools/sd_dma_check.py --hs       # 再加一组 IDMA @50MHz（切速失败会自动退回 25MHz）
    python tools/sd_dma_check.py --repeat 5 # 连跑 5 轮，抓间歇失败

【⚠ IDMA 可达性 —— 本工程的硬约束，改缓冲位置前必须先看】
    **SDMMC1 的 IDMA 只能访问 D1 域内存 = AXI SRAM；SRAM1/2/3（D2 域）和
    SRAM4（D3 域）它都到不了**（ST AN5200；NuttX stm32_sdmmc.c 引用同一条）。
    SDMMC2（在 D2 域）才支持 SRAM1/2/3，但本板卡座硬件锁死在 SDMMC1 引脚上。
    上板实测过：缓冲放 SRAM1 时 IDMA 一个字节都搬不动
    （写 TX_UNDERRUN / 读 RX_OVERRUN，DCOUNT 只走 28/4096 字节），
    搬到 AXI 尾部后立刻正常 —— 所以 .bss_sd_dma 段钉在 0x2407E000 不是随意选的。

退出码
------
    0   全部通过
    1   前置不满足（主循环没跑 / 卡没识别 / 符号重名 / 中断没进来）
    2   IDMA 数据通路失配（写进去的读回来不一致）
    3   L3 没生效（IDMA 写后 CPU 仍读到哨兵）—— 最危险的一类：静默损坏
    4   原内容恢复失败 —— 卡上数据可能被改写，需人工检查
    5   --repeat 下存在失败轮次
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

NSEC = 8
SECTOR_BYTES = 512
MULTI_BYTES = NSEC * SECTOR_BYTES
CPU_HZ = 400e6

VARS = [
    "g_sd_test", "g_sd_state", "g_sd_init_rc", "g_sd_clk_div", "g_sd_sector",
    "g_sd_mis2", "g_sd_restore_rc", "g_sd_restore_mis",
    "g_sd_wr_cyc", "g_sd_rd_cyc", "g_sd_hwfc", "g_sd_retry", "g_sd_step",
    # L2-4 新增
    "g_sd_dma_wr_cyc", "g_sd_dma_rd_cyc", "g_sd_dma_mis", "g_sd_dma_stale",
    "g_sd_dma_stale_base",
    "g_sd_dma_hal_rc", "g_sd_dma_sta", "g_sd_dma_errcode", "g_sd_dma_irq",
    "g_sd_dma_wrcplt", "g_sd_dma_rdcplt", "g_sd_dma_errcb",
    "g_sd_speed_mode", "g_sd_dma_buf_addr", "g_sd_dma_clkdiv",
    # 失败现场寄存器快照
    "g_sd_dma_snap_pre", "g_sd_dma_snap_post",
    "g_sd_dma_w_rc", "g_sd_dma_r_rc",
    "g_sd_dma_w_errcode", "g_sd_dma_r_errcode",
    "g_sd_dma_w_sta", "g_sd_dma_r_sta",
]

SNAP_NAME = ["STA", "DCOUNT", "DLEN", "DCTRL", "MASK", "IDMACTRL", "IDMABSIZE", "IDMABASE0"]

STA_BIT_NAME = [
    (1 << 22, "IDMATE IDMA 传输错误 ★"),
    (1 << 21, "RXFIFOE"), (1 << 20, "TXFIFOE"),
    (1 << 19, "RXFIFOF"), (1 << 18, "TXFIFOF"),
    (1 << 17, "RXFIFOHF"), (1 << 15, "RXACT"),
    (1 << 13, "IDMABTC IDMA 缓冲传输完成"),
    (1 << 12, "TXACT"), (1 << 11, "CMDACT"),
    (1 << 8,  "RXOVER ★"), (1 << 7, "DCRCFAIL ★"),
    (1 << 5,  "DTIMEOUT ★"), (1 << 4, "TXUNDERR ★"),
    (1 << 3,  "DATAEND"), (1 << 2, "DBCKEND"),
]

ERR_BIT_NAME = [
    (0x01, "CMD_CRC_FAIL"), (0x02, "DATA_CRC_FAIL"), (0x04, "CMD_RSP_TIMEOUT"),
    (0x08, "DATA_TIMEOUT"), (0x10, "TX_UNDERRUN"), (0x20, "RX_OVERRUN"),
    (0x40, "ADDR_MISALIGNED"), (0x80, "BLOCK_LEN_ERR"),
    (0x100, "ERASE_SEQ_ERR"), (0x200, "BAD_ERASE_PARAM"), (0x400, "WRITE_PROT_VIOL"),
    (0x800, "LOCK_UNLOCK_FAIL"), (0x1000, "COM_CRC_FAILED"), (0x2000, "ILLEGAL_CMD"),
    (0x4000, "CARD_ECC_FAILED"), (0x8000, "CC_ERROR"),
    (0x10000, "GENERAL_UNKNOWN_ERR"), (0x20000, "STREAM_READ_UNDERRUN"),
    (0x40000, "STREAM_WRITE_OVERRUN"), (0x80000, "CID_CSD_OVERWRITE"),
    (0x100000, "WP_ERASE_SKIP"), (0x200000, "CARD_ECC_DISABLED"),
    (0x400000, "ERASE_RESET"), (0x800000, "AKE_SEQ_ERROR"),
    (0x1000000, "REQUEST_NOT_APPLICABLE"), (0x2000000, "PARAM"),
    (0x4000000, "UNSUPPORTED_FEATURE"), (0x8000000, "ADDR_OUT_OF_RANGE"),
]


def bits_desc(val, table):
    if val == 0:
        return "0（无标志）"
    hit = [n for m, n in table if val & m]
    return " / ".join(hit) if hit else "0x%08X（无已知位命中）" % val


def show_snap(tag, arr):
    print("  %-5s " % tag + "  ".join("%s=0x%08X" % (SNAP_NAME[i], arr[i])
                                      for i in range(8)))
    print("         STA ⇒ %s" % bits_desc(arr[0], STA_BIT_NAME))
    print("         剩余 %d B / 共 %d B   DCTRL: DTEN=%d DTDIR=%s DMAEN=%d BLK=%d"
          % (arr[1], arr[2], arr[3] & 1, "读(卡→MCU)" if (arr[3] >> 1) & 1 else "写(MCU→卡)",
             (arr[3] >> 3) & 1, (arr[3] >> 4) & 0xF))
    print("         IDMA: EN=%d 双缓冲=%d  BASE0=0x%08X（缓冲实际地址应与之相同）"
          % (arr[5] & 1, (arr[5] >> 1) & 1, arr[7]))

CARD_RC = {0: "OK", 1: "ERROR", 2: "BUSY", 3: "TIMEOUT"}
SPEED_MODE = {0: "默认速度 25 MHz", 1: "高速 50 MHz", 2: "切速失败，退回 25 MHz"}


def rd1(n):
    return bench.rd(n, 1)[0]


def check_symbols():
    dup = {n: bench.T[n] for n in VARS if len(bench.T.get(n, [])) != 1}
    if dup:
        print("[FAIL] 以下符号缺失或重名，读数会取错地址：%s" % dup)
        return False
    return True


def trigger(mode, timeout_s=20.0):
    """写 g_sd_test=mode 触发一轮，等 g_sd_state 非 0。返回 g_sd_state。"""
    bench.wr("g_sd_state", [0])
    bench.wr("g_sd_test", [mode])
    n = int(timeout_s / 0.1)
    for _ in range(n):
        time.sleep(0.1)
        st = rd1("g_sd_state")
        if st != 0:
            return st
    return 0


def mbps(cyc):
    if cyc == 0:
        return 0.0, 0.0
    sec = cyc / CPU_HZ
    return sec * 1000.0, (MULTI_BYTES / 1e6) / sec


def show(label, wr_cyc, rd_cyc):
    wms, wmb = mbps(wr_cyc)
    rms, rmb = mbps(rd_cyc)
    print("   %-22s 写 %6.3f ms (%6.2f MB/s)   读 %6.3f ms (%6.2f MB/s)"
          % (label, wms, wmb, rms, rmb))


def run_once(with_hs):
    if not check_symbols():
        return 1

    # ---- 前置 1：内核必须在跑 ----
    a = rd1("g_loop_count")
    time.sleep(1.0)
    b = rd1("g_loop_count")
    if a == b:
        print("[FAIL] 主循环没在跑（g_loop_count %d 两次不变）→ 先 probe-rs reset" % a)
        return 1

    # ---- A. 轮询基线 ----
    st = trigger(1)
    if st != 2:
        print("[FAIL] 轮询基线没跑完（g_sd_state=%d）" % st)
        return 1
    base_wr, base_rd = rd1("g_sd_wr_cyc"), rd1("g_sd_rd_cyc")
    base_mis, base_rest = rd1("g_sd_mis2"), rd1("g_sd_restore_mis")

    # ---- B. IDMA @25MHz ----
    bench.wr("g_sd_dma_irq", [0])
    bench.wr("g_sd_dma_wrcplt", [0])
    bench.wr("g_sd_dma_rdcplt", [0])
    bench.wr("g_sd_dma_errcb", [0])
    st = trigger(4)
    if st != 2:
        print("[FAIL] IDMA 用例没跑完（g_sd_state=%d）" % st)
        return 1
    dma_wr, dma_rd = rd1("g_sd_dma_wr_cyc"), rd1("g_sd_dma_rd_cyc")
    dma_mis   = rd1("g_sd_dma_mis")
    dma_stale = rd1("g_sd_dma_stale")
    dma_base  = rd1("g_sd_dma_stale_base")
    # 真正判定"陈旧"的是 stale 相对基线的**增量**：图案里天然就有 ≈4096/256=16 个
    # 字节等于哨兵 0x5A，直接判 stale==0 一定会误报。
    dma_xstale = dma_stale - dma_base if dma_stale != 0xFFFFFFFF else 0xFFFFFFFF
    dma_irq   = rd1("g_sd_dma_irq")
    dma_wrcb  = rd1("g_sd_dma_wrcplt")
    dma_rdcb  = rd1("g_sd_dma_rdcplt")
    dma_errcb = rd1("g_sd_dma_errcb")
    dma_hal   = rd1("g_sd_dma_hal_rc")
    dma_sta   = rd1("g_sd_dma_sta")
    dma_err   = rd1("g_sd_dma_errcode")
    dma_rest  = rd1("g_sd_restore_mis")
    dma_restr = rd1("g_sd_restore_rc")
    dma_clk   = rd1("g_sd_dma_clkdiv")
    buf_addr  = rd1("g_sd_dma_buf_addr")
    sector    = rd1("g_sd_sector")

    # ---- C. IDMA @50MHz（可选）----
    hs = None
    if with_hs:
        st = trigger(5)
        if st != 2:
            print("[FAIL] 高速用例没跑完（g_sd_state=%d）" % st)
            return 1
        _stale = rd1("g_sd_dma_stale")
        hs = dict(
            wr=rd1("g_sd_dma_wr_cyc"), rd=rd1("g_sd_dma_rd_cyc"),
            mis=rd1("g_sd_dma_mis"), stale=_stale, base=rd1("g_sd_dma_stale_base"),
            xstale=_stale - rd1("g_sd_dma_stale_base"),
            mode=rd1("g_sd_speed_mode"), clk=rd1("g_sd_dma_clkdiv"),
            rest=rd1("g_sd_restore_mis"), irq=rd1("g_sd_dma_irq"),
            hal=rd1("g_sd_dma_hal_rc"), sta=rd1("g_sd_dma_sta"),
            err=rd1("g_sd_dma_errcode"),
        )

    # ================= 报告 =================
    print("")
    print("测试扇区 %d（卡尾部）   IDMA 缓冲 0x%08X   硬件流控=%d"
          % (sector, buf_addr, rd1("g_sd_hwfc")))
    print("IDMA 用例时钟：ClockDiv=%d ⇒ %.1f MHz" % (dma_clk, 200.0 / (2.0 * dma_clk) if dma_clk else 0.0))
    print("")
    print("【吞吐对比 · 8 扇区 = %d B】" % MULTI_BYTES)
    show("轮询（CPU 搬 FIFO）", base_wr, base_rd)
    show("IDMA @%.0f MHz" % (200.0 / (2.0 * dma_clk) if dma_clk else 0.0), dma_wr, dma_rd)
    if hs:
        show("IDMA %s" % SPEED_MODE.get(hs["mode"], "?"), hs["wr"], hs["rd"])
        if hs["wr"] and dma_wr:
            print("   %-22s 写 ×%.2f   读 ×%.2f  （相对 25MHz IDMA）"
                  % ("高速收益", float(dma_wr) / hs["wr"] if hs["wr"] else 0,
                     float(dma_rd) / hs["rd"] if hs["rd"] else 0))
    if base_wr and dma_wr:
        print("   %-22s 写 ×%.2f   读 ×%.2f"
              % ("IDMA 相对轮询",
                 float(base_wr) / dma_wr if dma_wr else 0,
                 float(base_rd) / dma_rd if dma_rd else 0))
    print("")
    print("【中断链路】SDMMC1_IRQHandler 进入 %d 次 · TxCplt %d · RxCplt %d · ErrorCb %d"
          % (dma_irq, dma_wrcb, dma_rdcb, dma_errcb))
    print("【判据】IDMA 读回失配 %d 字节（判据 0）   %s"
          % (dma_mis, "OK" if dma_mis == 0 else "FAIL"))
    print("【判据】★哨兵残留 %d 字节 − 基线 %d = %d（判据 0）   %s"
          % (dma_stale, dma_base, dma_xstale, "OK" if dma_xstale == 0 else "FAIL"))
    print("         （先把读缓冲填 0x5A 并读一遍，再让 IDMA 改写；基线 = 图案里本来就有的"
          " 0x5A 字节数，约 4096/256≈16。\n"
          "          超出基线才说明 CPU 读到的是 cache 里的旧副本）")
    print("【判据】原内容恢复失配 %d 字节（判据 0）  %s"
          % (dma_rest, "OK" if dma_rest == 0 else "FAIL"))
    if with_hs and hs:
        print("【判据】高速组：失配 %d  哨兵 %d−基线 %d=%d  恢复失配 %d  速度=%s"
              % (hs["mis"], hs["stale"], hs["base"], hs["xstale"], hs["rest"],
                 SPEED_MODE.get(hs["mode"], "?")))
    print("")
    print("失败现场（成功时全 0 / OK）：")
    print("  IDMA 写用例 rc=%d  ErrorCode=0x%08X ⇒ %s"
          % (rd1("g_sd_dma_w_rc"), rd1("g_sd_dma_w_errcode"),
             bits_desc(rd1("g_sd_dma_w_errcode"), ERR_BIT_NAME)))
    print("  IDMA 读用例 rc=%d  ErrorCode=0x%08X ⇒ %s"
          % (rd1("g_sd_dma_r_rc"), rd1("g_sd_dma_r_errcode"),
             bits_desc(rd1("g_sd_dma_r_errcode"), ERR_BIT_NAME)))
    print("  轮询重试 %d 次" % rd1("g_sd_retry"))
    pre = bench.rd("g_sd_dma_snap_pre", 8)
    post = bench.rd("g_sd_dma_snap_post", 8)
    print("")
    print("  寄存器快照（pre=启动 IDMA 之后 / post=出错或超时那一刻）：")
    show_snap("pre", pre)
    show_snap("post", post)

    # ================= 判定 =================
    if dma_irq == 0 or dma_wrcb == 0 or dma_rdcb == 0:
        print("")
        print("[FAIL] 中断链路不通（irq=%d TxCplt=%d RxCplt=%d）。" % (dma_irq, dma_wrcb, dma_rdcb))
        print("       先看 startup_gcc.s 里 IRQ49 那个槽位是否真的指向 SDMMC1_IRQHandler；")
        print("       再看 HAL_SD_MspInit 里有没有 HAL_NVIC_EnableIRQ(SDMMC1_IRQn)。")
        return 1

    if dma_xstale != 0:
        print("")
        print("[FAIL] ★ IDMA 缓冲不是非缓存的：哨兵残留比基线多 %d 字节（残留 %d / 基线 %d）。"
              % (dma_xstale, dma_stale, dma_base))
        print("       这就是 dma_check.py 用例② 预告过的静默数据损坏 —— 现在它出现在真实外设上。")
        print("       按下列顺序查：")
        print("       1. MPU 有没有覆盖 IDMA 缓冲 0x2407E000（AXI 尾部 8 KB），属性 NOT_CACHEABLE；")
        print("       2. 那条 region 的编号必须**大于**覆盖整块 AXI 的 region2 ——")
        print("          ARMv7-M 是「编号越大优先级越高」（第一版按反的理解写，实测残留 3438 字节）。")
        return 3

    if dma_mis != 0:
        print("")
        print("[FAIL] IDMA 数据通路失配 %d 字节。" % dma_mis)
        print("       HAL 返回 %s / ErrorCode 0x%08X / STA 0x%08X"
              % (CARD_RC.get(dma_hal, "0x%08X" % dma_hal), dma_err, dma_sta))
        return 2

    if dma_rest != 0 or dma_restr != 0:
        print("")
        print("[FAIL] 原内容恢复失败（rc=%d，失配 %d 字节）" % (dma_restr, dma_rest))
        print("       ⚠ 扇区 %d 起的 %d 个扇区可能已被改写，请人工检查！" % (sector, NSEC))
        print("       还原办法：python tools/sd_restore.py")
        return 4

    if with_hs and hs and (hs["mis"] != 0 or hs["xstale"] != 0 or hs["rest"] != 0):
        print("")
        print("[FAIL] 高速（50MHz）组未全过：失配 %d / 哨兵超基线 %d / 恢复 %d"
              % (hs["mis"], hs["xstale"], hs["rest"]))
        print("       固件在用例结束时已把总线退回默认 25MHz，不影响后续功能。")
        return 2

    print("")
    print("[PASS] IDMA 通路打通，全部判据通过：")
    print("   · 中断链路真实工作（IRQ %d 次、Tx/Rx 各 1 次完成回调、0 次错误回调）；" % dma_irq)
    print("   · IDMA 写→读 %d/%d 字节一致；" % (MULTI_BYTES, MULTI_BYTES))
    print("   · ★哨兵残留不超基线 ⇒ IDMA 缓冲（AXI 尾部 8 KB）确实是非缓存区，"
          "IDMA 写内存后 CPU 无需任何 invalidate；")
    print("   · 原内容已完整写回 ⇒ **卡上数据无损**。")
    return 0


def main():
    with_hs = "--hs" in sys.argv
    n = 1
    if "--repeat" in sys.argv:
        n = int(sys.argv[sys.argv.index("--repeat") + 1])

    fails = 0
    for i in range(n):
        if n > 1:
            print("")
            print("===================== 第 %d / %d 轮 =====================" % (i + 1, n))
        rc = run_once(with_hs)
        if rc != 0:
            fails += 1
            print("       → 本轮退出码 %d" % rc)

    if n > 1:
        print("")
        print("===================== 汇总 =====================")
        print("共 %d 轮，失败 %d 轮 ⇒ %s"
              % (n, fails, "全部通过" if fails == 0 else "存在间歇失败，见上方失败现场"))
    return 0 if fails == 0 else 5


if __name__ == "__main__":
    sys.exit(main())
