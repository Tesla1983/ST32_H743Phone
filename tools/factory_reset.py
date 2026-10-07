#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""恢复出厂 UI 设置（擦掉 W25Q128 扇区 240 = 0x000F0000）。

擦掉之后下次上电 AppConfig_Load 读不到 magic ⇒ g_cfg_loaded=0，
整套设置回到 blob_defaults()：中文 / 半透明开 / **亮度 50**。

用途：验证"出厂默认亮度 = 50"必须先清掉旧值 —— 上一轮验收用旧固件把
brightness=100 写进了扇区，而 100 在 1..100 区间内、钳制逻辑正确地不会
改它，于是默认值永远等不到生效。

⚠ 为什么必须走固件、不能从主机擦：配置在 XIP 映射区 0x900F0000，
  那个窗口是**只读**的，probe-rs 往那儿写会报
  "Failed to write register DRW at address 0xd0c"（试过，确实不通）。
  固件侧的 qspi_erase_sector 会先退出映射、擦完再把映射打开。

【安全性】只擦本项目自己用的那一个扇区（4 KB），该扇区只存 32 字节 blob、
其余是预留空间。擦完等价于"设置回默认值"，没有不可恢复的数据。
"""
import sys
import time

sys.path.insert(0, "tools")
import bench
import ribbon_check as rc


def factory():
    """触发固件擦扇区，等它跑完并读回结果。"""
    bench.wr("g_cfg_factory_reset", [1])
    # 擦一个扇区 ~1 s；轮询 g_cfg_factory_reset 归零 + rc 有效。
    # ⚠ 不忙等太紧：SWD 读会暂停内核，但这里等的是"内核做完事"，
    #   所以固定 sleep 反而更稳。
    for _ in range(20):
        time.sleep(0.5)
        if bench.rd("g_cfg_factory_reset", 1)[0] == 0:
            break
    rcv = bench.rd("g_cfg_factory_rc", 1)[0]
    return rcv


def main():
    print("恢复出厂设置：SWD 触发固件擦掉配置扇区 240")
    rcv = factory()
    if rcv != 0:
        print("  [FAIL] g_cfg_factory_rc = %d（0 才算成功）" % rcv)
        return 1
    print("  [OK] 扇区已擦除，g_cfg_factory_rc = 0")

    w = rc.read_flash_blob()
    if w is None:
        print("  [!] 复读失败（不能据此判定，继续看内存侧）")
    else:
        print("  复读 magic = 0x%08X ⇒ %s"
              % (w[0], "已擦净" if w[0] == 0xFFFFFFFF else "仍是旧值！"))

    print("\n内存侧当前生效值（应已是出厂默认）：")
    for n, want in (("g_cfg_lang", 0), ("g_cfg_ribbon", 1), ("g_cfg_brightness", 50)):
        v = bench.rd(n, 1)[0]
        print("  %-18s = %-4d %s" % (n, v, "OK" if v == want else "!= 期望 %d !" % want))
    print("\n现在重启（probe-rs reset）后 g_cfg_loaded 会变 0，读的也是这套默认值。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
