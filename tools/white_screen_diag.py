"""白屏急诊：读健康度 / 故障现场 / 堆余量。"""
import sys, time
sys.path.insert(0, "tools")
import bench

def rd1(k):
    try:
        return bench.rd(k, 1)[0]
    except Exception as e:
        return None

def show(name):
    v = rd1(name)
    print("  %-16s = %s" % (name, ("0x%08X (%d)" % (v, v)) if v is not None else "读不到/无此符号"))
    return v

print("== 健康度 ==")
bm = show("g_boot_magic")
show("g_shell_rc")
show("g_font_ok")
try:
    l0 = rd1("g_loop_count"); time.sleep(1.0); l1 = rd1("g_loop_count")
    print("  g_loop_count    = %s -> %s  %s" % (l0, l1, "内核在跑" if l0 != l1 else "★内核没在跑(卡死/halt)"))
except Exception as e:
    print("  g_loop_count 读取失败:", e)

print("\n== 故障现场 g_fault (magic 0xFA017CA7 = 出过故障) ==")
if "g_fault" in bench.T:
    a = bench.T["g_fault"][0]
    w = bench.rd("g_fault", 9)
    names = ["magic", "kind", "cfsr", "hfsr", "mmfar", "bfar", "shcsr", "ccr", "count"]
    for n, v in zip(names, w):
        print("  %-8s = 0x%08X (%d)" % (n, v, v))
    kind = {1: "HardFault", 2: "MemManage", 3: "BusFault", 4: "UsageFault", 5: "NMI"}.get(w[1], "无")
    print("  ⇒ 故障类型 =", kind, " 次数 =", w[8])
    if w[0] == 0xFA017CA7:
        cfsr = w[2]
        print("     CFSR 分解: MMARVALID=%d BFARVALID=%d PRECISERR=%d IMPRECISERR=%d"
              % (cfsr & 1, (cfsr >> 8) & 1, (cfsr >> 9) & 1, (cfsr >> 10) & 1))
        print("     BFSR(总线错误)=(0x%02X) MMFSR=(0x%02X) UFSR=(0x%04X)"
              % ((cfsr >> 8) & 0xFF, cfsr & 0xFF, (cfsr >> 16) & 0xFFFF))
        print("     MMFAR=0x%08X BFAR=0x%08X" % (w[4], w[5]))
        print("     HFSR FORCED(被升级为Hard)=%d" % ((w[3] >> 30) & 1))
else:
    print("  没有 g_fault 符号")

print("\n== 堆 ==")
for k in ("g_h0_total", "g_h0_used", "g_h0_peak", "g_h0_n_alloc", "g_h1_total", "g_h1_used"):
    show(k)
