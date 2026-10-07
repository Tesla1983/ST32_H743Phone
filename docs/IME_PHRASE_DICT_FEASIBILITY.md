# MB 级词组词典上外部 flash —— 可行性分析 + 方案 B 已落地

日期：2026-10-07
问题：`phrases_rime.bin`（47 280 条词组，1.5 MB）能不能搬到 W25Q128，让
`zhongguo` 的首位候选变成"中国"？

> ## ★★ 2026-10-07 深夜：方案 B 已上板跑通，本文"可行性"部分已是历史
>
> 用户拍板走 B，已做完并实测，结果见 **§8「方案 B 落地结果」**。一句话：
> **词典全量 47 280 条常驻外部 flash，零 RAM 索引，`zhongguo` 首位已经是"中国"。**
> 下面 §1~§6 保留作决策依据（那些推导后来被实测证实/推翻的地方都已就地更正）。
>
> **2026-10-07 追加 §9**：顺手把"按键本来就 42~68 ms"也修了 ——
> 组合 DP 内层的 `snprintf` 换有界 `memcpy`，**42~68 ms ⇒ 17~29 ms（−58%），
> 候选一字未变**。另有一次**改坏又回退**的尝试（`composeQuality()` 闭式）连同
> 原因一起记在 §9.4，免得下一个人再踩。

> ## 结论先行
>
> **外部 flash 的容量完全不是问题（16 MB 里只用掉 ~1 MB，规划还给 IME 词典留了
> `0x300000` 起 3 MB）。真正的瓶颈是两件事：RAM 装不下索引，以及 XIP 非缓存区的随机读延迟。**
>
> | 方案 | 做法 | 词频覆盖 | RAM 增量 | 内部 flash 剩余 | 每次击键（**已实测**） | 结论 |
> |---|---|---:|---:|---:|---|---|
> | **A** | Top-N 高频词组**裁进内部 flash**，原地扫描 | 72.3%（Top-5k）/ 84.7%（Top-12k） | ≈ 2.7 KB | **347 KB / 124.7 KB** | 与片上 flash 同速，**0.053 ms** | 稳，但不跨 85% 线只能到 Top-5000 |
> | **B** | 全量 47 280 条上 XIP（`0x90300000`） | **100%** | ≈ 2.7 KB | **502.3 KB** | 单次 **0.93 ms**；但 ×查询长度后 6 字 **≈5.6 ms** | **可做，但改造 2+4 是硬前提** |
> | C | A 的热区 + B 的冷区双层 | ~100% | ≈ 2.7 KB | 视切分 | 接近 A | 必要性下降，暂不做 |
>
> **A 与 B 共用同一套代码改造**（§3），区别只在 payload 放哪。
> ⇒ 所以推 A 不等于否定 B：做完 A，B 只是"把同一份 payload 换个地址 + 加一次灌库"。
>
> **唯一的硬前提：必须删掉 `entries[]` 索引。** 只要还想用现有的那个索引结构，
> 全量需要 **738.8 KB** RAM（heap1 总共才 212 KB），连 Top-2000（31.2 KB）都会把
> heap1 打到 ~97%。这是本报告的枢纽结论。
>
> **XIP 延迟已于 2026-10-07 上板实测（§6）**，并推翻了两条原假设：
> ① 逐字节 `readLe32()` **不是** 4 倍代价（实测 0.96 倍）；
> ② 成本单位是"一段连续读"（新起一段 ≈ 510 ns，段内每字节只加 25 ns），
> 不是"一条 load 指令"。⇒ **布局密度**才是决定 B 快慢的第二个杠杆。

> ### ★ 更新（2026-10-07 晚）：枢纽前提已在**单字词典**上落地，账本变了
>
> 上面那句"必须删掉 `entries[]`"已经在**单字词典**那一步先做了
> （`ime_candidates.inc` 新增 `CHAR_AT()` / `charEntryAt()`，CHARS_ROM 分支不再
> `GY_malloc1(count × 12 B)`）。顺带把 `s_char_matches` 的容量从"全词典条数"改成
> "最大桶跨度"。省下的 heap1：
>
> | 项 | 原 | 现 |
> |---|---:|---:|
> | `entries[]` 索引 | 87 492 B | **0** |
> | `s_char_matches` | 29 164 B（7291 × 指针） | **7 368 B**（614 × 12 B 实体） |
> | 合计 | 116 656 B | 7 368 B |
>
> ⇒ **heap1 峰值 176.1 KB → 69.4 KB（33%），余量 35.9 → 142.6 KB。**
>
> **已上板实测（2026-10-07，`python tools/heap_peak_check.py`）** —— 与上面按字节算的
> 预测**完全吻合**，不是估计值了：
>
> | 判据 | 实测 |
> |---|---|
> | `g_h1.peak` | **71 064 B = 69.4 KB / 212 KB = 33%**（改前 180 376 B = 176.1 KB） |
> | `g_h0.peak` | 32 688 B = 31.9 KB / 56.9 KB = 56% |
> | heap1 剩余 | **146 024 B = 142.6 KB** |
> | `tools/ime_probe.py nihao zhongguo` | 成功 2/2（候选与改前一致） |
> | A2 `g_gram_mismatch` / `g_ramp_mismatch` | 0 / 0 |
> | `g_fault.magic` | 0 |
> | FLASH | +24 B（1 576 644 → 1 576 668 B，75.18%） |
>
> ⇒ §2.2 那张"Top-N 装得下吗"的表**已经不成立**，重算如下（heap1 余 142.6 KB）：
>
> | 规模 | `entries[]` 索引 | 装得下吗 |
> |---|---:|:---:|
> | Top-5000 | 78.1 KB | ✅ 轻松 |
> | **Top-9000** | **140.6 KB** | ✅ 刚好（占余量 98.5%，偏紧） |
> | Top-12000 | 187.5 KB | ❌ |
> | 全量 47 280 | 738.8 KB | ❌ |
>
> 而走 §3 改造 1（原地扫描、不建索引）则只要 **≈2.7 KB**，上面四档**全都装得下** ——
> 所以 §3 改造 1 仍然是必做的，删索引的意义不是"够用了"，而是"RAM 从此不再是约束"。
>
> 对方案选择的影响：**A 的把握更大了**（现在它唯一的紧约束是 flash 93.6%，与 RAM 无关）；
> **B 依然要先量 XIP 延迟**（§6 那三个 case 一个数都没少）—— 删索引解决的是 RAM，
> 解决不了"外部 flash 每次读 350~500 ns"。
>
> 校验：`python tools/ime_char_index_check.py`（宿主机、不需要板），
> 四条整数断言全通过 —— 7291 条 `charEntryAt` 与旧索引 0 失配、
> cap=614、全枚举 702 个查询的 match 数上界 603/438 均不越界、桶归属 0 违规。

数据来源（全部实测/实读，非估算）：
- `third_party/YMGUI/project_Demo/chinese_ime/phrases_rime.bin` 头部逐字段解析 + 全量遍历
- `linker/stm32h743vit6.ld`、`third_party/BSP/MPU/mpu.c`、`src/qspi_port.c` 里的实际配置
- `ci/firmware_reference.sha256` 记录的当前固件大小
- 估算处一律单独标注

---

## 1. 现状：词组词典为什么一直是关的

`CMakeLists.txt:76-82` 的板级构建定义：

```
YMGUI_IME_CHARS_ROM=1        /* 单字词典 pinyin_gb2312.bin 内嵌内部 flash，零拷贝 */
YMGUI_IME_PHRASES_OFF=1      /* ★词组词典关★ */
YMGUI_IME_BSS_SECTION_NAME=bss_ime
```

后果就是用户看到的现象：中文候选只能按**单字频率**排序，`zhongguo` 首位是"中过"
（单字组合），而不是词组词典里的"中国"。

现有 loader（`ime_candidates.inc:91-138`）的做法是：把整个文件当 RAM 对象用 ——
要么从文件整读进 `GY_malloc1`（EXTERNAL 模式，`loadPhraseDictionary`），要么
"原地解析"（CHARS_ROM 模式）**但仍然要建 `entries[]` 索引**：

```c
ImeCharEntry* entries = (ImeCharEntry*)GY_malloc1(count * sizeof(ImeCharEntry));
```

这条 malloc 就是全部问题的来源。

---

## 2. 三个决定性的量

### 2.1 词典本体（实测）

| 文件 | 大小 | 条目数 | 记录 | blob | 平均 |
|---|---:|---:|---:|---:|---:|
| `phrases_rime.bin` | 1 578 847 B | **47 280** | 16 B/条 | 822 231 B | 17.4 B 字符串/条 · 33.4 B 总计/条 |
| `pinyin_gb2312.bin`（已在用） | 116 350 B | 7 291 | 12 B/条 | 28 722 B | — |
| `english_words.bin`（未用） | 4 703 911 B | 288 996 | 8 B/条 | 2 391 807 B | — |

一级桶分布（`z`=4636 · `s`=4270 · `j`=3564 · `y`=3407 · `d`=3149 · `x`=2986；均值 1818；`v`=0）。
二级桶（拼音前两个字母，实际只出现 100 个组合）：均值 **472**，最大 `sh`=3256、`ji`=3197、`zh`=3137。

### 2.2 RAM：`entries[]` 根本不可能

| 规模 | `PhraseEntry{4×uint32}` 索引大小 | 相对 heap1 |
|---|---:|---|
| Top-2000 | 31.2 KB | free 36 KB ⇒ **几乎打满** |
| Top-5000 | 78.1 KB | 放不下 |
| 全量 47 280 | **738.8 KB** | 3.5 倍 heap1 |

heap1 现状：`_heap1_start = 0x2404B000`，一直延伸到 AXI 末 ⇒ **212 KB**。

> ⚠ **本节数字已被上面"更新（2026-10-07 晚）"取代**：单字词典的 `entries[]`
> （87 492 B）已删，`s_char_matches` 也从 29 164 B 降到 7 368 B，
> 实测 heap1 峰值 **71 064 B = 69.4 KB（33%）**，free ≈ **142.6 KB**。
> 下面这张表保留原样作为"为什么当初必须删索引"的历史论据。

### 2.3 XIP 的单次读延迟（2026-10-07 已上板实测）

`src/qspi_port.c`：

```
QSPI CLK = hclk3(200 MHz) / (PRESCALER+1) = 100 MHz
映射模式 CCR：CMD 单线 · ADDR 四线 24 位 · DCYC=6 · DATA 四线
MPU region0：0x90000000 / 256 MB / PRIV_RO / XN=1 / NOT_CACHEABLE / NOT_BUFFERABLE
```

理论下界曾经这样算：8（CMD）+ 6（ADDR）+ 6（DUMMY）+ 2（1 字节数据）= 22 clk = 220 ns，
4 字节读 28 clk = 280 ns，加 AXI 往返与 CSHT（5 clk）⇒ **估 350~500 ns/次**。

**实测（micro.py case 20/21/22/27，见 §6）**，@400 MHz，取 3 次最小值：

| 访问形态 | 实测 | 说明 |
|---|---|---|
| 孤立 4 字节对齐读（地址随机） | **530 ns/次** | 单次 QSPI 事务的真实成本，落在当初估算的上沿 |
| 孤立 1 字节读（地址随机） | **510 ns/次** | 与 4 字节**几乎相同** —— 事务开销（CMD+ADDR+DUMMY+CSHT）远大于数据字节本身 |
| 稀疏顺序读（步长 16，每 16 B 取 4 B） | **510 ns/次** | **"顺序"本身没有红利**，因为预取窗口 < 16 B |
| 连续不断流（步长 1） | **25.1 ns/字节 ≈ 40 MB/s** | QUADSPI 的 read-ahead 把 SPI 时钟跑满（100 MHz 四线理论 50 MB/s） |

> **★ 结构性结论：XIP 的成本单位是"一段连续读"，不是"一条 load 指令"。**
> 每新起一段不连续读 ≈ **510 ns**；段内每多读 1 字节只加 ≈ 10 周期（25 ns）。
> 所以决定快慢的是**布局密度**（一次查询要新起多少段），不是读了多少字节。

⚠ **实测推翻了两条原假设（写这份报告时的推测）**：

1. ❌「`readLe32()` 逐字节拼 ⇒ 4 倍代价」—— 实测 **0.96 倍**（510 vs 530 ns）。
   那 4 个字节本来就在同一段连续区里，逐字节读只比一次字读多 ~30 周期。
   ⇒ §3 改造 3（换对齐字读）**收益≈0，已从必做降级为可选**。
2. ❌「每条 load 指令一次 QSPI 事务」—— 只对**每段的第一条** load 成立，
   段内后续 load 走预取 FIFO。真正的放大器是「每条记录要新起几段」，
   而当前格式每条记录要新起 **3 段**（记录 16 B / 拼音串 / 词串，三者的偏移互不相干）。

### 2.4 访问热点在哪（决定了"慢不慢"是否有害）

每次按键触发一次候选重建，走的是 `imeCandidatesReset()`：
- `resetPhraseSearch()` —— 扫**一个**一级桶（最大 4636 条）；
- `buildCompositeCandidates()` —— 对拼音的**每个字符位置**各扫一个一级桶，
  再做 DP 组词 ⇒ 复杂度 O(查询长度 × 桶长)，这是真正的放大器；
- `resetCharSearch()` —— 单字词典桶（均值 280），现在已在内部 flash。

好消息：`imeCandidatesReset` 只在**按键时**跑，不进每帧路径；
单次几毫秒是可接受的（最坏丢一帧）。所以"XIP 慢 10 倍"在这里**未必致命** ——
但必须实测量出来，不能凭"感觉有一点卡"来猜。

---

## 3. 公因数：不管最终放哪，都要做的四处改造

这四处对方案 A / B / C **完全通用**，做完后 payload 放内部还是外部只是一个地址差别。

| # | 改造 | 为什么必须 | 风险 |
|---|---|---|---|
| 1 | **删掉 `entries[]`，改成原地扫描**：桶边界 + 记录直接从 payload 地址算，不落地成指针数组 | 这是省下 738 KB 的唯一办法 | 低（就是现有 `CHARS_ROM` 那套再放宽一点） |
| 2 | **加二级桶索引**（拼音前两个字母 → 26×26 = 2728 B 表） | 把每次扫描量从"最大 4636 条"降到"均值 472 条"，XIP 方案全靠它 | 低（词典要重新 sort 生成，但 `tools/gen_ime_dict.py` 在仓库里） |
| 3 | **`readLe32()` 换对齐字读 / `LDM`** | 逐字节读在 XIP 上是 4 倍事务 | 低（局部改动） |
| 4 | **1 个字母的查询不走词组路径**（词组本来就 ≥2 音节），并对 `buildCompositeCandidates` 加 DP beam 上限与位置数上限 | O(len × bucket) 这个复杂度倍数必须压住 | 中（要用 `tools/ime_probe.py` 那一类自检盯住候选质量） |

⚠ loader 的校验是**硬编码**的（`ime_candidates.inc:106`：`bucket_count != 26u || record_size != 16u`），
改成二级桶时必须同步放宽，否则新版词典会被判 `"invalid"` 静静降级回"字符模式"。

---

## 4. 方案 A：Top-N 裁进内部 flash（推荐）

### 4.1 内部 flash 还剩多少

```
FLASH 2 MB = 2 097 152 B，已用 1 576 644 B（参考固件 CI 哈希那条）⇒ 余 520 508 B ≈ 508 KB
```

### 4.2 词频是极端长尾的 —— 这是本方案成立的全部理由

按 `frequency` 降序截取（已用真实数据算过）：

| Top-N | 词频覆盖 | payload（字符串去重后） | `entries[]` 索引 | 放进余下的 508 KB？ |
|---:|---:|---:|---:|:---:|
| 2 000 | 57.8% | 62.2 KB | 31.2 KB | 轻松 |
| 5 000 | 72.3% | 155.3 KB | 78.1 KB | 轻松 |
| **9 000** | **80.8%** | **281.9 KB** | 140.6 KB | ✅ flash 用到 ~89% |
| **12 000** | **84.7%** | **377.6 KB** | 187.5 KB | ✅ flash 用到 ~93.6%（余 ~133 KB） |
| 15 000 | 87.5% | 474.1 KB | 234.4 KB | ⚠ 太紧（98.3%） |
| 23 640（半数） | 93.0% | 754.5 KB | 369.4 KB | ❌ |

⇒ **推荐 Top-12000**（词频覆盖 84.7%，flash 余 ~133 KB），保守起见可先做 Top-9000。
**代价是剩下的 15.3% 长尾**（生僻词组打不出来），而收益是：不碰 QSPI、不碰 MPU、
不需要产线灌库、内部 flash 随机读 ~几十 ns 且还有 ART 加速 ⇒ 每次击键大概率在亚毫秒级。

⚠ 这些 payload 大小是**重排后新文件**的尺寸估算（沿用现有格式：header 136 B + 16 B/条记录
+ 字符串并按去重计），不是"原文件切前 N 条"的字节数；真正落地时以生成器输出为准。

### 4.3 flash 账本：做完还剩多少（2026-10-07 实测基准）

基准：**内部 flash 2048 KB，实测已用 1 576 668 B（75.18%），余 508.3 KB**
（2026-10-07 删单字索引后那版固件）。A / B 共用同一套代码改造，估 **~6 KB**
（二级桶表 2 728 B + 原地扫描 loader + 对齐读），下表已计入。

| 方案 | 词频覆盖 | 内部 flash 已用 | **内部剩余** | 跨 85% 外搬触发线？ |
|---|---:|---:|---:|:---:|
| 现状（无词组） | 0% | 75.18% | **508.3 KB** | 否 |
| B 全量 XIP | **100%** | 75.5% | **502.3 KB** | 否 |
| A Top-2 000 | 57.8% | 78.5% | **440.1 KB** | 否 |
| **A Top-5 000** | **72.3%** | **83.1%** | **347.0 KB** | **否（临界）** |
| A Top-9 000 | 80.8% | 89.2% | **220.4 KB** | 是 |
| A Top-12 000 | 84.7% | 93.9% | **124.7 KB** | 是 |

- **85% 触发线**出自 `docs/EXTERNAL_FLASH_OFFLOAD_FEASIBILITY.md`：
  「内部 flash 跨过 ~85%，或出现因为放不下而做不了的功能」就该回来重评外搬。
  85% = 已用 1 742 KB。
- **"够不够用"的口径**：日常加页面/控件是几十 KB 级，124.7 KB 也够；
  但**任何 ≥125 KB 的单一资源都放不下**，且烧录时间随体积线性拉长（190 s → 约 237 s）。
- **B 的外部账**：W25Q128 16 MB 已用 986 KB（字模区 982 016 B），放 1.5 MB 词典后
  → 2 528 KB（15.4%），**外余 13.53 MB**；用的是规划里早就留好的
  `0x300000~0x5FFFFF`（3 MB IME 区）的一半。
- **兜底刀**：内部 `.rodata.gb2312` = 982 016 B（959 KB）是纯冗余（运行期取字模走 XIP，
  它只在 `font_provision` 灌库时被读）。**即便做了 Top-12000，回收它能把占用打回 ~47%**
  —— 所以 Top-12000 不是死路，只是它把"回收 gb2312"从可选项变成必做项。

> **顺带排掉一个看着诱人的岔路**：如果以后真的回收 `.rodata.gb2312` 那 982 KB 冗余副本
> （见 `docs/EXTERNAL_FLASH_OFFLOAD_FEASIBILITY.md`，当时决策是不回收），
> 内部 flash 的可用空间会变成 508 + 982 = 1 490 KB —— 仍然**差约 52 KB**装不下全量
> 47 280 条（字符串去重后 1 541.8 KB），而且那样 flash 必然逼近 100%。
> ⇒ "全量词组"这件事**在内部 flash 上从原理就做不成**，只能去 XIP。

### 4.3 落地步骤

1. 改 `tools/gen_ime_dict.py`：按频率排序取 Top-N → 二级桶重排 → 新格式 `.bin`（保留 magic 便于校验）。
2. `CMakeLists.txt`：照 `ime_chars.bin` 那套 `objcopy` 再打包一个 `ime_phrases.bin`，
   新增 `YMGUI_IME_PHRASES_ROM=1`（替换 `YMGUI_IME_PHRASES_OFF=1`）。
3. `ime_candidates.inc`：新增 `PHRASES_ROM` 分支 + §3 四处改造，并把 `buckets` 参数化为两级。
4. 验收：`tools/ime_probe.py` 的候选质量 + `tools/bench.py` 确认**帧时未变**（词组不进每帧路径）
   + heap1 峰值不超 85% + A2 `gram=0`/`ramp=0`。

---

## 5. 方案 B：全量上外部 XIP

- **地址**：`src/qspi_port.h` 的规划里 `0x300000 ~ 0x5FFFFF` 就是留好的 **IME 词典区（3 MB）**，
  1.5 MB 放进去绰绰有余；XIP 基址 ⇒ `0x90300000`。**MPU 不用改**（region0 已覆盖整个 256 MB 窗口、只读、XN、非缓存）。
- **一次性灌库**：照 `src/font_provision.c` 的范式（记账头 magic/size/CRC + 跳过分支）加一个 `phrases_provision`。
  按 workspace 里"gb2312 982 KB ≈ 13~14 s（估算）"线性外推，**1.5 MB 约 20 s**（估算，未实测）。
- **必须先量**：见 §6。没有那个数，B 就是赌 —— **这个数现在已经量出来了（2026-10-07）**。
- **实测的单次查询成本**（§6 表）：二级桶均值 472 条 ⇒ **0.93 ms**；
  最坏桶 `sh` 3256 条 ⇒ **6.43 ms**。内部 flash 同模式是 **0.053 ms**，倍率 **17.4×**。
- **真正的放大器是 `buildCompositeCandidates` 的 O(查询长度 × 桶长)**（§2.4）：
  单次 `resetPhraseSearch` 的 0.93 ms 要乘上"查询有几个字符位置"。
  6 字拼音 + 二级桶 ⇒ 约 **5.6 ms**；不做 §3 改造 2（仍用一级桶，均值 1818）⇒ 约 **21 ms**。
  ⇒ **B 不是不能做，但"删索引"那一步远不够，§3 改造 2 + 改造 4 是硬前提。**

---

## 6. XIP 延迟：已上板实测（2026-10-07）

台架做在 `src/main.c` 的 `run_xip_micro()`（case 20~30），由 CMake 的 `YMGUI_XIP_BENCH`
控制，**默认 OFF —— 不进出货构建**（与 `YMGUI_DIAG` 同一口径，见 `CMakeLists.txt`）。

```bash
cmake -S . -B build -DYMGUI_XIP_BENCH=ON && cmake --build build
probe-rs download --probe <id> --chip STM32H743VITx build/ymgui-h743.elf && probe-rs reset ...
python tools/micro.py 20 21 22 23 24 25 26 27 28 29 30
# 量完编回出货形态并烧回：
cmake -S . -B build -DYMGUI_XIP_BENCH=OFF && cmake --build build && <再烧一次>
```

**访问模型**（每条记录）：16 B 记录里 3 次对齐 u32 读（拼音/词/词频三个偏移）+ 6 次字节读
（前缀比较，按查询长度 6 计）+ 3 次字节读（词串 UTF-8）= **12 次事务/条**，每条新起 **3 段**连续读。
模型**不依赖被读区域的内容**（地址按固定步长推进、每次读固定字节数），所以直接打在
还是空白（全 0xFF）的 IME 保留区 `0x90300000` 上 —— 事务时序只跟地址和总线属性有关。
内部 flash 对照打 `.rodata.gb2312`（982 KB，运行期唯一的大块只读常量），
两侧窗口同取 960 KB，苹果对苹果。

### 6.1 实测数据（@400 MHz，取 3 次最小值）

| case | 形态 | 总 cycles | 事务数 | ns/事务 | **合计 ms** |
|---:|---|---:|---:|---:|---:|
| 20 | XIP 随机 4B 对齐读 | 1 736 860 | 8 192 | **530.0** | 4.342 |
| 21 | XIP 顺序 4B（步长 16，稀疏） | 1 671 320 | 8 192 | **510.0** | 4.178 |
| 22 | XIP 随机 1B 读 | 1 671 358 | 8 192 | **510.1** | 4.178 |
| 27 | XIP 顺序 1B（步长 1，不断流） | 82 178 | 8 192 | **25.1** | 0.205 |
| 26 | 内部 flash 随机 4B 对齐读 | 160 881 | 8 192 | **49.1** | 0.402 |
| 23 | XIP 模拟查询·密集（472 条，步长 7/4） | 366 780 | 5 664 | 161.9 | **0.917** |
| 28 | XIP 模拟查询·**真实步长**（472 条，步长 17） | 372 914 | 5 664 | 164.6 | **0.932** |
| 30 | XIP 模拟查询·记录间随机（472 条） | 360 632 | 5 664 | 159.2 | **0.902** |
| 24 | XIP 模拟查询·密集（3256 条，`sh`） | 2 530 031 | 39 072 | 161.9 | **6.325** |
| 29 | XIP 模拟查询·**真实步长**（3256 条） | 2 572 346 | 39 072 | 164.6 | **6.431** |
| 25 | 内部 flash 模拟查询（472 条，与 23 同模式） | 21 027 | 5 664 | 9.3 | **0.053** |

派生：
- 随机 4B 对齐读 **外 530.0 ns vs 内 49.1 ns = 10.8 倍**；
- 同模式模拟查询 **外 0.917 ms vs 内 0.053 ms = 17.4 倍**。
- **三种布局假设（密集 0.917 / 真实步长 0.932 / 记录间随机 0.902）彼此差 <3%** ——
  这是对 §2.3 那条结构性结论的正面验证：成本由"新起了几段"决定，与步长、是否顺序无关。

### 6.2 判据与结论（原判据需要修正）

原判据（X2 ≤ 2 ms ⇒ B 直接做）只看 `resetPhraseSearch`：

| 口径 | 实测 | 原判据 |
|---|---:|---|
| 单次 `resetPhraseSearch`（二级桶均值 472 条） | **0.93 ms** | ✅ ≤ 2 ms |
| 单次 `resetPhraseSearch`（最坏桶 `sh` 3256 条） | **6.43 ms** | ⚠ 落在 2~10 ms |

但**原判据漏了 §2.4 那个放大器**：`buildCompositeCandidates` 对拼音的**每个字符位置**
各扫一次桶，复杂度 O(查询长度 × 桶长)。把实测值乘上去：

| 组合 | 单次扫描 | 6 字拼音总耗时 |
|---|---:|---:|
| 二级桶 472 条（§3 改造 2 已做） | 0.93 ms | **≈ 5.6 ms** |
| 一级桶 1818 条（**不做**改造 2） | ≈ 3.6 ms | **≈ 21 ms** |
| 最坏桶 `sh` 3256 条 | 6.43 ms | **≈ 38 ms** |

⇒ **结论：方案 B 可以做，但有硬前提**（不再只是"删索引"那一步）：

1. **§3 改造 2（二级桶）是硬前提**，不是优化 —— 不做它，每次按键 21 ms。
2. **§3 改造 4（beam 上限 / 位置数上限）也是硬前提** —— 即便有二级桶，6 字查询仍 5.6 ms
   （掉一帧）。beam 压到 top-K=32 量级可再降一个数量级。
3. **§3 改造 3（对齐字读）实测收益≈0（0.96 倍），已降级为可选项**，不必为此改代码。
4. **布局是第二个杠杆，且是免费的**：若 `gen_ime_dict.py` 把每个二级桶写成
   「记录 + 它自己的两个字符串紧邻、按扫描顺序」的**单块连续区**，整桶扫描就从
   3 段/条降到 1 段/条。按 case 27 实测的 25.1 ns/字节线性推，472 条 × 21 B ≈ **0.25 ms**
   —— 比 0.93 ms 再快 **3.7 倍**。⚠ 这条是**从实测流速率推出来的、没有单独立 case**，
   真要照这个布局做，先补一个 case 把它测实。

### 6.3 与方案 A 的取舍（量完之后）

- **flash 维度 B 完胜**：B 只多 ~6 KB 代码，内部余 **502 KB**；A 的 Top-12000 会吃到
  **93.9%，只剩 124.7 KB**，并跨过本项目自己定的 85% 外搬触发线（见 §4.3）。
- **延迟维度 A 完胜**：A 走内部 flash，同模式 **0.053 ms**，是 XIP 的 1/17.4。
- **决定性变量变成了"改造 4 的 beam 上限能不能把扫描量压下来"**：
  压得住 ⇒ B（覆盖 100%，不占内部 flash）；压不住 ⇒ A 的 Top-5000（覆盖 72.3%，
  内部 flash 83.1% 不跨线）。
- **A+B 双层的必要性下降了**：A 的意义本来是"把高频词放进快的内部 flash"，
  但既然 B 单次只 0.93 ms，热词放内部省下的 0.9 ms 不值得多一套双查逻辑 ——
  **除非 beam 上限把质量压坏了**，那时才值得回头做双层。

⚠ **审计**：这三个（实为 11 个）case 依赖 `YMGUI_XIP_BENCH`，**出货构建里没有**。
`tools/micro.py` 会在 `g_micro_cyc == 0` 时报"没跑起来"而不是给假数，
`tools/` 下**没有任何验收脚本依赖 case 20~30**（已确认：只有本节的测量步骤用它们）。

---

## 7. 最容易踏错的三点（写给下一个来改的人）

1. **`entries[]` 是 RAM 陷阱，不是性能问题。** 看到"词组进 RAM 只需要 738 KB"就会想去
   扩 heap1 —— AXI 总共才 512 KB，扩也是徒劳。**必须先改访问方式（§3 第 1 条）。**
2. **别靠截图/感觉判好坏。** 本项目已经踩过一次"截图看不出花屏"（只有 `g_gram_mismatch`
   算数）。IME 同理：**候选串内容到底变了没有**，要靠读 `g_words[]` 与真机抓屏比对，
   不是"看起来差不多了"。
3. **改 loader 校验别漏。** 二级桶会让 `bucket_count != 26` 直接判 invalid，
   表现出来的现象是"词典没生效"而不是报错 —— 和本项目历史上多次出现过的"静静降级"同款。

---

## 8. 方案 B 落地结果（2026-10-07 深夜，板上实测）

### 7.1 做了什么

| 环节 | 产物 | 说明 |
|---|---|---|
| 词典重排 | `tools/gen_ime_dict.py` → `build/phrases_xip.bin` **1 303 456 B** | v3 格式：每首字母桶一整块连续区、条目自描述首尾相接、桶内按词频降序；丢掉全项目从未读取的 `initials` 字段（比原 v2 **小 268.9 KB / −17.4%**） |
| 固件 | `ime_candidates.inc` 新增 `YMGUI_IME_PHRASES_XIP` 分支 | **零 `entries[]` 索引**，按桶顺序游标解码；`LearnedPhrase`/`s_phrase_pending` 改按值存；`IME_PHRASE_SCAN_CAP=512` 作延迟上界 |
| 灌库 | `src/qspi_provision.c` + `tools/qspi_provision.py` | 1.27 MB 装不进内部 flash，走"host 分块送进 SRAM1 暂存区 → 固件写进 W25Q128"；11 块 × 120 KB |
| 校验 | `tools/ime_phrase_xip_check.py`（宿主机）、板上 XIP CRC 复核 | 五条整数断言 + 端到端 CRC |

### 7.2 验收（全过）

| 判据 | 实测 |
|---|---|
| 灌库完整性 | `g_prov_done = 1 303 456`，擦除 319 扇区，`g_prov_rc = 0` |
| **端到端 CRC（走 XIP 读回）** | 板上 `0x02747acb` = host `zlib.crc32`，**1 303 456 字节全对** |
| 词典未灌时是否安全 | magic 不匹配 ⇒ 安静退回单字模式，`g_gram_mismatch/ramp = 0`、`fault = 0`（灌库前先跑过一轮） |
| heap1 峰值 | **71 064 B = 69.4 KB / 212 KB = 33%**（与词组没开时**完全相同** —— 零索引的证据） |
| A2 逐像素 | `g_gram_mismatch = 0`、`g_ramp_mismatch = 0` |
| 出货形态 FLASH | 1 577 444 B（**75.22%**）；SRAM1 回到 512 B（灌库用的 120 KB 暂存区不进出货构建） |

### 7.3 候选质量（用户最初的诉求）

`tools/ime_bench.py` 实测首位候选：

| 拼音 | 方案 B 之前 | 方案 B 之后 |
|---|---|---|
| `zhongguo` | 中**过** | **中国** ✅ |
| `zhongguoren` | 中过人 | **中国人** ✅ |
| `xianzai` | 想**在** | **现在** ✅ |
| `nihao` | 你好 | 你好 ✅（本来就是） |
| `shijie` | 是接 | **仍是"是接"** ❌ —— 但**与词典存放位置无关**，见 §8.5 |

### 7.4 每次按键的代价（引擎部分，不含渲染）

| 拼音 | 词组 OFF（基线） | 词组 XIP 之后 | 增量 |
|---|---:|---:|---:|
| `zhongguo` | 42.645 ms | **52.121 ms** | +9.5 ms |
| `zhongguoren` | 54.482 ms | **67.210 ms** | +12.7 ms |
| `wo` | 1.644 ms | 3.210 ms | +1.6 ms |

⇒ 方案 B 的成本 ≈ **+1.16 ms × 拼音字母数**。与 §6 的 XIP 模型（0.35 ms/桶扫描）
同量级但偏大约 3 倍 —— 差额在 `phraseCompleteLengths()` 的音节解析 CPU 开销，
不在 flash 读取本身。

### 7.5 ⚠ 顺带量出来的一件**与 B 无关**的事：基线本来就 42~54 ms

词组词典还没灌时，`zhongguo` 的 `imeCandidatesReset()` 就已经 **42.6 ms**
（3 次复测 42.639 / 42.645 / 42.6 ms，可复现）。也就是说：

- **"按键慢"不是方案 B 引入的**，它一直存在，只是没人量过；
- 瓶颈在 `buildCompositeCandidates()` 的最内层：每条目要做
  ≤6 次 `snprintf(combined, "%s%s", …)` + ≤48 次 `addComposeState()`
  （后者内部又是一次 96 字节 `snprintf`）。单字桶里**每个条目都至少匹配 1 个字母**，
  所以这条重路径**一條都不跳过**。
- 可做的修法（纯机械替换、语义不变）：把那两处 `snprintf` 换成有界 `memcpy`。
  **2026-10-07 已做完并实测：42~68 ms ⇒ 17~29 ms（−58%），见 §9。**

`shijie` 打不出"世界"是**另一个**独立问题，同样与 B 无关：
单字 **是** 的频率是 **1 180 957**，`composeQuality()` 对 2 段复合除以 16 之后
"是接"仍有 **74 920** 分，压过"世界"这个真词的 **30 808**。
（"世界"在 `s` 桶内词频排 **18/4270**，没有 `IME_PHRASE_SCAN_CAP=512` 截断的问题。）

### 7.6 复现步骤

```bash
python tools/gen_ime_dict.py                       # → build/phrases_xip.bin
python tools/ime_phrase_xip_check.py               # 宿主机五条断言

# 灌库（一次性；需 YMGUI_QSPI_PROVISION=ON 的构建）
cmake -S . -B build -DYMGUI_IME_PHRASES_XIP=ON -DYMGUI_QSPI_PROVISION=ON
cmake --build build
probe-rs download --probe <P> --chip STM32H743VITx build/ymgui-h743.elf
probe-rs reset    --probe <P> --chip STM32H743VITx
python tools/qspi_provision.py                     # ≈7 分钟

# 出货形态（灌库支持关掉，词典留在外部 flash 不受影响）
cmake -S . -B build -DYMGUI_IME_PHRASES_XIP=ON -DYMGUI_QSPI_PROVISION=OFF
cmake --build build
probe-rs download --probe <P> --chip STM32H743VITx build/ymgui-h743.elf
probe-rs reset    --probe <P> --chip STM32H743VITx
```

⚠ 数据块必须走 gdb（`probe-rs write` 会把 120 KB 摊成命令行参数，Windows 上
WinError 206），且 `probe-rs` CLI 与 `probe-rs gdb` server **不能同时占用探针**；
恢复目标运行靠的是**杀掉 server**而不是 `detach`。这些都写进了
`tools/qspi_provision.py` 的注释里。

---

## 9. 按键耗时：42~68 ms ⇒ 17~29 ms（2026-10-07，板上实测）

§8.5 挂着的"基线本来就 42~54 ms"已经修掉。改动是**纯机械替换**，做完
**6 个词 × 6 个候选逐字与改动前一致**——这是"语义不变"的唯一硬证据。

### 9.1 改了什么

| 位置（`ime_candidates.inc`） | 原来 | 现在 |
|---|---|---|
| `buildCompositeCandidates()` 词组分支 | `snprintf(combined, "%s%s", state.word, phrase.word)` | `imeConcat()`：两次有界 `memcpy` + 补 NUL |
| `buildCompositeCandidates()` 单字分支 | `snprintf(combined, "%s%s", state.word, ce.ch)` | 同上；并把 `strlen(phrase.word)` / `strlen(ce.ch)` 提到 state 循环**外**（词组现在在 XIP 上，原来每个 state 都要重读一遍外部 flash） |
| `addComposeState()` 与收尾 | `snprintf(dst, "%s", word)` | `imeCopyWord()`：一次有界 `memcpy` |
| 内层状态访问 | `ComposeState state = s_compose_states[pos][i];`（**按值拷 104 B**） | `const ComposeState* state = &…` |

`imeCopyWord` / `imeConcat` 的语义与 `snprintf("%s")` / `snprintf("%s%s")` 完全等价
（拷到 NUL 为止、超长截断、末尾补 NUL），只是不再付"解析格式串 + 走可变参数栈"的钱。

### 9.2 实测（@400 MHz，每个词跑 3 次取最小，`tools/ime_bench.py`）

| 拼音 | 改动前 reset | 改动前合计 | 改动后 reset | **改动后合计** | 降幅 |
|---|---:|---:|---:|---:|---:|
| `zhongguo` | 52.126 | 52.491 | 20.630 | **20.993 ms** | −60.0% |
| `zhongguoren` | 67.218 | 68.841 | 27.162 | **28.783 ms** | −58.2% |
| `xianzai` | 41.209 | 42.858 | 16.229 | **17.878 ms** | −58.3% |
| `nihao` | 19.504 | 19.692 | 8.277 | **8.464 ms** | −57.0% |
| `wo` | 3.205 | 3.205 | 1.788 | **1.789 ms** | −44.2% |
| `shijie` | 41.261 | 41.323 | 17.097 | **17.158 ms** | −58.5% |

最坏一次按键（引擎部分，不含渲染）**68.84 ms ⇒ 28.78 ms**。
一帧预算 14.71 ms，仍占 **196%** —— 再往下压必须动算法（会改候选），不在本轮范围。

### 9.3 语义不变的证据：候选串逐字一致

| 拼音 | 改动前后候选（6 个，逐字相同） |
|---|---|
| `zhongguo` | 中国 中过 中果 中郭 中各国 中锅 |
| `zhongguoren` | 中国人 中国仍 中国任 中国人你 中国热闹 中国扔 |
| `xianzai` | 现在 想在 先在 现在爱 现在唉 现在哎 |
| `nihao` | 你好 你号 你浩 你豪 你耗 你郝 |
| `wo` | 我 喔 窝 握 卧 沃 |
| `shijie` | 是接 是借 是解 是节 是姐 是金额 |

### 9.4 试过但**回退**的一步：`composeQuality()` 改闭式（别再试一遍）

迭代 n 次 `x -> (x+7)/8` 恒等于 `floor((x + 8^n - 1) / 8^n)`，于是把 parts 次除法
压成 2 次。**数值等价**（host 单测 `tools/ime_compose_quality_test.py`：
parts 0..20 × 4025 个 score 含 0 / UINT32_MAX / 随机，共 **84 525 组逐值相等**），
但**上板实测反而慢 28%**（`zhongguo` 20.99 → 26.91 ms），已回退并把结论写进
`ime_candidates.inc` 的函数注释里。

原因是我的成本模型错了：

- 循环里的 `(score + 7u) / 8u` 除数是**常量**，编译器直接编成移位，几乎不要钱；
- 真正花钱的只有开头那一次 `score / parts`（变量除数 ⇒ 软件除法）；
- 闭式把「parts−1 次移位」换成了「parts−1 次乘法 + 1 次**变量除数**除法」，
  净亏一次软件除法。实测 `parts` 大多 ≤3，所以越改越慢。

⇒ 教训：**先确认"/8 是移位"再谈"减少除法次数"**。

### 9.5 出货形态回归（关掉 `YMGUI_XIP_BENCH` 后重烧）

| 判据 | 实测 |
|---|---|
| FLASH | 1 577 532 B（**75.22%**；比优化前 +88 B） |
| heap1 峰值 | **71 064 B = 69.4 KB / 212 KB = 33%**（与优化前**完全相同**） |
| A2 逐像素 | `g_ramp_mismatch = 0`、`g_gram_mismatch = 0` |
| `g_fault.magic` | 0 |
| `tools/ime_probe.py` | 3/3：`zhongguo`→**中国**、`xianzai`→**现在**、`zhongguoren`→**中国人** |
| `tools/flash_and_verify.py` | 语言三层验收 + A2 + 输入法 + 光标 **全 PASS** |
| SRAM1 / SRAM2 / DTCM | 512 B / 62 000 B / 23 936 B（未变） |

⚠ **测堆峰值的一个坑**：`heap1.peak` 是**开机以来的累计最大值**，不是当前值。
先跑过 `lang_check.py`（会进英文模式、走一遍 flash 落盘）之后再来读，会读到
**106 936 B（104.4 KB）**，判据会误报 FAIL。正确做法是**先 `probe-rs reset`、
等主循环起来、再单独跑 `tools/heap_peak_check.py`**，此时读到 71 064 B。

### 9.6 复现

```bash
python tools/ime_compose_quality_test.py        # 闭式等价性单测（回退了但仍留着当证据）

# 台架（量耗时）
cmake -S . -B build -DYMGUI_XIP_BENCH=ON && cmake --build build --target ymgui-bin
probe-rs download --probe <P> --chip STM32H743VITx build/ymgui-h743.elf && probe-rs reset --probe <P> --chip STM32H743VITx
python tools/ime_bench.py --repeat 3 zhongguo zhongguoren xianzai nihao wo shijie

# 出货形态回归
cmake -S . -B build -DYMGUI_XIP_BENCH=OFF && cmake --build build --target ymgui-bin
python tools/flash_and_verify.py
probe-rs reset --probe <P> --chip STM32H743VITx   # ⚠ 测堆峰值前必须单独复位
python tools/heap_peak_check.py
python tools/ime_probe.py zhongguo xianzai zhongguoren
```

---

## 10. 复现本报告的数字

```bash
cd E:/ymgui-h743/third_party/YMGUI/project_Demo/chinese_ime
python - <<'EOF'
import struct
d=open('phrases_rime.bin','rb').read()
count,blob,hs,rs,bc=struct.unpack_from('<5I',d,8)     # 47280, 822231, 136, 16, 26
bk=[struct.unpack_from('<I',d,28+4*i)[0] for i in range(bc+1)]
EOF
```
桶分布 / Top-N 词频覆盖 / payload 大小用同一段脚本遍历即可（本文 §2.1、§4.2 的表即其输出）。
