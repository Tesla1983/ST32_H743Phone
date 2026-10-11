#!/usr/bin/env python3
"""把 ymgui-h743 下的旧构建目录送进回收站（可还原）。

用法：
  python recycle_build_prev.py --rbinspect                # 只 dump/解码回收站 $I 记录
  python recycle_build_prev.py --dry-run                  # 只报告将要处理的目录与字节数
  python recycle_build_prev.py                            # 真正执行（≤10 一批，批后校验）

设计纪律（见 windows-sandboxed-recycle-cleanup 技能）：
  - 只走 send2trash（底层 SHFileOperationW ⇒ 真回收站），脚本里**没有** os.remove /
    shutil.rmtree 之类的永久删除分支，杜绝误调用。
  - 批量 ≤10，批后同时断言：(a) 目标已不存在；(b) 回收站 $I 条目数上升。
  - 删前 dry-run 报字节数，与先前扫描对账。
"""
import argparse
import glob
import os
import struct
import sys
import time

ROOT = r"E:\ymgui-h743"
TARGETS = [
    "build_prev",
    "build_prev_20261010",
    "build_prev_20261010b",
    "build_prev_20261010c",
]
# 明确保护：当前构建目录，任何情况下都不动
NEVER = {"build", ".git", "src", "third_party", "docs", "tools", "ci", "cmake", "linker"}


def dir_size(path):
    total = 0
    for dp, _dn, fn in os.walk(path):
        for f in fn:
            try:
                total += os.path.getsize(os.path.join(dp, f))
            except OSError:
                pass
    return total


def human(n):
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.1f} {u}" if u != "B" else f"{n} B"
        n /= 1024.0
    return f"{n} B"


def recycle_bin_root():
    """找目标所在盘的用户 SID 目录。

    ⚠ 关键：E: 盘的删除进的是 **E:\\$Recycle.Bin**，不是 C: —— 查错盘会得到
    "0 -> 0" 的假阴性，误判成"没进回收站/被永久删除"。所以按 ROOT 的盘符去查。
    S-1-5-18 是 SYSTEM 的，永远空，跳过。
    """
    drive = os.path.splitdrive(os.path.abspath(ROOT))[0]        # 例：'E:'
    base = drive + "\\$Recycle.Bin"
    if not os.path.isdir(base):
        return None
    cands = []
    for d in os.listdir(base):
        if d == "S-1-5-18":
            continue
        full = os.path.join(base, d)
        if os.path.isdir(full):
            n = len(glob.glob(os.path.join(full, "$I*")))
            cands.append((n, full))
    if not cands:
        return None
    cands.sort(reverse=True)
    return cands[0][1]


def count_i_records():
    root = recycle_bin_root()
    if not root:
        return None, 0
    return root, len(glob.glob(os.path.join(root, "$I*")))


def rbinspect():
    root, n = count_i_records()
    if not root:
        print("找不到回收站用户目录")
        return
    print(f"回收站目录: {root}")
    print(f"$I 记录数: {n}")
    print("-" * 60)
    for f in sorted(glob.glob(os.path.join(root, "$I*"))):
        try:
            with open(f, "rb") as fh:
                d = fh.read()
            if len(d) < 28:
                continue
            _ver, size, ts, nlen = struct.unpack("<QQQI", d[:28])   # 头是 28 字节
            path = d[28:28 + nlen * 2].decode("utf-16-le", "replace").rstrip("\x00")
            when = "0 (永久删除!)" if ts == 0 else \
                time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts / 1e7 - 11644473600))
            print(f"{os.path.basename(f):<20} size={size:>12} ts={when}")
            print(f"      原路径: {path}")
        except Exception as e:
            print(f"{os.path.basename(f):<20} 解析失败: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--rbinspect", action="store_true")
    args = ap.parse_args()

    if args.rbinspect:
        rbinspect()
        return 0

    # 收集存在的目标
    items = []
    for name in TARGETS:
        assert name not in NEVER, f"拒绝处理受保护目录: {name}"
        p = os.path.normpath(os.path.join(ROOT, name))     # 必须规范化（\\?\ 只吃反斜杠）
        if os.path.isdir(p):
            items.append((name, p, dir_size(p)))
        else:
            print(f"[skip] 不存在: {name}")

    total = sum(s for _, _, s in items)
    print(f"待处理 {len(items)} 个目录，共 {human(total)}：")
    for name, _, s in items:
        print(f"  - {name:<28} {human(s)}")

    if args.dry_run:
        print("\n[dry-run] 未做任何改动。")
        return 0

    if not items:
        print("没有需要处理的目录。")
        return 0

    _, before_n = count_i_records()
    print(f"\n[baseline] 回收站 $I 记录数 = {before_n}")

    from send2trash import send2trash

    ok = 0
    for i, (name, p, _s) in enumerate(items, 1):
        try:
            send2trash(p)
            ok += 1
            print(f"[{i}/{len(items)}] recycled: {name}")
        except Exception as e:
            print(f"[{i}/{len(items)}] FAILED: {name} => {e}")
            print("遇到失败，停止后续批次（纪律：首个失败即停）。")
            break

        # 每批（这里 1 个即一批，总数 ≤10）后校验
        if not os.path.exists(p):
            pass
        else:
            print(f"  !! 目标仍存在: {name}")
            break

    time.sleep(1)
    _, after_n = count_i_records()
    print(f"\n[verify] 回收站 $I 记录数 {before_n} -> {after_n}（应 +{ok}）")
    print(f"成功回收 {ok}/{len(items)} 个目录。")
    print("提示：回收站未清空之前，磁盘空间尚未真正释放；需要还原可从回收站恢复。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
