#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""触摸 INT(PB13)→EXTI 门控 板上验收（2026-10-13）

验这条优化：把"每拍都进 I2C 读触摸"改成"INT 边沿中断说有触摸才读"。

判据：
  T1  内核在跑（probe-rs download 后必须 reset，否则一切读数无意义）
  T2  空闲门控生效：静置 2 s，g_tp_poll_skip 的增量 ≈ 帧数（几乎每拍都跳过），
      而 g_tp_scan_count 的增量只 ≈ 帧数/10（心跳兜底），说明空闲时 I2C 基本停了。
  T3  INT/EXTI 通路：手指触碰屏幕时 g_tp_int_count 增长（= INT 线在动、EXTI 向量
      与 NVIC 都通）。此步需要**人手触碰屏幕**；脚本会等待并提示。
      同时 g_tp_points 应增长（说明按下被识别并注入）。

⚠ EXTI 有没有真的触发，SWD 无法凭空造出一次触摸 —— 所以 T3 必须真人碰一下屏。
   若不方便，只看 T2 也能确认"门控逻辑本身在工作"（退化行为不劣于旧版）。

用法：
    python tools/touch_int_check.py          # 默认给 20 s 触碰窗口
    python tools/touch_int_check.py 40       # 自定义等待秒数
"""
import sys
import time

sys.path.insert(0, "tools")
import bench


def g(name):
    return bench.rd(name, 1)[0]


def alive():
    a = g("g_loop_count")
    time.sleep(0.5)
    return g("g_loop_count") != a


def main():
    wait_s = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    ok = True

    if not alive():
        print("[FAIL] 内核没在跑 —— probe-rs download 之后必须 reset。")
        return 1
    print("[PASS] T1 内核在跑")

    # ---------- T2：空闲门控 ----------
    lc0 = g("g_loop_count")
    sk0 = g("g_tp_poll_skip")
    sc0 = g("g_tp_scan_count")
    t0 = time.time()
    time.sleep(2.0)
    lc1 = g("g_loop_count")
    sk1 = g("g_tp_poll_skip")
    sc1 = g("g_tp_scan_count")
    dt = time.time() - t0

    d_loop = lc1 - lc0
    d_skip = sk1 - sk0
    d_scan = sc1 - sc0
    period = g("g_tp_poll_period")
    print("  静置 %.2fs：帧数 +%d，跳过 +%d，扫描 +%d（心跳周期=%d 帧）"
          % (dt, d_loop, d_skip, d_scan, period))

    if d_loop == 0:
        print("[FAIL] T2 主循环没推进（内核可能卡住）")
        return 1
    # 门控生效 ⇒ 跳过占绝大多数帧，扫描只剩心跳那点
    if d_skip >= (d_loop * 7 // 10):
        print("[PASS] T2 空闲门控生效：%d/%d 帧跳过 I2C" % (d_skip, d_loop))
    else:
        print("[WARN] T2 跳过帧数偏少（%d/%d）—— 可能有人正在碰屏，或 INT 线在抖"
              % (d_skip, d_loop))
        ok = False

    expect_scan = max(1, d_loop // max(1, period))
    if d_scan <= expect_scan * 2 + 2:
        print("[PASS] T2b 空闲扫描仅 %d 拍 ≈ 心跳预期 %d 拍（I2C 基本停了）"
              % (d_scan, expect_scan))
    else:
        print("[WARN] T2b 空闲扫描 %d 拍 > 心跳预期 %d 拍（门控可能没完全生效）"
              % (d_scan, expect_scan))
        ok = False

    # ---------- T3：INT/EXTI 通路（需真人碰屏） ----------
    ic0 = g("g_tp_int_count")
    pt0 = g("g_tp_points")
    print("\n>>> 请在 %d 秒内用手指触碰一次屏幕（滑动几下更好）… 当前 g_tp_int_count=%d"
          % (wait_s, ic0))

    trigger_at = None
    deadline = time.time() + wait_s
    while time.time() < deadline:
        time.sleep(1.0)
        ic = g("g_tp_int_count")
        if ic != ic0:
            trigger_at = ic
            break

    ic1 = g("g_tp_int_count")
    pt1 = g("g_tp_points")
    print("  g_tp_int_count: %d → %d   g_tp_points: %d → %d" % (ic0, ic1, pt0, pt1))

    if ic1 > ic0:
        print("[PASS] T3 EXTI 触发 %d 次 —— INT(PB13) 线在动、EXTI 向量与 NVIC 都通"
              % (ic1 - ic0))
        if pt1 > pt0:
            print("[PASS] T3b 触摸被识别并注入（g_tp_points +%d）" % (pt1 - pt0))
        else:
            print("[WARN] T3b 有 EXTI 但 g_tp_points 没涨：可能只碰了一下就抬手，"
                  "或 I2C 读坐标失败（重碰一次确认）")
    else:
        print("[WARN] T3 等待期内没等到 EXTI 触发（g_tp_int_count 未变）。"
              "两种可能：① %d 秒内没碰屏；② PB13 的 INT 线没接到 / 模块不驱动。"
              "若确实碰了屏仍无变化，则门控退化为心跳兜底（行为不劣于旧版），"
              "但这条优化没起到加速作用 —— 需要查 INT 接线。" % wait_s)
        ok = False

    print("\n[PASS] 触摸 EXTI 门控验收通过" if ok
          else "\n[WARN] 触摸 EXTI 门控验收有未决项（见上）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
