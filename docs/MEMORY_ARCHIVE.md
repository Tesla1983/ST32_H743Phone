# 记忆归档：细节条目（2026-10-04 从 MEMORY.md 迁出）

MEMORY.md 因超出注入长度限制被压缩，本文件保留被移出的**细节条目**。
只有 MEMORY.md 里指向的是"纪律"（改代码前必须知道的事）；这里是"展开"（具体数值、推导、历史教训）。
更早的细节散在 `E:/ymgui-h743/docs/` 的各验收报告里。

---

## 一、性能优化全史（P0 / P1 逐项，含被证伪的思路）

### 帧基准演进

| 阶段 | 满负载每帧 | FPS |
| --- | --- | --- |
| 初始 | 62.22 ms | 16.1 |
| P0（三项） | 34.84 ms | 28.7 |
| P1（四项） | 21.64 ms | 46.2 |
| P1-3（默认关，可选） | 16.66 ms | ≈60 |
| **2026-10-04 引擎修复 + 步骤 2b/2a/3** | **17.78 ms** | **56.3** |

2026-10-04 那一行的构成（桌面场景，逐步实测，详见根 README 同名小节）：
5/5 + 镜像开 + 软件叠黑 26.80 ms → 镜像关 24.26（**步骤 2b** −2.54）→
FMC 5/5→2/4 21.42（**步骤 3** −2.84）→ 调暗改硬件背光 PWM **17.78**（**步骤 2a** −3.65）。
⚠ **帧基准依赖当前场景**：同一份固件在"笔记"页是 23.17 ms / 43.2 FPS，跨场景比数字没意义。

累计 **3.50×**。剩余构成（17.78 ms）：flush ≈4.7（2/4 档实测）· wallpaper（α + 渐变） ·
毛玻璃 1.9（α）· 文字 2.6。
**再往下只剩 P1-3（−5 ms，视觉决策）与遮挡剔除/脏区「old+new」API（见根 README 的"本轮未做"）。**

### P0（41.45 → 34.84 ms）

- **ribbon 合并** — 等价性用 `tools/check_ribbon_equiv.py` 做软件 A/B 验证。
- **镜像开关 `g_frame_mirror`** — ⚠ **跑 A2（逐像素自检）前必须写回 1 并刷一整帧**，否则 gram/ramp 会失配。
- **per-object 计时 `tools/objtime_scan.py`** — 用 `nm` 解析符号，⚠ Thumb bit0 要掩掉。

### P1（34.84 → 21.64 ms）

| 编号 | 文件 | 手法 | 效果 |
| --- | --- | --- | --- |
| P1-1 | `YMGUI_DrawFill.c` | opa 分支外提 + 指针递增 + 对齐时 32 位双像素写（`may_alias` 类型防严格别名） | 不透明 11.07 → **1.04** cy/px（10.6×） |
| P1-2 | `YMGUI_DrawImg.c` | 缩放 blit 源列号定点累加 + 求交提前 + 指针直写 | 58.8 → **11.7** |
| P1-4 | `YMGUI_DrawText.c` | blitGlyph 索引写 → 指针递增 | 仅 7%，文字深挖不值得 |
| P1-5 | `phone_shell.c` | 壁纸渐变行 4 路展开（抖动周期 4，头尾对齐 + 主体 4px） | 8.02 → **1.29** |
| P1-3 | `phone_shell.c` | 三层 ribbon 改不透明（**默认关**） | 21.64 → **16.66 ms**（α 混合 129893 px 吃 ≈5 ms） |

⚠ **P1-3 改变外观，需用户目视确认**（对比图 `build/p1_ribbon_{semi,opaque}.png`）。
⚠ `may_alias` 类型是为**防严格别名**（写 32 位读 16 位会 UB）。

### 图元单价实测表（P1 后 @400 MHz，cycles/px）

不透明填充 **1.04** · 半透明 17.34 · 缩放 blit 11.74 · 文字 17.26 · **裸 16 位 store 地板 0.88**（32 位同价，M7 写缓冲会合并）。

⚠ **"纯指针直写地板 11–12 cy/px"是 P0 轮的错误结论，已被 case6/7 证伪** —— 那是"写穿(FORCEWT) + 变址寻址"叠出来的。
⚠ **in-context 成本 ≈2× 微基准**（band buffer 20 KB > 16 KB D-cache），外推收益要打对折。

### 四个"看着该优化、实测没用"的先例（无收益即回退）

1. **XIP 区改 `CACHEABLE|BUFFERABLE`** + `qspi_enter_mmap()` 里 `SCB_CleanInvalidateDCache()`
   ⇒ 帧基准 64.80 → 64.69 ms，**纹丝不动**。结论：字模读取不在渲染开销构成里。已回退，记录在 `mpu.c` 注释。
   （起因：以为"每画一个中文字都要走一遍 QSPI 取字模"是瓶颈。）
2. **圆角每行 Fill 合并** ⇒ ≈0%。
3. **`GY_MixPx` 三次 `/255`** ⇒ 5.7%，gcc 早就自动做了强度削减。
4. **关 FORCEWT 让 AXI SRAM 走写回**（case8/9）⇒ 1.0–1.1×，写缓冲本来就把 store 合并了。
   ⚠ 因此**删掉了 `g_cache_wb`/`g_cache_clean` 两个运行期开关**——留开关只增风险（写回模式让 SWD 读到陈旧值）。

### ★FMC 时序结构体没清零 ⇒ 写时序跨启动不确定（2026-10-04 挖出并修复）★

**这是本工程至今最"会伪装"的一个 bug**：它不报错、不影响正确性，只是让 flush **偶尔**慢 3.3×。

- **现象**：同一份固件、同一份配置，某次启动整帧 17.78 ms，另一次 28.92 ms；
  `flush_cb` 0.312 → 1.023 ms/band（≈100 ns/px）。而 A2 双自检**始终 0/0** ——
  写进面板的内容完全正确，**只是慢**。所以它不会被任何自检抓到。
- **定位**：直接读 FMC 外设寄存器（`probe-rs read b32 0x52004000`）：
  `BWTR1 = 0x3FFF0402` ⇒ ADDSET=2 / DATAST=4（这两项对）但
  `BUSTURN=15 CLKDIV=15 DATLAT=15`（高位全是垃圾）。
  (2+4+1)=7 周期 + BUSTURN 的 15 ≈ 22 周期 ≈ 100 ns ⇒ 与实测吻合。
- **根因**：`FMC_NORSRAM_TimingTypeDef` 有 **7** 个字段，而
  `FMC_NORSRAM_Timing_Init`/`FMC_NORSRAM_Extended_Timing_Init` 会把**全部 7 个**
  写进 `BTRx`/`BWTRx`。厂商 `lcd_init` 只赋 4 个（ADDSET/ADDHLD/DATAST/AccessMode），
  剩下 3 个（`BusTurnAroundDuration`/`CLKDivision`/`DataLatency`）是**未初始化栈垃圾**。
- **为什么之前一直没被发现**：那几次量到的"六档与理论逐档吻合"只是因为栈垃圾恰好为 0。
  ⇒ **"与理论吻合"本身不能证伪这个 bug**，必须看寄存器。
- **修法**：`lcd_fmc_write_timing_apply()` 与 `lcd_init()` 的
  `fmc_read_handle`/`fmc_write_handle` 全部 `= {0}` 并显式写 0 那三个字段。
- **验收**：`BWTR1 = 0x0FF00402`(BUSTURN=0)；纯整屏推送 4.89 ms = **31.81 ns/px = 7.0 FMC 周期**
  （与 `(2+4+1)/220MHz` 精确吻合）；**连续两次独立重启基准都是 17.78 ms / 56.3 FPS**（差 303 cyc / 7.11 M）。
- ⚠ **教训**：凡是把结构体交给 HAL 的 `*_Init()`，**必须整体清零**再赋字段 ——
  只赋几个字段在"厂商原码就这么写"时极容易被照抄下去。
  本工程同类风险点值得再扫一遍（所有 `FMC_NORSRAM_TimingTypeDef` / `DMA_InitTypeDef` 等）。

### FMC 内核时钟 220 MHz 的定性（2026-10-04 查证）

**起因**：`fmc_ker_ck` 跑在 220 MHz，而早期记录里留过一句"上限可能是 200 MHz"（当时标注**未核实**）。
查了厂商资料包里的官方数据手册（`E:\BaiduNetdiskDownload\...\5. 芯片手册\DS12110_STM32H743xI芯片数据手册.pdf`，
本地无 PDF 库，直接解压 PDF 的 FlateDecode 流取原文），结论是**那句话要分两半**：
200 MHz 是个真数字，但它属于**同步模式**，**不约束本工程**。

**数据手册原文**：

| 原文 | 含义 |
| --- | --- |
| "The maximum **FMC_CLK/FMC_SDCLK** frequency for **synchronous accesses** is the **FMC kernel clock divided by 2**." | 同步模式下 FMC_CLK = 内核时钟 / 2（最小分频） |
| FMC_CLK = **100 MHz** @20 pF（2.7–3.6 V）／100 MHz @15 pF（1.62–1.8 V）；FMC_SDCLK 同为 100 MHz | 同步输出时钟的上限 |
| 修订历史："Changed FMC NOR/NAND **maximum clock frequency to 100 MHz** in **Features and Synchronous waveforms and timings**." | ST 把这条明确归在"同步波形与时序"名下 |

联立 ⇒ `fmc_ker_ck ≤ 2 × 100 MHz = 200 MHz`。**但这是同步模式下的推论**，不是一句独立的"FMC 内核时钟上限"。

**为什么它不约束本工程**：ST7796 走的是 **8080 异步 SRAM 模式 A**
（`MemoryType = FMC_MEMORY_TYPE_SRAM`、`BurstAccessMode = DISABLE`）。
异步模式**不产生 FMC_CLK 输出** —— 板级连线也印证：`lcd.h` 的 FMC 控制线只有 WR/RD/CS/RS，
**没有 CLK 脚**。异步的交流特性是**直接用 ns 给的**（tWRL、tDS…），
本工程 `DATAST=4` = 22.7 ns ≥ 面板 tWRL 19 ns，**余量约 19%**。

而且数据手册里**查不到**异步模式的内核时钟上限：`TKERCK` 只作为"时序表单位"出现在同步/SDRAM 那几张表里
（原文 "In all timing tables, the TKERCK is the fmc_ker_ck clock period."），
异步 AC 表里没有 `TKERCK min` 这一类的行。

**⇒ 定性**：220 MHz 落在数据手册**"未规定"**的区域，**不是"超规格"**。
实证支撑：A2 双自检 0/0、六档时序扫描全部通过、跨启动确定性（见上一节）。

⚠ **不要为了"对齐 200 MHz"而降频**：降到 200 MHz 会让一次写从 31.8 ns 变 35.0 ns，
整屏 flush **4.89 → 5.38 ms**，净亏 0.49 ms/帧。
这一节写在这里，就是为了防止后来人把这个数字当成隐患去"修"。

### 横向对比（8080 并口 vs RP2350）

两者总线层同一量级、都够不着更高：H743 的 **fmc_ker_ck 实为 220 MHz**（早先记的 200 MHz 其实是**同步模式**的上限推导值，
不是本工程异步模式的约束 —— 见上面「FMC 内核时钟 220 MHz 的定性」），面板规格只约束 tWRL ≥ 19 ns，
故写时序可压到 2/4 = 31.8 ns/px（甚至 1/4 = 27.3）；
RP2350 没有 FMC 只能 PIO 推 IO，@150 MHz 下要满足 `tWRL ≥ 19 ns`（WR 低电平 ≥3 个PIO 周期 = 20 ns），
每像素必然 40–67 ns ⇒ 与 H743 已压到 31.8 ns 的现状不再重合。
**真正差距在别处**：H743 有 DMA2D/MDMA + 1 MB 多块 SRAM + 400 MHz M7；RP2350 只有 520 KB 单一 SRAM —— 本方案整帧缓冲 300 KB + heap1 212 KB + IME 池 60 KB = **642 KB > 520 KB，放不下**。

### 测量手段（做渲染优化直接用）

1. `tools/bench.py` — 满负载帧基准（`--mirror`/`--objtime` 控制诊断开关）。
2. `tools/micro.py` — 图元微基准 case1–11 批量 runner（1/2 填充、3 blit、4/5 文字圆弧、6/7 裸 store、8/9 FORCEWT 实验、10/11 渐变 A/B）。
3. `tools/objtime_scan.py` — per-object 耗时排行（先跑 bench 再读）。
4. `tools/render_scan.py` — band 行数扫描解 `T = n_bands×A + P×b`（`disp->buf_px_cnt` 是运行时字段，`g_band_rows` 运行时覆盖，不用重编译）。

---

## 二、cache / DMA 实测明细

### `tools/dma_check.py`（DMA1 MEM2MEM 双向比对，缓冲在 SRAM1）

| 用例 | 失配 | 结论 |
| --- | --- | --- |
| CPU 写 → DMA 读 | 0 / 64 | 方向①安全 |
| DMA 写 → CPU 读（不维护） | **64 / 64** | **风险真实存在，且完全静默** |
| DMA 写 → CPU 读（invalidate） | 0 / 64 | invalidate 能补救，但靠"调用方记得做" |

⇒ **决策：接 SDMMC/IDMA 前必须划"专用非缓存 DMA 区"（L3），不能靠手工 invalidate。**

### `HAL_DMA_Init` 的两个已验证细节

- 在 `Direction == DMA_MEMORY_TO_MEMORY` 时自动把 `Request` 强制成 `DMA_REQUEST_MEM2MEM`（`stm32h7xx_hal_dma.c:418`），调用方只需填 `Instance` + `Init.*`；`StreamBaseAddress`/`StreamIndex`/`DMAmuxChannel` 都由 HAL 自己填。
- 用 `HAL_DMA_PollForTransfer` + 有限超时是**安全**的：`stm32h7xx_it.c` 的 SysTick_Handler 调了 `HAL_IncTick()`，`HAL_GetTick()` 是活的 ⇒ 不会退化成无限循环。

### L3 方案要点（8 个 MPU region 已全满，需回收无器件的 region6=SDRAM / region7=NAND）

region0←SRAM1(128KB,NOT_CACHEABLE) · region3←SRAM2 · region4←SRAM3 · region6←QSPI（从原 region0 挪来，保持 RO+NOT_CACHEABLE）· region7←SRAM4。

---

## 三、IME（中文输入法）实现细节

- 静态池 59.8 KB 放 `.bss_ime`（SRAM2）。
- **heap1 峰值 97% ⇒ 词组词典/英文词典必须保持关闭**，代价是中文候选排序只能按单字频率（`zhongguo` 首位不是"中国"）—— 已知取舍。
- ⚠ **`PhoneIME_Show()` 的避让段有坐标系混用 bug**（未修）：
  ```c
  YMGUI_Obj_GetAbsArea(active->obj, &abs);            /* 绝对坐标 */
  if (abs.y + abs.h > KEYBOARD_Y - 6) {
      if (abs.y > KEYBOARD_Y - 40) {
          active->obj->area.y -= abs.y - (KEYBOARD_Y - 44);   /* ← 局部坐标 -= 绝对差值 */
  ```
  定长页面里两者恰好一致、凑巧能用；父对象是滚动容器（`scroll_y ≠ 0`）时会把控件挪出视口。
  **已实施对冲**：设置页 `scroll_apply_ime()` 收缩视口 + `PhoneIME_RegisterSelfAvoiding()` 注册制跳过避让。
  **但视口顶 y=80..88 仍有 79~82 个灰字像素溢出，未修完**（用户已叫停）。
- ⚠ **不能用纯结构判据判定"是否滚动容器"**：曾想按"祖先有 `GY_STATE_ClipChildren` 且 `scroll_y >= 0`"，但本工程有 6 处开 `ClipChildren`（`app_page`/`app_views[i]`/`recent_page`/`recent_viewport`/`viewport`/设置页滚动视口），大部分是普通裁剪容器、`scroll_y` 恒 0 ⇒ 会误伤定长页面（回归）。
  ⇒ 改成**注册制**：`PhoneIME_RegisterSelfAvoiding(root)` 显式登记，未注册者行为与改动前完全一致（安全侧）。
- `scroll_apply_ime` 的实现教训：曾用 `fo->area` 沿 parent 链累加定位焦点控件，但它由 `app_tick` 调用、**跑在 `PhoneIME_Update` 之后**（`phone_shell.c:648` 先 `IME_Update`、`659` 才 `PhoneApps_Tick`）⇒ 读到的 `area` 已被 IME 避让改坏。
  ⇒ 改用布局期就记下的、与 IME 无关的原始几何 `s_scroll.input_bottom`（`app_create` 里 `y + h` 算好），并用指针比对代替"找祖先链"。修好后 `scroll_y` 135 → 307 = `407+28-128` 精确匹配。

---

## 四、设置页布局史与踩过的坑

### 卡片与坐标

- 用户 2026-10-04 明确**否决压缩卡片高度**，要求滚动版布局。
- 七项（后加半透明成八项）：65 无线网络 / 133 海蓝色壁纸 / 201 界面语言 / 269 半透明 / 329 预览亮度 / 407 设备名输入框 / 451 关机按钮。
- ⚠ 改设置页布局**必须同步** `phone_selftest.inc` 里的坐标断言（`device_name_input->area.y` 已从 293 改到 347、后续又到 407）。
- ⚠ **`phone_shell.c` 的注释用 ASCII 双引号**（全角引号数为 0）⇒ **绝不能用 `replace_all` 盲替换字符串字面量**（会误伤注释），必须带上下文逐处改。

### 输入框与关机按钮只隔 16 px（用户明确抱怨过）

设备名输入框内容坐标 `y=407, h=28`（滚到底 abs y=364..392），关机按钮 `y=451, h=40`（abs y=408..448）—— **不重叠但只隔 16 px**。
用户原话："预览亮度卡片下面的Yaomi phone是输入框，你每次都点错关机，然后弹起来输入法。"
⇒ 脚本侧 `DRAG_XY=(6,380)` 避开输入框、关机按钮坐标一律读固件 `g_diag_power_btn_x/_y`。
⚠ **未做 UI 层面的命中区隔离或视觉强化** —— 用户报告的"误点关机"路径本身未被专门修复（用户随后叫停了额外排查）。

### 曾修掉的 8 个缺陷（半透明验收期间）

1. `ensure_ime_hidden` 误用 home 键导致退出设置页。
2. 拖动误触输入框弹键盘。
3. 设置页头部标题被滚动视口遮住（视口从 y=0 起）。
4. 引入固件每帧现算坐标诊断量并挂到每帧 tick。
5. 补齐关机→关机页→重新开机→桌面的完整闭环。
6. `wait_diag()` 顺序 bug（先读后进页 ⇒ 拿到动画中途坐标 ⇒ 误报"开关点不到"）。
7. `bdtap` 异步导致误判"滚不动"（`settle()`）。
8. 抓屏镜像滞后（`g_force_redraw`）。

### 我自己的两次误判（自我纠错的判据）

- **"静止时两次强制重绘 md5 相同"** 曾被我当失败证据 —— 其实那**是正确**的（画面真没变）。正确判据是"画面变了之后镜像能否跟上"。
- **`nm` 显示两份 `brightness_changed`** 曾被我当"文件里有重复定义" —— 实际那些是 `00000000` 的 **undefined 占位**，我的诊断方法本身有误。

---

## 五、亮度功能实现史（2026-10-04）

### 需求与数据结构

用户原话："把默认的预览亮度初始化调为50，然后如果用户自定义亮度后保持重启不改变亮度值"。

```c
typedef struct {
    uint32_t magic;              /* "YCON" 0x59434F4E */
    uint32_t version;            /* 1 */
    uint32_t lang;               /* 0=ZH 1=EN */
    uint32_t brightness;         /* ★本轮从"预留"变成"启用" */
    uint32_t theme;              /* 预留 */
    uint32_t ribbon_translucent; /* 0=不透明 1=半透明（占 reserved[0]） */
    uint32_t reserved[1];
    uint32_t crc;                /* 覆盖 crc 之前的全部字节 */
} AppConfigBlob;
```

⭐ `brightness` 字段**本来就存在于结构里** ⇒ **启用它不需要改结构布局、不升版本、不迁移已落盘数据**。

宏：`APP_CFG_BRIGHTNESS_DEFAULT 50u` / `MIN 1u` / `MAX 100u`。
函数：`brightness_valid()` · `AppConfig_SetBrightness/GetBrightness` · `AppConfig_FactoryReset()`。
诊断量：`g_cfg_brightness`、`g_cfg_factory_reset`、`g_cfg_factory_rc`。

`AppConfig_Load()` 的钳制放在采纳 flash 值之后、`s_flashed` 快照之前，并**重算 crc**（理由见 MEMORY.md）。

### 滑杆设置

```c
state.bright_slider = YMGUI_Creat_Slider_Creat(bright, 14, 32, 256, 24);
YMGUI_Slider_SetRange(state.bright_slider, 1, 100);
YMGUI_Slider_SetValue(state.bright_slider, PhoneHost_GetBrightness());
```

### 验收脚本 `tools/brightness_check.py`（新建）

三项：① 擦扇区+重启验默认 50 · ② 一次拖动只擦 1 次扇区（**必须分 5 段制造多个中间值**，否则测不出延迟落盘）· ③ flash CRC + 掉电重启保持。
⚠ 每段拖动都**重新读**滑杆几何（不缓存）—— 这样任何"页面被复位"的固件回归都会立刻暴露，而不是悄悄点空。

### 板级实测数据（修复后）

```
出厂默认：擦扇区+重启 → g_cfg_loaded=0 g_cfg_brightness=50
拖动：x0=32 x1=288 y=330，拖到 80% → brightness 50→82
      sld_press=5 sld_change=6 sld_commit=1（延迟落盘生效）
      save_cnt +1，erase_last=1（修复前是 +6）
      scroll_y 仍 = 135（页面没被复位）
flash：magic=0x59434F4E brightness=82 crc=0x09A9505D（算得一致）
重启：loaded=1 crc_ok=1 brightness=82
```

最终 flash（恢复出厂后）：`magic=0x59434F4E ver=1 lang=0 brightness=50 theme=0 ribbon_translucent=1 reserved=0 crc=0x30E8DDDA`。

### ★屏侧实现换成硬件背光 PWM（2026-10-04 晚，同一轮内）★

**动机**：软件叠黑罩实测仍值 **3.65 ms/帧**（21.42 → 17.77 ms）。背光本来就是 PB1 直连的
普通 GPIO（`lcd.c` 只做 `LCD_BL(1)` 常亮），全工程没有任何 TIM/PWM。

**为什么能成**：
- `PB1 = TIM3_CH4 / AF2` —— ST 官方 AF 表核实（MicroPython 的 `stm32h743_af.csv`：
  `PortB,PB1 , ,TIM1_CH3N ,TIM3_CH4 ,TIM8_CH3N`）。
- TIM3 在本工程**完全没被占用**；delay 走 SysTick。
- **不需要 HAL_TIM**：CMSIS 设备头（`stm32h743xx.h`）里 `TIM_CCMR2_OC4M_Pos`/`TIM_CCER_CC4E`/
  `TIM_CR1_CEN`/`TIM_EGR_UG` 等位定义齐全，直接写寄存器即可
  ⇒ 不引新 HAL 模块（省 30~40 KB flash）、**不用改 CMakeLists**（代码放在既有的 `src/ymgui_port.c`）。

**为什么必须先做"可逆实验"再改默认**：`docs/` 四份验收文档与 BSP 注释里**只有**
"PB1 = LCD_BL、置高点亮"，没有任何原理图/驱动方式（MOSFET？三极管？使能脚？）。
所以第一版：默认 `g_bl_pwm_mode=0` 完全不碰 PB1、运行期写 1 才接管、复位必回 GPIO 常亮。
上板 duty 1000↔0 交替，**用户目视确认背光随之亮/灭** ⇒ 判断为 PWM 调光输入，方案成立。

**板上客观证据**（都不依赖肉眼）：`PB1.MODER=2`(AF)、**`PB1.AFR=2`**(AF2=TIM3_CH4)、
`CCER=0x1000`(CC4E 使能)、实测 **2000 Hz**、`CCR4` 随 duty 精确变化且不重启定时器；
面板内容自检 ramp/gram 仍 **0/0**（背光与显示是两条独立通路）。

**切换顺序是个真坑（自己踩过）**：GPIO→PWM 必须"先在 PB1 还是 GPIO 时把定时器配好、
占空比设好、输出使能、计数启动，**最后一步**才切 PB1 的复用"；PWM→GPIO 必须
"**先**把引脚切回推挽输出（ODR 预置高）**再**停定时器"。反过来都会留下
"引脚已是 AF 但定时器还没/已经不在驱动"的空窗，背光闪一下。

**映射**（保持换机制后的观感）：不是 `duty = 亮度`，而是按原叠黑的等效亮度
`L(v)=1−opa/255`，`duty(v)=1000−(100−v)×160×1000/25500`（v=100→1000‰, 50→687‰, 1→379‰）。
好处是对比度更高（原来把黑也压成灰，现在黑仍是黑）。

**结果**：46.7 → **56.3 FPS**（17.78 ms），`g_dim_mode=2` 成为默认；
`g_dim_mode=1` 一个 SWD 写即可退回软件叠黑（无需重编译）。
⚠ `brightness_check.py` 只断言"默认 50 / 一次拖动擦 1 次 / 掉电保持"，**不做像素差分**，
所以换机制不影响它的判据（只改了它那条提示文案）。

### ★dim_buffer 改成按需分配（同一轮内，heap1 回收 30 KB）★

- 原来 `PhoneShell_BoardInit` 无条件预分配 `W×48×2 = 30720 B` 给软件叠黑；
  默认走硬件 PWM 时**根本用不到**，等于白占 heap1。
- 改成 `dim_buffer_ensure(need_px)`：只有真切到 `g_dim_mode=1` 时才申请，且按
  **实际 band 尺寸**（320×32 = 10240 px）而不是写死的 W×48。
- **顺带修掉潜在越界**：原容量 W×48 与"板级 band=32 行"只是巧合吻合，
  `main.c` 的 `g_band_rows` 钩子把 band 调到 >48 行时 `dim_apply` 会越界写。
- **不静默降级**：新增 `g_dim_buf_rc`（1 = 申请失败 ⇒ 软件回退不可用），否则表现为
  "把 g_dim_mode 写回 1 却完全不调暗"。
- 板上实测（读 `g_h1` 与 `dim_buffer` 指针）：开机默认 `dim_buffer=NULL / cap_px=0`、
  heap1 **180376 B (176.1 KB)**；切 mode=1 并强制重绘后 `dim_buffer=0x240770A8 / cap_px=10240`、
  heap1 200872 B ⇒ **增量 20496 B**（期望 20480+16 分配头），`g_dim_buf_rc=0`。
- ⇒ 默认配置 heap1 轻 **30.0 KB**；走回退时也从 30.0 KB 降到 20.0 KB。
  当前峰值 **176.1/212.0 KB = 83%**（改造前 README 记 205.2 KB / 97%）。
- ⚠ **测这个时的坑**：画面静止时 `YMGUI_Refresh` 因脏区为空**直接早返回**，
  `phone_flush` 根本不被调用 ⇒ 按需分配不会被触发，量出来"增量 0"。
  必须写 `g_force_redraw=1` 造一次真实 flush 再读。
⚠ `dim_buffer`（30 KB）现在只在 mode=1 下用得上；模式运行期可切，故**未回收**。


---

## 六、XIP / QSPI 坑

- **QSPI 引脚 IO2=PE2、IO3=PD13** —— 厂商资料包写反过，已更正。
- `font_provision.c:167` 是与 `app_config.c` 同样的"写完必须 `qspi_enter_mmap()`"处理，可作参照。
- ★★ **在 indirect 模式下读 XIP 窗口 = 精确 BusFault，升级 HardFault**（2026-10-08，
  图库 BMP 灌库时实测）。`qspi_write` 内部是"退映射 → indirect 写 → **不回映射**"
  （只有 `qspi_erase_sector` 会回），所以写完立刻去读 `0x9000_0000` 就崩：
  `g_fault.kind = 1`、`CFSR = 0x8200`（PRECISERR + BFARVALID）、`BFAR = 0x9001_0000`，
  主循环卡死。
  ⚠ **单次读不挂是假阴性**：探针只读一次时看起来"没事"，而导入时连读上百次必然踩中
  —— 曾据此得出"退出映射后读 XIP 也没事"的错误结论，多绕了一轮。
  规矩：`qspi_write` 之后、**下一次 XIP 读之前**必须 `qspi_enter_mmap()`；周期性任务末尾再兜一次。
  落地位置：`src/img_store.c` 的写循环末尾 + `img_store_poll()` 末尾。
- 字库末尾 `0xEFC00` 之后、擦写回环 `0xFE000` 之前的空隙可放配置扇区（不撞 `QSPI_HOUSEKEEP_ADDR`）。
- **图库区 `0x100000` 起 2 MB**（索引在 `0x100000`、数据从 `0x101000` 起）—— 这张布局表
  早就预留了"图片/壁纸常量区"，且实测**没有任何代码在使用它**，图库直接用上了。

---

## 七、工具脚本补充说明

| 脚本 | 说明 |
| --- | --- |
| `factory_reset.py` | 调 `bench.wr("g_cfg_factory_reset",[1])` 触发固件擦扇区，轮询等归零、读 `g_cfg_factory_rc`、复读 flash magic。**必须走固件**：XIP `0x900F0000` 只读，`probe-rs write` 会报 `Failed to write register DRW at address 0xd0c` |
| `read_vars.py` | ⚠ **遇到同名 static 变量会取错地址**（本项目 `active` 有 3 个、`visible` 有 2 个），重名时先 `arm-none-eabi-nm -S` 列出来确认 |
| `grab_ymgui.py` | 抓 YMGUI 源码/资源 |
| `count_cn_strings.py` | 统计中文字符串（**剔注释**） |
| `list_cn_strings.py` | 按行列出（**不剔注释**，输出含注释内容需甄别） |
| `bench.py` 的 `rd`/`wr` | 真实接口是 `rd(name,n)`/`wr(name,vals)`/`symtab()` —— 没有 `read()`/`rd1()` |
| `coldboot_vos.py` / `swj_pins_probe.py` / `dap_reset_pulse.py` / `hid_probe.py` | 冷启动/引脚探测辅助 |

---

## 八、历史遗留（用户已叫停，别再动）

- **IME 视口顶溢出**（y=80..88 有 79~82 灰字像素）—— 下一步方向在 `YMGUI_Invalidate.c:250` 的 `GY_Rect_Intersect(&newclip, &abs, &s->clip)` 打点。
- 输入框与关机按钮的 UI 层面命中区隔离未做。
- 板级 `selftest()` / `select_preview()` 是 SDL 桌面版入口，板上无触发路径（编译期 unused warning）。
- TF 卡 L2/L3 未开工（见 `docs/TF_CARD_CACHE_ROADMAP.md`）。

---

## 九、第 6 项（旧∪新失效）+ 第 7a 项（DMA 双缓冲）实施记录（2026-10-04）

> 依据是"6/7a/7b/8 风险与可行性评估"里的排序。
> **本轮只做 6 与 7a；7b（MDMA+中断）与第 8 项（遮挡剔除/子树剪枝）按评估结论不做（见 9.3）。**

### 9.1 第 6 项：把"拖动整屏重绘"降到局部失效

引擎侧新增唯一一个公开 API（其余全是 port/app 改动）：

```c
void YMGUI_Obj_SetPos(GYOBJ obj, GYcoord x, GYcoord y);   /* 声明在 YMGUI_Obj.h */
```

- **它做的事**：改 `obj->area` 并**同时标脏旧位置与新位置**。只改 area 再 `YMGUI_Obj_Invalidate`
  只脏新位置 —— 旧位置露出的下层内容永远不重绘，表现是拖动残影。
  这个"取旧矩形 → 改 → 脏旧 + 脏新"的两步法**引擎内部早就有**（`YMGUI_Anim.c` 的 `applyNode`
  对 MOVE/RESIZE 都这么做，`docs/API.md` 也写了"位置与尺寸动画会同时标脏旧区域和新区域"），
  只是没做成公开 API ⇒ app 只能靠脏整屏兜底。
- **顺序约束与 `YMGUI_Obj_SetHidden` 同类**：旧矩形必须在改 area **之前**取
  （`GetAbsArea` 算的是"当前"位置）。函数内还挡了两件事：位置没变直接返回（拖动回路里每帧都调，
  省掉大量空转）；Hidden 对象移动不产生脏区。
- **app 侧只改了一个函数**：`phone_ui.c` 的 `PhoneUI_set_pos` 改成委托 —— 它全工程有 **30 个
  调用点**（图标拖动 `phone_shell.c:1163`、桌面滑动 `:1372`、ghost 跟随、shade 滑入…），
  改一个函数 30 处全受益，调用点一处没动。

`brightness_apply`（`phone_shell.c`）的整屏标脏**按模式条件化**：

```c
if (g_dim_mode == 1u) { YMGUI_Obj_Invalidate(ctx->root); }   /* 只有软件调暗才需要整屏 */
```

| 模式 | 为什么 |
| --- | --- |
| `g_dim_mode==1`（软件叠黑） | 光罩是画进 framebuffer 的，`phone_flush` 按 band 重新叠黑 ⇒ 15 条 band 必须全刷 |
| `g_dim_mode==2`（硬件 PWM，出厂默认） | 改的是背光占空比，**framebuffer 一个像素都没变** |

后者的依据是**全工程搜引用**：`display_brightness` 唯一影响像素的读取点是 `phone_shell.c:974`
（`phone_flush` 里，紧跟 `g_dim_mode==1` 判断）；`PhoneHost_GetBrightness()` 只被用来同步滑杆数值；
界面上也没有显示亮度数值的文本（只有静态标题）。两个滑杆的拖柄由 `YMGUI_Slider` 自己标脏
（`YMGUI_Slider.c` L107/111/148/163/175）。

#### ★意外挖出并修掉的真问题：`scroll_moved()` 在位置没变时也整视口标脏★

第一次量"每次拖动刷多少像素"，得到**恒定 113920 px**（= 11 条整宽 band + 一条 320×4，
正好是整个滚动视口 320×356），而 `scroll_y` 从头到尾是 135 没变过。

根因在 `apps/settings.c`：滚动手势的 `GY_EVENT_Pressing` 分支是**无条件**赋值 `scroll_y` +
调 `scroll_moved()` 的（`SLOP` 只用来置 `s_scroll.moved` 标志，**没拦住那两行**），
而那个处理器挂在**整个滚动视口**上、是页内所有控件的祖先 ⇒ 设置页里任何拖动
（**包括亮度滑杆**）冒泡上来都会把整个视口标脏。
修法是在 `scroll_moved()` 里加一道"`scroll_y` 与滚动条几何都没变就直接返回"。

#### 第 6 项实测（`g_flush_pixels` 累积量，不开镜像也能量）

| | 每次亮度拖动 flush 的像素 | flush 次数 |
| --- | ---: | ---: |
| 修复前 | 113 920 px（≈0.74 整屏） | 12 |
| **修复后** | **6 144 px**（= 256×24 = 滑杆自身面积） | **1** |

⇒ 降 **18.5 倍**（相对整屏 153 600 px 是 **25 倍**），且**每次拖动只产生 1 条 band**，正是预期形态。

**正确性验证**（比"快了"更重要）：
- **滚动功能没被改坏**：真实竖直滚动 `scroll_y` 135→15→135，整视口照常重绘
  （这是**必要**的，内容整体位移）；横向无位移拖动现在是 **0 像素**；
- 滚动中途抓屏（`build/after_item6_scrolled.png`）目视**无残影/无重复行/无错位**；
- A2 双自检 0/0、`lang_check` 三层全过。

### 9.2 第 7a 项：DMA 双缓冲（flush 与渲染重叠）

> ⚠ **本节有两处结论在后续复测中被推翻**（"reenable=0 即安全"的推理、以及性能数字），
> 已在文末 **§9.5 更正**里写明；读本节时请一并读 §9.5。

**硬件前提**：`s_band2` 放 **D2 域 SRAM3**（链接脚本新增 `.bss_sram3`，20 KB / 32 KB）。
选它是因为：与 DMA1 同域不必跨域；**不占 heap1**（刚回收的 30 KB 不该被吃回去）；
MPU region3 把它配成 cacheable，而 `tools/dma_check.py` 已在同区实测"CPU 写 → DMA 读"
**0/64 失配**（FORCEWT=1）—— 有实证不是推断。

⚠ 链接脚本那两条位置陷阱照做：自定义段**必须放在 `.bss` 之前**（否则被 `*(.bss*)` 通吃、
链接器不报错也不生效）；本段 NOLOAD，且**不需要**在 `board_startup.c` 里清零
（每条 band 交给 DMA 前都会被 `drawObjRec` 整块渲染，不存在未初始化读）。

**驱动方式**：DMA1_Stream1（**刻意避开 `dma_bench` 占用的 Stream0**）、MEM2MEM 软件触发
（FMC 没有 DMA 请求线）。其中一个反直觉但关键的细节 —— **MEM2MEM 下 HAL 把 `SrcAddress`
放进 `PAR`、`DstAddress` 放进 `M0AR`**（`DMA_SetConfig`），所以"源递增、目的固定到 LCD"
要配 `PeriphInc=ENABLE`（管 PAR）+ `MemInc=DISABLE`（管 M0AR）；照字面义配反就全错。

#### ★途中撞到两个真 bug★

**① 本版 HAL 的 `HAL_DMA_Start` 是一次性的（成功分支没有 `__HAL_UNLOCK`）**

`stm32h7xx_hal_dma.c` L638-657：开头 `__HAL_LOCK(hdma)`，成功分支只置 `State=BUSY` + 使能流
就 `return`，**没有解锁**。而 `__HAL_LOCK` 宏在已锁时直接 `return HAL_BUSY`。
⇒ 第一笔成功后 `Lock` 永远卡在 `HAL_LOCKED`，之后每次调用都返回 **HAL_BUSY(2)**。
板级实测完全吻合：`g_lcd_dma_start_rc = 2`、`xfers` 只有 1、其余 2683 笔全 fallback 回 CPU
直推 —— **而且帧时间反而从 17.78 涨到 20.39 ms**。
⇒ 改为**只借 `HAL_DMA_Init` 配一次 CR，之后每笔自己写 PAR/M0AR/NDTR + 置 EN、自己轮询 TC 标志**，
绕开它的隐式状态机（少一层隐式状态，行为完全确定）。

**② app 的 `phone_flush` 包装层破坏了库的异步契约**

`phone_shell.c:2020-2021`：
```c
display_flush = disp->flush_cb;    /* 存下 port 的 lcd_flush_cb */
disp->flush_cb = phone_flush;      /* 换成 app 的包装（软件调暗/缩略快照层）*/
```
于是引擎调的 `disp->flush_cb` 其实是 app 的 `phone_flush`，port 的 `lcd_flush_cb` 被它转发调用。
**链路里"谁在什么时候等 DMA"就不再像库文档假设的那样确定**：实测
`g_lcd_dma_wait_enter = 0`（库的 `wait_cb` **从未被调到**），而 `g_lcd_dma_reenable` 占到
总传输数的**一半** —— 即**起新传输时上一笔还在跑**，下一块 buffer 已经把正在 DMA 的那块覆盖掉，
重负载下画面会花（A2 在空闲态测不出来，所以它当时仍是 0/0，**这正是这类 bug 的典型伪装**）。

⇒ 修法：**等待与交还都由 port 自己做，不依赖库的 `wait_cb`** ——
`flush_cb` 里先把上一笔 `lcd_dma_drain()` 等完（绝不覆盖在传的 buffer），再 kick 本笔，
然后立刻 `FlushReady`。
**重叠为什么还在**：CPU 渲染下一条 band 发生在 **`flush_cb` 之前**（引擎先 `drawObjRec` 填满整块
再调 flush），所以"渲染 N+1"与"DMA 推 N"天然重叠；port 只在 DMA 比渲染慢时才补等差额。
`lcd_dma_wait_cb` 保留为**安全网**（万一库真进了 `while(flush_busy)` 循环也不会假死）。

#### 第 7a 项实测

| 指标 | 中间版本（有覆盖风险） | **最终** |
| --- | ---: | ---: |
| `g_lcd_dma_reenable`（起新传输时上一笔还在跑的次数） | 1490 / 2815（**一半！**） | **0** |
| `g_lcd_dma_timeout` | 0 | 0 |
| `g_lcd_dma_xfers` vs `g_flush_cb_calls` | 2815 = 2815 | 2815 = 2815（**每笔都走 DMA**） |
| **满负载整帧** | 15.50 ms（快但会覆盖） | **15.98 ms / 62.6 FPS** |

- 相对第 7a 之前：**17.78 → 15.98 ms（−1.80 ms，−10.1%）**；
  两次连跑差 **331 cyc / 6.39 M**（几乎确定性）；
- `flush_cb 单次均值` 从 0.343 ms 掉到 **0.09~0.11 ms**（起 DMA 几乎免费）；
- ⚠ 该指标**含义变了**：等面板的时间现在记在 `g_flush_wait_cyc`/`g_flush_wait_calls`，
  但本设计下等待在 `flush_cb` 内部、与渲染重叠 ⇒ **报数时不要把两者相加**；
- 完整验收：**A2 ramp=0 / gram=0**（DMA 推送下面板与渲染逐像素一致）、`g_fault=0`、
  IME 2/2、caret 形状+周期 ✓、`lang_check` 三层全过。

### 9.3 决定：**7b 与第 8 项不做**

| 项 | 决定 | 理由 |
| --- | --- | --- |
| **7b** MDMA + 中断 | **不做** | 要改 `startup_gcc.s` 向量表 —— 当前 150 个 IRQ 槽位全是 `.word Default_Handler` 的**硬引用**，文件顶部"都是 weak 符号、在 C 里定义同名函数即可覆盖"那句注释是**错的**（定义了不会被调用，还会被 `--gc-sections` 丢掉）；再引入 `stm32h7xx_hal_mdma.c`（62.9 KB 源，CMake 变更触发全量重建）。而相对 7a 的增量只是"发起即返回 + 完成通知"，7a 已做到 |
| **8** 遮挡剔除 + 子树剪枝 + abs 缓存 | **不做** | 收益有实测上限（见下），但**正确性风险最高**：`GY_STATE_Opaque` 要逐个 draw_cb 审"是否真的填满自身矩形且不透明"，判错一个就是内容消失；子树剪枝/abs 缓存要在所有改 `area`/`scroll` 的地方接失效（引擎里至少 5 处）。工作量大、且收益被帧节拍吸收 |

第 8 项的**实测天花板**（用 `tools/render_scan.py` 扫 band 行数解 `T = n_bands×A + P×b`）：

```
每 band 固定开销 A = 48 222 cycles = 0.121 ms
15 band 的固定开销合计 = 1.81 ms = 一帧的 10%
对象树：60 个可见对象、深度 7 ⇒ 每对象每 band 摊 803.7 cycles
再加 root 那笔纯浪费（baseDrawCb 整屏不透明填充后被整页覆盖；micro.py case 1 = 1.05 cy/px ⇒ 0.40 ms）
⇒ 第 8 项天花板 ≈ 1.8~2.2 ms（10~12%）
```

**另一条框架性结论**（决定了 6/7/8 的价值在哪）：`FRAME_TARGET_MS = 16` 已把稳态收益封顶 ——
做完 7a 后满负载 **15.98 ms 刚好落进 16 ms 预算**，62.6 FPS。所以这些优化的价值体现在
**最坏帧**（笔记页曾实测 23.17 ms）与**交互响应**（拖动）上，而不是稳态 FPS 数字。

### 9.4 验收套件两个"假失败"的修复（都是本轮之前埋的，与本轮改动无关）

| 现象 | 真因 | 修法 |
| --- | --- | --- |
| `lang_check` 报"中/英两帧差异 0 个像素"、`ime_probe` 报"未通过" | ① 第 2 项把 `YMGUI_PORT_FRAME_MIRROR` 从出货构建摘掉（默认 0），而 `lang_check`/`ribbon_check`/`grab_ymgui` **读镜像却不自己开** ⇒ 读到的永远是开机旧值（抓屏 = 526 字节纯黑 PNG、像素差恒为 0）；② `flash_and_verify.run()` 只让父进程按 UTF-8 解码，子工具仍按 GBK 输出 ⇒ "成功 2/2" 变乱码、字符串判定恒假 | ① 自愈放进 `caret_check.frame()`（`lang_check`/`ribbon_check` 一次全好）+ `grab_ymgui` 自己开镜像并 `g_force_redraw` 造一帧；② `run()` 给子进程传 `PYTHONIOENCODING=utf-8`，并改 `errors="replace"`（宁可看到 � 也不悄悄吞字符） |

⚠ **教训**：把"诊断默认值"从出货构建里摘掉之后，**必须回头审计所有依赖它的验收脚本**。
本轮为此绕了一大段弯路：`lang_check` 的 0 像素差异看起来极像"置脏范围改坏了重绘"，
实际只是镜像没开 —— 差一点就为一个不存在的 bug 去改引擎。

---

## 十、§九 的更正：花屏根因 + 两个被推翻的结论（2026-10-04 同日复测）

用户上板看到**三页全花**，复查后本节把 §九 里两处错误结论更正掉。

### 10.1 花屏根因：窗口命令与在途 DMA 交错（真因，已修）

`lcd_flush_cb` 原来的顺序是：

```
lcd_set_window(0x2A/0x2B) → lcd_write_ram_prepare(0x2C) → [等上一笔 DMA] → 起本笔 DMA
                              ↑ 这三条命令也是经 FMC 发给面板的
```

上一笔像素**还在推**时，这些命令就插进了像素流 —— 面板把命令当像素数据收，整条 band 写到错误位置。
而且**不会自愈**：无脏区时 `YMGUI_Refresh` 直接早返回，花掉的那块一直留在屏上直到被覆盖。

- **修法**：把等待提到**任何 FMC 访问之前**（进 `flush_cb` 先 `lcd_dma_drain()`，再碰 FMC）。
- **证据**：新加诊断量 `g_lcd_dma_waited`（进 flush_cb 时有 DMA 在途的次数）= **3455 / 3456**
  —— 也就是说旧代码**每一次**发窗口命令时都有一笔在途，**花屏是必然而非偶发**。
- **修后**：在高频重绘（轻脏区、连续 8 次拖动）之后立刻回读面板，
  `g_gram_mismatch = 0`、`g_ramp_mismatch = 0`；`reenable = 0`、`timeout = 0`。

### 10.2 ★教训：截图看不出这类花屏，只有 `g_gram_mismatch` 算数★

| 自检手段 | 为什么漏掉传输层花屏 |
| --- | --- |
| A2 `gram` 回读 | 它是**空闲态**跑的：那时每笔 DMA 早就传完，命令与像素不可能交错 ⇒ 报 0/0 |
| 全帧镜像 / `grab_ymgui` 抓屏 | 镜像是在 `flush_cb` 里从**源 buffer** 拷的，反映"要画什么"、**压根不经过面板** ⇒ 截图永远正常 |

**修复前后桌面页截图的 SHA256 完全相同**（都是 `2c384ef8…`）——这不是"没重抓"，而是
镜像在原理上就照不到这个故障。**以后凡是"面板上看着不对、截图却正常"的现象，
一律先怀疑传输层，用 `g_gram_recheck` 判，不要靠截图。**

### 10.3 更正一：`reenable = 0` **不能**证明"不覆盖在传 buffer"

§9.2 里写过"`g_lcd_dma_reenable = 0` ⇒ 不会覆盖在传 buffer ⇒ 安全"。**这个推理是错的**：
`reenable` 只在 `lcd_dma_kick` 那一点检查流的 EN 位，而当时等待被放在**窗口命令之后**，
所以它照不到"FMC 访问发生在 DMA 在途期间"这件事 —— 实测它稳定为 0，而那时花屏正在发生。
**该看的量是 `g_lcd_dma_waited`（放在函数开头）**：它一出来就是 3455/3456。

### 10.4 更正二：性能数字 —— 之前"没有收益"是**跨场景比**出来的反结论

§9.2 的实测表把不同场景的数字并列了，结论"修对后与基线持平"因此是错的。
本板帧基准**随当前页面变化**（桌面 18.14 / 设置页 20.39 / 笔记页历史 23.17），
A/B 必须**同一页、背靠背**跑。同场景（桌面页）三次连跑：

| 通路 | 满负载整帧 | cycles |
| --- | ---: | ---: |
| CPU 直推 | 18.14 ms / 55.1 FPS | 7 257 405 |
| **DMA 双缓冲** | **14.71 ms / 68.0 FPS** | 5 883 591 |
| CPU 直推（复测） | 18.14 ms / 55.1 FPS | 7 257 378 |

两次 CPU 复测只差 **27 cyc / 7.26 M**（本板基准本身很干净）⇒ **不是噪声**。
**⇒ DMA 真实快 3.43 ms（−18.9%）**；设置页场景同样更快（20.39 → 17.75 ms）。

另外更正 §9.2 里"15.98 ms 是带病速度"的说法：**15.98 只是 `flash_and_verify` 之后的那个场景
的数字**（caret 验收会把界面停在笔记页附近），修序前后同场景都是 15.98 ms
—— **顺序修复本身零性能代价**，那个数字与花屏无关。

### 10.5 最终形态

- `g_lcd_dma_enable`：**0 = CPU 直推（备用）／1 = DMA 双缓冲（出厂默认）**，
  CMake 选项 `YMGUI_PORT_LCD_DMA` 默认 **1**；运行期 SWD 可写，不必重编译。
- `flush_cb` 开头那次 drain **不受该开关约束** ⇒ 运行期 1→0 切换也不会交错。
- 最终验收：A2 ramp=0 / **gram=0**、`g_fault=0`、IME 2/2、caret ✓、`lang_check` 三层 ✓；
  `g_lcd_dma_reenable=0`、`g_lcd_dma_timeout=0`。
- 用户明确不做：把 `s_band2` 从 SRAM3 挪到 AXI SRAM（复测收益，放弃）。

---

# 迁回的关键约定（2026-10-08 归位）

> 这一批条目原先寄存在另一个工程的记忆文件里，现已归位到本项目。
> 内容按原样保留，只去掉了对其他工程的引用。改对应代码前先看这里。

## 诊断量纪律（本项目踩坑最多）
**坐标/状态一律固件每帧现算；脚本只读诊断量、绝不按算式推。** 根因：`app_page` 建在 `y=480`，
靠 `open_app` 的 330 ms 动画归位，`on_recent()` 又把它推回下方 ⇒ "进页时算一次"的坐标会凭空多 480。
- 绝不在"状态变化处"增量同步（`-O2` 内联后写入被合并 ⇒ 画面与诊断量长期不一致）。
- `Settings_UpdateDiag()` 必须定义在 `static AppState state;` **之后**；wrapper(450 行) 不能比
  `state.bright_slider`，只记指针由 UpdateDiag 比对。
- **bdtap 注入异步** ⇒ 断言前必须 `settle()`。**`wait_diag()` 必须在 `open_settings()` 之后**。
- 抓屏静止时脏区为空 ⇒ 两张 PNG 逐字节相同是**正确**的；要强制重绘写 `g_force_redraw=1`。
- SWD 传负数要转补码（`bench.wr` 已做）。

## 引擎行为（YMGUI）
命中取最深对象；`sendEvent` **不冒泡**、`event_cb==NULL` **静默丢弃** ⇒ 必须递归给子树每个对象装
wrapper（`PhoneUI_panel` 无 `event_cb` ⇒ 空白处怎么拖都不动）。`GYsw_data` 是 Switch.c 私有类型 ⇒
用 `SetOn/GetOn`。`GY_STATE_Hidden=0x04`。**`YMGUI_Slider_SetValue/SetRange` 不触发 changed、只写值**。
**滑杆回调在 `Pressed` 与每次 `Pressing` 都触发**。

## 设置页布局（滚动版）
`SCROLL_VIEW_H=412-56`，`content_h=491` ⇒ clamp 上限 135。**视口必须从 `y=56` 起**（不透明+ClipChildren，
会盖兄弟对象）。卡片内容 y=65/133/201/269/329/407/451 ⇒ 底部初始在屏外，先滚到底。`SCROLL_WRAP_MAX=48`，
装满是**静默 return**（表现为"后面几个控件滚不动"）。
- ⚠⚠ `settings_sync_controls(int reset_scroll)`：`app_show` 传 1、`app_command("sync")` 传 0。
  `PhoneApps_Command(...,"sync")` 会调到 `app_show()` 把 `scroll_y` 复位 ⇒ 拖动中发它 = 页面弹回顶部、
  滑杆 y 从 330 跳 465、手指还在 330 ⇒ **"拖不动"**。
- 拖动会误触输入框（引擎 `Pressed` 给可聚焦对象置焦点）⇒ `scroll_drag()` 首次判定为滚动时
  `YMGUI_SetFocus(c,NULL)`；脚本拖动起点用 `DRAG_XY=(6,380)`（用 (160,380) 会命中设备名输入框）。
- **scene**：`BOOT0 HOME1 APP2 RECENTS3 SHUTDOWN4 OFF5`（**4 是 SHUTDOWN 不是 OFF**）。
  点"关机…"只开面板、scene 不变 ⇒ 读 `g_diag_power_dialog`。
- 软键盘占 y=226..448，收起键 **(287,267)**；**别用 home (160,464)**（会退出到桌面），用返回 (64,464)。
- 已验收关机/开机闭环坐标：取消 (94,404)、关机 (226,404)、电源键 (160,428)、重新开机 (160,359)。

## 配置持久化（语言 / 半透明 / 亮度）
写 **W25Q128 扇区 240 = `0x000F0000`**，magic `"YCON"` + CRC32 双校验，损坏回退默认。
三份同构桥：`phone_shell` 是库侧代码、不许 include 应用层 `app_config.h` ⇒ main.c 里
`app_lang_persist`/`app_ribbon_persist`/`app_brightness_persist` + shell 侧 `Phone*_Set*PersistCallback`。
- ⚠⚠ 写/擦完**必须 `qspi_enter_mmap()`**（`qspi_*` 内部先退出映射，中文字模从 `0x9000_0000` 读 ⇒
  忘了重进**整屏中文消失**）。**XIP 窗口只读**，主机侧写报 `DRW` 错 ⇒ 恢复出厂走固件
  `AppConfig_FactoryReset()` + 写 `g_cfg_factory_reset=1`。
- ⚠ **钳制 `s_cur` 内容后必须重算 `crc`，且钳制放在 `s_flashed` 快照之前** ⇒ 否则 Save 出的 blob
  CRC 自相矛盾，重启后整套设置全丢。
- 同名 static 在 ELF 里有多个符号（`active` 3 个）⇒ 诊断量必须固件显式导出 `volatile` 全局。
- **亮度三层一致**：blob / `display_brightness`（初值 50）/ 滑杆 range **1..100**（出厂 50）；
  屏侧叠 `alpha=(100-v)*160/100` 黑罩（**非线性，系数 160 不是 255；下限取 1 不是 0**，0 = 全屏死黑
  与屏坏无法区分）。
- 语言 `T("中文原文")`：中文模式直接返回原文。**文案两类**——绘制期取值（每帧读
  `PhoneApps_Get(id)->title`）包 `T()` 自动跟随；**创建期设定**（Label/Button 是创建时拷进对象的副本）
  **必须显式重设**（`PhoneHost_RefreshLanguage()` + `PhoneDesktop_Retranslate()`，各 APP 靠 `show()` 惰性刷新）。

### ⚠⚠ 滑杆类控件必须两段式落盘（闪存磨损）
一次拖动经过 5 个**不同**中间值 ⇒ 板级实测**擦 6 次扇区**（标称 10 万次/扇区），"值相同不写"挡不住。
做法：拖动中走 `PhoneHost_SetBrightnessPreview()`（只改内存+重绘，**不碰持久化回调、不发 sync**），
松手（`Released` **和** `ReleasedOff`，后者是拖出界外松手）才调 `PhoneHost_SetBrightness()` 落盘一次；
由 `scroll_wrap_event` 在转发前调 `brightness_commit()`。
**⚠ 隐蔽坑**：Preview 已把值推到最终值 ⇒ 松手时 `SetBrightness(GetBrightness())` 传同一值 ⇒
命中"值相同直接 return" ⇒ **落盘回调永不被调用**（现象：press/change/commit 全 +1 而亮度不变）
⇒ 加 `s_bright_dirty` 粘滞标志，**只在 Preview 置位**（开机那次同值同步仍被挡住，不会每次开机擦扇区）。
三段判读（`g_diag_sld_press/_change/_commit`）：`press=0` 没命中；`press>0 change=0` wrapper 转了但
Slider 没回调；`change>0 commit=0` 值变了没落盘。

## Cache / DMA（接 TF 卡前必读）
D-Cache 常开 + **FORCEWT 恒 1**（`g_cache_wb`/`g_cache_clean` 已删）。FORCEWT 只覆盖"写"方向 ⇒
CPU 写必达内存（**DMA 读方向无需任何 cache 维护**）；**DMA 写 → CPU 读**管不了，实测 **64/64 全部静默读到陈旧值**。
未完成：**L2** SDMMC 通路（TF 卡到货）；**L3** SRAM1 整块 128 KB 设 `NOT_CACHEABLE`（上 IDMA 前，不依赖卡）。
L3 判据闭环：重跑 `dma_check.py`，"DMA 写→CPU 读"应 64/64 → 0/64。
四约束：① DMA1/2 **访问不到 DTCM** ⇒ 缓冲不得来自 `board_alloc0`；② DMA1/2 属 D2 域，**访问不到 D3 域
SRAM4**；③ 缓冲必须 **32 B 对齐**；④ **MPU 8 region 已全满**且 ARMv7-M **编号大者优先**（DDI0403：highest region number
takes priority；2026-10-08 更正，此前写成"编号小者优先"是错的）⇒ 给被大 region 覆盖
的小区域改属性时必须取**更大**的编号 ⇒ 常常得重排编号
（region6=SDRAM / region7=NAND 本板无器件可回收）。region 长度须 2 的幂；**`SubRegionDisable` 不能配
"子区非缓存"**（语义是"不吃本 region 配置"⇒ 落默认 Cacheable，方向相反）。新 NOLOAD 段
（`.bss_dma`/`.bss_ime`）**必须放 `.bss` 之前** + 在 `board_startup.c` 手动清零。DMA1 MEM2MEM 实测 ≈160 MB/s。

## 已知遗留（用户已叫停排查，别再动）
- **IME 视口顶溢出**：键盘弹起时视口顶 y=80..88 有 79~82 个灰字像素。已排除 IME 挪控件、引擎裁剪两个
  假设；下一步在 `YMGUI_Invalidate.c:250` 的 `GY_Rect_Intersect` 打点。
- 板级 `selftest()`/`select_preview()` 是 SDL 桌面版入口，板上无触发路径。

## 性能与构建的两个坑（本项目实测）
**四个"看着该优化、实测没用"的先例（无收益即回退）**：XIP 改可缓存 · 圆角每行 Fill 合并(≈0%) ·
`GY_MixPx` 三次 `/255`（gcc 自动强度削减）· 关 FORCEWT 走写回(1.0–1.1×)。
**两个构建坑**：① 工程曾是 `-O0`（`CMAKE_BUILD_TYPE` 从没设过），现用 `add_compile_options(-O2)` 固化
—— **从厂商例程搬代码时构建配置也是契约的一部分**；② **`-O2` 不跨编译单元内联**，热点里的
`lcd_wr_data()` 必须在本文件自写内联 store。
渲染优化累计 2.88×，瓶颈是**逐像素光栅化单价**（不透明 1.04 · 半透明 17.34 · blit 11.74 · 文字 17.26 cy/px），
不是 overdraw(2.27 层) 也不是树遍历(≈3%)。⚠ in-context 成本 ≈2× 微基准，外推收益打对折。

## UI 文本布局：宽度按**字体真实步进**给，不要拍脑袋（2026-10-08 实测）
`PhoneUI_left_label(obj, x, y, **w**, text, color, size)` 的 `w` 是**硬裁剪宽**，
文本超出部分直接切掉（不是换行、不是省略号）。字号 → 字体表的对应：
`size 0=regular(24px 高) · 1=display(66px) · 2=small(18px) · 3=title(36px)`。

各字体的 ASCII 步进表是**普通数组**，可直接从 elf 里量出来（不用猜、不用试）：
```bash
A=$(arm-none-eabi-nm build/ymgui-h743.elf | grep " T phone_font_display_advance" | cut -d' ' -f1)
probe-rs read --probe 0416:5021:0123456789AB --chip STM32H743VITx b8 0x$A 96
# 数组下标 0 = ASCII 0x20(空格)，所以 '0'(0x30) 在下标 16、':'(0x3A) 在下标 26
```
实测（单位 px，步进 = 字形宽 + 右侧留白，连排时就是这个值）：

| 字体 | 数字 '0'..'9' | 冒号 ':' | 说明 |
| --- | --- | --- | --- |
| regular（size 0/2 用的是 regular） | **9** | 5 | "23:35" = 9+9+5+9+9 = **41** ⇒ 54px 宽足够 |
| display（size 1，桌面大时间） | **33** | **18** | "23" 要 **66px** |
| title（size 3，控制中心） | **17** | 9 | "09:41" = 17+17+9+17+17 = **77** ⇒ 165px 宽足够 |

⚠ **踩过的坑**：桌面大时间拆成 小时/冒号/分钟 三个 label 做"冒号闪烁"时，
小时格给了 60px —— 而两个 display 数字要 66px ⇒ **第 2 位数字右边被切掉**
（用户肉眼发现："第 2 位和第 4 位数字右边被裁剪"）。改成 70px 正常。
⇒ 规矩：**凡是给固定宽度的 label，先按上表算一遍**（或现场量），
别用"看上去差不多"的整数。

