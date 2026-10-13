#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""I2S TX DMA 板上验收 / 根因定位（2026-10-13）

背景：第一版 I2S DMA（DMA1_Stream2 循环双缓冲）当年的结论是"SPI_SR 恒为 0、
NDTR 不涨"，追两轮未果退回 TIM6 每 20 µs 中断滴灌（≈5 万次/秒中断）。
但那个结论有个方法论隐患：当时是主机侧 **直接读外设寄存器** —— 而本工程自己的
记录写着"probe-rs 读 0x4000_xxxx 不可信，只能读 RAM"。所以这次全部走 RAM 快照
（g_ao_dbg2），并显式补上最可能漏的一步 SPI_CFG1.TXDMAEN。

本脚本做的事：置 g_ao_use_dma=1 → 触发内置正弦自检（3 s）→ 采 RAM 快照 → 判定。

判据：
  D1  DMA 起播成功：g_ao_dma_rc == 0
  D2  DMA 在搬：g_ao_dbg2[5]（DMA_SxNDTR）在两次采样间**变化**（0..4096 之间）
  D3  半/全传中断在响：g_ao_dbg2[9](half)/[10](cplt) 增长
  D4  SPI 的 DMA 请求开了：g_ao_dbg2[0] & 0x8000（CFG1.TXDMAEN）!= 0
  D5  DMAMUX 请求号对：g_ao_dbg2[8] & 0x7F == 40（SPI2_TX）
  D6  出声链路通：g_ao_frames > 0，且 **g_ao_isr == 0**（TIM6 已不参与）
  D7  无 DMA 错误：g_ao_dbg2[11] == 0

⚠ 只验数据通路；听不听得见仍由耳朵判定。
用法：
    python tools/i2s_dma_check.py
"""
import sys
import time

sys.path.insert(0, "tools")
import bench


def g(name):
    return bench.rd(name, 1)[0]


def rd_dbg2():
    return bench.rd("g_ao_dbg2", 16)


def alive():
    a = g("g_loop_count")
    time.sleep(0.5)
    return g("g_loop_count") != a


def main():
    ok = True

    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1

    print("置 g_ao_use_dma=1 并触发内置正弦自检（3 s，走 DMA 路径）")
    bench.wr("g_ao_use_dma", [1])
    bench.wr("g_ao_demo", [1])
    time.sleep(0.5)

    s1 = rd_dbg2()
    print("  采样1：CFG1=0x%08X CR1=0x%08X SR=0x%08X I2SCFGR=0x%08X"
          % (s1[0], s1[1], s1[2], s1[3]))
    print("         DMA_CR=0x%08X NDTR=%d PAR=0x%08X M0AR=0x%08X MUX_CCR=0x%08X"
          % (s1[4], s1[5], s1[6], s1[7], s1[8]))
    print("         half=%d cplt=%d err=%d ring_used=%d ISER0=0x%08X AHB1ENR=0x%08X ready=%d"
          % (s1[9], s1[10], s1[11], s1[12], s1[13], s1[14], s1[15]))

    time.sleep(0.7)
    s2 = rd_dbg2()
    fr = g("g_ao_frames")
    isr = g("g_ao_isr")
    rc = g("g_ao_dma_rc")

    print("  采样2：NDTR=%d half=%d cplt=%d err=%d"
          % (s2[5], s2[9], s2[10], s2[11]))
    print("  g_ao_frames=%d  g_ao_isr(TIM6)=%d  g_ao_dma_rc=%d" % (fr, isr, rc))

    if rc != 0:
        print("[FAIL] D1 DMA 起播失败：g_ao_dma_rc=%d（1=HAL_DMA_Init 失败 2=Start_IT 失败）" % rc)
        ok = False
    else:
        print("[PASS] D1 DMA 起播成功（g_ao_dma_rc=0）")

    if (s1[0] & 0x8000) == 0:
        print("[FAIL] D4 SPI_CFG1.TXDMAEN=0 —— SPI 不会发 DMA 请求（这正是关键漏项）")
        ok = False
    else:
        print("[PASS] D4 SPI_CFG1.TXDMAEN=1（CFG1=0x%08X）" % s1[0])

    mux = s1[8] & 0x7F
    if mux != 40:
        print("[FAIL] D5 DMAMUX 请求号 = %d，应为 40（SPI2_TX）" % mux)
        ok = False
    else:
        print("[PASS] D5 DMAMUX 请求号 = 40（SPI2_TX）")

    if s2[5] == s1[5]:
        print("[FAIL] D2 DMA_SxNDTR 两次采样都是 %d —— DMA 没在搬（请求没来 / 时钟没起）"
              % s2[5])
        ok = False
    else:
        print("[PASS] D2 DMA 在搬：NDTR %d → %d" % (s1[5], s2[5]))

    if s2[9] > s1[9] or s2[10] > s1[10]:
        print("[PASS] D3 半/全传中断在响：half %d→%d  cplt %d→%d"
              % (s1[9], s2[9], s1[10], s2[10]))
    else:
        print("[FAIL] D3 半/全传中断没响（half %d→%d cplt %d→%d）"
              % (s1[9], s2[9], s1[10], s2[10]))
        ok = False

    if fr > 0 and isr == 0:
        print("[PASS] D6 出声链路走 DMA：frames=%d（在涨），TIM6 isr=0（已不参与）" % fr)
    elif fr == 0:
        print("[FAIL] D6 g_ao_frames=0：DMA 在搬但没有有效音频帧（也许是静音）")
        ok = False
    else:
        print("[WARN] D6 frames=%d 但 TIM6 isr=%d ≠ 0（两条路同时在跑？）" % (fr, isr))
        ok = False

    if s2[11] != 0:
        print("[WARN] D7 DMA 错误回调 %d 次" % s2[11])
        ok = False
    else:
        print("[PASS] D7 无 DMA 错误")

    # 收尾：恢复默认（TIM6 路径）。demo 自己 3 s 播完会停，不需要额外触发。
    time.sleep(0.2)
    print("恢复 g_ao_use_dma=0（默认走 TIM6）")
    bench.wr("g_ao_use_dma", [0])

    print("\n[PASS] I2S TX DMA 验收通过（听感由耳朵判定）" if ok
          else "\n[FAIL] I2S TX DMA 验收未通过（见上）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
