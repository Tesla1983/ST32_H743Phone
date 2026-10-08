#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""FatFs 文件系统自检一键验收 —— 路线图 L2-3 的判据。

L2-2 证明了"扇区读写的数据通路是对的"；本脚本证明**文件语义**也是对的：

  ① f_mount 能挂上（并报出 FAT 类型：FAT32 / exFAT...）
  ② 建目录 /YMGUI
  ③ 写 1 KB 文件 → 关 → 重新打开 → 读回 → **逐字节比对失配 0**
  ④ 删除该测试文件

为什么必须"关掉再打开重读"：不关文件直接从写缓冲里读，读到的是内存里的副本，
根本没走卡。重新打开才真正从 FAT 簇链里把数据读回来。

用法
----
    python tools/fs_check.py

退出码
------
    0   全过
    1   挂载失败（卡没插 / 没文件系统 / 文件系统不被支持）
    2   文件读写比对失配
    3   清理失败（测试文件没删掉，卡上会留下 /YMGUI/fs_selftest.txt）
"""
import sys
import time

sys.path.insert(0, "tools")
import bench

TEXT_BYTES = 1024          # 与 src/fatfs_port.h 的 FS_TEXT_BYTES 一致

# FRESULT（ff.h）
FR = {
    0: "FR_OK", 1: "FR_DISK_ERR", 2: "FR_INT_ERR", 3: "FR_NOT_READY",
    4: "FR_NO_FILE", 5: "FR_NO_PATH", 6: "FR_INVALID_NAME", 7: "FR_DENIED",
    8: "FR_EXIST（目录已存在，属正常）", 9: "FR_INVALID_OBJECT",
    10: "FR_WRITE_PROTECTED", 11: "FR_INVALID_DRIVE", 12: "FR_NOT_ENABLED",
    13: "FR_NO_FILESYSTEM", 14: "FR_MKFS_ABORTED", 15: "FR_TIMEOUT",
    16: "FR_LOCKED", 17: "FR_NOT_ENOUGH_CORE", 18: "FR_TOO_MANY_OPEN_FILES",
    19: "FR_INVALID_PARAMETER",
}
FS_TYPE = {1: "FAT12", 2: "FAT16", 3: "FAT32", 4: "exFAT"}

VARS = [
    "g_fs_test", "g_fs_state", "g_fs_mount_rc", "g_fs_fstype",
    "g_fs_mkdir_rc", "g_fs_open_rc", "g_fs_write_rc", "g_fs_written",
    "g_fs_close_rc", "g_fs_size", "g_fs_open2_rc", "g_fs_read_rc",
    "g_fs_readn", "g_fs_mis", "g_fs_unlink_rc", "g_fs_free_kb",
    "g_fs_getfree_rc", "g_fs_cyc",
]


def rd1(n):
    return bench.rd(n, 1)[0]


def fr(v):
    return FR.get(v, "未知(%d)" % v)


def check_symbols():
    dup = {n: bench.T[n] for n in VARS if len(bench.T.get(n, [])) != 1}
    if dup:
        print("[FAIL] 以下符号缺失或重名：%s" % dup)
        return False
    return True


def main():
    if not check_symbols():
        return 1

    a = rd1("g_loop_count")
    time.sleep(1.0)
    b = rd1("g_loop_count")
    if a == b:
        print("[FAIL] 主循环没在跑（g_loop_count %d 两次不变）→ 先 probe-rs reset" % a)
        return 1
    print("主循环在跑：g_loop_count %d → %d" % (a, b))

    bench.wr("g_fs_state", [0])
    bench.wr("g_fs_test", [1])

    st = 0
    for _ in range(150):
        time.sleep(0.1)
        st = rd1("g_fs_state")
        if st != 0:
            break
    if st == 0:
        print("[FAIL] 15 秒内没跑完（g_fs_state 仍 0）")
        return 1

    mount = rd1("g_fs_mount_rc")
    if st == 1 or mount != 0:
        print("[FAIL] 挂载失败：f_mount 返回 %s" % fr(mount))
        print("       先跑 python tools/sd_check.py 确认扇区级通路是好的；")
        print("       若那里正常，多半是卡上没有 FatFs 认得的文件系统（FR_NO_FILESYSTEM）。")
        return 1

    fstype  = rd1("g_fs_fstype")
    mkdir   = rd1("g_fs_mkdir_rc")
    open1   = rd1("g_fs_open_rc")
    wrc     = rd1("g_fs_write_rc")
    written = rd1("g_fs_written")
    close   = rd1("g_fs_close_rc")
    size    = rd1("g_fs_size")
    open2   = rd1("g_fs_open2_rc")
    rrc     = rd1("g_fs_read_rc")
    readn   = rd1("g_fs_readn")
    mis     = rd1("g_fs_mis")
    unlink  = rd1("g_fs_unlink_rc")
    freekb  = rd1("g_fs_free_kb")
    cyc     = rd1("g_fs_cyc")

    print("")
    print("文件系统：%s    卡剩余 %d KB（%.2f GB）"
          % (FS_TYPE.get(fstype, "未知(%d)" % fstype), freekb, freekb / 1048576.0))
    print("")
    print("① f_mount                 %s" % fr(mount))
    print("② f_mkdir(/YMGUI)         %s" % fr(mkdir))
    print("③ 写 %d B                 %s  实际写入 %d B  文件大小 %d B  close %s"
          % (TEXT_BYTES, fr(wrc), written, size, fr(close)))
    print("④ 重开并读回              open2 %s  read %s  实际读出 %d B"
          % (fr(open2), fr(rrc), readn))
    print("⑤ 读回与写入内容逐字节比对  失配 %d 字节   判据 0   %s"
          % (mis, "OK" if mis == 0 else "FAIL"))
    print("⑥ 删除测试文件            %s" % fr(unlink))
    print("")
    print("整轮耗时：%d cycles @400MHz = %.1f ms" % (cyc, cyc / 400000.0))

    if written != TEXT_BYTES or readn != TEXT_BYTES or mis != 0:
        print("")
        print("[FAIL] 文件级读写不一致（写入 %d / 读出 %d / 失配 %d）"
              % (written, readn, mis))
        return 2

    if unlink != 0:
        print("")
        print("[FAIL] 测试文件没删掉：/YMGUI/fs_selftest.txt 仍留在卡上（%s）" % fr(unlink))
        return 3

    print("")
    print("[PASS] 文件系统可用：建目录 → 写 %d B → 关 → 重开 → 读回 %d B → 逐字节一致 → 删除。"
          % (TEXT_BYTES, readn))
    print("      卡上留下一个空目录 /YMGUI（给后续持久化用），无其他改动。")
    print("")
    print("⇒ 下一步可以做：笔记/设置/截屏落盘（L2-3 的应用层）。")
    print("  ⚠ 仍然不要动 IDMA 高速模式 —— 那是 L2-4，必须先完成 L3（SRAM1 非缓存 DMA 区）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
