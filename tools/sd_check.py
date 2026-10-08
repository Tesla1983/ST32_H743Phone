#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""TF 卡（SDMMC1 轮询通路）一键验收 —— 路线图 L2-1 / L2-2 的判据。

它回答的问题是整数、不是肉眼：

  ① 卡能不能识别            g_sd_init_rc == 0，容量 > 0
  ② 写→读逐字节比对（单扇区） g_sd_mis1 == 0
  ③ 写→读逐字节比对（8 扇区） g_sd_mis2 == 0
  ④ 原内容恢复是否成功       g_sd_restore_mis == 0（对卡做到无损）

为什么必须有 ④：卡是用户自己的 32 GB FAT 卡，上面可能有数据。
自检选在卡**尾部**扇区，且"先备份 → 写图案 → 比对 → 写回原内容 → 再验证"。
④ 不过就说明卡内容被改动了，必须当场报告，不能当作测试通过。

用法
----
    python tools/sd_check.py

退出码
------
    0   四项全过
    1   前置不满足（主循环没跑 / 卡没识别 / 符号重名）
    2   数据通路失配（② 或 ③ 非 0）—— SDMMC 配置有问题
    3   恢复失败（④ 非 0）—— 卡内容被改动，需人工检查该扇区
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

# 与 src/sd_card.c 的 SD_BENCH_NSEC 保持一致
NSEC = 8
SECTOR_BYTES = 512
MULTI_BYTES = NSEC * SECTOR_BYTES

VARS = [
    "g_sd_test", "g_sd_state", "g_sd_init_rc",
    "g_sd_card_type", "g_sd_block_nbr", "g_sd_block_size", "g_sd_cap_mb",
    "g_sd_clk_div", "g_sd_sector",
    "g_sd_bak_rc", "g_sd_write_rc", "g_sd_read_rc",
    "g_sd_mis1", "g_sd_mis2", "g_sd_restore_rc", "g_sd_restore_mis",
    "g_sd_wr_cyc", "g_sd_rd_cyc", "g_sd_buf_addr", "g_sd_card_state",
    # 第二轮新增：用于定位"间歇失败"的寄存器级证据
    "g_sd_wr_addr", "g_sd_rd_addr", "g_sd_bak_addr",
    "g_sd_hal_rc", "g_sd_errcode", "g_sd_sta", "g_sd_retry", "g_sd_step",
    "g_sd_hwfc",
]

STEP_NAME = {0: "空闲", 1: "备份读", 2: "①写", 3: "①读",
             4: "②写", 5: "②读", 6: "恢复写", 7: "恢复读"}

# SDMMC->STA 里我们关心的位（RM0433）
STA_BITS = [
    (1 << 21, "RXFIFOE 接收 FIFO 空"),
    (1 << 20, "TXFIFOE 发送 FIFO 空"),
    (1 << 19, "RXFIFOF 接收 FIFO 满"),
    (1 << 18, "TXFIFOF 发送 FIFO 满"),
    (1 << 17, "RXFIFOHF 接收 FIFO 半满"),
    (1 << 15, "RXACT 正在接收"),
    (1 << 12, "TXACT 正在发送"),
    (1 << 11, "CMDACT 命令正在传输"),
    (1 << 8,  "RXOVER 接收 FIFO 上溢 ★"),
    (1 << 7,  "DCRCFAIL 数据 CRC 失败 ★"),
    (1 << 5,  "DTIMEOUT 数据超时 ★"),
    (1 << 4,  "TXUNDERR 发送 FIFO 下溢 ★"),
    (1 << 3,  "DATAEND 数据结束"),
    (1 << 2,  "DBCKEND 数据块结束"),
]


def sta_desc(sta):
    if sta == 0:
        return "无异常标志"
    hit = [d for m, d in STA_BITS if sta & m]
    return " / ".join(hit) if hit else "0x%08X（无已知位命中）" % sta

# 枚举值取自 stm32h7xx_hal_sd.h:377-379（不是猜的）：
#   0=CARD_SDSC(<2GB)  1=CARD_SDHC_SDXC(<32GB / SDXC<2TB)  3=CARD_SECURED
# 第一次写错成 {1: SDSC}，把 29 GB 的卡显示成 "SDSC(<=2GB)" —— 读数是 1，
# 按 HAL 的定义 1 正是 SDHC/SDXC。
CARD_TYPE = {0: "SDSC(<2GB)", 1: "SDHC/SDXC", 3: "SECURED"}


def rd1(n):
    return bench.rd(n, 1)[0]


def check_symbols():
    dup = {n: bench.T[n] for n in VARS if len(bench.T.get(n, [])) != 1}
    if dup:
        print("[FAIL] 以下符号缺失或重名，读数会取错地址：%s" % dup)
        return False
    return True


def run_once():
    if not check_symbols():
        return 1

    # ---- 前置：内核必须在跑（probe-rs download 之后内核仍 halt，必须 reset）----
    a = rd1("g_loop_count")
    time.sleep(1.0)
    b = rd1("g_loop_count")
    if a == b:
        print("[FAIL] 主循环没在跑（g_loop_count %d 两次不变）→ 先 probe-rs reset" % a)
        return 1
    print("主循环在跑：g_loop_count %d → %d" % (a, b))

    # ---- 触发自检 ----
    bench.wr("g_sd_state", [0])
    bench.wr("g_sd_test", [1])

    # ---- 等跑完：轮询模式下 4KB 写 + 读 + 恢复，几百毫秒量级，给 15 秒 ----
    state = 0
    for _ in range(150):
        time.sleep(0.1)
        state = rd1("g_sd_state")
        if state != 0:
            break
    if state == 0:
        print("[FAIL] 自检 15 秒内没跑完（g_sd_state 仍为 0）—— 卡可能卡在忙状态")
        return 1
    if state == 1:
        rc = rd1("g_sd_init_rc")
        print("[FAIL] 卡初始化失败：g_sd_init_rc = %d（0=HAL_OK）" % rc)
        print("       常见原因：卡没插到底 / 引脚虚焊 / ClockDiv 太小 / 卡槽供电")
        return 1

    init_rc   = rd1("g_sd_init_rc")
    ctype     = rd1("g_sd_card_type")
    blocks    = rd1("g_sd_block_nbr")
    bsize     = rd1("g_sd_block_size")
    cap_mb    = rd1("g_sd_cap_mb")
    clkdiv    = rd1("g_sd_clk_div")
    sector    = rd1("g_sd_sector")
    bak_rc    = rd1("g_sd_bak_rc")
    write_rc  = rd1("g_sd_write_rc")
    read_rc   = rd1("g_sd_read_rc")
    mis1      = rd1("g_sd_mis1")
    mis2      = rd1("g_sd_mis2")
    rest_rc   = rd1("g_sd_restore_rc")
    rest_mis  = rd1("g_sd_restore_mis")
    wr_cyc    = rd1("g_sd_wr_cyc")
    rd_cyc    = rd1("g_sd_rd_cyc")
    buf_addr  = rd1("g_sd_buf_addr")
    card_st   = rd1("g_sd_card_state")
    hal_rc    = rd1("g_sd_hal_rc")
    errcode   = rd1("g_sd_errcode")
    sta       = rd1("g_sd_sta")
    retry     = rd1("g_sd_retry")
    step      = rd1("g_sd_step")
    hwfc      = rd1("g_sd_hwfc")

    print("")
    print("缓冲地址：wr=0x%08X rd=0x%08X bak=0x%08X（固件自报，不猜段布局）"
          % (rd1("g_sd_wr_addr"), rd1("g_sd_rd_addr"), rd1("g_sd_bak_addr")))
    print("硬件流控：%d（1=已启用，FIFO 满/空时 SDMMC 自动暂停时钟）" % hwfc)
    print("卡识别：init_rc=%d  类型=%s  块 %d × %d B = %d MB（%.2f GB）"
          % (init_rc, CARD_TYPE.get(ctype, "未知(%d)" % ctype), blocks, bsize,
             cap_mb, cap_mb / 1024.0))
    print("SDMMC_CK：sdmmc_ker_ck(200MHz) / (2 × ClockDiv=%d) = %.1f MHz"
          % (clkdiv, 200.0 / (2.0 * clkdiv) if clkdiv else 0.0))
    print("测试扇区：%d（卡尾部，避开 MBR 与 FAT 前部）；缓冲地址 0x%08X"
          % (sector, buf_addr))
    print("卡状态：HAL_SD_GetCardState = %d（4 = TRANSFER 就绪）" % card_st)
    print("")
    print("① 单扇区 512 B   备份rc=%d 写rc=%d 读rc=%d   失配 %d 字节   判据 0   %s"
          % (bak_rc, write_rc & 0xFF, read_rc & 0xFF, mis1,
             "OK" if mis1 == 0 else "FAIL"))
    print("② 8 扇区 %d B   写rc=%d 读rc=%d            失配 %d 字节   判据 0   %s"
          % (MULTI_BYTES, (write_rc >> 8) & 0xFF, (read_rc >> 8) & 0xFF, mis2,
             "OK" if mis2 == 0 else "FAIL"))
    print("③ 原内容恢复      恢复rc=%d                    失配 %d 字节   判据 0   %s"
          % (rest_rc, rest_mis, "OK" if rest_mis == 0 else "FAIL"))
    print("")
    print("失败现场（成功时全为 0 / OK）：")
    print("  HAL 返回码=%s  ErrorCode=0x%08X  重试 %d 次  停在步骤：%s"
          % ({0: "OK", 1: "ERROR", 2: "BUSY", 3: "TIMEOUT"}.get(hal_rc, "0x%08X" % hal_rc),
             errcode, retry, STEP_NAME.get(step, "未知(%d)" % step)))
    print("  SDMMC->STA = 0x%08X  ⇒ %s" % (sta, sta_desc(sta)))
    if wr_cyc:
        print("吞吐：写 %d B = %.2f ms（%.2f MB/s）   读 %d B = %.2f ms（%.2f MB/s）"
              % (MULTI_BYTES, wr_cyc / 400000.0,
                 (MULTI_BYTES / 1e6) / (wr_cyc / 400e6),
                 MULTI_BYTES, rd_cyc / 400000.0,
                 (MULTI_BYTES / 1e6) / (rd_cyc / 400e6)))
        print("      （25 MHz 4-bit 理论上限约 12.5 MB/s；轮询模式 CPU 搬 FIFO，会低于此值）")

    # ---- 判定 ----
    if init_rc != 0 or cap_mb == 0:
        print("")
        print("[FAIL] 卡没识别（init_rc=%d，容量 %d MB）" % (init_rc, cap_mb))
        return 1

    if mis1 != 0 or mis2 != 0:
        print("")
        print("[FAIL] 写→读比对失配（单扇区 %d 字节 / 多扇区 %d 字节）" % (mis1, mis2))
        print("       数据通路有问题。下一步按下列顺序排查：")
        print("       1. 失配是否成片且按 512 B 对齐 ⇒ 多块传输地址没递增；")
        print("       2. 失配恒为某固定值 ⇒ 卡没真写进去（写保护 / 状态未等 TRANSFER）；")
        print("       3. 先跑 python tools/dma_check.py 确认内存侧没问题，再怀疑 SDMMC。")
        return 2

    if rest_mis != 0 or rest_rc != 0:
        print("")
        print("[FAIL] 原内容恢复失败（rc=%d，失配 %d 字节）" % (rest_rc, rest_mis))
        print("       ⚠ 扇区 %d 起的 %d 个扇区内容可能已被改写，请人工检查！"
              % (sector, NSEC))
        print("       还原办法：内存备份还在（未复位）就直接 python tools/sd_restore.py；")
        print("       若已复位，用先前抢救出来的 build/sd_bss_sd.bin 同样走 sd_restore.py。")
        return 3

    print("")
    print("[PASS] TF 卡数据通路打通，四项判据全过：")
    print("   · 卡识别为 %s、%d MB；" % (CARD_TYPE.get(ctype, "未知"), cap_mb))
    print("   · 单扇区写→读 512/512 字节一致；")
    print("   · 连续 %d 扇区写→读 %d/%d 字节一致（每个扇区图案不同 ⇒ 地址递增正确）；" %
          (NSEC, MULTI_BYTES, MULTI_BYTES))
    print("   · 原内容已完整写回并验证 ⇒ **卡上数据无损**。")
    print("")
    print("⇒ 下一步：L2-3（FatFs 挂到 sd_read_blocks / sd_write_blocks 上）。")
    print("  ⚠ L2-4（IDMA 高速模式）必须等 L3（SRAM1 非缓存 DMA 区）做完再动，")
    print("    否则会引入 dma_check.py 用例② 那种「不报错、不 HardFault」的静默损坏。")
    return 0


def main():
    """--repeat N：连跑 N 轮。

    为什么要连跑：第一轮四项全过、第二轮却翻车过一次 —— 单次通过不能证明通路稳，
    间歇失败正是本工程最警惕的东西。所以验收要看"连续 N 轮全过"，
    任何一轮失败都要把失败现场（STA / ErrorCode / 步骤号）打出来。"""
    n = 1
    if "--repeat" in sys.argv:
        n = int(sys.argv[sys.argv.index("--repeat") + 1])

    fails = 0
    for i in range(n):
        if n > 1:
            print("")
            print("===================== 第 %d / %d 轮 =====================" % (i + 1, n))
        rc = run_once()
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
