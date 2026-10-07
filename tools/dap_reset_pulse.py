#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""通过 CMSIS-DAP 给目标发一次 NRST 引脚脉冲（DAP_SWJ_Pins）。

【为什么需要】
  `probe-rs reset` 走的是 **SYSRESETREQ**（内核软复位请求），按 STM32H7 手册
  **不复位 PWR / 备份域**。若电源域被卡住（本例：`PWR_D3CR.VOSRDY = 0`，
  请求 Scale1 却停在最低档且未就绪），软复位清不掉它，只有两种办法：
    ① **NRST 引脚复位**（本脚本，属"系统复位"）
    ② **真断电 ≥10 秒**（POR，最彻底；需用户动手）

【安全边界】
  · 只操作探针自己的 **nRESET 输出线**（标准开漏复位线）——这是探针被设计来做的事，
    不是"把 GPIO 设成推挽去强灌引脚"。
  · 绝不触碰 SWCLK / SWDIO / TDI / TDO 的输出电平（那才会干扰目标）。
  · 脉冲宽度默认 50 ms 低 + 30 ms 高，远小于任何硬件敏感范围。

【已知局限】
  `DAP_SWJ_Pins` 的**请求**与**响应**位定义不对称：请求 bit7 = nRESET，
  响应 bit7 = TDI 输入。所以**无法从响应回读 nRESET 实际电平**（见 swj_pins_probe.py
  里的说明，我曾据此误判过一次）。本脚本只负责"发出脉冲"，判据靠随后读变量。

用法
----
    python tools/dap_reset_pulse.py                 # 默认 50ms 低 / 30ms 高
    python tools/dap_reset_pulse.py --low-ms 80 --high-ms 50
"""
import sys
import hid

VID, PID = 0x0416, 0x5021
CMD_DAP_SWJ_PINS = 0x0B
PIN_NRESET = 0x80


def open_probe():
    devs = hid.enumerate(VID, PID)
    if not devs:
        print("!! 没枚举到探针 HID 设备")
        return None
    h = hid.device()
    for d in devs:
        try:
            h.open_path(d["path"])
            return h
        except Exception:                            # noqa: BLE001
            continue
    print("!! 打不开探针 HID 接口")
    return None


def pins(h, out, sel, wait_us):
    """DAP_SWJ_Pins：请求 [0x00(报告ID), 0x0B, pin_out, pin_sel, wait(4B LE), 0...]"""
    pkt = [0x00, CMD_DAP_SWJ_PINS, out, sel,
           wait_us & 0xFF, (wait_us >> 8) & 0xFF,
           (wait_us >> 16) & 0xFF, (wait_us >> 24) & 0xFF]
    pkt += [0x00] * (64 - len(pkt))
    h.write(pkt)
    r = h.read(64, timeout_ms=2000)
    return r[2] if r and len(r) >= 3 else None


def main():
    low_ms, high_ms = 50, 30
    if "--low-ms" in sys.argv:
        low_ms = int(sys.argv[sys.argv.index("--low-ms") + 1])
    if "--high-ms" in sys.argv:
        high_ms = int(sys.argv[sys.argv.index("--high-ms") + 1])

    h = open_probe()
    if h is None:
        return 1

    st0 = pins(h, 0, 0, 0)
    print("复位前 pin_state = %s（Pin Select=0，只读）"
          % ("None" if st0 is None else "0x%02X" % st0))

    print("→ 拉低 nRESET：%d ms" % low_ms)
    pins(h, 0x00, PIN_NRESET, low_ms * 1000)

    print("→ 释放(驱动为高)：%d ms" % high_ms)
    pins(h, PIN_NRESET, PIN_NRESET, high_ms * 1000)

    st1 = pins(h, 0, 0, 0)                       # 交回探针控制
    print("交回后 pin_state = %s" % ("None" if st1 is None else "0x%02X" % st1))

    h.close()
    print("⇒ 已发出一次 NRST 脉冲，随后请读 g_loop_count / PWR_D3CR 判断是否恢复")
    return 0


if __name__ == "__main__":
    sys.exit(main())
