#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""composeQuality() 闭式改写与原始迭代写法的**逐值等价**证明。

固件里的原始实现（ime_candidates.inc）：

    static uint32 composeQuality(uint32 score, uint8 parts)
    {
        if (parts == 0) return 0;
        score /= parts;
        for (uint8 i = 1; i < parts; ++i) score = (score + 7u) / 8u;
        return score;
    }

改写后（闭式）：迭代 n 次 x -> (x+7)/8 恒等于 floor((x + 8^n - 1) / 8^n)，
于是 parts 次除法固定降为 2 次。数值必须**逐位相同**，否则候选排序就变了。

本脚本用 Python 精确模拟两边的 uint32 语义（含 32 位回绕），做：
  A1 穷举 parts = 0..20 × score 取一批边界值与随机值（含 0、UINT32_MAX）；
  A2 断言两种写法结果完全相等；
  A3 断言闭式分支的守卫区间内没有 uint32 溢出（这是改写唯一的风险点）。

用法：python tools/ime_compose_quality_test.py
"""
import random
import sys

MASK = 0xFFFFFFFF


def old_quality(score, parts):
    """原始迭代写法（逐字照抄 C 语义）"""
    if parts == 0:
        return 0
    score = (score // parts) & MASK
    for _ in range(1, parts):
        score = ((score + 7) // 8) & MASK
    return score & MASK


def new_quality(score, parts):
    """闭式写法（逐字照抄 C 语义，含 parts > 11 时回退到迭代）"""
    if parts == 0:
        return 0
    q = (score // parts) & MASK
    if parts > 11:
        for _ in range(1, parts):
            q = ((q + 7) // 8) & MASK
        return q & MASK
    pow8 = 1
    for _ in range(1, parts):
        pow8 = (pow8 * 8) & MASK
    return ((q + pow8 - 1) // pow8) & MASK


def overflow_free(score, parts):
    """A3：闭式分支里 q + pow8 - 1 不能溢出 uint32"""
    if parts == 0 or parts > 11:
        return True
    q = score // parts
    pow8 = 1
    for _ in range(1, parts):
        pow8 *= 8
    return q + pow8 - 1 <= MASK


def main():
    scores = [0, 1, 2, 3, 7, 8, 9, 15, 16, 63, 64, 255, 256, 1023, 1024,
              65535, 65536, 1180957, 1180957 * 2, 1180957 * 8,
              10 ** 6, 10 ** 7, 2 ** 31 - 1, 2 ** 31, 2 ** 32 - 1]
    rnd = random.Random(20261008)
    scores += [rnd.randrange(0, 2 ** 32) for _ in range(4000)]

    bad = 0
    checked = 0
    for parts in range(0, 21):
        for s in scores:
            a = old_quality(s, parts)
            b = new_quality(s, parts)
            checked += 1
            if a != b:
                bad += 1
                if bad <= 10:
                    print(f"[失配] parts={parts} score={s}: 旧={a} 新={b}")
            if not overflow_free(s, parts):
                bad += 1
                if bad <= 10:
                    print(f"[溢出] parts={parts} score={s}")
    print(f"A1/A2 逐值比对：{checked} 组（parts 0..20 × {len(scores)} 个 score）")
    print(f"A3 溢出检查：parts<=11 时 q + 8^(parts-1) - 1 <= UINT32_MAX")

    if bad:
        print(f"\n[FAIL] {bad} 处不等价 / 溢出 —— 闭式改写不可用")
        return 1
    print("\n[OK] 闭式与迭代写法逐值等价，且守卫区间内无溢出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
