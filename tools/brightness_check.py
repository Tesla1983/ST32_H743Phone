#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""设置页「预览亮度」—— 出厂默认 50 + 用户自定义后掉电重启保持 —— 一键验收。

用户需求（2026-10-04）："把默认的预览亮度初始化调为50，然后如果用户自定义亮度后
保持重启不改变亮度值"。所以判据只有两条，但都必须板上实测，不能靠读代码：

  ① 出厂默认 = 50
     擦掉配置扇区（tools/factory_reset.py，走固件执行）→ 重启 → g_cfg_brightness == 50。
     ⚠ 必须真的擦扇区才能验：扇区里若存着上一轮写进去的合法值（比如 77/100），
       读到的就是那个值，会误判成"默认值没生效"。同理"100 在 1..100 区间内、
       钳制逻辑正确地不会改它"——这不是固件 bug。

  ② 自定义后掉电重启保持
     拖动滑杆到目标值 → 校验 flash 扇区 240 的 brightness 字段 + CRC →
     probe-rs reset → 重读 g_cfg_brightness 仍等于该值，且 g_cfg_loaded == 1。

  ③ 顺带验「一次拖动只擦一次扇区」（本次实现的核心动机）
     修复前板级实测：一次拖动 g_cfg_save_cnt +6 ⇒ 擦了 6 次 W25Q128 扇区
     （标称 10 万次/扇区，调一天亮度就磨掉一大截）。修复后应恰好 +1。
     这是 bdtap 一次拖动只注入 5 段位移、Slider 每次 Pressing 都回调造成的。

坐标依据
--------------------------
  ⚠ 一律固件现算（g_diag_bright_x0/x1/y），**不在脚本里推**。
    这类"布局一改坐标就失效"的坑已踩过三轮（桌面设置图标、语言两段、半透明开关），
    见 lang_check.py / ribbon_check.py 的注释。滑杆 range 本次也从 0..100
    改成 1..100，写死坐标必偏。

  ⚠ bdtap 是异步注入（写 seq → 下一次 BoardTick 才消费），读状态量前必须 settle()。
"""
import subprocess
import sys
import time
import zlib

sys.path.insert(0, "tools")
import bench
import factory_reset

SETTINGS_ID = 3
HOME_KEY = (160, 464)
SETTINGS_ICON = (250, 268)

CFG_SECTOR_XIP = 0x90000000 + 0x000F0000      # 扇区 240
CFG_MAGIC = 0x59434F4E                        # "YCON"
BRIGHT_DEFAULT = 50


def rd1(n):
    return bench.rd(n, 1)[0]


def tap(x, y, wait=0.9):
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def drag(x, y, dx, dy, wait=0.9):
    """注入一次拖动。

    ⚠⚠ wait 之后**不要**紧接着读状态量：bdtap 是异步注入（写 seq → 下一次
      BoardTick 才消费），读出来可能是上一次手势的旧值。断言前用 settle()。"""
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [dx])
    bench.wr("g_bdtap_dy", [dy])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def settle(extra=0.0):
    """等注入被主循环消费完，再读状态量。"""
    time.sleep(0.8 + extra)


def goto_home():
    for _ in range(2):
        tap(*HOME_KEY, wait=0.7)


def slider_geom():
    """滑杆两端的绝对屏幕坐标（固件每帧现算）。"""
    return rd1("g_diag_bright_x0"), rd1("g_diag_bright_x1"), rd1("g_diag_bright_y")


def wait_slider(tag="滑杆", timeout=4.0):
    """等滑杆坐标有效（进设置页有 330 ms 归位动画，动画未完读到的是旧值/0）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        x0, x1, y = slider_geom()
        if 0 < x0 < x1 < 320 and 0 < y < 480:
            return (x0, x1, y)
        time.sleep(0.2)
    print("  [!] %s：等不到有效坐标（最后读到 %s）" % (tag, slider_geom()))
    return slider_geom()


def scroll_to_bottom():
    """滚到底：关机/输入框/滑杆都可能在初始视口之外。"""
    # 起点 (6,380) 是卡片圆角外的空白，落不到任何控件上。
    # ⚠ 别用 (160,380)：那里是设备名输入框，按下即触发焦点、弹出软键盘。
    for _ in range(3):
        drag(6, 380, 0, -200, wait=0.6)
    settle()


def open_settings():
    goto_home()
    tap(*SETTINGS_ICON, wait=1.1)
    app = rd1("current_app")
    if app != SETTINGS_ID:
        print("[FAIL] 没进到设置页（current_app=%d）" % app)
        return False
    return True


def reset_board():
    """掉电重启（等同断电重启：probe-rs reset 走 NRST 复位，非软复位）。"""
    subprocess.run("probe-rs reset %s --chip %s" % (bench.PROBE, bench.CHIP),
                   shell=True, capture_output=True, text=True,
                   encoding="utf-8", errors="ignore")
    time.sleep(12)
    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("  [FAIL] 复位后主循环没起来（g_loop_count 不变）")
        print("        判据提醒：probe-rs download 后内核仍 halt，必须跟一次 reset。")
        return False
    return True


def read_flash_blob():
    """直读扇区 240 前 32 字节（经 XIP，不靠固件自报）。"""
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08X 8"
                       % (bench.PROBE, bench.CHIP, CFG_SECTOR_XIP),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    words = [int(t, 16) for t in r.stdout.split()
             if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    return words if len(words) >= 8 else None


def check_blob(tag):
    """校验扇区内容，返回 (ok, brightness)。字段顺序见 AppConfigBlob。"""
    w = read_flash_blob()
    if w is None:
        print("  [FAIL] %s：读不到扇区 240" % tag)
        return None, None
    raw = b"".join(x.to_bytes(4, "little") for x in w[:8])
    magic, version, lang, bright = w[0], w[1], w[2], w[3]
    stored_crc = w[7]
    calc = zlib.crc32(raw[:28]) & 0xFFFFFFFF
    print("  %s 扇区 240：magic=0x%08X ver=%d lang=%d brightness=%d crc=0x%08X(算得 0x%08X)"
          % (tag, magic, version, lang, bright, stored_crc, calc))
    if magic != CFG_MAGIC:
        print("  [FAIL] magic 不符（期望 0x%08X）" % CFG_MAGIC)
        return False, bright
    if stored_crc != calc:
        print("  [FAIL] CRC 不符 —— 写入过程损坏")
        return False, bright
    print("  [ OK ] magic + CRC32 均通过")
    return True, bright


def do_drag_to(target_pct, tag):
    """把滑杆拖到目标百分比（0..100），返回 (save_cnt_before, save_cnt_after)。

    分 5 段拖，刻意制造多个中间值—— 这正是"一次拖动擦了 6 次扇区"的成因，
    所以这个用例必须走多段，否则测不出延迟落盘是否生效。

    ⚠⚠ 每段都**重新读**一次滑杆几何，不要在循环外缓存：
      滚动位置一旦被复位（scroll_y 135→0），滑杆屏幕 y 就从 330 变成 465，
      用缓存坐标会全部落空。2026-10-04 就是这样误判"拖不动"的，
      真实原因是固件侧 Preview 误发了 sync 命令（已修，见 brightness_apply）。
      这里保留每段重读，是为了让脚本对固件侧的任何回归都能立刻暴露，
      而不是悄悄点空。
    """
    before = rd1("g_cfg_save_cnt")
    steps = 5
    x0, x1, y = slider_geom()
    span = x1 - x0
    cur = x0 + span * target_pct // 200      # 从目标的一半处起拖
    for i in range(1, steps + 1):
        x0, x1, y = slider_geom()
        if not (0 < x0 < x1 < 320 and 0 < y < 480):
            print("  [!] %s：第 %d 段滑杆坐标无效 %s" % (tag, i, (x0, x1, y)))
            return before, rd1("g_cfg_save_cnt")
        nx = x0 + span * target_pct // 100
        drag(cur, y, nx - cur, 0, wait=0.35)
        cur = nx
    settle(1.2)          # 松手事件也要等主循环消费
    return before, rd1("g_cfg_save_cnt")


def main():
    print("=" * 64)
    print("预览亮度验收：① 出厂默认 50  ② 一次拖动只擦一次  ③ 掉电重启保持")
    print("=" * 64)

    # ---- 前置：内核在跑 ----
    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("[FAIL] 主循环没在跑 → 先 probe-rs reset")
        return 1

    # ================= ① 出厂默认 = 50 =================
    print("\n" + "-" * 64)
    print("① 出厂默认值：擦掉配置扇区后重启，看 g_cfg_brightness")
    print("-" * 64)
    print("  当前：g_cfg_loaded=%d g_cfg_brightness=%d"
          % (rd1("g_cfg_loaded"), rd1("g_cfg_brightness")))
    print("  ⚠ 扇区里若存着上一轮写进去的合法值，读到的就是那个值 ⇒ 必须先擦。")

    factory_reset.main()
    settle(1.0)
    if not reset_board():
        return 1

    loaded = rd1("g_cfg_loaded")
    bright = rd1("g_cfg_brightness")
    print("  擦除并重启后：g_cfg_loaded=%d g_cfg_brightness=%d" % (loaded, bright))
    if loaded != 0:
        print("  [FAIL] 擦除后仍 loaded=1 —— 扇区没真擦掉，① 无法判定")
        return 1
    if bright != BRIGHT_DEFAULT:
        print("  [FAIL] 出厂默认亮度 = %d，期望 %d" % (bright, BRIGHT_DEFAULT))
        return 1
    print("  [ OK ] 出厂默认亮度 = %d" % BRIGHT_DEFAULT)

    # 顺带看一眼屏：默认亮度下界面应当是"半亮"而不是全黑/全白
    # ⚠ 2026-10-04：屏侧实现已从"软件叠黑罩"换成"硬件背光 PWM"（g_dim_mode=2 默认），
    #   所以**本脚本不做像素级断言**这一条依然成立 —— 它只读持久化值与扇区计数，
    #   换机制不影响判据。但"屏上看起来亮度对不对"只能人眼看：
    #   现在 framebuffer 里是**未调暗**的画面，调暗由 PB1(TIM3_CH4) 的占空比承担。
    print("  （亮度侧已改硬件背光 PWM：framebuffer 不再叠黑罩，"
          "duty(v)=1000−(100−v)×160×1000/25500）")

    # ================= ② 拖动 + 一次拖动只擦一次 =================
    print("\n" + "-" * 64)
    print("② 拖动滑杆：值应变化，且**一次拖动只擦一次扇区**")
    print("-" * 64)
    if not open_settings():
        return 1
    scroll_to_bottom()
    geom = wait_slider()
    print("  滑杆几何（固件现算）：x0=%d x1=%d y=%d" % geom)
    if not (0 < geom[0] < geom[1] < 320 and 0 < geom[2] < 480):
        print("  [FAIL] 滑杆坐标无效")
        return 1

    target = 80     # 拖到 80%
    before, after = do_drag_to(target, "80%")
    val = rd1("g_cfg_brightness")
    saves = after - before
    erases = rd1("g_cfg_erase_last")
    print("  拖到 %d%% 后：g_cfg_brightness=%d" % (target, val))
    print("  g_cfg_save_cnt +%d，g_cfg_erase_last=%d" % (saves, erases))

    if val <= BRIGHT_DEFAULT:
        print("  [FAIL] 拖动后亮度没变（还是 %d）—— 拖动坐标不对或滑杆没响应" % val)
        print("        事件通路诊断：sld_press=%d sld_change=%d sld_commit=%d"
              % (rd1("g_diag_sld_press"), rd1("g_diag_sld_change"),
                 rd1("g_diag_sld_commit")))
        print("        判读：")
        print("          sld_press=0     ⇒ wrapper 没收到滑杆的按压 ⇒ 命中测试没命中它")
        print("          press>0 change=0 ⇒ wrapper 转发了，但 Slider 的 changed 没调")
        print("          change>0 commit=0 ⇒ 值变了但没落盘（松手判定或拖动中标志的问题）")
        print("        几何：x0=%d x1=%d y=%d scroll_y=%d scroll_max=%d"
              % (rd1("g_diag_bright_x0"), rd1("g_diag_bright_x1"),
                 rd1("g_diag_bright_y"), rd1("g_diag_scroll_y"),
                 rd1("g_diag_scroll_max")))
        return 1
    print("  [ OK ] 值已变化（%d → %d）" % (BRIGHT_DEFAULT, val))
    print("  事件通路：sld_press=%d sld_change=%d sld_commit=%d"
          % (rd1("g_diag_sld_press"), rd1("g_diag_sld_change"),
             rd1("g_diag_sld_commit")))

    # 顺带判据：拖动不该把页面弹回顶部。
    # ⚠ 这条曾经真的失败过 —— Preview 误发 sync 命令 → app_show 把 scroll_y
    #   复位成 0 → 滑杆屏幕 y 从 330 跳到 465 → 后续位移全部落空。
    #   保留它作为回归哨兵。
    sy = rd1("g_diag_scroll_y")
    if sy == 0:
        print("  [FAIL] 拖动后 scroll_y 变成 0 —— 页面被复位到顶部了")
        print("        成因：Preview 路径误发 PhoneApps_Command(SETTINGS,\"sync\")，")
        print("        而 sync → app_show → scroll_y=0。滑杆 y 因此从 330 变 465，")
        print("        手指还在 330 处 ⇒ 拖不动。")
        return 1
    print("  [ OK ] 拖动后 scroll_y 仍 = %d（页面没被复位）" % sy)

    if saves != 1:
        print("  [FAIL] 一次拖动擦了 %d 次扇区，期望 1 次。" % saves)
        print("        成因：Slider 的回调在 Pressed 和每次 Pressing 都触发，")
        print("        而 AppConfig_Save 是'值变就擦一个扇区'。")
        print("        期望做法：拖动中走 PhoneHost_SetBrightnessPreview（不落盘），")
        print("        松手才走 PhoneHost_SetBrightness（落盘一次）。")
        return 1
    print("  [ OK ] 一次拖动恰好擦 1 次扇区（延迟落盘生效）")

    # ================= ③ 掉电重启保持 =================
    print("\n" + "-" * 64)
    print("③ 落盘证据 + 掉电重启保持")
    print("-" * 64)
    ok, fb = check_blob("拖动后")
    if ok is None:
        return 1
    if not ok:
        return 1
    if fb != val:
        print("  [FAIL] flash 里 brightness=%d，固件内存里是 %d —— 不一致" % (fb, val))
        return 1
    print("  [ OK ] flash 与内存一致（brightness=%d）" % val)

    if not reset_board():
        return 1
    loaded2 = rd1("g_cfg_loaded")
    crc_ok = rd1("g_cfg_crc_ok")
    val2 = rd1("g_cfg_brightness")
    print("  重启后：loaded=%d crc_ok=%d brightness=%d" % (loaded2, crc_ok, val2))
    if loaded2 != 1:
        print("  [FAIL] 重启后没读到有效配置")
        return 1
    if val2 != val:
        print("  [FAIL] 重启后亮度 = %d，期望保持 %d —— 持久化没生效" % (val2, val))
        return 1
    print("  [ OK ] 掉电重启后亮度仍是 %d ⇒ 持久化生效" % val2)

    print("\n" + "=" * 64)
    print("[PASS] 出厂默认 50 · 一次拖动只擦一次 · 掉电重启保持")
    print("=" * 64)
    return 0


if __name__ == "__main__":
    sys.exit(main())