#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 hidapi 直接探测 FireDAP 的 HID 通道是否可用（只读，不发任何破坏性命令）。

背景：probe-rs 0.29.1 全程走 nusb/WinUSB，对 hidusb 绑定的设备打不开，
日志停在 "Attempt 1 to find packet size"。本脚本绕过 probe-rs，直接用 hidapi
按 CMSIS-DAP v1 协议发一条最无害的 DAP_Info 查询，判断：
  · HID 通道能否打开
  · 探针固件是否有响应（能响应 ⇒ 可用 pyocd 之类的 HID 工具烧录）

CMSIS-DAP v1 报文（HID 报告固定 64 字节）：
  byte[0] = 0x00        报告 ID（v1 固定 0）
  byte[1] = 命令
  byte[2..] = 参数
命令 0x00 = DAP_Info，参数为 info id：
  0x01 = 固件版本（字符串）   0x02 = 目标器件名
"""
import sys

try:
    import hid
except ImportError:
    print("!! 没有 hidapi 模块；本脚本需要 pip install hidapi")
    sys.exit(1)

VID, PID = 0x0416, 0x5021
CMSIS_DAP_INFO = 0x00
INFO_FW_VERSION = 0x01
INFO_DEVICE_VENDOR = 0x02
INFO_CAPABILITIES = 0xF0


def build_pkt(cmd, info_id, with_report_id):
    """CMSIS-DAP v1 报文（HID 报告 64 字节）。

    ⚠ report ID 前缀有歧义：HID 描述符里若定义了 report ID，首字节必须是它；
    若没定义，首字节就是数据。两种都试，避免把"格式不对"误判成"固件不响应"。
    """
    if with_report_id:
        body = [0x00, cmd, info_id]
        return body + [0x00] * (64 - len(body))
    body = [cmd, info_id]
    return body + [0x00] * (64 - len(body))


def try_info(h, label, info_id):
    """两种报文格式都试一遍，返回 (是否收到响应, 描述)。

    ⚠ 解析偏移与"发送"格式**无关**：实测 hidapi 的 read() **会剥掉 report ID**，
    因此响应报文恒定是
        resp[0] = 命令回显
        resp[1] = 数据长度
        resp[2..] = 数据
    而**请求**必须带 report ID 前缀（探针要求），否则无响应。
    早期版本把两者绑在同一个标志上，导致数据整体少读第一个字符
    （"Memory" 显示成 "emory"、"FireDAP CMSIS-DAP" 显示成 "ireDAP CMSIS-DAP"）。
    """
    for with_rid in (True, False):
        pkt = build_pkt(CMSIS_DAP_INFO, info_id, with_rid)
        try:
            h.write(pkt)
            resp = h.read(64, timeout_ms=800)
        except Exception as e:                      # noqa: BLE001
            return False, "读写异常 %s" % e
        if resp:
            if len(resp) < 2:
                return False, "响应过短：%r" % bytes(resp)
            length = resp[1]
            data = resp[2: 2 + length] if length else b""
            if label == "能力位":
                val = "0x%02X" % (data[0] if data else 0)
                extra = ""
                if data:
                    caps = data[0]
                    names = []
                    for bit, nm in ((0, "SWD"), (1, "JTAG"), (2, "SWO-UART"),
                                    (3, "SWO-Manchest"), (4, "原子命令"),
                                    (5, "测试域定时器"), (6, "SWO 流式"), (7, "UART")):
                        if caps & (1 << bit):
                            names.append(nm)
                    extra = "  → 支持：" + "、".join(names)
                val += extra
            else:
                val = repr(bytes(data).decode("utf-8", "replace").rstrip("\x00"))
            return True, "%s（请求%s report ID）" % (val, "带" if with_rid else "不带")
    return False, "**两种报文格式都无响应（超时）**"


def probe():
    devs = hid.enumerate(VID, PID)
    if not devs:
        print("!! 没有枚举到 VID_%04X:PID_%04X 的 HID 设备" % (VID, PID))
        return 1

    print("枚举到 %d 个 HID 接口：" % len(devs))
    for i, d in enumerate(devs):
        print("  [%d] iface=%s usage_page=0x%04X usage=0x%04X  %s"
              % (i, d.get("interface_number"), d.get("usage_page") or 0,
                 d.get("usage") or 0, d.get("product_string")))
    print("")

    opened = False
    responsive = 0
    for i, d in enumerate(devs):
        path = d["path"]
        try:
            h = hid.device()
            h.open_path(path)
            opened = True
            print("[%d] 打开成功" % i)
        except Exception as e:                      # noqa: BLE001
            print("[%d] 打开失败：%s" % (i, e))
            continue

        # ---- 对照①：读 HID 描述符。这一步走 HidD_GetXxxString，
        #      不涉及 CMSIS-DAP 协议。若它能读到，说明 HID 通道本身是通的。
        for name, fn in (("product", h.get_product_string),
                         ("manufacturer", h.get_manufacturer_string),
                         ("serial", h.get_serial_number_string)):
            try:
                v = fn()
                print("     描述符 %-12s = %r" % (name, v))
            except Exception as e:                  # noqa: BLE001
                print("     描述符 %-12s = 读取失败（%s）" % (name, e))

        # ---- 对照②：CMSIS-DAP 命令往返 ----
        for label, info_id in (("固件版本", INFO_FW_VERSION),
                               ("器件厂商", INFO_DEVICE_VENDOR),
                               ("能力位", INFO_CAPABILITIES)):
            ok, desc = try_info(h, label, info_id)
            if ok:
                responsive += 1
                print("     %-8s：%s" % (label, desc))
            else:
                print("     %-8s：%s" % (label, desc))
        h.close()
        print("")

    if not opened:
        print("!! 所有 HID 接口都无法打开 ⇒ 被别的进程独占，或权限不足")
        return 1

    print("=" * 58)
    if responsive:
        print("结论：HID 通道可用，探针响应正常（%d/3 条命令有应答）" % responsive)
        print("      ⇒ 探针固件没挂；probe-rs 打不开是 v2/WinUSB 路线问题，")
        print("        可用 pyocd（hidapi 路线）烧录。")
        return 0
    print("结论：HID 通道能打开、描述符也读得到，但 **CMSIS-DAP 命令零响应**")
    print("      ⇒ 探针固件挂起。需要物理复位（拔插 USB 或按探针复位键）。")
    print("      （已排除报文格式因素：含/不含 report ID 两种都试过）")
    return 2



if __name__ == "__main__":
    sys.exit(probe())
