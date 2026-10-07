#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""烧录 + 三层语言验收 + 回归，一键跑完。

【为什么单独做一个】
  `probe-rs download` 这个固件要 **~187 秒**，之后还要等启动、跑 lang_check 的三层验收
  （交互 / 直读 flash / 掉电重启）、再跑 A2+IME+光标回归。手工一条条敲容易漏步骤，
  且中途任何一步失败都要重来。本脚本把它串成一条命令，每步失败即停并给出原因。

【前置】探针必须可用。脚本会先轮询等它恢复（拔插 USB 后即刻可用），
  等不到就明确提示，不会去做无用功。

用法
----
    python tools/flash_and_verify.py            # 完整流程
    python tools/flash_and_verify.py --wait 300 # 等待探针最多 300 秒（拔插前就启动它）
    python tools/flash_and_verify.py --skip-flash  # 只想重跑验收（固件已在板上）
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import bench  # noqa: E402

PROBE = "--probe 0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
ELF = "build/ymgui-h743.elf"


def run(cmd, timeout=None):
    """跑一个子工具并把输出按 UTF-8 收回来。

    ⚠ 必须同时给子进程定住编码（PYTHONIOENCODING=utf-8），**不能只在父进程解码**：
      本机控制台默认是 GBK，子工具（ime_probe 等）会按 GBK 输出，父进程却按
      UTF-8 解码 ⇒ "成功 2/2" 变成乱码，`"成功 2/2" in stdout` 恒为假，
      表现为"IME 未通过"这种**假失败**（2026-10-04 实际踩到：独立跑 ime_probe
      明明是 成功 2/2）。errors="replace" 而不是 "ignore"：
      宁可看到 � 也不能悄悄吞掉字符，否则同类问题又会伪装成"内容不对"。
    """
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout,
                          env=env)


def probe_ok():
    """探针能否真正打开（不只是枚举到）。"""
    r = run("probe-rs info %s --chip %s" % (PROBE, CHIP), timeout=60)
    out = (r.stdout or "") + (r.stderr or "")
    return "could not be opened" not in out and "Probe not found" not in out


def wait_probe(seconds):
    print("等待探针可用（最多 %d 秒）..." % seconds)
    t0 = time.time()
    while time.time() - t0 < seconds:
        if probe_ok():
            print("  [OK ] 探针已可用（等了 %.1f 秒）" % (time.time() - t0))
            return True
        time.sleep(5)
    print("  [FAIL] 等不到探针。请拔插一次调试器 USB（或按探针上的复位键）后重跑本脚本。")
    return False


def rd1(n):
    return bench.rd(n, 1)[0]


def main():
    wait_s = 60
    if "--wait" in sys.argv:
        wait_s = int(sys.argv[sys.argv.index("--wait") + 1])
    skip_flash = "--skip-flash" in sys.argv

    if not wait_probe(wait_s):
        return 1

    if not skip_flash:
        if not os.path.exists(ELF):
            print("[FAIL] 找不到 %s，先构建" % ELF)
            return 1
        print("\n烧录 %s（实测约 187 秒）..." % ELF)
        t0 = time.time()
        r = run("probe-rs download %s --chip %s %s" % (PROBE, CHIP, ELF))
        if "Error" in (r.stdout or "") + (r.stderr or "") or r.returncode != 0:
            print("[FAIL] 烧录失败：\n%s%s" % (r.stdout, r.stderr))
            return 1
        print("  [OK ] 烧录完成（%.1f 秒）" % (time.time() - t0))

    print("\n复位并等待启动...")
    run("probe-rs reset %s --chip %s" % (PROBE, CHIP))
    time.sleep(14)

    a = rd1("g_loop_count")
    time.sleep(1.5)
    if rd1("g_loop_count") == a:
        print("[FAIL] 复位后主循环没起来（g_loop_count 不变）")
        print("       判据提醒：probe-rs download 后内核仍 halt，必须跟一次 reset。")
        return 1
    print("  [OK ] 主循环在跑（g_loop_count 在增长）")

    print("\n" + "=" * 62)
    print("第一步：语言切换 + 持久化三层验收（tools/lang_check.py）")
    print("=" * 62)
    rc = subprocess.call([sys.executable, "tools/lang_check.py"])
    if rc != 0:
        print("\n[FAIL] 语言验收未通过（退出码 %d）" % rc)
        return rc

    print("\n" + "=" * 62)
    print("第二步：回归（A2 自检 / 故障计数器 / 输入法 / 光标）")
    print("=" * 62)

    ok = True

    # A2 逐像素自检
    bench.wr("g_gram_recheck", [1])
    time.sleep(6)
    ramp = rd1("g_ramp_mismatch")
    gram = rd1("g_gram_mismatch")
    print("A2  ramp mismatch = %d（期望 0）%s" % (ramp, "" if ramp == 0 else "  ← FAIL"))
    print("A2  gram mismatch = %d（期望 0）%s" % (gram, "" if gram == 0 else "  ← FAIL"))
    ok = ok and ramp == 0 and gram == 0

    # 故障计数器（g_fault 是结构体，读前两个字：magic 与 count）
    fault_magic = rd1("g_fault")
    print("g_fault.magic    = %d（0 = 从未发生内核故障）%s"
          % (fault_magic, "" if fault_magic == 0 else "  ← FAIL"))
    ok = ok and fault_magic == 0

    # 输入法
    r = run("%s tools/ime_probe.py nihao zhongguo" % sys.executable, timeout=300)
    ok_ime = "成功 2/2" in (r.stdout or "")
    print("IME              = %s" % ("2/2" if ok_ime else "未通过（见下方输出）"))
    if not ok_ime:
        print(r.stdout or "")
    ok = ok and ok_ime

    # 光标
    r = run("%s tools/caret_check.py" % sys.executable, timeout=300)
    out = r.stdout or ""
    ok_caret = "16 x 2 px" in out and "下划线" in out
    print("caret            = %s" % ("形状 16×2 下划线 / 周期正常" if ok_caret else "未通过"))
    if not ok_caret:
        print(out)
    ok = ok and ok_caret

    print("")
    if ok:
        print("=" * 62)
        print("[PASS] 烧录 + 语言三层验收 + 回归 全部通过")
        print("=" * 62)
        return 0
    print("[FAIL] 回归有未通过项，见上方标记")
    return 1


if __name__ == "__main__":
    sys.exit(main())
