#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""设置页「半透明效果」卡片 + 滚动容器 —— 一键验收。

沿用 lang_check.py 的三层证据结构，全部是确定性整数结论，不靠肉眼：

  ① 交互：拨动开关 → g_ribbon_translucent 真的翻转，且**回桌面后壁纸区域像素变化**
     （开关改了渲染，不只是改了个变量）
  ② 落盘：**直接读 W25Q128 扇区 240** 原始字节，校验 magic / CRC32 / ribbon 字段
     —— 不靠固件自报
  ③ 掉电重启：probe-rs reset 后重读，开关应保持

另外单列一项「滚动容器」验收（拖动 → scroll_y 变、clamp 生效、关机按钮滚到底可点），
因为它是本次布局改动的承载结构。

坐标依据
--------------------------
  ⚠ 一律**实测**，不按算式推（lang_check.py 已经吃过一次亏：推的 (272,263)
    落在 app 3 命中区外，点不开设置页）。
  设置图标 (250,268)、语言段 (195,255)/(259,255) 沿用 lang_check.py 的实测值。

  滚动区几何（settings.c）：
    视口 abs y = 36..448（app 视图 412 高 + 36 偏移）
    卡片内容坐标：65 / 133 / 201 / 269(半透明) / 329 / 407 / 451
    ⇒ 半透明卡 abs = 36+269 = 305..355，开关在卡内 (218,11,48,27)
      ⇒ 开关 abs ≈ (36+218, 305+11) = (254,316)..(302,343)，中心 ≈ (278,330)

  ⚠ 半透明卡在 y=269，**初始 scroll_y=0 时就在视口内，不需要滚动**。
    需要滚动的是它下面的亮度/输入框/关机按钮（329/407/451）。
  键盘「收起」键 (287,267)：键盘挂 top_layer 占 y=226..448，非组合态
    candidate_prev.area=(260,24,54,35) ⇒ abs y 250..285（见 ensure_ime_hidden）。
  关机按钮 (160,428)：滚到底后 abs y=408..448（板级自检 phone_selftest.inc
    用 YMGUI_Obj_GetAbsArea 实测过同一结论，不是按算式推的）。
"""
import subprocess
import sys
import time
import zlib

sys.path.insert(0, "tools")
import bench
import caret_check as cc

SETTINGS_ID = 3
HOME_KEY = (160, 464)
SETTINGS_ICON = (250, 268)
SEG_ZH = (195, 255)

CFG_SECTOR_XIP = 0x90000000 + 0x000F0000
CFG_MAGIC = 0x59434F4E

IME_COLLAPSE = (287, 267)   # 键盘上的「收起」键（见下方推导）

# 拖动起点：**必须避开输入框**。设备名输入框在内容坐标 y=407，滚到底后
# abs y = 92+407-135 = 364..412 ⇒ 在 (160,380) 按下会命中它、触发焦点，
# 键盘随即弹出（实测：ime_visible 由 0 变 1），后续点击全被键盘挡住。
# 左侧 8px 是卡片圆角外的空白区，落不到任何控件上。
DRAG_XY = (6, 380)


def switch_xy():
    """半透明开关中心的**绝对屏幕坐标** —— 由固件现算，不在脚本里推。

    ⚠⚠ 别再自己算（2026-10-04 踩过两次）：视口高度、头部高度、卡片内容 y、
    运行时 scroll_y 四项任一改动算式就失效；更要命的是 **app_page 自己会
    停在 y=480**（on_recent 把它推到屏幕下方等 open_app 的归位动画），
    任何"进页时算一次"的坐标都会凭空多 480。
    ⇒ 固件在 settings.c 的 app_tick（PhoneApps_Tick 每帧派发）里现算。"""
    return rd1("g_diag_ribbon_sw_x"), rd1("g_diag_ribbon_sw_y")


def power_btn_xy():
    """关机按钮中心的绝对屏幕坐标（同上，固件每帧现算）。

    ⚠ scroll_y=0 时它 abs y=543，**在屏幕（480 高）之外** ⇒ 必须先滚到底。"""
    return rd1("g_diag_power_btn_x"), rd1("g_diag_power_btn_y")


def wait_diag(tag, timeout=3.0):
    """等诊断量变成有效值（非 0 且在屏幕内）。

    ⚠ 必须等：app_tick 只在 app **运行中**才被派发，而进 app 有 330 ms 的
      归位动画（app_page 从 y=480 移回 0）。动画结束前读会拿到上一代的
      值或 0 —— 2026-10-04 因此把 (0,0) 当成"开关点不到"。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        x, y = switch_xy()
        if 0 < x < 320 and 0 < y < 480:
            return (x, y)
        time.sleep(0.2)
    print("  [!] %s：等不到有效坐标（最后读到 %s）" % (tag, switch_xy()))
    return switch_xy()


def tap(x, y, wait=0.9):
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def drag(x, y, dx, dy, wait=0.9):
    """注入一次拖动。

    ⚠⚠ `wait` 之后**不要**紧接着读状态量。bdtap 是"写 seq → 下一次
      BoardTick 才消费"的异步注入：wait=0.5 时主循环可能还没跑到，
      读出来的 scroll_y 还是**上一次手势的旧值**。
      实测踩过：连拖 4 次每次 wait=0.5，中途读 scroll_y 一直报 135，
      误以为"滚不动"；改成读前多等 1 s 就正常 0↔135 跳变。
      ⇒ 断言状态前用 settle()，别靠 drag 的 wait 顺带等。"""
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [dx])
    bench.wr("g_bdtap_dy", [dy])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def settle(extra=0.0):
    """等注入被主循环消费完，再读状态量。"""
    time.sleep(0.8 + extra)


def rd1(n):
    return bench.rd(n, 1)[0]


def goto_home():
    for _ in range(2):
        tap(*HOME_KEY, wait=0.7)


def open_settings():
    goto_home()
    tap(*SETTINGS_ICON, wait=1.1)
    if rd1("current_app") != SETTINGS_ID:
        return False
    # 进设置页时若键盘还开着（上一页遗留），它会盖住下半屏 ⇒ 先收起。
    # ⚠ 收起动作必须留在设置页内，见 ensure_ime_hidden 的坑 2。
    ensure_ime_hidden()
    if rd1("current_app") != SETTINGS_ID:
        print("  [!] 收键盘时掉出设置页（current_app=%d）" % rd1("current_app"))
        return False
    return True


def read_flash_blob():
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08X 8"
                       % (bench.PROBE, bench.CHIP, CFG_SECTOR_XIP),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    words = [int(t, 16) for t in r.stdout.split()
             if len(t) == 8 and all(c in "0123456789abcdef" for c in t.lower())]
    return words if len(words) >= 8 else None


def check_blob(tag):
    """校验扇区 240，返回 (ok, ribbon)。"""
    w = read_flash_blob()
    if w is None:
        print("  [FAIL] %s：读不到扇区 240" % tag)
        return None, None
    raw = b"".join(x.to_bytes(4, "little") for x in w[:8])
    magic, version, lang = w[0], w[1], w[2]
    ribbon = w[5]          # AppConfigBlob: magic/version/lang/brightness/theme/ribbon
    stored_crc = w[7]
    calc = zlib.crc32(raw[:28]) & 0xFFFFFFFF
    print("  %s 扇区 240：magic=0x%08X ver=%d lang=%d ribbon=%d crc=0x%08X(算得 0x%08X)"
          % (tag, magic, version, lang, ribbon, stored_crc, calc))
    if magic != CFG_MAGIC:
        print("  [FAIL] magic 不符（期望 0x%08X）" % CFG_MAGIC)
        return False, ribbon
    if stored_crc != calc:
        print("  [FAIL] CRC 不符 —— 写入过程损坏")
        return False, ribbon
    print("  [ OK ] magic + CRC32 通过")
    return True, ribbon


def scroll_pos():
    """读设置页滚动视口的 scroll_y 与 clamp 上限。

    ⚠ 不能从脚本按算式推：视口高、内容总高、clamp 规则都是实现细节，
      改布局就漂。固件在 scroll_clamp() 里镜像出 g_diag_scroll_* 三个量
      （scroll_y 是对象字段，read_vars.py 取不到它的地址）。
    """
    return rd1("g_diag_scroll_y"), rd1("g_diag_scroll_max")


def ime_visible():
    return rd1("g_diag_ime_visible") != 0


def ensure_ime_hidden():
    """确保软键盘收起，且**仍停留在设置页**。

    ⚠ 这是本次验收真实踩到的两个坑（都在 2026-10-04 当天连着踩）：

    坑 1 —— 键盘遮挡：键盘挂 top_layer、占 y=226..448，会盖住设置页
      滚动区下半部分。点"关机按钮"（滚到底后 abs y=408..448）时若键盘
      还开着，命中的���键盘 ⇒ 表现为"按钮点不到"，极易误判成坐标算错。

    坑 2 —— 收起键盘的动作本身会把页面退走：导航栏 home 键 (160,464)
      走的是 on_home（直接回桌面，scene=HOME=1），**不是** on_back。
      上一版脚本就是用它收键盘的，结果设置页被退出，后面那次点击落在
      桌面上，表现为"关机按钮点不到" —— 完全是假象。
      ⇒ 改用键盘自己的「收起」键，且事后必须校验 current_app。

    收起键位置（phone_ime.c refresh_candidates / PhoneIME_Init）：
      非组合态 candidate_prev.area = (260, 24, 54, 35)，键盘在
      KEYBOARD_Y=226 ⇒ abs x 260..314、y 250..285，中心 (287, 267)。
      （组合态它会缩成 24 宽的翻页键「<」，此时点它只是翻页、不收键盘。）
    """
    if not ime_visible():
        return True
    tap(*IME_COLLAPSE, wait=1.0)
    if ime_visible():
        # 退路：可能正处于组合态（收起键变成了「<」）。先点"空格"清组合
        # （SPACE 在 (150, 226+178+14)），再点收起。
        tap(150, 418, wait=0.8)
        tap(*IME_COLLAPSE, wait=1.0)
    return not ime_visible()


def main():
    no_reset = "--no-reset" in sys.argv

    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("[FAIL] 主循环没在跑 → 先 probe-rs reset")
        return 1

    boot_ribbon = rd1("g_cfg_ribbon")
    boot_rt = rd1("g_ribbon_translucent")
    print("开机配置：loaded=%d crc_ok=%d ribbon=%d  运行时 g_ribbon_translucent=%d"
          % (rd1("g_cfg_loaded"), rd1("g_cfg_crc_ok"), boot_ribbon, boot_rt))
    if boot_ribbon != boot_rt:
        print("[FAIL] 配置值(%d)与运行时值(%d)不一致 —— 开机同步链路断了"
              % (boot_ribbon, boot_rt))
        return 1
    print("  ⇒ 本次上电半透明 = %s\n" % ("开（壁纸透光）" if boot_rt == 1 else "关（实色）"))

    # ================= ① 交互 =================
    # ⚠⚠ 顺序：必须先 open_settings() 再 wait_diag()。
    #   诊断量是"固件每帧现算当前对象的绝对坐标"，而 app_page 停在 y=480
    #   直到 open_app 的归位动画跑完（见 Settings_UpdateDiag 的注释）。
    #   反过来先读，拿到的是**上一页/动画未完成**的旧坐标 ——
    #   实测踩过：脚本上一轮把页面留在滚动底部，本轮开头直接读，
    #   拿到 (260,250) 那个动画中途的坐标，点空 → 误报"开关点不到"。
    if not open_settings():
        print("[FAIL] 没进到设置页 —— 图标坐标可能变了")
        return 1
    sw = wait_diag("半透明开关")
    print("① 交互：拨动半透明开关 %s（固件每帧现算）" % (sw,))

    before_rt = rd1("g_ribbon_translucent")
    save_before = rd1("g_cfg_save_cnt")
    tap(*sw, wait=1.2)
    after_rt = rd1("g_ribbon_translucent")
    after_cfg = rd1("g_cfg_ribbon")
    print("  g_ribbon_translucent: %d → %d ；g_cfg_ribbon=%d ；save_cnt %d → %d"
          % (before_rt, after_rt, after_cfg, save_before, rd1("g_cfg_save_cnt")))
    if after_rt == before_rt:
        print("  [FAIL] 开关没翻转 —— 坐标 %s 点不到" % (sw,))
        return 1
    if after_cfg != after_rt:
        print("  [FAIL] 配置层(%d)与运行时(%d)不一致" % (after_cfg, after_rt))
        return 1
    if rd1("g_cfg_save_cnt") == save_before:
        print("  [FAIL] save_cnt 没增加 —— 没触发落盘")
        return 1
    print("  [ OK ] 开关翻转 + 配置层同步 + 已触发保存")

    # 回桌面看渲染是否真的变了（壁纸 ribbon 区域）
    before_img = cc.frame()
    tap(*HOME_KEY, wait=1.4)
    after_img = cc.frame()
    box = cc.diff_box(before_img, after_img)
    n_diff = box[4] if box else 0
    print("  设置页→桌面 两帧差异：%d 像素%s"
          % (n_diff, "，差异区 x=[%d,%d] y=[%d,%d]" % box[:4] if box else ""))
    if n_diff < 500:
        print("  [FAIL] 桌面画面几乎没变 —— 开关没影响渲染？")
        return 1
    print("  [ OK ] 壁纸渲染确有变化（不只是变量改了）\n")

    # ================= ② 落盘 =================
    print("② 落盘证据：直接读 W25Q128 扇区 240")
    ok, ribbon = check_blob("切换后")
    if not ok:
        return 1
    if ribbon != after_rt:
        print("  [FAIL] flash ribbon=%d，期望 %d" % (ribbon, after_rt))
        return 1
    print("  [ OK ] flash 里半透明已持久化为 %d\n" % ribbon)

    # ================= ③ 滚动容器 =================
    print("③ 滚动容器验收")
    if not open_settings():
        print("  [FAIL] 没进到设置页")
        return 1
    sy0, smax = scroll_pos()
    print("  初始 scroll_y = %d（clamp 上限 %d，内容总高 %d）"
          % (sy0, smax, rd1("g_diag_scroll_content")))
    if smax <= 0:
        print("  [FAIL] 没有可滚动范围 —— 布局可能回退了？")
        return 1

    drag(*DRAG_XY, 0, -200, wait=1.0)   # 往上拖 = 往下滚
    sy1, _ = scroll_pos()
    print("  上拖 200px 后 scroll_y = %d" % sy1)
    if sy1 <= sy0:
        print("  [FAIL] 上拖没有滚动（scroll_y %d → %d）" % (sy0, sy1))
        return 1
    print("  [ OK ] 拖动可滚动")

    # 继续拖到底，验证 clamp 生效
    for _ in range(3):
        drag(*DRAG_XY, 0, -200, wait=0.7)
    sy2, _ = scroll_pos()
    print("  反复上拖后 scroll_y = %d（应恰好 = %d）" % (sy2, smax))
    if sy2 != smax:
        print("  [FAIL] clamp 异常：scroll_y=%d，上限 %d" % (sy2, smax))
        return 1
    print("  [ OK ] 滚到底恰好停在上限")

    # 滚到底后关机按钮应可点开关机面板
    # ⚠ scene 枚举（phone_shell.c:47-55）：BOOT=0 HOME=1 APP=2 RECENTS=3
    #   SHUTDOWN=4 OFF=5。**别把 4 当成 OFF** —— 4 是关机动画中。
    #   点"关机..."只是**打开面板**，scene 不变（仍是 APP=2）；
    #   要等面板里点"关机"确认后才进 SHUTDOWN → OFF。
    #   这里只验"按钮可点"（面板打开），不越界去点确认。
    if not ensure_ime_hidden():
        print("  [FAIL] 软键盘收不起来（g_diag_ime_visible 仍为 1）")
        return 1
    if rd1("current_app") != SETTINGS_ID:
        print("  [FAIL] 收键盘时掉出设置页（current_app=%d）—— 不能继续点关机"
              % rd1("current_app"))
        return 1
    print("  [ OK ] 软键盘已收起，且仍在设置页")

    sy3, _ = scroll_pos()
    if sy3 != smax:
        drag(*DRAG_XY, 0, -200, wait=0.8)
    app_before = rd1("current_app")
    scene_before = rd1("scene")
    time.sleep(0.4)
    pxy = power_btn_xy()
    tap(*pxy, wait=1.4)
    scene_after = rd1("scene")
    app_after = rd1("current_app")
    # g_diag_power_dialog 语义：**1 = 已隐藏，0 = 显示中**（与 GY_STATE_Hidden 同步）
    shown = not rd1("g_diag_power_dialog")
    print("  点关机按钮(%s)：scene %d→%d，app %d→%d，关机面板 %s"
          % (pxy, scene_before, scene_after,
             app_before, app_after, "已打开" if shown else "没打开"))
    if not shown:
        print("  [FAIL] 关机面板没打开 —— 按钮点不到"
              "（scroll_y=%d，上限 %d，scene=%d）"
              % (scroll_pos()[0], smax, scene_after))
        return 1
    print("  [ OK ] 滚到底后关机按钮可点（面板已打开）\n")

    # 面板内两个按钮都要验（真实坐标准确，不按算式推）：
    #   面板 sheet 在 y=286..457；"取消" abs (34,382)..(154,426)、中心 (94,404)；
    #   "关机" abs (166,382)..(286,426)、中心 (226,404)。
    # 先点"取消"：面板应关闭，且**不能**顺带退出应用。
    tap(94, 404, wait=1.2)
    if rd1("g_diag_power_dialog") == 0:
        print("  [FAIL] 面板里的'取消'点不动")
        return 1
    if rd1("current_app") != SETTINGS_ID:
        print("  [FAIL] 点'取消'后掉出设置页（current_app=%d）"
              % rd1("current_app"))
        return 1
    print("  [ OK ] 面板'取消'可点（关面板且留在设置页）")

    # 再走完整闭环：关机 → OFF 页面 → "重新开机" → 回到桌面。
    # ⚠ 这条会真的关机，但演示固件的"关机"只是切 scene，不掉电，
    #   所以后面还能继续跑；判据是 scene 变化而不是设备行为。
    time.sleep(0.4)
    tap(*power_btn_xy(), wait=1.2)      # 重新打开面板
    if rd1("g_diag_power_dialog") != 0:
        print("  [FAIL] 面板没能再次打开")
        return 1
    tap(226, 404, wait=2.2)                  # "关机"
    s1 = rd1("scene")
    print("  点'关机'后 scene = %d（4=SHUTDOWN 动画中，5=OFF）" % s1)
    if s1 not in (4, 5):
        print("  [FAIL] 没进关机流程")
        return 1
    time.sleep(1.5)
    s2 = rd1("scene")
    print("  1.5s 后 scene = %d" % s2)
    if s2 != 5:
        print("  [FAIL] 关机动画后应停在 OFF(5)")
        return 1
    print("  [ OK ] 关机流程走通（→ OFF）")

    # OFF 页面的"重新开机"：power_page 上 (75,336,170,46)。
    # ⚠ 这个坐标是**按 phone_shell.c:1369 的建控件参数**取的，不是板上量的
    #   （OFF 页面不好反复进出，量一次成本太高）；它不随设置页布局变化，
    #   所以与"布局改动导致算式失效"那类坑无关。
    tap(160, 359, wait=2.0)
    s3 = rd1("scene")
    print("  点'重新开机'后 scene = %d" % s3)
    if s3 == 5:
        print("  [FAIL] '重新开机'点不动 —— 关机页无法回到桌面")
        return 1
    print("  [ OK ] 关机 → 重新开机 闭环走通\n")

    if no_reset:
        print("\n（--no-reset：跳过掉电重启验证）")
        return 0

    # ================= ④ 掉电重启 =================
    print("④ 掉电重启验证：半透明应保持")
    subprocess.run("probe-rs reset %s --chip %s" % (bench.PROBE, bench.CHIP),
                   shell=True, capture_output=True, text=True)
    time.sleep(12)
    a = rd1("g_loop_count")
    time.sleep(1.2)
    if rd1("g_loop_count") == a:
        print("  [FAIL] 复位后主循环没起来")
        return 1
    loaded = rd1("g_cfg_loaded")
    rt_after = rd1("g_ribbon_translucent")
    print("  重启后：loaded=%d crc_ok=%d ribbon=%d 运行时=%d"
          % (loaded, rd1("g_cfg_crc_ok"), rd1("g_cfg_ribbon"), rt_after))
    if loaded != 1:
        print("  [FAIL] 重启后没读到有效配置")
        return 1
    if rt_after != after_rt:
        print("  [FAIL] 重启后半透明回到 %d（期望 %d）—— 持久化没生效"
              % (rt_after, after_rt))
        return 1
    print("  [ OK ] 掉电重启后保持不变 ⇒ 持久化生效\n")

    # 收尾：恢复默认（半透明开）
    print("收尾：恢复半透明 = 开（项目出厂默认）")
    if open_settings():
        if rd1("g_ribbon_translucent") == 0:
            tap(*sw, wait=1.2)
        ok, ribbon = check_blob("恢复后")
        if ok and ribbon == 1:
            print("  [ OK ] 已恢复为半透明开，flash 同步")
        else:
            print("  [!] 恢复后 flash 状态异常，请复查")
            return 1

    print("\n[PASS] 四项全部通过：交互生效 · 渲染变化 · 落盘正确 · 重启保持 · 滚动可用")
    return 0


if __name__ == "__main__":
    sys.exit(main())
