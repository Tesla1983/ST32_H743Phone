#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""天气 app 抓屏验收（2026-10-10）

回答两个问题，缺一不可：
 ① **数据**：三个城市的真实天气都取到了吗（SWD 直接读城市表，不靠屏幕）
 ② **视觉**：界面上真的是这三个城市的真数据吗？副标题还有"离线示例天气"吗？

① 由 `read_city_table()` 从 s_city_wx 直接解结构体得到 —— 它是**唯一真源**，
   屏幕上写什么都不能反过来证明数据对。
② 只能看图：打开第二页的天气图标 → 连按 4 次「切换城市」各抓一张。
   判据是**循环性**，不依赖起始城市：
     前三张互不相同（3 个城市画得确实不一样）
     第 4 张 == 第 1 张（确实按 3 循环，不是"每次都不一样"的假切换）
   最后还得人工看一眼 PNG：数字与城市名对不对得上（脚本只验"变了"，
   验不了"变成什么"）。

用法
----
    python tools/weather_shot.py              # 全流程：数据 + 两张抓屏
    python tools/weather_shot.py --data-only  # 只读数据，不抓屏
    python tools/weather_shot.py --probe-icon # 只探测天气图标坐标

⚠⚠ 抓屏必须走 pwr_shot.snap()（内部强制 g_force_redraw=1）。画面静止时
   脏区为空、镜像停在老帧，自己重写一定踩（已踩过两次）。
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench
import caret_check as cc
import pwr_shot
import ribbon_check as rc

WEATHER_ID = 5          # phone_app_catalog.def：TASKS=0 MUSIC=1 GALLERY=2 SETTINGS=3 CLOCK=4 WEATHER=5

def scene():
    """phone_shell.c 的 `scene`：0=BOOT 1=HOME 2=APP 3=RECENTS 4=POWER。

    ⚠⚠ **判"app 是否已打开"只能用 scene，不能用 current_app**。
      2026-10-10 实测：`current_app` 在**桌面页面**上仍读 5（上一次开过的天气），
      于是 `current_app == WEATHER_ID` 恒真，脚本很自信地打开了一个 app ——
      其实是打开了桌面，后面的抓屏全是桌面截图，而断言一路 PASS。
      ⇒ 真正的场景判据是 scene == 2。（gallery_shot.py 里的用法同理可疑。）
    """
    return read_u32(bench.T["scene"][0])


def icon_xy(app_id):
    """由固件里的真实布局算出某个 app 图标的**中心屏幕坐标**。

    ⚠ 为什么不写死：桌面的 `order[]` 是**可被用户拖动重排**的数组
      （phone_desktop.c:10），今天 order = 恒等不代表明天还是。
      所以坐标一律现算：读 order[] 找到 app 在**显示序**里的下标，
      再按 PhoneLauncher_Place() 的公式推 slot。

    ⚠⚠ **纵向还有一层 +28 的容器偏移**（phone_shell.c:1645
      `viewport = (0, 28, W, 408)`，图标挂在它下面的 home_strip 里）。
      少了这一层，算出来的 y 会比真图标高 ~28 px ⇒ 点在上面那格空白上。
      2026-10-10 就是这么错的：算出 (122,53)，实际有效区间是 y=64..124
      （实测扫描得出），于是"点了但什么都没发生"。
      命中判定（phone_desktop.c:66）给的是 `|y - (slot.y+28+25)| < 47`，
      那个 +28 正是这个容器偏移 —— 公式与实测因此能对上。
    """
    order = read_words(bench.T["order"][0], 16)
    try:
        i = order.index(app_id)
    except ValueError:
        return None
    if i < 4:
        page, local = 0, i
    else:
        page, local = 1 + (i - 4) // 8, (i - 4) % 8
    x = 23 + (local % 4) * 75
    y = 238 if page == 0 else 28 + (local // 4) * 110
    # +28 = 桌面 viewport 的容器偏移；+23 ≈ 图标高度的一半
    return (x + 24, 28 + y + 23)


# 兜底候选：万一 order[] 解不出来（符号被优化掉等），按"恒等 order"的常见情况试。
# ⚠ 命中判定（phone_desktop.c:66）是 |point_x - (slot.x+24)| < 38，
#   所以 x 有 ±38 的容差；y 的容差是 ±47，中心 = slot.y + 28 + 25。
#   第 1 页第 1 行实测有效区间 y=64..124（2026-10-10 逐格扫描）。
ICON_CANDIDATES = [(122, 81), (122, 95), (47, 81), (197, 81), (122, 191)]

# 「切换城市」按钮：weather.c 里 PhoneUI_button(view, 18, 369, 284, 37)
#   ⇒ view 坐标中心 (160, 387)；app_views 建在 (0,36) ⇒ 绝对 (160, 423)
NEXT_XY = (160, 423)

CITY_N = 3
CITY_SIZE = 52           # 7×int(28) + text[24]
NAMES = ["杭州", "上海", "成都"]


def alive():
    a = rc.rd1("g_loop_count")
    time.sleep(0.3)
    return a != rc.rd1("g_loop_count")


def read_bytes(addr, n):
    """按字节读内存（bench 只有按字读的接口，结构体里有 UTF-8 文本要按字节解）。"""
    r = subprocess.run("probe-rs read %s --chip %s b8 0x%08x %d"
                       % (bench.PROBE, bench.CHIP, addr, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split()
           if len(x) == 2 and all(c in "0123456789abcdefABCDEF" for c in x)]
    if len(tok) < n:
        raise RuntimeError("读 0x%08x 失败：%s%s" % (addr, r.stdout, r.stderr))
    return bytes(int(x, 16) for x in tok[:n])


def read_words(addr, n):
    """按 32 位读 n 个字。"""
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x %d"
                       % (bench.PROBE, bench.CHIP, addr, n),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split() if len(x) == 8]
    if len(tok) < n:
        raise RuntimeError("读 0x%08x 失败：%s%s" % (addr, r.stdout, r.stderr))
    return [int(x, 16) for x in tok[:n]]


def read_city_table():
    """直接解 s_city_wx[]。这是数据层唯一真源 —— 不读屏幕。

    ⚠ 地址从符号表动态取（bench.T），不写死：结构体一改、链接布局一动，
      硬编码地址就会静默读错地方，那是比"读不到"更糟的假数据。"""
    addr = bench.T["s_city_wx"][0]
    raw = read_bytes(addr, CITY_N * CITY_SIZE)
    out = []
    for i in range(CITY_N):
        d = raw[i * CITY_SIZE:(i + 1) * CITY_SIZE]
        valid, gen, tx, rh, pm25, aqi, code = struct.unpack("<7i", d[:28])
        txt = d[28:52].split(b"\x00")[0].decode("utf-8", "replace")
        out.append(dict(name=NAMES[i], valid=valid, gen=gen, temp=tx / 10.0,
                        rh=rh, pm25=pm25, aqi=aqi, code=code, text=txt))
    return out


def read_u32(addr):
    r = subprocess.run("probe-rs read %s --chip %s b32 0x%08x 1"
                       % (bench.PROBE, bench.CHIP, addr),
                       shell=True, capture_output=True, text=True,
                       encoding="utf-8", errors="ignore")
    tok = [x for x in r.stdout.split() if len(x) == 8]
    if not tok:
        raise RuntimeError("读 0x%08x 失败：%s%s" % (addr, r.stdout, r.stderr))
    return int(tok[0], 16)


def current_page():
    """读桌面当前页号。

    ⚠ phone_desktop.c 里 `current_page` 是 **int 指针**（指向 phone_shell 的页号），
    直接 bench.rd 读出来的是**地址**不是页号（读到 536894148 这种数就是它）。
    必须**解两次引用**：符号地址 → 指针值 → 页号。
    ⚠ 2026-10-10 连踩两次：第一次没解引用（读到地址当页号），第二次只解了一次
      （还是读到指针）。两次的症状都是翻页循环"永远到不了第 1 页"，
      一路拖到最后一页才停，然后"成功打开了 app"、打开的却是别的 app ——
      这种错最难发现，因为它不报错。"""
    ptr = read_u32(bench.T["current_page"][0])
    return read_u32(ptr)


def goto_page(idx):
    """桌面横向拖到指定页（0 基）。已在就一步不动。"""
    rc.goto_home()
    time.sleep(0.7)
    for _ in range(5):
        p = current_page()
        if p == idx:
            return True
        # 往左拖 = 下一页，往右拖 = 上一页
        rc.drag(260, 300, -200 if p < idx else 200, 0, wait=1.0)
        rc.settle(0.3)
    return current_page() == idx


def goto_page2():
    return goto_page(1)


def probe_icon():
    """在**第 1 页（下标 1）**的 8 个槽位里逐个试，直到 current_app == WEATHER。

    ⚠ 先按 order[] 现算的坐标试（正常一次命中）；不中就把整页 8 格都扫一遍。
      每格试之前都重新导航到第 1 页并**断言页号**——不这么做的话，某次点击
      可能落在别的页上，后面的判断全成了"在错的地方找对的图标"。
    """
    print("探测天气图标坐标（页号必须是 1，current_app 必须变成 %d）" % WEATHER_ID)
    xy = icon_xy(WEATHER_ID)
    print("  由 order[] + PhoneLauncher_Place() 现算：%s" % (xy,))

    # 第 1 页的 8 格：local 0..3 → slot.y=28；local 4..7 → slot.y=138
    # 中心 = (slot.x+24, 28 + slot.y + 23)   ← 2026-10-10 实测区间 y=64..124 / 174..234
    grid = [(23 + (k % 4) * 75 + 24, 28 + (28 if k < 4 else 138) + 23) for k in range(8)]
    cands = ([xy] if xy else []) + [g for g in grid if g != xy]
    for x, y in cands:
        if not goto_page2():
            print("  [!] 到不了第 1 页（当前 %d）" % current_page())
            return None
        rc.tap(x, y, wait=1.2)
        s, app = scene(), rc.rd1("current_app")
        print("  点 (%3d,%3d) → scene=%d current_app=%d %s"
              % (x, y, s, app, "命中" if s == APP_SCENE and app == WEATHER_ID else ""))
        if s == APP_SCENE and app == WEATHER_ID:
            return (x, y)
        if s == APP_SCENE:
            rc.goto_home()      # 点开了别的 app：先退出来再试下一格
            time.sleep(0.5)
    return None


APP_SCENE = 2


def open_weather(xy):
    """导航到第 2 页 → 点图标 → 用 scene==APP **且** current_app==WEATHER 判成功。"""
    goto_page2()
    rc.tap(*xy, wait=1.4)
    return scene() == APP_SCENE and rc.rd1("current_app") == WEATHER_ID


# ---- 抓屏 + 区域哈希 ----
# ⚠ 为什么不能拿整张 PNG 的 md5 比"两次画面是否相同"：
#   状态栏有**走秒/走分的时间**，副标题有"实时 HH:MM 更新"里的**分钟**。
#   跨到下一分钟，两张图必然不同 —— 于是"按 3 循环"这条判据会误报 FAIL
#   （2026-10-10 实测：4 张 md5 全不同）。
#   ⇒ 只比**城市区域**（大字号城市名/温度/天气/湿度行），它不含任何走时的东西。
#
# ⚠ 窗口是按"**绝对**屏幕 y"算的，不是 weather.c 里的 view 坐标 ——
#   app_views 建在 (0, 36)（phone_shell.c:1709），所有控件都要 +36：
#     副标题 37..55 → 绝对 73..91   ← **必须排除**（"实时 HH:MM 更新"里的分钟会变）
#     城市名 72..108 → 绝对 108..144
#     温度   123..189 → 绝对 159..225
#     天气   195..219 → 绝对 231..255
#     湿度行 219..237 → 绝对 255..273
#     三行   241..357 → 绝对 277..393  ← 也要排除（它虽然不变，但没必要比）
#   2026-10-10 第一次把窗口取成 60..240，正好把副标题（73..91）圈了进去，
#   于是"第 4 张 == 第 1 张"这条判据**因为跨了一分钟而恒假**（逐像素 diff 出来
#   只有 16 行不同：状态栏时钟 9..16 行 + 副标题 78..85 行）。
CITY_Y0, CITY_Y1 = 100, 275


def snap_region(tag, path, y0=CITY_Y0, y1=CITY_Y1):
    """抓一帧：存整幅 PNG（给人看），同时返回**城市区域**的内容哈希（给断言）。"""
    bench.wr("g_frame_mirror", [1])
    bench.wr("g_force_redraw", [1])
    time.sleep(0.35)
    px = cc.frame()
    pwr_shot.save_png(px, path)
    print("  [%s] 已存 %s" % (tag, path))
    buf = bytearray()
    for y in range(y0, y1):
        row = y * cc.G.W
        for x in range(cc.G.W):
            v = px[row + x]
            buf += bytes(((v >> 8) & 0xFF, v & 0xFF))
    return hashlib.md5(bytes(buf)).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-only", action="store_true")
    ap.add_argument("--probe-icon", action="store_true")
    ap.add_argument("--icon-xy", type=int, nargs=2, default=None)
    a = ap.parse_args()

    if not alive():
        print("[FAIL] 内核没在跑（g_loop_count 不变）—— download 之后忘了 reset？")
        return 1

    print("== 数据层（SWD 直读 s_city_wx）==")
    print("  g_city_req=%d  g_city_ok=%d  g_city_to=%d  g_city_scan=%d  "
          "g_city_last_scan=%d  g_net_wx_pkts=%d"
          % (rc.rd1("g_city_req"), rc.rd1("g_city_ok"), rc.rd1("g_city_to"),
             rc.rd1("g_city_scan"), rc.rd1("g_city_last_scan"),
             rc.rd1("g_net_wx_pkts")))
    rows = read_city_table()
    if rows is None:
        print("  [!] bench 没有按字节读内存的接口，跳过结构体解析（只看了上面的计数）")
    else:
        ok = 0
        for r in rows:
            if r["valid"]:
                ok += 1
                print("  %s  %5.1fC  湿度%d%%  PM2.5 %d  AQI %d  %s(code=%d) gen=%d"
                      % (r["name"], r["temp"], r["rh"], r["pm25"], r["aqi"],
                         r["text"], r["code"], r["gen"]))
            else:
                print("  %s  <无数据>" % r["name"])
        print("  => 有数据的城市 %d/%d %s" % (ok, CITY_N, "PASS" if ok == CITY_N else "FAIL"))

    if a.data_only:
        return 0

    if a.probe_icon:
        xy = probe_icon()
        print("结果：%s" % (str(xy) if xy else "所有候选都没命中"))
        return 0 if xy else 1

    icon = tuple(a.icon_xy) if a.icon_xy else icon_xy(WEATHER_ID)
    print("== 视觉层 ==")
    print("  天气图标坐标（现算）：%s" % (icon,))
    if not icon or not open_weather(icon):
        print("  %s 没命中，改扫第 1 页全格…" % (icon,))
        icon = probe_icon()
        if not icon:
            print("[FAIL] 打不开天气页")
            return 1
    rc.settle(0.4)

    # 连按 4 次「切换城市」，抓 4 张。
    # 判据（不依赖"起始是哪一城"）：先后 3 张互不相同（3 个城市确实不一样），
    # 第 4 张**与第 1 张相同**（确实按 3 循环，而不是"每次都不一样"的假切换）。
    shots = []
    for k in range(4):
        path = "build/weather_%d.png" % (k + 1)
        shots.append(snap_region("第 %d 屏" % (k + 1), path))
        if k < 3:
            rc.tap(*NEXT_XY, wait=1.2)
            rc.settle(0.4)

    print("  城市区域哈希：%s" % " / ".join(shots))
    ok = shots[0] != shots[1] and shots[1] != shots[2] and shots[0] == shots[3]
    print("  => %s（前三张互异 = 三个城市画得不一样；第 4 张 = 第 1 张 = 按 3 循环）"
          % ("PASS" if ok else "FAIL"))
    print("  ⚠ 这条只证明「画面确实按 3 个城市循环」，证明不了「数字对不对」——")
    print("     数字必须人工看 build/weather_1..3.png（城市名/温度/天气 三处都要对）。")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
