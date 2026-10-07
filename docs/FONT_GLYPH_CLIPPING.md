# 字母 W 渲染残缺 —— 根因定位报告

日期：2026-10-08（本机 00:0x）
现象：UI 里出现字母 **W** 的地方（`"Wi-Fi"`、`"Weather"` 等英文标签）图形看起来缺一块、右边那一竖不完整。
结论：**不是绘制代码的 bug，是字模生成时把字形裁掉了。**

---

## 1. 一句话结论

`third_party/YMGUI/tools/gen_font.py` 用 **15 pt 的 DejaVuSansMono（等宽步进 = 9 px）**
去填 **8 px 宽**的字符单元，`glyph_bytes()` 里那句

```python
nib = (px[x, y] >> 4) if x < cell_w else 0     # ← x >= 8 的墨迹直接归零
```

把第 9 列（x=8）的墨迹**整列丢弃**。95 个 ASCII 字形里有 **41 个**的墨迹伸到了 x=8；
其中 **W 和 w 丢的是最右侧一整条"实心"列（覆盖度 15/15）**，所以 W 看上去就是"右边少了一竖"。

---

## 2. 证据链

工具：`tools/font_clip_check.py`（本报告的每一步都能用它复跑）。
字体：DejaVuSansMono.ttf（脚本会自己找；找不到时可从 matplotlib 的 wheel 里提取）。

### A1 —— 先证明"仓库里的字模确实就是这么生成的"

用同一个 TTF、同一组参数（`PT=15, Y_OFF=-1, cell_w=8`）重算 95 个字形，
与 `third_party/YMGUI/YMGUI/CORE/YMGUI_FontData.c` 逐像素比对：

```
完全一致 95 个，不一致 0 个
```

⇒ 排除"版本不对/数据被手改过"等其他可能，问题就出在这组参数本身。

### A2 —— 量真实墨迹宽度，找出所有被裁的字

把每个字画进 32 列宽的画布量包围盒（单元只有 8 列，`真实右边界 >= 8` 即被裁）：

| 字符 | 墨迹 x 范围 | 被丢掉的 x=8 列覆盖度 | 观感 |
|---|---|---|---|
| **W** | 0..8 | **15（全实心）** | **最扎眼：右边一竖少了芯** |
| **w** | 0..8 | **14** | 同上（小写） |
| **#** | 0..8 | **15** | 右侧少一列 |
| **_** | 0..8 | **15** | 下划线短 1 px |
| % & T R K @ Z A X V Y M … | 0..8 | 1..12 | 程度递减，多数只有淡淡的抗锯齿边被切 |

共 **41 / 95** 个字形被裁。ASCII 里最宽的几个字（W / w / M / m）全在名单上。

### A3 —— W 的"完整版 vs 出货版"并排

同一个 TTF、同一字号，**完整 32 列**（左）与 **gen_font.py 实际产出 8 列**（右）：

```
 完整（真字形）        gen_font.py 产出（出货）
 @#     #@             @#     #
 @@     @@             @@     @
 @@     @@             @@     @
 #@ *@* @%             #@ *@* @
 +@ @@@ @+             +@ @@@ @
 :@.@%@ @-             :@.@%@ @
  @-@.@-@               @-@.@-@
  @%@ @%@               @%@ @%@
  @@@ @@@               @@@ @@@
  @@+ +@@               @@+ +@@
  *@: .@#               *@: .@#
```

右边那一竖的**实心部分（x=8，覆盖度 15）被丢**，只剩 x=7 上一道半透明的抗锯齿边
（覆盖度 7~9）—— 这就是肉眼看到的"W 缺了一块"。

（`blitGlyph()` 在 `YMGUI_DrawText.c` 里按"高 nibble 在左"取像素，与生成器一致；
A1 的 95/95 也证明了板上画的就是这张图。）

---

## 3. 一个顺带的观察：现在的英文其实还被挤了 1 px

15 pt 的 DejaVuSansMono 自然步进是 **9 px**，而 `cell_w = 8`。也就是说现在每个字母
不但右边被切 1 列，字与字之间还比自然排布**紧了 1 px**（左右边白被吃掉）。
W 之所以特别难看，是**"挤 + 切"两件事叠加**在同一个字上。

---

## 4. 影响范围

UI 文本统一走 `&YMGUI_Font_Default`（见 `phone_ui.c`），所以任何英文/数字都会用到这张字模。
明确含 W 的界面文案：

| 位置 | 文本 |
|---|---|
| `phone_lang.c:31` | `"Wi-Fi"`（设置项"无线网络"的英文） |
| `phone_lang.c:99` | `"Weather"`（"天气"的英文） |
| `phone_quick_builtin.c:111` | `"Wi-Fi"`（快捷开关磁贴） |

此外拼音/英文输入法的候选与状态栏文字也全是这张字模。

---

## 5. 两种修法（都已在宿主机量过，数字如下）

| 方案 | 改法 | 被裁字符 | W 观感 | 字模体积 | 副作用 |
|---|---|---:|---|---:|---|
| 现状 | `PT=15 / cell_w=8` | 41/95 | 残缺 | 6 080 B | — |
| **方案 1（推荐）** | `cell_w=8 → 9`，`PT=15` 不变 | **0/95** | 完整、字形最舒展 | 7 600 B（**+1 520 B**） | 英文整体宽 12.5%（这才是 15pt 的真实步进） |
| 方案 3 | `PT=15 → 13`，`cell_w=8` 不变 | **0/95** | 完整但偏挤、字变小 | 6 080 B（+0） | 字形高度也略缩，观感不如方案 1 |
| 方案 4 | `PT=14 / cell_w=9` | 0/95 | 完整 | 7 600 B | 与方案 1 比字号小 1pt，无收益 |
| ❌ 方案 2 | `PT=14 / cell_w=8` | **10/95（含 W）** | 仍残缺 | 6 080 B | 实测**不解决** W，别选 |

方案 1 的产出（W 完整）：

```
@#     #@
@@     @@
@@     @@
#@ *@* @%
+@ @@@ @+
:@.@%@ @-
 @-@.@-@
 @%@ @%@
 @@@ @@@
 @@+ +@@
 *@: .@#
```

### A4 —— 修好的字模已经生成出来了（未覆盖仓库文件）

```bash
python tools/font_clip_check.py --emit-fixed build/YMGUI_FontData_fixed.c
```

产出 `build/YMGUI_FontData_fixed.c`：**7 600 B（95 字形 × 16 行 × 5 字节）**，
`cell_w=9 / bytes_per_row=5`，被裁字符 **0/95**。其中 W 的 x=8 列逐行覆盖度
`[0,0,15,12,10,8,5,3,0,…]` —— 就是现在被丢掉的那条右竖笔芯：

```
@#     #@      ← 右边一竖回来了
@@     @@
@@     @@
#@ *@* @%
+@ @@@ @+
:@.@%@ @-
 @-@.@-@
 @%@ @%@
 @@@ @@@
 @@+ +@@
 *@: .@#
```

### 改动面

- `third_party/YMGUI/tools/gen_font.py`：`emit_ascii()` 里的 `CELL_W, CELL_H, PT, Y_OFF = 8, 16, 15, -1`
  改成 `9, 16, 15, -1`（`bpr` 会自动算成 5，不用另改）。
- 重新生成 `YMGUI_FontData.c`（95 字形 × 80 B）。
- 需要重新过一遍的：`ci/firmware_reference.sha256`（固件哈希会变）、
  `tools/flash_and_verify.py` 全套回归、A2 逐像素自检。
- 代码侧**没有**硬编码 8 px 字宽的地方（UI 一律走 `YMGUI_Font_TextWidth()` /
  `font->cell_w`），所以 cell_w 从 8 变 9 不需要改业务代码。

⚠ 未验证项：GB2312 / CJK 字库是 `cell_w=16 / PT=16`（Noto Sans CJK），
单元宽 = 1 em，理论上不会溢出，但本机没有那份字体，**本报告没覆盖**。

---

## 5A. 落地记录 —— 方案 1 已实施并上板验收（2026-10-07 深夜）

上面 §5 写的时候还是"推荐待实施"。**现已做完**，过程与结果如下。

### 实施步骤（可复跑）

1. `gen_font.py` 的 `CELL_W` 改为 9（带注释说明原因与"别改回 8"的实测依据）；
2. 用改后的生成器重跑 `emit_ascii()`，与 `font_clip_check.py --emit-fixed` 的产出
   **逐字节比对：7 600 B，0 失配** ⇒ "生成器 = 产物"，仓库文件确实由仓库脚本生成；
3. 用生成器正式产出覆盖 `third_party/YMGUI/YMGUI/CORE/YMGUI_FontData.c`
   （结构段 `cell_w=9 / bytes_per_row=5`，diff 仅点阵重排 + 这两个字段 + 描述行）；
4. 出货形态构建（XIP=ON / PROVISION=OFF / BENCH=OFF）。

### 上板实测

| 判据 | 结果 |
|---|---|
| FLASH | **1 579 052 B（75.30%）**，+1 520 B 与字模增量**分毫不差** |
| 字模一致性 | 宿主机重算 95/95 逐字节一致（font_clip_check A1） |
| W | **0/95 被裁**，x=8 列覆盖度 `[0,0,15,12,10,8,5,3,…]` —— 右竖笔芯回来了 |
| A2 逐像素 | `g_ramp_mismatch = 0`、`g_gram_mismatch = 0` |
| heap1 峰值 | **71 064 B（33%）**，与改动前完全一致（字模走 const flash，不占堆） |
| 输入法 | `ime_probe` zhongguo→中国、xianzai→现在、zhongguoren→中国人，3/3 |
| 光标 | 形状 16×2 下划线、周期正常 |
| 语言三层 | 交互生效 · 落盘 CRC 一致 · 掉电重启保持（flash_and_verify） |

视觉确认说明：抓屏工具链已备好（`tools/frame_grab.py` / 现成的 `pwr_shot.py`），
但设置页可见文本全是中文 + 一行不走 Default 字体的标题（见下节"踩的坑"），
屏上没有可直接 OCR 的 Default 渲染字符。W 已修复的判据因此落在**数据链**上：
板上字模 = 仓库文件 = 宿主机按 cell_w=9 重算（95/95 逐字节一致、0/95 被裁），
A2 逐像素保证面板内容 == YMGUI 渲染结果 —— 渲染忠实于字模，字模已是完整 W。

### CI 闸门

固件字节变了，按 ci/README.md 的第 a 类规则（有意为之的功能改动）更新
`ci/firmware_reference.sha256`：

```
371532c63523d1d18f61c3fe6ae64ddf0b8892baff10a97d304c25e402c1b390  ymgui-h743.bin
1d15a6d60fea5fcf9a395344cfbe2db3ba4aefe8025ffb5563fb7c47c4841e73  ymgui-h743.elf
```

`check_firmware_hash.py` 本地复跑 **2/2 PASS**。

### 过程中踩的坑（抓屏验证 W 时）

- **画面静止时镜像里是老帧**：`g_frame_mirror=1` 只在 `phone_flush` 被调时生效，
  画面静止时 `YMGUI_Refresh` 因脏区为空直接 return ⇒ 抓到全 0 帧（153 600 像素同色）。
  必须先写 `g_force_redraw=1` 再等 2 拍 —— `pwr_shot.py` 的 `snap()` 已经封装好了，
  **别自己重写抓屏**（我重写了一遍 `frame_grab.py` 踩完坑才发现 `pwr_shot.py` 现成）。
- `probe-rs read` 的大块读取走 `-o 文件 -f binary`（45 s 读完整帧），
  格式名是 `binary` 不是 `bin`；stdout 解析 76 800 个 token 又慢又脆。
- 设置页顶部那行英文标题**不是** `YMGUI_Font_Default` 渲染的（用 9px/8px 字模
  做模板匹配 OCR 残差都 >16，识别全是乱码）——它走了别的绘制路径。想靠
  "屏上 OCR"验证字体时，先确认目标文字真的走 Default 字体（软键盘按键、
  输入法候选数字前缀是；这个标题不是）。

---

## 6. 复现

```bash
python tools/font_clip_check.py                              # 自动找 TTF
python tools/font_clip_check.py --ttf /path/DejaVuSansMono.ttf
python tools/font_clip_check.py --emit-fixed build/YMGUI_FontData_fixed.c
```

输出含：
- **A1** 95/95 复算比对（证明仓库数据确实出自这套参数）；
- **A2** 被裁字符清单 + 丢失列的覆盖度；
- **A3** W 的"完整版 vs 出货版"三图对比；
- **A4**（`--emit-fixed` 时）按 `cell_w=9` 重新生成一份字模到指定路径，**不覆盖仓库文件**。

字体来源：脚本依次找 `--ttf` → `third_party/YMGUI/tools/` → `build/_fonttmp/`；
都没有时才 `pip download matplotlib` 并从其 wheel 里取
`matplotlib/mpl-data/fonts/ttf/DejaVuSansMono.ttf`（dejavu-fonts 仓库里只有 .sfd
源码、没有 ttf，直接去 GitHub 下会 404）。
