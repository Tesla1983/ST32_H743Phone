# -*- coding: utf-8 -*-
"""主机侧直接驱动 SPI2/I2S2（只碰寄存器+TXDR，与固件行为一致，无 GPIO 风险）：
观察 ① TXP 正常翻转 ② 欠载(UDR)后 TXP 是否恢复 ③ 清 UDR 能否救活。
跑完恢复 SPE=0。"""
import subprocess
import time

PROBE = "0416:5021:0123456789AB"
CHIP = "STM32H743VITx"
SPI2 = 0x40003800
CR1, CR2, CFG1, CFG2, SR, IFCR, TXDR = (SPI2 + o for o in (0x00, 0x04, 0x08, 0x0C, 0x14, 0x18, 0x20))


def rd(addr, bits=32, tries=5):
    fmt = {8: "b8", 16: "b16", 32: "b32"}[bits]
    for i in range(tries):
        o = subprocess.run(["probe-rs", "read", "--probe", PROBE, "--chip", CHIP,
                            fmt, "0x%08x" % addr, "1"],
                           capture_output=True, text=True).stdout
        tok = [x for x in o.split()
               if len(x) == 2 * bits // 8 and all(c in "0123456789abcdef" for c in x.lower())]
        if tok:
            return int(tok[0], 16)
        time.sleep(0.3)
    return None


def wr(addr, val, bits=32, tries=5):
    fmt = {8: "b8", 16: "b16", 32: "b32"}[bits]
    for i in range(tries):
        rc = subprocess.run(["probe-rs", "write", "--probe", PROBE, "--chip", CHIP,
                             fmt, "0x%08x" % addr, "0x%0*x" % (bits // 4, val)],
                            capture_output=True).returncode
        if rc == 0:
            return True
        time.sleep(0.3)
    return False


def show(tag):
    sr = rd(SR)
    if sr is None:
        print("%s SR read fail" % tag)
        return
    ftlvl = (sr >> 12) & 3
    print("%s SR=0x%08x  TXP=%d RXP=%d DXP=%d EOT=%d TXTF=%d SUSP=%d UDR=%d OVR=%d FTLVL=%d"
          % (tag, sr, (sr >> 1) & 1, sr & 1, (sr >> 2) & 1, (sr >> 3) & 1,
             (sr >> 4) & 1, (sr >> 5) & 1, (sr >> 8) & 1, (sr >> 9) & 1, ftlvl))
    return sr


print("== 起始状态 ==")
print("CR1=0x%08x I2SCFGR=0x%08x CFG1=0x%08x CFG2=0x%08x"
      % (rd(CR1), rd(0x40003850), rd(CFG1), rd(CFG2)))
show("before")

# 清标志 + 起引擎
wr(IFCR, 0x0FF8)
wr(CR1, 0x00000001)          # SPE
wr(CR1, 0x00000201)          # SPE | CSTART
show("after CSTART")

print("== 阶段1：快写 16 个包（不等，看 FIFO 容量/TXP）==")
for i in range(16):
    wr(TXDR, 0x1111, bits=16)
show("fast x16")

print("== 阶段2：每 150ms 写一包（必然欠载），共 24 次 ==")
for i in range(24):
    sr = show("w%02d" % i)
    wr(TXDR, 0x2222 if i % 2 else 0x1111, bits=16)
    time.sleep(0.15)

print("== 阶段3：尝试清 UDR 后连写 3 包，看 TXP 是否恢复 ==")
wr(IFCR, 0x0FF8)
for i in range(3):
    show("clr%d" % i)
    wr(TXDR, 0x3333, bits=16)
    time.sleep(0.1)

print("== 阶段4：CSUSP 挂起后重启 CSTART，再写 3 包 ==")
cr1 = rd(CR1)
wr(CR1, cr1 | (1 << 10))     # CSUSP
time.sleep(0.2)
cr1 = rd(CR1)
print("CSUSP 后 CR1=0x%08x（CSTART 应被硬件清掉）" % cr1)
wr(CR1, (cr1 & ~(1 << 10)) | (1 << 9))   # 清 CSUSP + 置 CSTART
for i in range(3):
    show("re%d" % i)
    wr(TXDR, 0x4444, bits=16)
    time.sleep(0.1)

print("== 收尾：关 SPE ==")
wr(CR1, 0x00000000)
show("end")
