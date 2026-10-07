#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""只读探测探针的 SWJ 引脚状态（CMSIS-DAP DAP_SWJ_Pins）—— 用于判断
目标板是否被探针的 nRESET 拉住。

【为什么需要】
  "目标完全不响应 SWD"有几个原因，其中**只有一个是软件可解决的**：
    ① nRESET 被探针持续拉低 ⇒ 目标一直在复位 ⇒ SWD 无响应（**可解**）
    ② 板子没供电 / SWD 线接触不良 / 需断电重启（需用户动手）
  本脚本先**只读**引脚状态：`DAP_SWJ_Pins` 里 `Pin Select = 0` 表示
  **不改动任何引脚**，只回读当前电平。

【安全边界】
  · `--release-reset` 只把 **nRESET 置为高**（释放复位），
    这是探针被设计来做的事（标准开漏复位线），不属"强灌电流"。
  · 默认**不做任何写操作**；不带参数就是纯只读。
  · 绝不触碰 SWCLK/SWDIO/TDI/TDO 的输出电平（那才会干扰目标）。

CMSIS-DAP 报文：
  请求（必须带 report ID 前缀）：[0x00, 0x0B, pin_out, pin_sel, wait(4B LE), ...]
  响应（hidapi 会剥掉 report ID）：[cmd回显, 长度, pin_state]

pin_sel / pin_out 位定义（**请求**方向）：
  bit0 SWCLK, bit1 SWDIO, bit2 TDI, bit3 TDO, bit4 nTRST, bit7 nRESET

pin_state 位定义（**响应**方向，注意与请求**不对称**）：
  bit0..3 = 输出状态(SWCLK/SWDIO/TDI/TDO)，bit4 = nTRST 输出，
  bit5..7 = 输入状态(SWCLK/SWDIO/TDI 的**回读电平**)

  ⚠ **规范不对称，务必注意**：请求里的 bit7 是 nRESET，但响应里的 bit7 是
  **TDI 输入**。也就是说 —— **本命令无法回读 nRESET 的实际状态**。
  我曾据此误判过一次（把响应 bit7=0 读成"nRESET 被拉低"），
  实测也印证了解读错误：写入 `pin_sel=0x80, pin_out=0x80` 后响应仍是 `0x31`，
  说明那个 bit 根本不受写影响（它是 TDI 输入）。
  ⇒ 所以 `--release-reset` 只能算"尽力尝试拉高复位线"，**不能用来诊断复位线状态**；
  要判断目标是否被复位拉住，只能靠"目标是否响应 SWD"这个间接证据。

本脚本真正能提供的信息是 bit5/bit6（SWCLK/SWDIO 的**输入回读**）：
  在探针不驱动的时刻，若两者都不是高，提示线材/接触可能有问题
  （但仍需结合"换速率是否影响"一起判断，单看这一项不足以定性）。
"""
import sys
import time

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


def swj_pins(h, pin_out=0, pin_sel=0, wait_us=0):
    pkt = [0x00, CMD_DAP_SWJ_PINS, pin_out, pin_sel,
           wait_us & 0xFF, (wait_us >> 8) & 0xFF,
           (wait_us >> 16) & 0xFF, (wait_us >> 24) & 0xFF]
    pkt += [0x00] * (64 - len(pkt))
    h.write(pkt)
    resp = h.read(64, timeout_ms=1000)
    if not resp or len(resp) < 3:
        return None
    return resp[2]                      # pin_state


def describe(state):
    """只解读**可以可靠解读**的位；bit7 是 TDI 输入，不要当 nRESET。"""
    out = []
    if state & 0x01:
        out.append("SWCLK/out=1")
    if state & 0x02:
        out.append("SWDIO/out=1")
    if state & 0x04:
        out.append("TDI/out=1")
    if state & 0x08:
        out.append("TDO/out=1")
    if state & 0x10:
        out.append("nTRST/out=1")
    in_sclk = 1 if (state & 0x20) else 0
    in_sdio = 1 if (state & 0x40) else 0
    in_tdi = 1 if (state & 0x80) else 0
    out.append("回读: SWCLK_in=%d SWDIO_in=%d TDI_in=%d" % (in_sclk, in_sdio, in_tdi))
    return "、".join(out)


def main():
    release = "--release-reset" in sys.argv

    h = open_probe()
    if h is None:
        return 1

    print("== 只读探测：当前 SWJ 引脚状态（Pin Select=0，不改动任何引脚）==")
    for i in range(3):
        st = swj_pins(h, 0, 0, 0)
        if st is None:
            print("  [%d] 无响应（探针可能又挂起）" % i)
        else:
            print("  [%d] pin_state = 0x%02X  → %s" % (i, st, describe(st)))
        time.sleep(0.15)

    if release:
        print("\n== 释放 nRESET（Pin Select=0x80, Pin Output=0x80）==")
        st = swj_pins(h, PIN_NRESET, PIN_NRESET, 20000)   # 保持 20 ms
        print("  写入后 pin_state = %s" % ("None" if st is None else "0x%02X → %s"
                                          % (st, describe(st))))
        time.sleep(0.3)
        # 写完之后把 nRESET 交回探针控制（Pin Select=0），避免一直被我们的值驱动
        st = swj_pins(h, 0, 0, 0)
        print("  交回后 pin_state = %s" % ("None" if st is None else "0x%02X → %s"
                                          % (st, describe(st))))
        print("  ⇒ 已发出一次 nRESET 释放；随后请重试 probe-rs verify 看目标是否响应")

    h.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
