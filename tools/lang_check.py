#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""界面语言（中/英）切换 + W25Q128 持久化 —— 一键验收。

三层证据，全部是确定性整数结论，不靠肉眼：

  ① 交互：点设置页的 "English" 段 → 界面真的变了（中/英两帧的差异像素数）
  ② 落盘：**直接读 W25Q128 扇区 240** 的原始字节，校验 magic / lang 字段 / CRC32
     —— 这是"持久化真的写进去了"的直接证据，不是靠固件自报
  ③ 掉电重启：probe-rs reset 后重读诊断量，语言应仍是英文

用法
----
    python tools/lang_check.py            # 完整流程（含掉电重启验证）
    python tools/lang_check.py --no-reset # 不重启（只验 ①②）

坐标依据
--------------------------
  ⚠ 图标坐标**以板上实测为准，不要按算式推**（2026-10-04 修正）。
    之前这里写的是"推自源码：x=23+3*75=248, y=238 ⇒ 中心 (272,263)"，
    推错了 —— 那个点落在 app 3 的命中区边缘外，点不开设置页，
    表现为 current_app 恒为 0xFFFFFFFF，看起来像"固件没跑"。

    正解做法：用网格扫描实测（tap 后读 current_app）。
      y=268 扫描结果 → 每个 app 的命中 x 区间（区间宽约 ±25，点准比点偏重要）：
        app 0: x=20..70     app 1: x=95..145
        app 2: x=170..220   app 3: x=245..295
      取 app 3 的区间中点附近即可，x=250 已实测命中。

  设置图标：实测命中区间 x=245..295, y≈268 ⇒ 取 (250, 268)
  应用页展开后是全屏 (0,0,W,H)（phone_shell.c:498 的 Anim_ResizeTo）
    ⇒ settings.c 里的控件坐标就是屏幕坐标
  语言卡片里的分段按钮在 settings.c 是**面板内相对坐标**：
    中文段 (170,12,40,26)、English 段 (212,12,58,26)
    面板 y 位置**实测**为 ≈243（脚本原先按 201 算，差了 42 px ⇒ 26 高的按钮
    整个落在点击点上方，点不动）。
    实测命中：中文 (195,255)、English (259,255)。
    ⚠ 中文段的 x 实测 195 ≠ 按 (18+170+20) 算出的 208 —— 相对→屏幕的换算
      同样不能只靠算式。**两个段都用实测值。**
"""
import re
import subprocess
import sys
import time
import zlib

sys.path.insert(0, "tools")
import bench
import caret_check as cc

SETTINGS_ID = 3
HOME_KEY = (160, 464)
SETTINGS_ICON = (250, 268)   # 实测值，见文件头说明（旧值 (272,263) 推错，点不开）

CFG_SECTOR_XIP = 0x90000000 + 0x000F0000      # 扇区 240 在 XIP 映射区的地址
CFG_MAGIC = 0x59434F4E                        # "YCON"


def seg_zh():
    """中文段的绝对屏幕坐标 —— **由固件每帧现算**，不在脚本里存。

    ⚠ 不要再在本文件写死 (195,255)/(259,255)：设置页改成滚动版并在语言卡
      下方插入「半透明」卡片后，卡片整体下移，旧坐标点空，报
      "点 English 段后 g_cfg_lang 仍 = 0"。同类坑已经踩过三轮
      （桌面设置图标、语言两段、半透明开关），统一收敛到固件诊断量。
    ⚠ 必须先 open_settings + settle：诊断量"每帧现算"只保证**当下新鲜**，
      不保证调用时页面已归位（app_page 会停在 y=480 直到归位动画跑完）。
      顺序反了会拿到动画中途的旧坐标。"""
    return (rd1("g_diag_lang_zh_x"), rd1("g_diag_lang_zh_y"))


def seg_en():
    """English 段的绝对屏幕坐标（同上理由由固件现算）。"""
    return (rd1("g_diag_lang_en_x"), rd1("g_diag_lang_en_y"))


def tap(x, y, wait=0.9):
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def rd1(n):
    return bench.rd(n, 1)[0]


def goto_home():
    for _ in range(2):
        tap(*HOME_KEY, wait=0.7)


def read_flash_blob():
    """直接读 W25Q128 扇区 240 的前 32 字节（经 XIP 映射读，绕过固件自报）。"""
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08X 8"
                       % (bench.PROBE, bench.CHIP, CFG_SECTOR_XIP),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    words = [int(t, 16) for t in r.stdout.split()
             if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    return words if len(words) >= 8 else None


def crc32_ieee(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def check_blob(tag):
    """解析并校验扇区内容，返回 (ok, lang) 。ok=False 表示 magic/CRC 不符。"""
    w = read_flash_blob()
    if w is None:
        print("  [FAIL] %s：读不到扇区 240" % tag)
        return None, None
    raw = b"".join(x.to_bytes(4, "little") for x in w[:8])
    magic, version, lang = w[0], w[1], w[2]
    stored_crc = w[7]
    calc = crc32_ieee(raw[:28])
    print("  %s 扇区 240 原始：magic=0x%08X version=%d lang=%d crc=0x%08X(算得 0x%08X)"
          % (tag, magic, version, lang, stored_crc, calc))
    if magic != CFG_MAGIC:
        print("  [FAIL] %s magic 不符（期望 0x%08X = \"YCON\"）" % (tag, CFG_MAGIC))
        return False, lang
    if stored_crc != calc:
        print("  [FAIL] %s CRC 不符 —— 写入过程损坏" % tag)
        return False, lang
    print("  [ OK ] %s magic + CRC32 均通过" % tag)
    return True, lang


def main():
    no_reset = "--no-reset" in sys.argv

    # ---- 前置：内核在跑 ----
    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("[FAIL] 主循环没在跑 → 先 probe-rs reset")
        return 1

    # ---- 前置：开机时读配置的结果 ----
    print("开机读配置：rc=%d loaded=%d lang=%d magic=0x%08X crc_ok=%d"
          % (rd1("g_cfg_rc"), rd1("g_cfg_loaded"), rd1("g_cfg_lang"),
             rd1("g_cfg_magic_read"), rd1("g_cfg_crc_ok")))
    boot_lang = rd1("g_cfg_lang")
    print("  ⇒ 本次上电生效语言 = %s\n" % ("English" if boot_lang == 1 else "中文"))

    # ---- 导航到设置页 ----
    goto_home()
    tap(*SETTINGS_ICON, wait=1.1)
    app = rd1("current_app")
    print("当前应用 current_app = %d（期望 %d = 设置）" % (app, SETTINGS_ID))
    if app != SETTINGS_ID:
        print("[FAIL] 没进到设置页 —— 图标坐标可能变了")
        return 1

    # ---- 归位：保证起点是"中文" ----
    # 不可省：若上一步运行已把语言切成 English（或板上原本就是 English），
    # 直接点 English 段不会有任何变化 ⇒ 差异 0 像素，会被误判成"切换没生效"。
    if rd1("g_cfg_lang") != 0:
        print("  （当前是 English，先点中文段归位）")
        tap(*seg_zh(), wait=1.0)
        if rd1("g_cfg_lang") != 0:
            print("[FAIL] 归位失败 —— 中文段坐标 %s 不可点" % (seg_zh(),))
            return 1

    # ---- ① 交互：切到 English ----
    print("\n① 交互：点 English 段 %s（固件现算坐标）" % (seg_en(),))
    before = cc.frame()
    tap(*seg_en(), wait=1.2)
    after = cc.frame()
    if rd1("g_cfg_lang") != 1:
        print("  [FAIL] 点 English 段后 g_cfg_lang 仍 = %d（坐标不可点？）"
              % rd1("g_cfg_lang"))
        return 1
    box = cc.diff_box(before, after)
    n_diff = box[4] if box else 0
    print("  中/英两帧差异：%d 个像素%s"
          % (n_diff, "，差异区 x=[%d,%d] y=[%d,%d]" % box[:4] if box else ""))
    if n_diff < 200:
        print("  [FAIL] 差异太小 —— 界面没变（切换没生效？）")
        return 1
    print("  [ OK ] 界面确有变化")
    print("  固件自报：lang=%d save_cnt=%d erase_last=%d write_rc=%d"
          % (rd1("g_cfg_lang"), rd1("g_cfg_save_cnt"),
             rd1("g_cfg_erase_last"), rd1("g_cfg_write_rc")))

    # ---- ② 落盘：直接读 flash 扇区 ----
    print("\n② 落盘证据：直接读 W25Q128 扇区 240（不依赖固件自报）")
    ok, lang = check_blob("切换后")
    if not ok:
        return 1
    if lang != 1:
        print("  [FAIL] flash 里 lang=%d，期望 1（English）" % lang)
        return 1
    print("  [ OK ] flash 里语言已持久化为 English\n")

    if no_reset:
        print("（--no-reset：跳过掉电重启验证）")
        return 0

    # ---- ③ 掉电重启 ----
    print("③ 掉电重启验证：reset 后语言应仍是 English")
    subprocess.run("probe-rs reset %s --chip %s" % (bench.PROBE, bench.CHIP),
                   shell=True, capture_output=True, text=True)
    time.sleep(12)
    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("  [FAIL] 复位后主循环没起来")
        return 1
    loaded = rd1("g_cfg_loaded")
    lang_after = rd1("g_cfg_lang")
    print("  重启后：loaded=%d crc_ok=%d lang=%d" % (loaded, rd1("g_cfg_crc_ok"), lang_after))
    if loaded != 1:
        print("  [FAIL] 重启后没读到有效配置")
        return 1
    if lang_after != 1:
        print("  [FAIL] 重启后语言回到了 %d —— 持久化没生效" % lang_after)
        return 1
    print("  [ OK ] 掉电重启后语言仍是 English ⇒ 持久化生效\n")

    # ---- 收尾：切回中文 ----
    print("收尾：切回中文（保持项目默认状态）")
    goto_home()
    tap(*SETTINGS_ICON, wait=1.1)
    # ⚠ 进设置页后必须等页面归位再读坐标：诊断量"每帧现算"只保证当下新鲜，
    #   app_page 归位动画未跑完时读到的仍是动画中途的 y（实测能偏 480 px）。
    #   这里多等 1 秒再取坐标，取不到有效值就重试一次。
    for _ in range(2):
        p = seg_zh()
        if 0 < p[0] < 320 and 0 < p[1] < 480:
            break
        time.sleep(1.0)
    tap(*seg_zh(), wait=1.2)
    ok, lang = check_blob("切回后")
    if ok and lang == 0:
        print("  [ OK ] 已切回中文且 flash 同步为 lang=0")
    else:
        print("  [!] 切回中文后 flash 状态异常，请复查")
        return 1

    print("\n[PASS] 三项全部通过：交互生效 · 落盘正确 · 掉电重启保持")
    return 0


if __name__ == "__main__":
    sys.exit(main())
