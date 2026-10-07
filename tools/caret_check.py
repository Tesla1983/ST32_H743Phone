#!/usr/bin/env python3
"""文本光标（caret）上板验收：形状 + 闪烁周期。

两项判定都是确定性的，不靠肉眼：
  ① 形状：把闪烁冻住（半周期设很大），强制"亮"重画抓一帧、强制"灭"重画抓一帧，
     两帧的差异像素就是光标本体 → 打印包围盒。
        下划线：宽 = 一个字符（中文 16 / 英文 8），高 = 2
        竖线  ：宽 = 2，高 = 字高（16）
  ② 周期：读设备端自带的 g_ymgui_caret_ms / g_ymgui_caret_flips，
     两者都在设备时间里累加（SWD 读数会暂停内核，用墙上时间算会偏快），
     周期 = 累计毫秒 / 翻转次数。

用法：python tools/caret_check.py
"""
import sys
import time

sys.path.insert(0, "tools")
import bench           # noqa: E402  复用 SWD 读写（rd / wr / 符号地址解析）
import grab_ymgui as G  # noqa: E402  复用全帧镜像读取


def tap(x, y, wait=0.5):
    bench.wr("g_bdtap_x", [x])
    bench.wr("g_bdtap_y", [y])
    bench.wr("g_bdtap_dx", [0])
    bench.wr("g_bdtap_dy", [0])
    bench.wr("g_bdtap_seq", [bench.rd("g_bdtap_seq", 1)[0] + 1])
    time.sleep(wait)


def rd1(n):
    return bench.rd(n, 1)[0]


_MIRROR_ON = False


def ensure_mirror():
    """确保固件的整帧镜像已打开（读 _frame_buf 之前必须先做）。

    ⚠ 为什么必须自己开、而不是靠固件默认值：步骤 2 为省 2.54 ms/帧，把
      YMGUI_PORT_FRAME_MIRROR 从出货构建摘掉了（默认 OFF）。镜像不开时
      phone_flush 根本不往 _frame_buf 写 ⇒ 读出来永远是开机时的旧内容：
        · 抓屏 = 全黑（526 字节纯色 PNG）
        · 像素差分恒为 0 ⇒ 调用方必然误报"界面没变"
      2026-10-04 实际踩到：lang_check 报"中/英两帧差异 0 个像素"，
      看着像"置脏范围优化改坏了重绘"，实际只是镜像没开。
      把自愈放进 frame() 里，任何调用方（lang_check / ribbon_check / 新脚本）
      都不会再踩这个坑。开关幂等：每个进程只写一次。
    """
    global _MIRROR_ON
    if not _MIRROR_ON:
        bench.wr("g_frame_mirror", [1])
        _MIRROR_ON = True


def frame():
    ensure_mirror()
    words = G.rd(G.FRAME_BUF, (G.W * G.H + 1) // 2)
    px = []
    for w in words:
        px.append(w & 0xFFFF)
        if len(px) < G.W * G.H:
            px.append((w >> 16) & 0xFFFF)
    return px[: G.W * G.H]


def diff_box(a, b):
    xs, ys, n = [], [], 0
    for y in range(G.H):
        row = y * G.W
        for x in range(G.W):
            if a[row + x] != b[row + x]:
                xs.append(x)
                ys.append(y)
                n += 1
    if n == 0:
        return None
    return min(xs), min(ys), max(xs), max(ys), n


def main():
    bench.wr("g_frame_mirror", [1])
    time.sleep(0.5)

    # 导航：回桌面 → 第 2 页 → 笔记 → 点正文（弹键盘、进编辑态）
    for _ in range(2):
        tap(160, 464, 0.6)
    page = rd1("home_index")
    if page != 1:
        tap(160, 348 + 7 if page == 0 else 422 + 7, 0.8)
    print("桌面页 home_index =", rd1("home_index"))
    tap(196, 80, 1.4)          # 笔记图标
    tap(70, 215, 0.9)          # 正文 → 键盘 + 编辑态
    print("已进入笔记（current_app = %d）" % rd1("current_app"))

    # ① 形状
    print("\n① 形状（冻结闪烁，亮/灭各抓一帧做差分）")
    bench.wr("g_ymgui_caret_blink_ms", [100000])
    bench.wr("g_ymgui_caret_on", [1])
    tap(70, 215, 0.8)
    a = frame()
    bench.wr("g_ymgui_caret_on", [0])
    tap(70, 215, 0.8)
    b = frame()
    box = diff_box(a, b)
    if box is None:
        print("  ❌ 两帧完全相同：光标没画出来（可能没进编辑态）")
        return 1
    x0, y0, x1, y1, n = box
    w, h = x1 - x0 + 1, y1 - y0 + 1
    print("  差异区 x=[%d,%d] y=[%d,%d] → %d x %d px，共 %d 个像素" % (x0, x1, y0, y1, w, h, n))
    print("  判定：%s" % ("下划线（高 2px，宽 = 一个字符）" if h <= 3 else "竖线（宽 2px，高 = 字高）"))

    # ② 周期（设备端计数器，避开 SWD 暂停内核的干扰）
    print("\n② 闪烁周期（设备端 ms/flips，采样期间不读变量）")
    bench.wr("g_ymgui_caret_blink_ms", [300])
    bench.wr("g_ymgui_caret_ms", [0])
    bench.wr("g_ymgui_caret_flips", [0])
    time.sleep(12.0)
    ms = rd1("g_ymgui_caret_ms")
    fl = rd1("g_ymgui_caret_flips")
    print("  设备累计 %d ms，翻转 %d 次 → 半周期 %.1f ms（设定 300）"
          % (ms, fl, ms / fl if fl else 0))

    # 留一张能看见光标的截图
    bench.wr("g_ymgui_caret_blink_ms", [100000])
    bench.wr("g_ymgui_caret_on", [1])
    tap(70, 215, 0.8)
    bench.wr("g_ymgui_caret_blink_ms", [300])
    print("\n已恢复默认：style=%d blink_ms=%d on=%d"
          % (rd1("g_ymgui_caret_style"), rd1("g_ymgui_caret_blink_ms"), rd1("g_ymgui_caret_on")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
