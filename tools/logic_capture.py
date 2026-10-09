#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""逻辑分析仪抓 TX 脚 + UART 自解码（Saleae Logic 2 自动化接口）
=============================================================================
干嘛用的
-----------------------------------------------------------------------------
串口"发了但对方收不到"这类问题，固件侧能查的全查完（引脚复用、外设使能、BRR）
之后，剩下的只有一件事要证明：**电平到底出没出脚、位宽对不对、空闲电平是不是高**。
这三件事只有波形能回答。本脚本一键完成：抓 → 存 → 解码 → 打印结论。

为什么**自己解码**而不用 Saleae 自带的 Async Serial 解码器
-----------------------------------------------------------------------------
自带解码器要先把波特率喂给它，它只会按这个数去解 —— 而"实际跑在哪个波特率"
恰恰是我们要测的量（BRR 算错会跑成 115200 而固件自认为 921600）。
所以这里从**原始电平跳变**反推位宽：取所有跳变间隔的最小值当位宽候选，
用它采样，再反算实际波特率。解出来是乱码 ⇒ 位宽候选不对，会直接报出来。

前置条件
-----------------------------------------------------------------------------
  Logic 2 必须带自动化参数启动（默认端口 10430）：
      & 'C:\\Program Files\\Logic\\Logic.exe' --automation
  或在 Logic 2 的 Preferences 最底部勾选 "Enable Automation Server"。

用法
-----------------------------------------------------------------------------
  python tools/logic_capture.py                       # 抓 CH7，8 秒，按 921600 解
  python tools/logic_capture.py --seconds 15 --rate 50000000
  python tools/logic_capture.py --csv 抓到的.csv       # 不抓，只解码已有 CSV
  python tools/logic_capture.py --channel 0 --baud 115200

判据
-----------------------------------------------------------------------------
  - 空闲电平 = 1（高）。恒 0 ⇒ 脚被外设/TX 拉死，对端永远看到起始位
  - 实测波特率 ≈ 917431（标称 921600，BRR=109 的必然结果）
  - 解出的帧 ≈ `$?PING*2F\\r\\n`
  - 一个跳变都没有 ⇒ 这一脚没在发（脚找错了 / 夹具没夹到 / 没共地）
"""

import argparse
import csv
import glob
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


# --------------------------- 解码部分（与 Saleae 无关） ---------------------------

def load_csv(path):
    """流式读 CSV，**只保留跳变点**。

    为什么必须流式压缩：25 MSa/s 抓 1 秒就是 2500 万行，全读进内存要几个 GB。
    而 UART 解码只需要"什么时候从 0 变 1 / 从 1 变 0"，中间那些重复样本全是废的，
    边读边丢即可 —— 内存占用与抓多久无关。

    兼容 Saleae 的两种导出格式：
       (a) 'Time [s], Value'           —— 单通道 digital_N.csv
       (b) 'Time [s], Channel 0, ...'  —— 多通道合并
    返回 (header, 跳变列表, (首时刻, 末时刻))。跳变列表首元素是**初始电平**。
    """
    trans = []
    t_first = t_last = None
    prev = None
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        rdr = csv.reader(f)
        try:
            header = next(rdr)
        except StopIteration:
            return None, None, (0.0, 0.0)
        ti = 0
        for i, h in enumerate(header):
            if "time" in h.lower():
                ti = i
                break
        val_idx = [i for i in range(len(header)) if i != ti]
        for r in rdr:
            if not r:
                continue
            try:
                t = float(r[ti])
            except (ValueError, IndexError):
                continue
            vals = []
            for i in val_idx:
                try:
                    vals.append(1 if float(r[i]) >= 0.5 else 0)
                except (ValueError, IndexError):
                    vals.append(0)
            if prev is None:
                trans.append((t, vals))
                prev = vals
            elif vals != prev:
                trans.append((t, vals))
                prev = vals
            t_last = t
            if t_first is None:
                t_first = t
    if not trans:
        return header, trans, (0.0, 0.0)
    return header, trans, (trans[0][0], t_last)


def pick_col(header, data, want):
    """返回"值列"中的下标（已经排除时间列）。
       want 不为 None 时优先按列名匹配通道号；否则选跳变最多的列。"""
    if not data:
        return 0
    # 值列在 header 里的下标（时间列除外）
    val_idx = [i for i, h in enumerate(header) if "time" not in h.lower()]
    if want is not None:
        for vi, hi in enumerate(val_idx):
            if str(want) in header[hi]:
                return vi
    best, bestn = 0, -1
    for c in range(len(val_idx)):
        n = sum(1 for k in range(1, len(data))
                if data[k][1][c] != data[k - 1][1][c])
        if n > bestn:
            best, bestn = c, n
    return best


def decode_uart(data, col, baud_hint):
    """从 (时间, 电平) 序列解 UART：8N1，LSB first，非反相。"""
    if len(data) < 2:
        return {"idle": (data[0][1][col] if data else -1),
                "err": "整段没有任何跳变 —— 这一脚没在发（电平恒定）",
                "t_span": 0.0}

    # 1) 空闲电平 = 占时间最长的电平
    tot = {0: 0.0, 1: 0.0}
    for k in range(1, len(data)):
        dt = data[k][0] - data[k - 1][0]
        if dt > 0:
            tot[data[k - 1][1][col]] += dt
    idle = 1 if tot[1] >= tot[0] else 0

    # 2) 跳变时刻
    edges = []
    for k in range(1, len(data)):
        a, b = data[k - 1][1][col], data[k][1][col]
        if a != b:
            edges.append((data[k][0], b))

    if not edges:
        return {"idle": idle, "err": "整段没有任何跳变 —— 这一脚没在发（恒 %d）" % idle,
                "t_span": data[-1][0] - data[0][0]}

    # 3) 位宽：先取最短跳变间隔当粗值，再**网格搜索精修**。
    #
    #    为什么必须精修（自检踩过）：25 MSa/s 下边沿时间被量化到 40 ns，
    #    而 921600 的位宽才 1090 ns —— 粗值有 ±3.7% 误差，按 1.5T…8.5T 采样时
    #    误差逐位累积，到第 8 位已经偏出 30%，解出来全是乱码。
    #    精修的评分函数是"停止位是否等于空闲电平"：只有位宽猜对了，
    #    每一字节的第 9.5T 处才都落在高电平上。
    gaps = [edges[i][0] - edges[i - 1][0] for i in range(1, len(edges))]
    gaps = [g for g in gaps if g > 0]
    if not gaps:
        return {"idle": idle, "err": "跳变间隔全为 0（CSV 时间列不对？）"}
    bit0 = min(gaps)

    starts = [t for (t, v) in edges if v != idle]

    # 精修：UART 的每个边沿都落在"起始位边沿 + 整数个位宽"上。所以拿
    #   r_k = (t_k - t_ref) / bit，看它离最近整数差多少；位宽猜准了，
    #   所有 r_k 都几乎正好是整数。对**整帧所有边沿**求残差平方和取最小，
    #   就等于用上百个边沿一起平均 —— 精度远超单个位宽的 40 ns 量化误差，
    #   足以分辨"到底是 921600 还是 115200"。
    def resid(bit):
        s, n = 0.0, 0
        t_ref = starts[0] if starts else edges[0][0]
        for (t, _) in edges[:4000]:
            r = (t - t_ref) / bit
            d = r - round(r)
            s += d * d
            n += 1
        return s / n if n else 1e9

    lo, hi, steps = 0.95, 1.05, 401
    cand = [(bit0 * (lo + (hi - lo) * i / (steps - 1)), i) for i in range(steps)]
    bit = min(cand, key=lambda c: resid(c[0]))[0]

    # 4) 逐字节解码 —— 必须**按帧顺序消费**，不能见下降沿就当起始位。
    #
    #    （自检踩过：数据位本身就有 1→0 的下降沿。把它们也当起始位的话，
    #     11 字节的帧会被解成 32 个"字节"，其中混着真字节 —— 看起来像是
    #     波特率差一点，其实是解码器自己的错。）
    #    正确做法：解出一个字节后，把搜索起点推到"起始位 + 10 bit"之后
    #    （1 起始 + 8 数据 + 1 停止），再找下一个起始位。
    cur = []
    next_ok = -1e30
    for t in starts:
        if t < next_ok:
            continue                      # 还落在上一字节的帧内，是数据位
        val, ok = 0, True
        for i in range(8):
            lv = level_at(data, col, t + bit * (1.5 + i))
            if lv is None:
                ok = False
                break
            val |= (lv << i)
        stop = level_at(data, col, t + bit * 9.5) if ok else None
        if ok:
            cur.append((t, val, stop))
            next_ok = t + bit * 10.0

    # 5) 切帧。帧内字节是**背靠背**的（相邻起始位正好差 10 bit），
    #    所以只有间隔明显大于 10 bit 才算换帧 —— 取 11.5 bit 作阈值。
    groups, g = [], []
    for (t, val, stop) in cur:
        if g and (t - g[-1][0]) > 11.5 * bit:
            groups.append(g)
            g = []
        g.append((t, val, stop))
    if g:
        groups.append(g)

    out = []
    bad_stop = 0
    for grp in groups:
        bs = bytes(v for (_, v, _) in grp)
        for (_, _, s) in grp:
            if s is not None and s != idle:
                bad_stop += 1
        try:
            txt = bs.decode("ascii", errors="replace")
        except Exception:
            txt = "?"
        out.append({"t0": grp[0][0], "n": len(bs), "hex": bs.hex(" "),
                    "txt": txt.replace("\r", "\\r").replace("\n", "\\n")})

    return {"idle": idle, "bit_s": bit, "baud": 1.0 / bit,
            "n_edges": len(edges), "frames": out, "bad_stop_bits": bad_stop,
            "t_span": data[-1][0] - data[0][0]}


def level_at(data, col, t):
    """在时刻 t 采电平（阶梯保持）。超出范围返回 None。"""
    if t < data[0][0] or t > data[-1][0]:
        return None
    lo, hi = 0, len(data) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if data[mid][0] <= t:
            lo = mid
        else:
            hi = mid - 1
    return data[lo][1][col]


def report(r, expect, nominal=921600):
    print("=" * 72)
    print("解码结果")
    print("=" * 72)
    if "err" in r:
        print("  [FAIL] %s" % r["err"])
        print("        空闲电平=%d  跨度=%.4f s" % (r.get("idle", -1),
                                                    r.get("t_span", 0)))
        return
    print("  空闲电平      %d  %s" % (r["idle"], "(高，正确)" if r["idle"] == 1
                                    else "!! UART 空闲必须是高"))
    print("  跳变次数      %d" % r["n_edges"])
    print("  实测位宽      %.4f µs" % (r["bit_s"] * 1e6))
    print("  实测波特率    %d  （标称 921600 / BRR=109 ⇒ 917431）" % r["baud"])
    dev = abs(r["baud"] - nominal) / float(nominal) * 100.0
    if dev > 2.0:
        print("  ⚠ 与标称 %d 差 %.1f%% —— **波特率不对**，对端不可能解出来" % (nominal, dev))
    else:
        print("  ✓ 与标称 %d 差 %.2f%%（8N1 容差约 ±3%%）" % (nominal, dev))
    print("  停止位异常    %d" % r["bad_stop_bits"])
    print("")
    if not r["frames"]:
        print("  [FAIL] 有跳变但解不出任何字节 —— 多半是波特率不对或采样率太低")
        return
    print("  共 %d 帧：" % len(r["frames"]))
    for f in r["frames"][:8]:
        print("    t=%+.6f s  %2d 字节  %s" % (f["t0"], f["n"], f["txt"]))
        print("                     hex: %s" % f["hex"])
    hit = [f for f in r["frames"] if expect and expect in f["txt"]]
    print("")
    if hit:
        print("  [PASS] 解出了预期帧 %r（共 %d 个）" % (expect, len(hit)))
        print("        ⇒ 这一脚确实在按 %d baud 往外打真实命令帧" % r["baud"])
    else:
        print("  [FAIL] 没解出预期帧 %r —— 看上面的 hex 是什么" % expect)


# --------------------------- 抓取部分（Saleae） ---------------------------

def do_capture(args):
    try:
        from saleae import automation
    except ImportError:
        print("[FAIL] 没装 logic2-automation：pip install logic2-automation")
        return None

    try:
        manager = automation.Manager.connect(port=args.port)
    except Exception as e:
        print("[FAIL] 连不上 Logic 2 的自动化接口（端口 %d）：%s" % (args.port, e))
        print("       解决：& 'C:\\Program Files\\Logic\\Logic.exe' --automation")
        print("       或在 Logic 2 → Preferences 最底部勾选 Enable Automation Server")
        return None

    devs = manager.get_devices()
    real = [d for d in devs if not getattr(d, "is_simulation", False)]
    print("  设备：%s" % (real[0] if real else (devs[0] if devs else "无")))
    if not devs:
        print("[FAIL] 没找到设备（Logic 2 里能看到分析仪吗？）")
        manager.close()
        return None

    cfg = dict(enabled_digital_channels=[args.channel],
               digital_sample_rate=args.rate)
    try:
        dev_cfg = automation.LogicDeviceConfiguration(
            digital_threshold_volts=3.3, **cfg)
    except Exception:
        dev_cfg = automation.LogicDeviceConfiguration(**cfg)   # Logic 8 固定阈值

    cap_cfg = automation.CaptureConfiguration(
        capture_mode=automation.TimedCaptureMode(duration_seconds=args.seconds))

    print("  抓 CH%d，%d MSa/s，%g 秒 ... 现在本板应该正在发（另开一个终端跑 tx_burst.py）"
          % (args.channel, args.rate // 1000000, args.seconds))
    with manager.start_capture(device_configuration=dev_cfg,
                               capture_configuration=cap_cfg) as cap:
        cap.wait()

        outdir = args.out
        os.makedirs(outdir, exist_ok=True)
        raw_dir = os.path.join(outdir, "raw")
        os.makedirs(raw_dir, exist_ok=True)
        try:
            cap.export_raw_data_csv(directory=raw_dir,
                                    digital_channels=[args.channel])
        except Exception as e:
            print("  原始导出失败：%s" % e)
        sal = os.path.join(outdir, "capture.sal")
        try:
            cap.save_capture(filepath=sal)
            print("  已保存 %s" % sal)
        except Exception as e:
            print("  保存 .sal 失败：%s" % e)

    manager.close()

    csvs = sorted(glob.glob(os.path.join(raw_dir, "*.csv")))
    if not csvs:
        print("[FAIL] 没导出任何 CSV（%s）" % raw_dir)
        return None
    return csvs[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", type=int, default=7, help="数字通道号，默认 7")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--rate", type=int, default=25_000_000)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--port", type=int, default=10430)
    ap.add_argument("--out", default="build/logic")
    ap.add_argument("--csv", default=None, help="不抓，直接解码已有 CSV")
    ap.add_argument("--expect", default="$?PING")
    ap.add_argument("--nominal", type=int, default=921600,
                    help="期望波特率，用于算偏差；默认 921600")
    a = ap.parse_args()

    path = a.csv
    if path is None:
        print("=" * 72)
        print("步骤 1/2  抓取")
        print("=" * 72)
        path = do_capture(a)
        if path is None:
            return 1

    print("")
    print("=" * 72)
    print("步骤 2/2  解码 %s" % path)
    print("=" * 72)
    header, data, (t0, t1) = load_csv(path)
    if not data:
        print("[FAIL] CSV 读不出数据：%s" % path)
        return 1
    print("  CSV：列=%s" % (header,))
    print("  跨度 %.6f s，跳变点 %d 个（原始样本已按跳变压缩）" % (t1 - t0, len(data)))
    col = pick_col(header, data, a.channel)
    r = decode_uart(data, col, a.baud)
    report(r, a.expect, a.nominal)
    return 0


if __name__ == "__main__":
    sys.exit(main())
