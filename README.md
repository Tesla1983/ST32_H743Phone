# ymgui-h743 —— YMGUI 移植到 STM32H743VIT6 小系统板

一个运行在 STM32H743 上的移植手机 OS。

> 目标：在本板（320×480 8080 并口 ST7796 + GT1158 触摸 + W25Q128 16MB QSPI）上跑起  
> [YMGUI](https://github.com/Yao-Mi/YMGUI)，承载其自带的中文手机界面 `project_Demo/phone_shell`。
>
> 原始移植计划（**已归档，不作当前待办/配置依据**）见 `E:\stm32-tetris\docs\YMGUI_PORT_PLAN.md`；阶段 0 的 Rust 参照工程实测记录见  
> `E:\stm32-tetris\docs\FMC_WRITE_TIMING_SCAN.md`。当前 C 移植状态与实测以本 README 及 `docs/MEMORY_ARCHIVE.md` 为准。

---

## 工程结构

```text
E:\ymgui-h743\
├─ CMakeLists.txt               # 构建脚本（YMGUI 库 + 厂商 HAL/BSP + 板级代码）
├─ cmake\arm-none-eabi.cmake    # 交叉编译工具链文件
├─ linker\stm32h743vit6.ld      # 链接脚本（显式 DTCM + AXI SRAM 分区）
├─ src\
│   ├─ startup_gcc.s            # GCC 版启动 + 向量表（CMSIS 只带 Keil 版，故自写）
│   ├─ board_startup.c          # .data 搬运 / .bss 清零 / _init·_fini 桩 / 自写 _sbrk
│   ├─ board_alloc.c/.h         # 双档堆（DTCM / AXI），对应 GY_malloc0 / GY_malloc1
│   ├─ board_fault.c/.h         # 故障现场快照（CFSR/HFSR/MMFAR/BFAR），供 SWD 事后读
│   ├─ ymgui_port.c/.h          # GYdisp 初始化 + flush_cb + 全帧镜像缓冲 + 显示自检
│   ├─ touch_port.c/.h          # GT9xxx → YMGUI_Inject_Pointer / Inject_Tick
│   ├─ qspi_port.c/.h           # W25Q128：indirect 读写擦 + memory-mapped XIP + 验收
│   ├─ main.c                   # 主程序
│   ├─ stm32h7xx_hal_conf.h     # HAL 配置头（取自厂商例程）
│   └─ stm32h7xx_it.c/.h        # 中断处理（fault handler 已接故障快照）
├─ tools\
│   └─ grab_ymgui.py            # 读 AXI 全帧镜像缓冲还原 PNG（目视核对用）
├─ third_party\
│   ├─ YMGUI\                   # git 仓库根（库本体在再下一级 YMGUI\）
│   ├─ CMSIS\                   # 内核头（已从官方 CubeH7 补齐 cmsis_gcc.h）
│   ├─ STM32H7xx_HAL_Driver\     # HAL 驱动
│   ├─ BSP\{LCD,LED,MPU}\       # 厂商外设驱动（已转 UTF-8）
│   └─ SYSTEM\{sys,delay,usart}\ # 厂商系统层（已转 UTF-8）
└─ build\                       # 构建输出 ymgui-h743.{elf,bin,hex}
```

---

## 构建与烧录

```bash
cd /e/ymgui-h743
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
cmake --build build --target ymgui-bin        # 生成 bin/hex 并打印尺寸

P="--probe 0416:5021:0123456789AB --chip STM32H743VITx"
probe-rs download $P build/ymgui-h743.elf
probe-rs reset    $P            # ⚠ 必须显式复位：download 结束后内核是 halt 的
```

**工具链**：arm-none-eabi-gcc 14.3.1 + Ninja（本机无 `make`）+ probe-rs 0.29.1。

> ⚠ **`probe-rs download` 跑完内核仍然 halt**，不会自己跑起来。只 download 不 reset 的话，  
> RAM 里还是上一份固件的残余，用新固件的符号地址去读会得到一串莫名的数据  
> —— 看起来像"新功能没生效"，其实是**压根没跑**。每次烧完都要跟一条 `probe-rs reset`。

### 板上读数小工具

```bash
python tools/read_vars.py g_loop_count g_shell_rc g_fault   # 读一组全局变量（自动从 ELF 取地址、合并相邻读）
python tools/ime_probe.py nihao zhongguo                     # 板上跑一遍中文输入法：拼音→候选→上屏
```

`read_vars.py` 需要留意**同名静态变量**（例如 `active` / `visible` 在多个 `.c` 里都有），  
脚本取的是 nm 里最后一次出现那个 —— 要精确时先用 `arm-none-eabi-nm -S` 把重名列出来确认地址。

### 上电第一屏的"彩色波纹"是什么（开机自检开关）

上电闪一下的彩色波纹（约 86 ms）是 **A2 的 ramp 自检图案**：`ymgui_port_ramp_selftest()`
把整屏 GRAM 写成「像素 i = i & 0xFFFF」再读回比对，证明 8080 读通路可信，
随后自动恢复开机画面。**不是花屏**——时序真出问题时图案会错位且 `g_ramp_mismatch ≠ 0`。

该自检 **默认不随上电运行**（`src/ymgui_port.h` 的 `YMGUI_PORT_BOOT_SELFTEST = 0`，
打开方式见 `CMakeLists.txt` 里的注释位）。要跑自检不用重编译：SWD 写 1 到
`g_gram_recheck` 即可随时触发同一套 ramp + gram_verify，完成后读
`g_ramp_mismatch` / `g_gram_mismatch`（0 = 通过）。历史背景见
`E:\stm32-tetris\docs\YMGUI_PORT_PLAN.md` §7.7（归档计划中的记录；当前开关和验收以本文与代码为准）。

---

## 阶段进度

| 阶段    | 内容                                       | 状态                                                          |
| ----- | ---------------------------------------- | ----------------------------------------------------------- |
| **0** | FMC 写时序收紧（在参照工程 `stm32-tetris` 内做）       | ✅ 完成，固化 `5/5 = 25ns`，整屏 8.45ms ≈ 118 FPS                    |
| **1** | 新建独立 C 工程（CMake + GCC + 链接脚本 + MPU + 时钟） | ✅ 完成                                                        |
| **2** | 显示 `flush_cb` + ST7796 初始化               | ✅ 完成，面板 GRAM 回读 153600 px **0 失配**                          |
| **3** | 触摸 GT9xxx + Tick 注入                      | ✅ 完成：`0x8140`→`"1158"`；事件注入端到端打通（合成点击可触发按钮回调）               |
| **4** | 只读常量上 W25Q128（memory-mapped）             | ✅ 完成：XIP 与 indirect 逐字节一致（比对 64 KB、覆盖全部 16 MB）              |
| **5** | phone_shell 降配并上板                        | ✅ 完成：`g_shell_rc = 0`，中文字库走 XIP，桌面/应用/最近任务均出画，**板上中文输入法可用** |
| **6** | 整机验收 A1–A5                               | ✅ 完成：**A1–A5 全部达成**（详见下节）                                   |

### 阶段 5 + 6 状态（✅ 完成，2026-10-03）—— 验收判据 A1–A5

| 编号     | 判据                        | 板上实测                                                         | 达成 |
| ------ | ------------------------- | ------------------------------------------------------------ | -- |
| **A1** | 全屏刷新 ≥ 19 FPS             | `g_flush_cyc = 3 071 849` @400 MHz ⇒ **7.68 ms ⇒ 130.2 FPS**（优化后；原 48.8，见下节） | ✅  |
| **A2** | ramp 失配 0，跑多帧后复测仍 0       | `g_ramp_mismatch = 0`、`g_gram_mismatch = 0`（数千帧后复测）          | ✅  |
| **A3** | GT9xxx 产品 ID 返回 `1158`    | `g_tp_pid = 0x31313538`                                      | ✅  |
| **A4** | XIP 与 indirect 逐字节一致      | 比对 64 KB 覆盖全部 16 MB，失配 0                                     | ✅  |
| **A5** | 三页桌面可滑动、可开关应用、**中文输入法可用** | 合成滑动/单击 `g_synth_done` 0→2；中文输入见下表                           | ✅  |

**板上中文输入法（A5 的关键一环）**：

```bash
python tools/ime_probe.py nihao zhongguo shijie dianzi gongzuo
```

| 拼音         | 候选（板上真实读出，**2026-10-07 词组词典开通后**） | 上屏后文本 |
| ---------- | ------------------- | ----- |
| `nihao`    | 你好 / 你号 / 你浩 / 你豪   | `你好`  |
| `zhongguo` | **中国** / 中过 / 中果 / 中郭 | **`中国`** |
| `xianzai`  | **现在** / 想在 / 先在 / 现在爱 | **`现在`** |
| `zhongguoren` | **中国人** / 中国仍 / 中国任 / … | **`中国人`** |
| `shijie`   | 是接 / 是借 / 是解 / 是节   | `是接`  |

单字表 `pinyin_gb2312` **7291 条**、词组表 `phrases_rime` **47 280 条全部就位**。

> ### ★ 2026-10-07：词组词典（方案 B）已开通 —— 全量上外部 flash XIP
>
> 用户最初的缺口是"`zhongguo` 首位不是中国"。**已经修好**：上表是出货形态下
> `tools/ime_probe.py` 走**真实软键盘路径**读出来的结果，`zhongguo` 上屏就是 `中国`。
>
> | 项 | 数 |
> |---|---|
> | 词典 | v3 重排 `phrases_xip.bin` **1 303 456 B**（比原 v2 小 268.9 KB）常驻 W25Q128 `0x300000` |
> | **RAM** | **零索引** —— heap1 峰值 **71 064 B（33%）**，与词组没开时**完全相同** |
> | 内部 flash | 1 577 444 B（**75.22%**），词组不占内部 flash 一分 |
> | 灌库 | 一次性；`tools/qspi_provision.py`，11 块 × 120 KB，约 7 分钟 |
> | 端到端校验 | 固件**从 XIP 读回** 1 303 456 字节算 CRC32 = host `zlib.crc32`（`0x02747acb`）|
> | 每次按键增量 | **+1.16 ms × 拼音字母数**（`zhongguoren` 共 +12.7 ms）|
>
> 关键实现点：① 每个首字母桶写成**一整块连续区**（XIP 的成本单位是"一段连续读"，
> 见下）；② 删掉 `entries[]` 索引改顺序游标解码；③ 桶内按词频降序 +
> `IME_PHRASE_SCAN_CAP=512` 作**延迟上界**（不是容量限制，全量 47 280 条都在 flash 里）。
> 详见 `docs/IME_PHRASE_DICT_FEASIBILITY.md` §8。
>
> ### ★ 2026-10-07：按键耗时 42~68 ms → 17~29 ms（−58%，候选一字未变）
>
> 上一条里挂着的"按键本来就慢"**已经修掉**，而且是纯机械替换、`snprintf` 换有界
> `memcpy`，**6 个词 × 6 个候选逐字与改动前一致**（语义不变的证据）。
>
> | 拼音 | 改动前 | 改动后 | 降幅 |
> |---|---:|---:|---:|
> | `zhongguo` | 52.491 ms | **20.993 ms** | −60.0% |
> | `zhongguoren` | 68.841 ms | **28.783 ms** | −58.2% |
> | `xianzai` | 42.858 ms | **17.878 ms** | −58.3% |
> | `nihao` | 19.692 ms | **8.464 ms** | −57.0% |
> | `wo` | 3.205 ms | **1.789 ms** | −44.2% |
>
> 改动四处：① 词组分支 `snprintf("%s%s")` → `imeConcat()`；② 单字分支同上，并把
> `strlen(phrase.word)`/`strlen(ce.ch)` 提到 state 循环外（词组在 XIP 上，原来每个
> state 都要重读一遍外部 flash）；③ `addComposeState()` 与收尾的 `snprintf("%s")`
> → `imeCopyWord()`；④ 内层 `ComposeState state = …`（按值拷 104 B）改成指针。
> 详见 `docs/IME_PHRASE_DICT_FEASIBILITY.md` §9。
>
> ⚠ **一个与"词典放哪儿"无关、仍挂着的问题**：`shijie` 打不出"世界"。
> 单字**是**的频率 1 180 957，`composeQuality()` 对 2 段复合除以 16 后"是接"仍得
> 74 920 分 > 世界的 30 808。是引擎打分问题，且"世界"在桶内排 18/4270，
> **没有**被 512 的扫描上限截断。要改就得动打分公式，会连带影响别的输入，
> 所以单独一轮做。

> ### ★ 2026-10-07：字母 W 渲染残缺已修（字模单元宽 8 → 9，方案 1 上板）
>
> 根因：`gen_font.py` 的 ASCII 单元宽 `CELL_W=8`，而 15pt DejaVuSansMono 等宽步进
> 是 **9.0 px** —— 95 个字形里 41 个第 9 列被整列归零，**W 丢的是覆盖度 15/15 的
> 整条实心右竖**（`Wi-Fi`/`Weather` 最扎眼），且英文整体还被挤了 1 px。
> 修复：`CELL_W` 改 9 重生成字模（`gen_font.py` 与 `YMGUI_FontData.c` 同步），
> 被裁 **0/95**。代价：字模 6 080 B → 7 600 B（**+1 520 B**），
> FLASH 1 579 052 B（75.30%）；heap1 峰值 71 064 B 不变；A2 ramp/gram = 0；
> IME 3/3；光标正常；`ci/firmware_reference.sha256` 已按第 a 类规则换新基准
> （`check_firmware_hash.py` 2/2 PASS）。证据链与踩坑：`docs/FONT_GLYPH_CLIPPING.md`。

### 收尾性能优化（2026-10-03，阶段 6 之后）—— A1 从 48.8 提到 130.2 FPS

收官时 A1 记的是 48.8 FPS。顺着往下挖发现那 20.50 ms **绝大部分不是总线付的**：
**整个工程此前一直是 `-O0` 编译的**（`CMakeLists.txt` 从没设过 `CMAKE_BUILD_TYPE`，
工具链文件里也没有任何 `-O`）。厂商 `lcd.c` 里那句 `data = data; /* 使用-O2优化的时候… */`
其实早就提示了 —— 本该用 `-O2`，移植时把代码搬过来了、优化等级忘搬。

严格分步做的，每步单独上板量过：

| 步骤 | 改动                                    | 整屏 flush   | FPS        | 相对基线    |
| ---- | ------------------------------------- | ---------- | ---------- | ------- |
| 基线   | `-O0` + `lcd_wr_data()` 逐像素调用          | 20.50 ms   | 48.8       | —       |
| ①    | `CMakeLists.txt` 加 `-O2`              | 13.97 ms   | 71.6       | **1.47×** |
| ②    | flush 热点改内联 volatile 写               | **7.68 ms** | **130.2** | **2.67×** |
| ③    | 主循环 `delay_ms(15)×2` → 按实际用量补足 16 ms  | 节拍 33.3 → **66.2 Hz** | — | 2.0× |
| ④    | XIP 区改可缓存（**证伪，已回退**）                  | 64.80 → 64.69 ms（无效） | — | 无收益 |

细节、 why、以及 **"flush 已见底、渲染才是真瓶颈"** 的完整论证见计划书 §7.6。

⚠️ **最重要的一个结论**：新加的端到端基准 `g_bench_frames`（每帧强制整屏脏）显示，
最坏情况下每帧 64.80 ms，其中**渲染占 54.88 ms（85%）**，flush 只占 9.92 ms。
**flush 已经打到总线地板 50.0 ns/px，再优化它也涨不了帧率了。**

已试过并被证伪的一条：以为每画一个中文字都要走一遍 QSPI 取字模是瓶颈，
把 XIP 区改成可缓存 —— **帧基准纹丝不动**，说明慢在**纯 CPU 软件光栅化**
（圆角、渐变、alpha 混合、位图缩放）。改动只带来 cache 维护负担 ⇒ 已回退，记录留在 `mpu.c`。

优化后全套回归：`g_ramp_mismatch` / `g_gram_mismatch` 均 0、`g_fault[0..8]` 全 0、
中文输入法 2/2 通过、text 反而缩了 3.6%（`-O2` 顺带收益）。

### 渲染侧 P0 优化（2026-10-03）—— 满负载 41.45 → 34.84 ms（24.1 → 28.7 FPS）

按 `docs/RENDER_OPT_FEASIBILITY.md` §6 的 P0 三项实施，全部上板实测：

| 配置 | 满负载整帧 | FPS | 说明 |
| --- | --- | --- | --- |
| 基线（P0 之前） | 41.45 ms | 24.1 | 上一轮（壁纸行内直写）之后 |
| + P0-1 ribbon 合并 | 37.64 ms | 26.6 | **省 3.82 ms** |
| + P0-2 全帧镜像关 | 35.13 ms | 28.5 | **再省 2.51 ms**（比预估的 1–2 ms 大） |
| + 诊断计时关 | **34.84 ms** | **28.7** | 产线形态；per-object 计时自身只吃 0.28 ms |

- **P0-1**（`phone_shell.c`）：壁纸三层 ribbon 原来逐列 1 px 宽填充，一帧 **4558 次**
  非空 `Draw_Fill`；改成"crest 相同的相邻列合并成一段 + 与本 band 不相交的整段跳过"
  后降到 **1366 次**。等价性不是靠抓屏看的 —— `tools/check_ribbon_equiv.py` 在主机上
  把两种算法各跑一遍完整一帧，逐像素比对 (层, 颜色, opa, 顺序) 写入序列，
  **70415 个像素全部一致**（旧 4558 次/新 1366 次也与脚本算出来的完全吻合）。
- **P0-2**（`ymgui_port.c`）：全帧镜像 memcpy 改成 `g_frame_mirror` 运行时开关
  （默认开，A2 的 gram_verify 和 `grab_ymgui.py` 抓屏都靠它）+
  `YMGUI_PORT_FRAME_MIRROR` 编译期宏。**跑 A2 前必须写回 1** 并让它刷一整帧。
- **P0-0**（库内 `YMGUI_Invalidate.c`，`YMGUI_DIAG_OBJTIME`）：`drawObjRec` 给每个对象
  记 self（draw_cb 自身）/ sub（含后代子树）/ calls 三项，`tools/objtime_scan.py` 读表并把
  draw_cb 地址用 nm 解析成函数名。基准启动时自动清零，也可写 `g_objtime_reset` 手动清。

per-object 排行（60 个对象，60 帧折算）把上一轮"其余 25%"定位清楚了：

| 对象（draw_cb） | self ms/帧 | 占比 | 说明 |
| --- | --- | --- | --- |
| `wallpaper` | **15.23** | 40% | 其中 ribbon 半透明填充 ≈10.5 ms（129893 px × 32.4 cy/px）、渐变直写 ≈4.7 ms（153600 px × 12.3 cy/px） |
| `baseDrawCb`（root 背景） | **4.25** | 11% | 153600 px 不透明填充 = 11.07 cy/px，**已贴图元地板**，只能靠 P1-1 再挖 |
| `PhoneUI_glass_draw` ×2 | 3.42 | 9% | 两块毛玻璃面板 |
| `text_draw` ×多 | ≈2.4 | 6% | 中文/西文文字 |
| 其余（按钮/天气/状态栏等） | ≈1.6 | 4% | — |
| **self 合计** | **26.88** | 71% | flush ≈9.7 ms（26%）、遍历/裁剪 ≈1.1 ms（3%） |

⇒ 下一刀的方向已经明确：**P1-1（`Draw_Fill` 内层循环）+ P1-3（ribbon 半透明改不透明，
需目视确认）**，两者都直接打在 15.23 ms 的壁纸上。

回归（全部通过）：A2 `g_ramp_mismatch` / `g_gram_mismatch` 均 0、`g_fault[0..8]` 全 0、
中文输入法 2/2、抓屏目视桌面/ribbon 无异常。探针曾出现"枚举得到但 open 失败"
（USB 卡死），拔插一次恢复。

### 渲染侧 P1 优化（2026-10-03）—— 满负载 34.84 → 21.64 ms（28.7 → 46.2 FPS）

按 `docs/RENDER_OPT_FEASIBILITY.md` §6 的 P1 实施（详细数据见该文件 §6.2）：

| 配置 | 满负载整帧 | FPS | 说明 |
| --- | --- | --- | --- |
| 基线（P1 之前） | 34.84 ms | 28.7 | P0 之后 |
| + **P1-1** `Draw_Fill` 内层重写 | ≈24.9 ms | ≈32.2 | 不透明 11.07→**1.04 cy/px**（10.6×）、半透明 32.36→**17.3 cy/px**（1.9×）、缩放 blit 58.8→**11.7 cy/px**（5.0×） |
| + **P1-2** 缩放 blit 增量采样 + 指针直写 | （与 P1-1 合计） | — | 已并入上行 |
| + **P1-5** 壁纸渐变 4 路展开 | 23.25 → **21.64 ms** | **46.2** | 渐变行 8.02→**1.29 cy/px**（6.2×，微基准 A/B case 10/11） |
| + P1-3 ribbon 改不透明（**默认关**，`g_ribbon_opaque`） | **16.66 ms** | **60.0** | ⚠ 改变外观，需目视确认后才可默认打开 |

- **P1-1**（库 `YMGUI_DrawFill.c`）：opa 分支提出循环、指针递增、对齐时 32 位双像素写。
  逐位等价（裁剪逻辑未动，双像素写=两个相同半字）。
- **P1-2**（库 `YMGUI_DrawImg.c`）：源列号改定点累加（去掉每像素 64 位乘法），
  矩形求交提前 + `*d++` 直写（省掉每像素 6 次边界检查）；colorkey 与诊断计数语义保留。
  当前 UI 没用到缩放 blit，但图元单价已从 58.8 → 11.7 cy/px，为引入位图扫清障碍。
- **P1-4**（库 `YMGUI_DrawText.c`）：`blitGlyph` 索引写改指针递增。文字 18.52→17.26 cy/px
  （≈7%，收益有限但无害，保留）。
- **P1-5**（应用 `phone_shell.c`）：壁纸渐变行内直写再改 4 路展开
  （抖动周期是 4：头/尾 ≤3 px 对齐 + 主体一次 4 px，无逐像素查表与地址计算）。逐位等价。
- **P1-3**（应用 `phone_shell.c`）：`g_ribbon_opaque` 运行时开关（**默认 0 = 保持半透明**）。
  实测整帧 21.64 → 16.66 ms，但**改变外观**——两种模式的抓屏已存
  `build/p1_ribbon_semi.png`（现状）与 `build/p1_ribbon_opaque.png`（不透明），
  是否默认打开交由目视决定。

**重要更正**：P0 轮曾写下"纯指针直写的地板是 11–12 cy/px，是 AXI SRAM store 的本征成本"
—— **这是错的**。P1 轮加了裸 store 微基准（case 6/7）后实测地板只有 **0.88 cy/px**，
之前的 11 cy/px 是"写穿策略 + 循环内变址寻址"的结果，不是内存本征。
顺带证伪了 cache 假设：关掉 `SCB->CACR.FORCEWT`（强制透写）对填充**无收益**
（case 8/9，1.00–1.10×）。
⚠ **2026-10-04 更新**：当时保留的 `g_cache_wb` / `g_cache_clean` 两个运行期开关
**已删除**（理由与替代保证见下一节「cache 策略固化」）。
微基准 case 8/9 仍保留 —— 它们是**局部、原子、测完立刻恢复**的对照动作，
不依赖任何全局开关。

P1 后的剩余构成（21.64 ms）：flush ≈7.9 ms（总线地板 7.68 + 窗口命令）·
`wallpaper` 8.2 ms（其中 **ribbon 半透明 α ≈5.0 ms**、渐变 ≈3.2 ms）·
毛玻璃 α ≈1.9 ms · 文字 ≈2.6 ms · 其余 ≈1 ms。
**再往下的最大单笔仍是"半透明 α 混合"**：要么 P1-3 改不透明（-5 ms，视觉决策），
要么 P2-1 DMA2D（工作量以周计）。另一个实测规律：**满负载下 in-context 成本 ≈2× 微基准**
（band buffer 20 KB > 16 KB D-cache，流式写+RMW 互相驱逐），评估收益时要打对折。

回归（全部通过）：A2 双失配 0、`g_fault[0..8]` 全 0、中文输入法 2/2、
抓屏目视桌面正常。固件 text 1557780 B（较 P0 +2.7 KB，主要是新代码）。

### 引擎缺陷修复 + 渲染侧步骤 1–5（2026-10-04）—— 板上实测 37.3 → 56.3 FPS

上一轮的"审查渲染引擎"结论落成了五条改动，**已在板上烧录实测并通过全部回归**。逐条如下。

#### 板上实测（DAPLink 烧录；桌面场景；`probe-rs` + `tools/bench.py`，60 帧整屏标脏）

| 配置 | 整帧 | FPS | 说明 |
| --- | --- | --- | --- |
| 起点：5/5 + 全帧镜像开 + 软件叠黑 | 26.80 ms | 37.3 | 改动前的出货形态 |
| 5/5 + 镜像关 + 软件叠黑 | 24.26 ms | 41.2 | **步骤 2b** 省 2.54 ms |
| 5/5 + 镜像关 + 不调暗 | 20.55 ms | 48.7 | 软件叠黑本身值 3.71 ms |
| 2/4 + 镜像关 + 软件叠黑 | 21.42 ms | 46.7 | **步骤 3** 再省 2.84 ms |
| 2/4 + 镜像关 + 不调暗 | 17.77 ms | 56.3 | — |
| **2/4 + 镜像关 + 硬件 PWM 调背光（现出货形态）** | **17.78 ms** | **56.3** | **步骤 2a 硬件版**：调暗成本归零 |

两次重复测量差 243 cycles（8.57 M 中），**可复现性极好**。合计 **−9.02 ms / +19.0 FPS**。
注意：**帧基准依赖当前场景** —— 同一份固件在"笔记"页量到 23.17 ms / 43.2 FPS，
所以跨场景比数字没有意义，比之前先确认场景（本表的数字都是重置后的桌面页）。

| 单项 | 板上实测 |
| --- | --- |
| **步骤 3** FMC 写时序六档扫描（每档都跑 A2 双自检） | 5/5 = 7.68 ms → 4/4 = 6.28 → 2/4 = 4.89 → **1/4 = 4.19 ms**，六档失配全 0；实测与理论 `(ADDSET+DATAST+1)/220MHz` **逐档吻合**，反证 FMC 内核时钟确实是 **220 MHz**（`lcd.c` 旧注释的 200 MHz 是**速率**记错；另有"200 MHz 上限"一说属同步模式，见 `docs/MEMORY_ARCHIVE.md`）。现取 **2/4**（留余量），比 5/5 省 2.79 ms |
| **步骤 4** 半透明填充（`tools/micro.py` case 2） | **17.3 → 15.33 cy/px（−11.4%）**；不透明填充保持 1.05 cy/px（该路径未被改动，符合预期） |
| **步骤 2b** 全帧镜像 | **2.54 ms/帧**（26.80 vs 24.26），与 README 原先估的 ≈2.5 ms 吻合 |
| **步骤 2a** 调暗 | 软件查表版曾把 7.4 ms 砍到 **3.65 ms**；改硬件背光 PWM 后**这 3.65 ms 归零** ⇒ 46.7 → **56.3 FPS**。见下方"步骤 2a 收尾" |
| **步骤 5a/5b** | 反汇编已确认（`hspan` 紧循环、`GY_BlendPx` 符号消失）。桌面场景不画大实心圆，所以这一项**在本场景量不出来**；它对画廊那类页面与抗锯齿密集页面才有效 |

**回归（全部通过）**：A2 双自检 `ramp`/`gram` 失配**均 0**（改前、改后、新时序 2/4、以及背光 PWM 接管后各测一次）；
`g_fault` = 0；`g_port_rc`/`g_ctx_rc`/`g_shell_rc`/`g_clock_rc` 全 0；中文输入法 **2/2**；
文本光标 16×2 下划线、闪烁周期 297.5 ms（设定 300）；抓屏目视桌面正常
（渐变壁纸、半透明 ribbon、毛玻璃卡片、图标与中文都无异常，见 `build/after_steps_1_5.png`）。

逐条改动细节如下。

| 步骤 | 改了什么 | 性质 | 证据 |
| --- | --- | --- | --- |
| **4** | `GY_MixPx` 改成**在存储域（5/6bit）混合**、只量化一次 | 修**系统性偏暗**的缺陷 | 30 万样本偏置 `R−2.27/G−1.24/B−2.29` → **0.00**；30 万样本、`opa=255` 时与 `GY_ColorToPx` **逐位相同**（0 失配） |
| **5a** | `hspan` 裁剪从"每像素一遍"提到"整条跨度一遍"+32 位双像素写 | 性能 | 反汇编确认 `strh.w r,[r],#2` / `str.w r,[r],#4` 紧循环 |
| **5b** | `GY_PutPx`/`GY_BlendPx` 加 `always_inline` | 性能 | 反汇编确认 `GY_BlendPx` 符号**消失**（原来两份 264 B 真函数）、调用点 0 |
| **2b** | 诊断三项（`YMGUI_DIAG_FILLPX`/`OBJTIME`/全帧镜像）从出货构建**摘掉** | 性能（省自己的开销） | `nm` 确认 `g_diag_*`/`g_objtime` 全部消失；DTCM −1816 B |
| **2a** | 亮度叠加改"融合单遍 + 通道查表" | 性能 | 65536×6 档位穷举：查表路径与引擎内核**逐位等价** |
| **3** | FMC `ADDSET`/`DATAST` 改**运行期可扫**（免重编译/免复位） | 实验工具 | 新增 `lcd_fmc_write_timing_apply()` + `g_lcd_fmc_*` + `g_fmc_timing_sweep` + `tools/fmc_sweep.py` |

**步骤 4 的实质**：原实现把 dst 解成 8 位（`5bit<<3`，31→248 而不是 255）、在 8 位域混合、
再用 `&0xF8` 截断回 5 位，两处都在向下取整 ⇒ **77.6% 的混合结果偏暗**，最差 −8 LSB。
三层 ribbon 叠出来 R 分量比正确值低 8。改成在 5/6 位域里 `out5 = round((src5·opa + dst5·inv)/255)`
之后偏置归零。附带收益：**混合内层循环反而从约 20 条指令降到 15 条**
（分量位数降低 + `sr5·opa` 被 -O2 提到循环外 + `+127` 与恒等式的 `+1` 被折成常数 `0x8080`）。
⚠ **这是有意的外观变化**：半透明/抗锯齿像素整体亮回约 1%，
`tools/check_ribbon_equiv.py` 与既有抓屏 PNG 的基线要重采，那不是回归。

**步骤 5 的实质**（都拿到了反汇编证据）：
`hspan` 原来每像素重新从内存读 9 个字段（clip.x/w/y/h + buf_area.x/y/w/h + stride）、
重跑 8 个分支、再对 `g_diag_putpx` 做一次全局读-改-写，约 24 条指令/像素；
而画廊那三个装饰圆（r=31/94/116）实心核约 73000 像素全部走这条路。
`GY_BlendPx` 则被 GCC 编成了真函数，调用点恰好是 `Draw_CircleFill` 羽化环 /
`Draw_ArcThick` / `Draw_Line` 三个最热的抗锯齿图元 —— 每个 AA 像素多付一次 AAPCS 调用。

**步骤 2b 的取舍**：三项诊断都**站在热路径上**，所以"量出来的帧时间"一直含着它们自己的成本
（全帧镜像 ≈2.5 ms/帧，OBJTIME 是"每 band×每对象"不是"每帧"）。现在 `-DYMGUI_DIAG=ON`
才编入；`g_frame_mirror` 仍是可读写量，**跑 A2 / 抓屏前 SWD 写 1 即可临时打开**。
main.c 里读这些计数的代码已用 `#if` 摘掉，关掉不会缺符号。

**步骤 2a 的实质**：出厂默认 `brightness = 50` ⇒ `opa = 80` ⇒ **每一个被 flush 的像素**
都要 memcpy 一次 + 走一遍半透明混合（≈17 cy/px）≈ **7.4 ms/帧**，而 README 的帧时间表里
从来没有这一笔。因为源色恒为黑，`混暗` 退化成 `dst × (255−opa)/255`，而 RGB565 每通道
只有 5/6 位 ⇒ 它是**一张 32/64 项查表**。于是每次 flush 变成"建表 128 次乘法 + 逐像素
3 查表 2 或 1 写"，并把 memcpy 与调暗**融合成一遍**。

**步骤 2a 收尾：把调暗整个搬到硬件背光 PWM（2026-10-04，已板上验证并设为默认）**

原计划这里留一句"要上 PWM 就说一声"。后来查清了两件事，就把它做完了：

1. **引脚复用成立**：`PB1 = TIM3_CH4 / AF2` —— ST 官方 AF 表核实
   （MicroPython `ports/stm32/boards/stm32h743_af.csv`：`PortB,PB1 , ,TIM1_CH3N ,TIM3_CH4 ,TIM8_CH3N`）；
   TIM3 在本工程**完全没被占用**（全仓库搜过；delay 走 SysTick）。
2. **驱动电路是未知项**：`docs/` 四份验收文档与 BSP 注释里只有"PB1 = LCD_BL、置高点亮"，
   没有原理图。所以按"可逆实验"做：默认 `mode=0` 完全不碰 PB1、
   运行期 `g_bl_pwm_mode=1` 才接管、复位一定回到 GPIO 常亮。
   上板一试：**duty 1000↔0 交替时背光确实随之亮/灭**（用户目视确认），
   寄存器回读也吻合（`PB1.MODER=2`、`PB1.AFR=2`、`CCER.CC4E=1`、实测 2000 Hz、
   `CCR4` 随占空比精确变化），且面板内容自检 ramp/gram 仍 0/0
   —— 背光与显示是两条独立通路。

于是 **`g_dim_mode` 现在三态：0=不调暗 / 1=软件查表叠黑（保留作回退）/ 2=硬件 PWM 调背光（默认）**。
`mode=2` 时 `phone_flush` 整段跳过软件叠加，亮度改由 `src/main.c` 的"亮度→PWM"桥换算占空比。

映射不是 `duty = 亮度`，而是**按原软件叠加的等效亮度**换算，让换机制后观感基本不变：

```
软件叠加亮度 L(v) = 1 − opa/255,  opa = (100−v)×160/100
duty(v) = 1000 × L(v) = 1000 − (100−v)×160×1000/25500
        v=100 → 1000‰   v=50 → 687‰   v=1 → 379‰
```

好处是**对比度反而更高**：原来是把画面整体压暗（黑也变灰），现在是背光变暗（黑仍然是黑的）。
若更想要"50 就是 50% 背光"的直觉映射，把 `main.c` 里那行换成 `duty = v*10` 即可。

⚠ **外观语义变了**：抓屏对比 `build/after_steps_1_5.png`（软件叠黑，暗紫）与
`build/after_pwm_brightness.png`（硬件调暗，饱和紫）能看出差别 —— 这正是"调暗不再写进 framebuffer"
的直接证据（同一张桌面 PNG，差异颜色数 292 → 428）。
用户已目视确认背光随占空比变化；**但"这个亮度观感是否满意"仍需人眼拍板**，
不满意一个 SWD 写就退回：`g_dim_mode = 1`（无需重编译）。

**dim_buffer 已回收（30 KB 还给 heap1）**

原来 `PhoneShell_BoardInit` 里无条件预分配 `W × 48 × 2 = 30720 B` 的整带暂存给软件叠黑用。
默认模式（硬件 PWM）根本走不到那条路，等于白占 heap1 30 KB。改成**按需分配**
（`dim_buffer_ensure()`，只有真的切到 `g_dim_mode=1` 时才申请），并且顺带做了两件事：

- **按实际 band 尺寸分配**（`a->w × a->h` = 320×32 = 10240 px，而不是写死的 W×48=15360 px）。
  这同时修掉一个潜在越界：原写法与"板级 band = 32 行"只是巧合吻合，
  `main.c` 的 `g_band_rows` 钩子一旦把 band 调到 >48 行，`dim_apply` 就会越界写。
- **失败不再静默**：新增 `g_dim_buf_rc`（0=用不到或成功，1=申请失败）。
  否则 heap1 吃紧时会表现为"把 g_dim_mode 写回 1 却完全不调暗"，那种最难查。

板上实测（同一份固件内做对照，读 `g_h1` 结构体与 `dim_buffer` 指针）：

| 状态 | `dim_buffer` | `cap_px` | heap1 used / peak | 说明 |
| --- | --- | --- | --- | --- |
| ① 开机默认（mode=2） | **0x00000000** | 0 | **180 376 B / 176.1 KB** | 默认**一个字节都不占** |
| ② 切 mode=1 + 强制重绘 | 0x240770A8 | **10240** | 200 872 B / 196.2 KB | 按需申请 +20 496 B（= 20480+16 头） |
| ③ 切回 mode=2 | 保留 | 10240 | 200 872 B | 不回收，避免来回分配 |

⇒ 默认配置下 heap1 比改造前**轻 30.0 KB**（30720 B 预分配 → 0）；
即使走软件回退，占用也从 30.0 KB 降到 **20.0 KB**（按 band 尺寸 + 分配头）。
当前 heap1 峰值 **176.1 / 212.0 KB = 83%**（改造前 README 记的是 205.2 KB / 97%）。
回退可用性也当场验证过：切 mode=1 后帧时间回到 **21.45 ms**（软件叠黑生效），`g_dim_buf_rc = 0`。

**步骤 3 的实质**：A1 那句"50.0 ns/px 已是总线地板"**只对 5/5 这一档成立**。
一次 16 位写 = `(ADDSET+DATAST+1)/fmc_ker_ck`，而 **fmc_ker_ck = PLL2_R = 220 MHz**
（`sys.c` 里 PLL2M=25/N=440/R=2 + `RCC_FMCCLKSOURCE_PLL2`，sys.c 自己的注释也写 220 MHz）——
`lcd.c` 原来写"200 MHz"是错的，两种说法在 5/5 上刚好都算成 50.0 ns 所以没被发现。
⚠ 这里说的是**速率记错**（它把 FMC 当成 HCLK3 来的 200 MHz，实际是 PLL2_R 的 220 MHz）。
另外还有一个**"200 MHz 上限"**的说法，那是**同步模式**由 `FMC_CLK = 内核时钟/2` 且 ≤100 MHz 推出来的，
**不约束本工程的异步 8080 接口**（异步模式下数据手册未规定内核时钟上限）——
完整查证见 `docs/MEMORY_ARCHIVE.md` 的「FMC 内核时钟 220 MHz 的定性」，**别为它降频**。
面板规格只约束**写脉宽 tWRL ≥ 19ns**（那是 DATAST ≥4 的事），
**ADDSET 从来没被单独扫过**。`ADDSET=1/DATAST=4` 理论上是 6 周期 ≈ 27.3 ns/px ⇒ 整屏约 4.19 ms（省 45%）。
扫描用新加的 `tools/fmc_sweep.py`（SWD 驱动，免重编译/免复位，`--verify` 会每档自动跑 A2 双自检）。

**步骤 3 收尾时挖出一个更严重的既有 bug：FMC 时序结构体没清零 ⇒ 写时序跨启动不确定**

扫完六档、把 2/4 设为默认、烧录验证过 17.78 ms 之后，我又随便重启了一次 —— 结果整帧变成
**28.92 ms**，而 `flush_cb` 从 0.312 ms/band 涨到 **1.023 ms/band**（≈100 ns/px）。
A2 依然是 0/0，说明**写的内容没错，只是变慢**。直接读 FMC 外设寄存器才看清：

```
BWTR1(写) = 0x3FFF0402 : ADDSET=2 DATAST=4  ← 这两项是对的
                          BUSTURN=15 CLKDIV=15 DATLAT=15  ← 高位全是垃圾
```

7 周期（31.8 ns）+ 垃圾 BUSTURN 的 15 周期 ≈ 22 周期（100 ns），与实测吻合。

根因：`FMC_NORSRAM_TimingTypeDef` 有 **7** 个字段，而
`FMC_NORSRAM_Timing_Init` / `FMC_NORSRAM_Extended_Timing_Init` 会把**全部 7 个**写进
`BTRx`/`BWTRx`。厂商 `lcd_init` 只赋了 4 个（ADDSET/ADDHLD/DATAST/AccessMode），
我照抄的模式也只赋 4 个 ⇒ 剩下 3 个是**未初始化的栈垃圾**，直接进寄存器。

⇒ **这个 bug 比它看起来严重**：`BUSTURN` 一垃圾，连续写之间就多插约 15 个 FMC 周期，
整屏 flush 慢 3.3×；而且它**跨启动不确定**（取决于那次栈上残留了什么）：
之前量到的"六档与理论逐档吻合"只是因为那一次栈垃圾恰好是 0。
**也就是说，这个固件（包括我改动之前的版本）一直存在"某些次启动 flush 慢 3 倍"的可能。**

修法（两处，都已改）：`lcd_fmc_write_timing_apply()` 与 `lcd_init()` 里的
`fmc_read_handle`/`fmc_write_handle` 全部改成 `= {0}` 初始化并显式把
`BusTurnAroundDuration`/`CLKDivision`/`DataLatency` 写 0。

修后复核：
- `BWTR1 = 0x0FF00402`（BUSTURN=0），纯整屏推送 **4.89 ms = 31.81 ns/px = 7.0 FMC 周期**（与理论精确吻合）；
- 六档扫描重新复现（5/5 = 7.68 / 2/4 = 4.89 / **1/4 = 4.19 ms**，三档自检全 OK）；
- **连续两次独立重启后基准都是 17.78 ms / 56.3 FPS**（两次差 303 cycles / 7.11 M）⇒ 不确定性消除。

⚠ 顺带说明：`CLKDIV`/`DATLAT` 读回仍是 0xF —— 那是异步 SRAM 模式下用不到的字段、
复位值就是 0xF，HAL 不写它们；本模式下无影响（实测已与理论吻合）。

**本轮构建**：`text 1571188 B / data 3896 / bss 81296`，FLASH 75.11%（余 ~424 KB），DTCM 23448 B。
（DTCM 比步骤 4+5 那一版少 1816 B，主要是 `g_objtime` 那张 2016 B 的表随 OBJTIME 一起消失。）

**本轮未做**（按之前给的顺序，等板上数字再定）：`InvalidateOldNew` API（app 现在拖一下
滑块就 `Invalidate(ctx->root)` 变全屏重绘）、MDMA + 第二块 band buffer 去重叠 flush、
遮挡剔除/子树剔除、`GY_Isqrt` 半径 ≥256 溢出、`ArcThick` 按角度裁剪扫描、`DrawImg` 逐像素裁剪。

**板上怎么量（步骤 1 / 3，都不需要重新编译）**：

```bash
# 先确认固件在跑、主循环没停
python tools/read_vars.py g_boot_magic g_loop_count g_shell_rc

# 步骤 1：亮度叠加到底值多少 —— g_dim_mode 是可读写全局量（0=不调暗 1=软件查表）
#   ① 调暗开着（出厂默认）测一遍
python tools/bench.py 60
#   ② 关掉调暗再测一遍，差值就是这次软件叠加的净代价
python tools/read_vars.py g_dim_mode          # 确认当前是 1
#    用 probe-rs write 把 g_dim_mode 写 0（地址见 read_vars 输出），然后
python tools/bench.py 60

# 步骤 3：FMC 写时序二维扫描（每档 apply 后自动重测整屏 flush；--verify 会同时跑 A2 双自检）
python tools/fmc_sweep.py --verify 5/5 5/4 3/4 2/4 1/4
#   扫完恢复到一个验证过的档位：
python tools/fmc_sweep.py --verify 5/5
```

判据：`g_flush_cyc` 越小越好，且它应当与 `(ADDSET+DATAST+1)` 成正比；
**通过与否只看双自检**（`g_ramp_mismatch` 与 `g_gram_mismatch` 必须都是 0）。
`g_frame_mirror` 现在默认 0，`fmc_sweep.py --verify` 会临时打开它（A2 需要）。

### cache 策略固化 + DMA ↔ D-Cache 一致性台架（2026-10-04）

为引入 TF 卡（SDMMC + IDMA，SD 卡本身是**总线主设备**，绕过 CPU 的 L1 D-Cache）
先做的两项前置工作。

#### 一、L1 加固：cache 策略固化为不可运行期更改

**删除了** `g_cache_wb`（切换写回/透写）与 `g_cache_clean`（手动刷 cache）两个开关。

| 删除理由 | 依据 |
| --- | --- |
| 实测无收益 | 关 FORCEWT 对填充 1.00–1.10×（微基准 case 8/9），写缓冲本来就把连续 store 合并了 |
| 留着只有风险 | 写回模式下 **SWD（probe-rs）绕过 D-Cache**，抓屏 / 读 AXI 变量会读到陈旧值 |
| 无外部依赖 | 删除前已全库确认 `tools/` 下没有任何脚本引用这两个变量 |

**替代保证**：D-Cache 常开 + **FORCEWT 恒为 1**（`sys.c` 的 `sys_cache_enable()`）。
由此得到一条**结构性**保证 —— **CPU 写一定直达内存**，
SWD 抓屏 / 读变量永远拿最新值，不依赖"谁记得先 clean"。

> ⚠ 但 FORCEWT **只覆盖"写"这一个方向**。DMA 写内存后，CPU 的 D-Cache 里可能仍是
> **旧副本**，此后 CPU 读到陈旧数据。这条风险**不可能靠 FORCEWT 消除** —— 见下。

#### 二、DMA 双向台架：把上面那条论断变成板上实测

`src/dma_bench.c`（+ `src/dma_bench.h`、`tools/dma_check.py`）。
用 **DMA1 MEM2MEM**（`HAL_DMA_Init` 在 `Direction == DMA_MEMORY_TO_MEMORY` 时自动把
Request 强制为 `DMA_REQUEST_MEM2MEM`，见 `stm32h7xx_hal_dma.c:418`）做 CPU↔DMA 双向搬运比对。

**为什么用 DMA1 MEM2MEM 而不是等 SD 卡**：它不需要卡、不占引脚、不用中断，
而缓存语义与 SDMMC IDMA **完全一致**（都是不做 cache 维护的总线主设备）。
本工程此前已为此吃过亏（阶段 3 的"以为在防、实际漏了"），所以先量清楚再动手。

**硬件约束（配错就静默搬不到数据）**：

| 约束 | 原因 |
| --- | --- |
| 缓冲区**不能**在 DTCM | **DMA1/DMA2 无法访问 TCM**（TCM 是 CPU 专有，不在总线矩阵从设备侧）。⇒ 放在 **D2 域 SRAM1（0x3000_0000）**，与 DMA1 同域，且不与 heap1（97%）抢空间 |
| 必须是 **32 B 对齐** | Cortex-M7 cache line = 32 B；不对齐会让 `SCB_InvalidateDCache_by_Addr` 误伤相邻数据的脏行 |
| MEM2MEM **不能用循环模式** | HAL 头文件明确注明 |

**实测结果**（`g_dma_*` 诊断量，SWD 读；缓冲区实测落在 `src=0x30000100 / dst=0x30000000`）：

| 用例 | 内容 | 结果 | 判据 |
| --- | --- | --- | --- |
| **①** | CPU 写 → DMA 读 | 失配 **0 / 64** | `FORCEWT=1` ⇒ CPU 写直达内存，**DMA 读方向无需 cache 维护** |
| **②** | DMA 写 → CPU 读（不维护） | 失配 **64 / 64** | **风险真实存在**：CPU 命中的是旧副本，且**完全静默** |
| **③** | DMA 写 → CPU 读（invalidate） | 失配 **0 / 64** | `SCB_InvalidateDCache_by_Addr` 能完整补救 |

②③ 成对才有意义：只有「②失败 + ③通过」才能同时证明"风险真实"且"补救有效"。
逐字位图（`g_dma_b_fresh_lo/hi`，每 8 字 = 一条 32 B line）实测 ② 的 8 条 line
**全部陈旧**；三轮连测稳定 64/64（首次上电那轮出现过 56/64 —— 偶发有一条 line
被无关流量按组相联换出。**量值有抖动 ⇒ 更危险，而不是更安全**）。

环境交叉验证（固件自报 vs 直接读 PPB）：`SCB->CCR = 0x00070200`（bit16 DC=1）、
`SCB->CACR = 0x00000005`（bit2 FORCEWT=1），与固件上报一致。

**⇒ 工程结论**：接 SDMMC/IDMA 之前应划**专用非缓存 DMA 区**（L3：
重排 MPU region 编号使 SRAM1 前段 `NOT_CACHEABLE`），
而**不是**依赖每个调用点手工 invalidate —— 后者在本工程已有失败先例。

**回归**（全绿）：`g_cache_wb`/`g_cache_clean` 符号已从 ELF 消失、A2 双失配 0、
`g_fault[0..8]` 全 0、中文输入法 2/2、光标形状 16×2 px / 半周期 297.5 ms。
DMA 台架的额外成本：FLASH 74.50% → **74.74%**，SRAM1 新增 **512 B**（缓冲区本身）。

#### ⚠ 还没做的事：L2 / L3（详见独立路线图）

本节完成的是 **L1（策略固化）+ 一致性台架**。后续两项**均未开始**，
执行依据（引脚、MPU region 重排方案、判据、12 条硬约束）已单独立项：

> **📄 `E:/stm32-tetris/docs/TF_CARD_CACHE_ROADMAP.md`**

| 项 | 内容 | 状态 | 触发条件 |
| --- | --- | --- | --- |
| **L2** | SDMMC 数据通路（先**轮询模式**避开缓存问题 → 再接 FatFs → 最后 IDMA） | **未开始** | 实体 TF 卡到货 |
| **L3** | 专用非缓存 DMA 区（重排 MPU region 编号，SRAM1 整块 `NOT_CACHEABLE`） | **未开始** | 上 IDMA 高速模式之前；**不依赖卡，可提前做** |
| L4 | 手工 clean/invalidate（备选，有失败先例） | 不推荐 | — |

**L3 的验收判据是天然闭环**：做完重跑 `python tools/dma_check.py`，
用例 ②（DMA 写 → CPU 读，不维护）应从现在的 **64/64 变成 0/64** —— 这就是唯一判据。

> 相关事实：`src/` 下**没有任何 SD/MMC 驱动**，全库**没有文件系统**；
> HAL 的 `stm32h7xx_hal_sd.c` / `hal_mmc.c` / `ll_sdmmc.c` 在仓库里但**未加入构建**。
> SDMMC1 引脚（`PC8-12` / `PD2`）经核实**全部空闲**。

### 文本光标（2026-10-03）—— 原版静态竖线 → 闪烁下划线

**原版行为**：`YMGUI_TextInput` / `YMGUI_EditView` 各画一条 **2px 宽、常亮的竖线**（仅编辑态显示），
**全库没有任何闪烁实现**（`Demo/`、`project_Demo/`、`DEVLOG`、`API.md` 都检索过，别的项目也没有）。
不符合中文输入习惯，故改成**下划线 + 闪烁**（样式 C：一个字符宽、2px 高、贴行底）。

改动都在库里，两个文本控件共用一份实现：

| 文件 | 改动 |
| --- | --- |
| `CORE/YMGUI_DrawText.c` + `CORE/YMGUI_Font.h` | 新增 `YMGUI_Draw_Caret()`：样式 0=竖线(原版) / 1=下划线；宽取光标**后**那个字符（串尾取前一个）⇒ 中文 16px、英文 8px；"灭"半周期直接返回 0 |
| `WIDGET/YMGUI_TextInput.c`、`WIDGET/YMGUI_EditView.c` | 两处原来的常亮竖线改调 `YMGUI_Draw_Caret()` |
| `HAL/YMGUI_Hal.c` + `.h` | 新增 `YMGUI_CaretTick()`，**挂在已有的 `YMGUI_Inject_Tick(ms)` 上**（不开新定时器）；翻转时置脏焦点控件 |

运行期可调（SWD 直接写，改外观不用重编译）：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `g_ymgui_caret_style` | `1` | `0`=竖线（原版）　`1`=下划线 |
| `g_ymgui_caret_blink_ms` | `300` | 半周期毫秒数（亮 300 / 灭 300）；`0`=不闪，等价原版 |
| `g_ymgui_caret_on` | 自动翻转 | 当前半周期是否显示（只读相位） |

⚠ **闪烁能生效的关键是置脏**：只翻变量屏幕不会变，必须在翻转时 `YMGUI_Obj_Invalidate()` 焦点控件。

验收用 `tools/caret_check.py`（不靠肉眼）：

| 项 | 方法 | 实测 |
| --- | --- | --- |
| 形状 | 冻结闪烁后"亮/灭"两帧做像素差分 | 差异区 **16×2 px**（一个汉字宽 × 2px 高）= 下划线 |
| 周期 | `g_ymgui_caret_ms ÷ g_ymgui_caret_flips`（两个量都在设备时间里累加） | **300 ms**（2026-10-03 修余数后，60 s 采样 60688/202=300.4） |

⚠ 测设备端周期不能用墙上时间：**SWD 读变量会暂停内核，DWT 跟着停**。实测连续读数时，
墙上 25 s 设备只走了 17.1 s（丢 31%），会把 300 ms 误测成 ≈460 ms。

⚠ 光标 y 随**所在行**变化（行高 18px），差分位置不同≠bug：正文为空时点 `(70,215)`
光标落在第 1 行（下划线 y=209），有文本"中过"时在行尾（第 0 行，y=191、x 紧跟最后一个字）。
判断形状只看**宽×高**（16×2），别把 y 当异常。

### 界面语言中英切换 + W25Q128 持久化（2026-10-04）

设置页新增「界面语言」卡片（**样式 B：分段选择器**），切换后整个 UI 在中/英之间切换，
**选择会掉电保存**。

#### 一、为什么文案机制是"以中文原文为 key"

原有 254 条文案全是硬编码中文（`tools/count_cn_strings.py` 实测，已剔除注释）。
若改成"每条一个枚举 ID"，要动全部调用点、还要单独维护 ID 表，改动面等于文案总数。
改用 `T("设置")` 原文查表后：

| 好处 | 说明 |
| --- | --- |
| 中文模式零开销 | `PhoneLang_Tr()` 在中文下**直接返回原文，不查表** |
| 不必给结构加字段 | `PhoneApps_Get(id)->title` 本来就指向中文原文，包一层 `T()` 即可（16 个应用标题零改动） |
| 查不到不空白 | 表里没有就返回原文 —— 宁可显示中文，也不要空白/乱码 |

#### 二、改动文件

| 文件 | 作用 |
| --- | --- |
| `project_Demo/phone_shell/phone_lang.c/.h` | 语言状态 + 翻译表 + `T()`。**不依赖 flash**，所以桌面构建同样可用；持久化靠回调注入 |
| `src/app_config.c/.h` | 把语言存到 **W25Q128 扇区 240（`0x000F0000`）**，带 magic `"YCON"` + CRC32 双校验 |
| `apps/settings.c` | 语言卡片（分段选择器）+ 六项布局重排 + `settings_retranslate()` |
| `phone_shell.c` / `phone_desktop.c` | 阶段 1 文案包 `T()`；新增 `PhoneHost_RefreshLanguage()` 与 `PhoneDesktop_Retranslate()` |

#### 三、设置页六项布局（原五项，空余只有 71 px）

| 项 | 原 (y, h) | 新 (y, h) |
| --- | --- | --- |
| 无线网络 | 65, 60 | **65, 50** |
| 海蓝色壁纸 | 135, 60 | **133, 50** |
| **界面语言** | — | **201, 50**（新增） |
| 预览亮度 | 205, 79 | **269, 62** |
| 设备名输入 | 293, 30 | **347, 28** |
| 关机按钮 | 334, 43 | **391, 40** |

底部导航栏固定 `y=448` 不动；`391+40 = 431`，留 17 px。
⚠ **改布局必须同步 `phone_selftest.inc` 的坐标断言**（`device_name_input->area.y` 293 → 347），
否则自检失败。**已同步。**

#### 四、最容易做错的一点：文案取用分两类

| 类别 | 例子 | 语言切换后 |
| --- | --- | --- |
| **绘制期取值** | Dock 标签（每次重绘读 `PhoneApps_Get(id)->title`） | **自动跟随**，包 `T()` 即可，无需干预 |
| **创建期设定** | 桌面 16 个图标标签、各 APP 内所有 Label/Button | **必须显式重设** —— 文字是创建时**拷进对象**的副本（`PhoneText.value[]`），不是指针 |

后者是绝大多数，所以有 `PhoneHost_RefreshLanguage()`：
设置页切换后立即调用，重设桌面小组件 / 最近任务页 / 应用页标题；
16 个桌面标签与长按菜单转交 `PhoneDesktop_Retranslate()`。
各 APP 内部的文案由它们自己的 `show()` **惰性刷新**（比对 `PhoneLang_Version()`，变了才刷）。

#### 五、持久化：为什么放 W25Q128 而不是内部 Flash

| 维度 | W25Q128（选定） | 内部 Flash |
| --- | --- | --- |
| 容量 | 16 MB，已用字库 982 KB + 2 扇区 ⇒ **约 15 MB 空闲** | 2 MB 已用 **74.7%** |
| 通路 | `qspi_write()` 已成熟：自动进出映射 + 扇区读-改-擦-写回 + **内容未变则跳过擦除** | 需自己写擦写逻辑，且写时阻塞取指 |
| 寿命 | 低频改动 + 跳过擦除 ⇒ 等于无限 | 同左，但空间紧张 |

扇区选址 `0x000F0000`（扇区 240）：落在**字库末尾 `0xEFC00` 之后、擦写回环 `0xFE000` 之前**的空隙，
不撞任何既有地址（`QSPI_VERIFY_LOOP_ADDR=0xFE000` / `QSPI_HOUSEKEEP_ADDR=0xFF000`）。

⚠ **本模块最大的坑：写完必须重新进入映射模式。**
`qspi_read()`/`qspi_write()` 内部都会 `qspi_ensure_indirect()`，即**先退出 memory-mapped**；
而中文字模正是从映射区 `0x9000_0000` 读的。Load/Save 末尾都补了 `qspi_enter_mmap()`
（`font_provision.c:167` 是同样处理）。少了它 ⇒ **整屏中文立刻消失**。

#### 六、验收

`python tools/lang_check.py` —— 三层证据，全是确定性整数结论：

| 层 | 方法 |
| --- | --- |
| ① 交互 | 点设置页 English 段，中/英两帧**像素差分**（不是肉眼看） |
| ② 落盘 | **直接读 W25Q128 扇区 240 原始字节**，校验 magic + lang + CRC32（不依赖固件自报） |
| ③ 掉电重启 | `probe-rs reset` 后重读，语言应仍是 English |

坐标依据（推自源码）：设置图标 id=3 ⇒ `page0, x=23+3×75=248, y=238`，中心 `(272,263)`；
应用页展开后是全屏 ⇒ 设置页坐标即屏幕坐标；语言段中心 `(208,226)` / `(259,226)`。

### RAM 预算（实测峰值）

> 数据来源：`build/ymgui-h743.map` 的段地址（静态部分，链接期即定死）+ 板上读
> `g_h0/g_h1` 的 `peak`（堆峰值）。**改了 `.ld` 或 IME 池之后这张表会变，请按
> `docs/` 里的实测值重算，不要沿用。**
> 物理总量 = 128 + 512 + 128 + 128 + 32 + 64 = **992 KB**。

| 区 | 容量 | 静态占用 | 运行时预留 | **剩余** |
|---|---:|---:|---|---:|
| DTCM | 128 KB | `.data` 3.8 + `.bss` 19.3 = **23.1 KB** | sbrk 16 KB · heap0 56.9 KB · 栈 32 KB | **heap0 余 24.9 KB**（峰值 31.9/56.9 = 56%） |
| AXI SRAM | 512 KB | 0 | 帧镜像 300 KB + heap1 212 KB | **heap1 余 142.6 KB**（峰值 69.4/212.0 = 33%） |
| SRAM1 | 128 KB | `.bss_dma` **0.5 KB** | — | **127.5 KB**（最大一块连续空区） |
| SRAM2 | 128 KB | `.bss_ime` **59.8 KB** | — | **68.2 KB** |
| SRAM3 | 32 KB | band 缓冲 **20.0 KB** | — | **12.0 KB** |
| SRAM4 | 64 KB | **0** | — | **64.0 KB** |

- 静态合计 **103.4 KB**；加堆峰值 101.3 KB、帧镜像 300 KB ⇒ **已承诺 504.7 KB（50.9%）**。
- heap0 峰值 **31 920 B（31.9 KB）**、heap1 峰值 **71 064 B（69.4 KB）** 是**双档堆**的实测高点
  （`board_alloc.c` 的 `peak`，2026-10-07 板上读 `g_h0/g_h1`：`python tools/heap_peak_check.py`），
  不是静态占用，所以"剩余"那一列是峰值之后的余量，不是恒定可用量。
- heap1 的三大块（删索引后）：draw buffer 30 KB、应用快照（1/4 降采样后 17.5 KB/项，已限项）、
  IME 单字候选数组 **7.2 KB**（614 × 12 B）。**原先最大的那一块 —— IME 单字索引
  85.4 KB（7291 × 12 B）—— 已经删掉了**，见下面"单字词典 entries[] 索引"一节。
- ⚠ **AXI 起始那 300 KB 帧镜像是"占用中"不是"空闲"**：只服务于 SWD 抓屏
  （`ymgui_port.c:468` 用它把分条像素拼回整屏再读出来），显示链路不依赖它。
  真要腾空间它是最大的一块，代价是失去抓屏能力，别默认它能拿来用。

**更新记录（2026-10-07）**：本表原先写 `.bss 21.8 KB`、`heap1 205.2/212 KB（97%，只剩 ~7 KB）`、
`SRAM3/SRAM4 = 0/0`。三处都已过期 —— `.bss` 现为 19.3 KB；heap1 先因
`dim_buffer` 改按需分配 + 背光换硬件 PWM 从 205.2 降到 176.1 KB（见
`docs/MEMORY_ARCHIVE.md` "dim_buffer 改成按需分配"一节），再因**删掉单字词典的
`entries[]` 索引**降到 **69.4 KB（33%）**；SRAM3 已被 DMA 双缓冲的第二块 band
缓冲占掉 20 KB。那句"加任何大对象前先重算"的提醒**依然成立**，但不再是"只剩 7 KB"
这种紧绷状态 —— 现在 heap1 余 142.6 KB。

### 单字词典 `entries[]` 索引的删除（2026-10-07）

`pinyin_gb2312.bin` 的 payload 早就在片上 flash（`YMGUI_IME_CHARS_ROM=1` 零拷贝），
但 loader 仍然 `GY_malloc1(count × 12 B)` 在大对象池建了一份完整索引 ——
**索引里没有任何 payload 之外的新信息，纯粹是 (py, ch, frequency) 的搬运冗余**。

| 项 | 改前 | 改后 |
|---|---:|---:|
| `entries[]` 索引 | 87 492 B（7291 × 12） | **0**（按序号现场解记录） |
| `s_char_matches` | 29 164 B（7291 × 指针） | **7 368 B**（614 × 12 B 实体） |
| **heap1 峰值** | **180 376 B（176.1 KB，83%）** | **71 064 B（69.4 KB，33%）** |

实现要点：

1. 新增访问抽象 `CHAR_AT(index, dst)` —— CHARS_ROM 分支走 `charEntryAt()`（从片上 flash
   现场解 12 B 记录），其余分支仍是 `dst = s_full_dict[index]`。三个词典分支共用，
   **调用点一处没改**，这也是将来上 XIP（外部 flash 同样"只有 payload、没有索引"）的同一把钥匙。
2. **`loadCharDictionary` 的逐条校验保留**（`memchr` + 桶归属），改成只校验不落地。
   删索引不能连校验一起删，否则坏词典会从"返回 0、单字候选安静关闭"变成"运行时野指针"。
3. `s_char_matches` 容量从"全词典条数"改成"最大桶跨度 614"。这不是估计而是**构造性上界**：
   `resetCharSearch()` 一次只扫 `query[0]` 那一个桶，匹配数必然 ≤ 桶跨度。
   脚本 `tools/ime_char_index_check.py` 全枚举 702 个查询验证了这个上界（最大 603，桶 `'y'`）。

验收（板上实测，`python tools/heap_peak_check.py`）：

| 判据 | 结果 |
|---|---|
| heap1 峰值 | **71 064 B（69.4 KB，33%）**，改前 180 376 B |
| `tools/ime_probe.py nihao zhongguo` | **成功 2/2**（候选内容与改前一致） |
| A2 逐像素自检 | `g_gram_mismatch = 0`、`g_ramp_mismatch = 0` |
| `g_fault.magic` | 0（从未内核故障） |

FLASH 只增加 **24 B**（1 576 644 → 1 576 668 B）。详见
`docs/IME_PHRASE_DICT_FEASIBILITY.md` 的"更新（2026-10-07 晚）"一节 ——
这一步把 heap1 余量从 35.9 KB 抬到 142.6 KB，直接改变了词组词典的账本。

### 阶段 5 的 RAM 腾挪：IME 静态池搬进 SRAM2

开 `YAOMI_IME=1` 后，DTCM 的 `.bss` 一下涨约 60 KB（IME 的几个静态池：  
`g_words[8192]` 32 KB、`s_compose_states` ~19.5 KB、`s_user_stats`、`s_composites`、  
用户词组/英文学习链表）。后果是 heap0 区间**倒挂**（`_heap0_start > _heap0_end`），  
症状是"任何小对象都申请不到"，但界面还能显示，很容易被误读成别的问题。

处置三步（都属于：非此不可，且每处都留了注释说明为什么必须这样）：

1. `ime_candidates.inc` 给这 6 个池加 `IME_BSS_ATTR`
   ```c
   #if defined(YMGUI_IME_BSS_SECTION_NAME)
   #define IME_STR2(x) #x
   #define IME_STR(x)  IME_STR2(x)
   #define IME_BSS_ATTR __attribute__((section("." IME_STR(YMGUI_IME_BSS_SECTION_NAME))))
   #endif
   ```
   CMake 传 `YMGUI_IME_BSS_SECTION_NAME=bss_ime`（**不带引号不带点**），再在源码里 stringify。  
   直接传 `"\"bss_ime\""` 的写法在 Windows 上会被转义吃掉，实测无效。
2. 链接脚本加 `.bss_ime (NOLOAD) > SRAM2`，**必须放在 `.bss` 之前** ——  
   放在之后会被 `*(.bss*)` 的通配先吞掉，链接器不会报错，只是静静地不生效。  
   另外注释里别出现 `*/`，会提前闭合注释导致 ld 语法错误。
3. `board_startup.c` 里 `_sime_bss.._eime_bss` 自行清零（NOLOAD 段没人帮你清）。

⚠ **改 `.ld` 之后 `cmake --build` 有时不触发重链接**，要删掉 ELF 再构建，  
否则地址对不上，读出来的一堆数据看着像"程序坏了"。

### 阶段 3 状态（✅ 完成）

**验收判据 —— 触摸驱动**：

| 量          | 值                | 含义                                                     |
| ---------- | ---------------- | ------------------------------------------------------ |
| `g_tp_rc`  | 0                | `gt9xxx_init()` 成功                                     |
| `g_tp_pid` | **`0x31313538`** | **寄存器 `0x8140` 读回 ASCII `"1158"`（GT1158）**，计划书 A3 判据达成 |
| `g_dwt_hz` | `0x17D78400`     | DWT 时基 400 MHz，`YMGUI_Inject_Tick` 正常注入                |

引脚按本机实测改（厂商资料包是 V1.2，本机是 V1.1）：  
`ctiic.h` SDA `PB12→PB14`；`gt9xxx.h` RST `PB13→PB12`、INT `PC4→PB13`。

**验收判据 —— 事件注入（端到端）**：

| 量                   | 值     | 含义                                                 |
| ------------------- | ----- | -------------------------------------------------- |
| `g_inject_ok`       | 持续增长  | **`YMGUI_Inject_Pointer()` 每次都能正常返回**              |
| **`g_fault.magic`** | **0** | **从未发生过任何内核故障**（故障快照为空）                            |
| `g_synth_done`      | 1     | 合成点击脚本跑完（按下 + 抬起）                                  |
| **`g_click_cnt`**   | **1** | **按钮的 `Clicked` 回调真的被调用了** —— 注入→命中→派发→回调→标脏，整条链路通 |
| `g_frame_seq`       | 1 → 3 | 点击确实产生脏区并触发重绘（按下帧 + 抬起帧）                           |

**合成触摸自测**：`g_synth_x/g_synth_y` 默认落在按钮中心 `(160,114)`  
（按钮建在 `(90,90,140,48)`）。用 `probe-rs write` 把 `g_synth_test` 置 1  
即触发一次脚本化的 按下+抬起，**不需要真的碰屏**。  
之所以做这个钩子：本工程刚因为"故障被当成死循环"走了一大圈弯路，  
**能给出确定整数结论的自测，比"看图对不对"可靠得多**。

### 阶段 3 的根因：**newlib 的 sbrk 堆与 YMGUI 的 heap0 是同一块内存**

现象：`YMGUI_Inject_Pointer()` 一被调用主循环就停住（`g_poll_stage` 停在 3）。  
**看起来像死循环，实际是 BusFault 升级成的 HardFault** —— 厂商模板的 fault handler  
是 `while(1)`，与死循环外观完全一致，所以最初把排查方向全找偏了  
（栈 / 递归 / 环 / printf 四个方向逐一实测排除都无效：**故障在那些代码之前就发生了**）。

定位过程（关键：不再猜，直接读现场）：

1. 复现卡死后读内核故障寄存器：  
   `HFSR=0x40000000`（FORCED）、`CFSR=0x00008200`（bit9 `PRECISERR` = 精确数据总线错误）、  
   **`BFAR=0x0D38353F`（出错的数据地址是个野地址）**。
2. 用 GDB（`probe-rs gdb` + `arm-none-eabi-gdb`）取栈上的异常帧：  
   `pc=0x0800BCCE`、`r3=0x0D383531`。
3. `objdump` 反汇编该处：`800bcce: ldrb r3,[r3,#14]` —— 即 `hitRecD()` 在读  
   `obj->state`。`0x0D383531 + 0x0E = 0x0D38353F` 与 BFAR **完全吻合**；  
   且这是 `hitRecD(obj = ctx->root)` 的顶层调用（`lr` 指回 `hitRec`）。
4. 读 `s_inject_ctx` 与 `s_ctx`（同一个 `0x2000_04C0`）：  
   **`ctx->root = 0x0D383531`、`ctx->top_layer = 0x2000_050A` 都是垃圾**，  
   而 `ctx->disp` 正确、其余字段为 0 ⇒ **ctx 创建之后被覆盖**。
5. dump 堆区原始内存：`0x2000_04B8` 起是一段 **ASCII 串 `"CTP ID:1158\r\n"`**，  
   正好压在 `ctx->root` / `ctx->top_layer` 上。
6. 全项目搜 `printf` → 命中 `gt9xxx.c:132` 的 `printf("CTP ID:%s\r\n", temp)`；  
   再查链接符号 → **`_end` = `_heap0_start` = `0x2000_04B0`（同一地址）**：  
   `printf` 首次调用时 newlib 用 `malloc` 经 `sbrk` 给 stdout 分配行缓冲，  
   **拿到的就是 YMGUI 的 heap0**。

完整因果链：

```
lcd_init() 里 printf("LCD ID:%x")        → stdout 行缓冲落在 0x2000_04B0（此时堆还没用，无害）
YMGUI_Creat_Ctx_Creat → board_alloc0()   → ctx 拿到 0x2000_04C0（与上面同一块内存）
gt9xxx_init() 里 printf("CTP ID:%s")     → 写回同一个 stdout 缓冲 0x2000_04B8
                                          → 盖掉 ctx->root / ctx->top_layer
YMGUI_Inject_Pointer → hitRecD(ctx->root) → 读野指针的 obj->state
                                          → 精确总线错误 → HardFault → 主循环假死
```

（这也解释了为什么 `YMGUI_Refresh` 一直没崩：首帧渲染完 `inv_cnt=0`，  
`YMGUI_Refresh` 直接早返回，**不再读 `ctx->root`**。）

**修复（三层，互相独立）**：

1. **两个堆彻底分离**（根因层）：链接脚本新划出 16 KB 的 `_sbrk_start.._sbrk_end`  
   保留区，`_heap0_start` 从它之后开始；并自写 `_sbrk()`（`src/board_startup.c`），  
   从该保留区取内存、池耗尽返回 `-1`，**绝不越界**。  
   实测：`_sbrk_start=0x2000_0340`、`_heap0_start=0x2000_4340`，不再重合。
2. **去掉厂商驱动里的 printf**（触发层）：`gt9xxx.c:132`、`lcd.c:684`、  
   `mpu.c` 的 `MemManage_Handler` 共 4 处。现在 ELF 里  
   **`printf` / `malloc` / `__sf` / `_impure_ptr` 全部不存在**，stdio 彻底脱离链接。  
   ID 值不丢：仍可用 SWD 读 `g_tp_pid` / `g_lcd_id`。  
   （`sbrk` 保留区与自写的 `_sbrk` 仍然留着作安全网：将来若有人再引入 printf/malloc，  
   它会自动用那块保留区，而不会再去踩 heap0。）
3. **加故障现场快照**（诊断层）：`src/board_fault.c` 把 CFSR/HFSR/MMFAR/BFAR/SHCSR 等  
   落进 `g_fault`，供事后 SWD 读。**以后再出故障不会再被误判成"死循环"**。

修复后：`ctx->root = 0x2000_4420`、`ctx->top_layer = 0x2000_4468`（都在 heap0 内，合法），  
`g_fault` 全 0，`g_inject_ok` 稳定增长。

### 阶段 3 排查中先走错、后已回退的四个方向

| 曾怀疑                           | 实测                | 处置                             |
| ----------------------------- | ----------------- | ------------------------------ |
| 栈溢出（原 8 KB）                   | 加大到 32 KB 后**仍卡** | 32 KB 保留作裕量，注释已更正为"**不是**故障原因" |
| `hitRec` 无限递归                 | 加深度上限 64 后**仍卡**  | **已完全回退**为库原样                  |
| `GetAbsArea` 的 parent 链成环     | 加迭代 guard 后**仍卡** | **已完全回退**为库原样                  |
| 库的调试打印（`YMGUI_DEBUG_PRINT` 等） | 关掉后**仍卡**         | 开关保持关闭（**因为不需要**，注释已更正）        |

这四条当时留下的注释里写的"关掉就没问题了""说明对象树存在环"等因果**都是错的**，  
现已全部改写为符合实测的表述 —— 不留未经证实的归因。

### 阶段 4 状态（✅ 完成）

新增 `src/qspi_port.c/.h`：indirect 读 / 写 / 擦除 + memory-mapped 进出 + 阶段 4 验收；  
MPU 新增 XIP 只读区（`third_party/BSP/MPU/mpu.c` 的 region 0）。

**验收判据（计划书 §6.6，实机实测）**：

| 量                             | 值                | 含义                                                    |
| ----------------------------- | ---------------- | ----------------------------------------------------- |
| `g_qspi_rc`                   | 0                | QSPI 初始化成功                                            |
| `g_qspi_jedec`                | **`0x00EF4018`** | 0x9F 读回：华邦 EFh + 40h + 18h(16 MB)                     |
| `g_qspi_id90`                 | `0xEF17`         | 0x90 读回厂商/器件 ID                                       |
| `g_qspi_sr_snap`              | `0x0200`         | SR1=0x00 / SR2=0x02 ⇒ **QE 位已置**（IO2/IO3 可当数据线）       |
| `g_qspi_ker_sel`              | 0                | QSPISEL = HCLK3 ⇒ ker_ck 200 MHz；预分频 1 ⇒ **100 MHz**  |
| `g_qspi_mm_ok`                | 1                | 进入 memory-mapped 成功，且稳态停在映射模式                         |
| `g_qspi_cmp_blocks` / `bytes` | **256 / 65536**  | 每 64 KB 块首 256 B ⇒ **覆盖整个 16 MB 地址空间**                |
| **`g_qspi_cmp_mismatch`**     | **0**            | **XIP 与 indirect 逐字节完全一致**（首个失配 = 无）                  |
| `g_qspi_loop_ok`              | 1                | 擦写回环：擦 → 写 → indirect 读回 → **退出映射再重进** → XIP 读回，两次都一致 |
| `g_qspi_loop_w0/r0/x0`        | 全是 `0xA8A7A6A5`  | 写入值 = indirect 读回值 = XIP 读回值                          |
| `g_qspi_err_cnt`              | 0                | 无读失败                                                  |


**二次启动** `g_qspi_erase_cnt = 0` ⇒ 走了"内容已与目标一致就不擦写"的分支，  
每次上电不会白白消耗 Flash 寿命（读-改-擦-写回逻辑正确）。

**没有影响其它链路**：`g_ramp_mismatch` / `g_gram_mismatch` = 0、`g_tp_pid` = `"1158"`、  
`g_inject_ok` 持续增长、`g_fault` 全 0，抓屏图与阶段 3 **像素完全一致**。

### 阶段 4 的四个坑

**① 计划书 §6.2 的三条都预先规避了**

自己配 NCS（PB10 → AF9，HAL 的 QSPI 初始化不含片选）、DR 一律按 **8 位**访问、  
事务**一次配完**（顺序 `DLR → CCR → AR`）—— 都在 `src/qspi_port.c` 的注释里标了坑号。

**② 我自己转录参照实现时写错了数据模式位（真实故障，已修）**

`qspi_read_sr` / `qspi_write_sr` / `qspi_read_jedec` 三处的 mode 参数我写成了  
`(0u << 6) | ...`，而 **`mode[7:6]` 是数据模式，`0` = 无数据阶段** ——  
函数却按"间接读 1 字节"配了 `FMODE=01` + `DLR=0`。后果是控制器等一个  
**永远不会被采样的数据阶段**：`SR.BUSY` 恒为 1、`SR.FLEVEL` 恒为 0，  
所有"读状态 / 读 ID"全部超时（`g_qspi_rc = 1`）。

现象与"片选悬空 / 芯片没焊"**完全一样**，极易误判成硬件问题。区分手法：

- `exit_qpi`（**无数据阶段**的命令）**成功** ⇒ 控制器、时钟、引脚都正常；  
  只有**带数据阶段**的事务卡住 ⇒ 问题在数据阶段或 Flash 侧，不在控制器。
- 读回 QUADSPI 寄存器：最后的 `CCR = 0x04000135` 是 `FMODE=01`（间接读）+ 指令 `0x35`，  
  而 `DLR = 0`、`DMODE = 0` —— "要收 1 字节，但数据模式说没有数据阶段"，  
  这个自相矛盾就在寄存器里摆着。

**③ 计划书 §1.2 的 QSPI 引脚顺序需要更正**

§1.2 写的是 `IO0/IO1/IO2/IO3 = PD11/PD12/PD13/PE2`，但 **AF9 的引脚↔信号映射  
由芯片固定，不能靠软件调换**。实测结论是 **IO2 = PE2、IO3 = PD13**（与厂商 `qspi.h` 一致）。

依据不是猜、也不是照抄厂商：0xEB **四线读**把 IO0..IO3 分别当 bit0..bit3，  
若 IO2/IO3 接反，读回每字节的 bit2/bit3 会互换 ⇒ 数据必然错。  
实测 256 块 × 256 B **失配为 0**，证明四线通路正是按 `IO2=PE2、IO3=PD13` 工作的；  
同时也就验证了 IO0=PD11、IO1=PD12。

**④ 顺带修掉一个既有 bug**：`src/ymgui_port.h` 把 `YMGUI_PORT_H` 既当包含卫  
又当屏幕高度宏（`#define YMGUI_PORT_H 480`），编译器报 redefine 警告。  
已把包含卫改名为 `YMGUI_PORT_H_INCLUDED`。

### MPU：XIP 区必须显式加（region 0）

`mpu_memory_protection()` 用的是 `HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT)` ⇒ `PRIVDEFENA=1`，  
**未命中任何 MPU 区域的地址会落到 ARM 默认存储图**，而 `0x8000_0000~0x9FFF_FFFF`  
在默认图里是"可缓存 Normal"。所以 `0x9000_0000` 这条必须显式加：

| 属性             | 取值                       | 理由                                            |
| -------------- | ------------------------ | --------------------------------------------- |
| 权限             | `MPU_REGION_PRIV_RO`     | 只读常量区，误写立即 MemManage                          |
| cache / buffer | 都禁止                      | 每次读真走 QSPI，与 indirect 结果逐字节可比，且不需要任何 cache 维护 |
| XN             | 1                        | 不从 XIP 区取指（这里只放数据）                            |
| 范围             | 256 MB（整个 QSPI bank1 窗口） | `FSIZE=23` 只映射 16 MB，区域按窗口设即可                 |

⚠ 将来若为了字模读性能把它改成可缓存，**必须同时**补上  
`SCB_CleanInvalidateDCache()`（进映射前 / 擦写前后），否则读到陈旧内容。

### 字模等常量数据的灌入属于阶段 5

本阶段做的是**机制**（indirect 读写 + XIP）与**验收**，还没有把 GB2312 字模灌进 Flash ——  
计划书 §7.2 的降配表明确把"字模按 §6.5 上 QSPI"列在阶段 5。  
届时直接用本阶段的 `qspi_write()` 灌入即可：它会自动处理"退出映射 → 读-改-擦-写回"。

### 阶段 2 验证证据（实机）

| 量                               | 值     | 含义                                       |
| ------------------------------- | ----- | ---------------------------------------- |
| `g_port_rc`                     | 0     | 显示端口初始化成功                                |
| `g_ctx_rc`                      | 1     | 上下文与控件创建成功                               |
| `g_flush_calls` / `g_frame_seq` | 2 / 1 | `flush_cb` 被调用、整帧完成                      |
| `g_ramp_mismatch`               | **0** | **读通路有效**（整屏写 `i` 再读回，153600 px 全对）      |
| `g_gram_mismatch`               | **0** | **面板 GRAM 里 153600 个像素与 YMGUI 渲染结果逐一相同** |

最后一项是"像素真的到了屏上"的铁证：整屏回读面板 GRAM，与 `flush_cb` 累积的全帧镜像缓冲逐像素比对，全部一致。  
抓屏图见 `tools/grab_ymgui.py` 输出（黑底 + 标签 "YMGUI on STM32H743" + 蓝色按钮 "Click me"）。

### 阶段 2 最关键的一个坑（**唯一必要的驱动改动**）

**厂商 `lcd_set_window()` 只发 `0x2A`/`0x2B` 设窗口范围，不发 `0x2C`（写 GRAM）。**

参照工程 `stm32-tetris` 的 `set_window` 末尾是发 `0x2C` 的，所以移植时极易漏掉这一步。漏掉的后果很迷惑人：

- **写不进去** → GRAM 里还是旧内容；
- 但 `flush_cb` 累积的**全帧镜像缓冲在 RAM 里，完全正常** → 抓屏图看起来完美；
- 只有做"整屏回读面板 GRAM 比对"才会暴露：`ramp` 自检 153598/153600 全失配。

排查过程中我先后怀疑并**实测排除了**三个方向（都不是根因，代码已回退到厂商原值，避免留下无根据的改动）：

| 曾怀疑                                   | 实测结果                          |
| ------------------------------------- | ----------------------------- |
| FMC `ACCMOD` 模式 D（HAL 默认）vs 参照工程的模式 A | 改成模式 A 后**仍全失配** → 不是根因，已回退   |
| `BCR1` 多出的 bit20/bit7                 | 与参照工程逐位对齐后**仍全失配** → 不是根因，已回退 |
| MPU 的 `XN` 位（厂商 0 / 参照工程 1）           | 两种取值**都通过** → 不是根因，已恢复厂商原值    |
| L1 D-Cache 使能                         | 关掉后**仍全失配** → 不是根因（现已开启，不影响）  |

真正修好它的是在 `src/ymgui_port.c` 里 `lcd_set_window()` 之后补一行 `lcd_write_ram_prepare()`。

### App 冻结事故：字体 fallback 链成环（2026-10-04，已修）

**现象**：界面完全不动，但**没有任何 HardFault**（`g_fault` 全 0、`CFSR`/`HFSR` 全 0、
`DHCSR.S_LOCKUP=0`）—— 内核在跑正常代码，只是永远出不来。

**定位手法（以后卡死照这个顺序来）**：

1. `g_loop_count` 隔几秒读两次：不变就是停了。
2. 比 `g_mark_tick / g_mark_poll / g_mark_refr` 与 `g_loop_count`：主循环每拍依次写这三个标记，
   哪个落后就说明卡在哪一步。当时 tick=2126、poll=2126、**refr=2125** ⇒ 卡在 `YMGUI_Refresh()` 内部。
3. `probe-rs gdb` + `arm-none-eabi-gdb -ex "target remote localhost:1337" -ex bt` 抓栈。
   ⚠ 两个细节：① gdb 服务器要用**后台任务**跑（前台一退出服务器就掉线，gdb 报 error 138）；
   ② 工程没带 debug info，`call` 必须写成 `call (int)fn(...)` 显式强转，否则报
   "unknown return type"。
4. **目标端确定性复现**：`set {char}0x2001F000 = 10` 然后
   `call (int)YMGUI_Font_TextWidthN((void*)0x200000A0, (void*)0x2001F000, 1)`。
   修复前 25 s 超时（124）、PC 停在 `glyphIndex+36`，与真机冻结的 PC 一模一样；
   量字库里有的「中」则瞬间返回 16 —— A/B 对照把根因钉死。

**根因（三层叠加）**：

| 层 | 问题 |
| --- | --- |
| 直接原因 | `resolveGlyph()` 遍历 fallback 链**没有任何环保护**，遇到字库里没有的码点就永远走不到链尾 |
| 触发条件 | `PhoneLocale_Init()` **不幂等**：板级开机被调两次（`main.c:385` 与 `PhoneShell_BoardInit()` 内的 `phone_shell.c:1565`），第二次 `old_fallback = GetFallback()` 拿到的已经是 `&chinese_font`，于是 `chinese_font.fallback = &chinese_font` **自成环**（实测 0x200000A0） |
| 引爆点 | 新加的 `YMGUI_Draw_Caret()` 会量光标处字符的宽度；光标停在**换行符**上时量 `'\n'`（0x0A，字库里没有）⇒ 死循环。原版静态竖线不量宽度，所以这个潜伏缺陷一直没暴露 |

**修复（三层， defence in depth）**：

| 文件 | 改动 |
| --- | --- |
| `project_Demo/phone_shell/phone_locale.c` | `Init()` 幂等：已挂则直接返回；`old_fallback` 只记第一次；链尾恒用原始兜底 |
| `CORE/YMGUI_DrawText.c` `resolveGlyph()` | 加深度上限 `GY_FALLBACK_MAX_DEPTH=16` + `f->fallback == f` 自环直接收尾 |
| `CORE/YMGUI_DrawText.c` `YMGUI_Draw_Caret()` | 控制字符（`< 0x20` 或 `0x7F`）不量宽度，直接用 `cell_w`（换行本来就没有字形宽度） |

**回归**：`chinese_font.fallback = 0`（不再指向自己）、量 `'\n'` 返回 16 耗时 2 s；
A2 ramp/gram 失配 0、`g_fault` 0、IME 2/2、`caret_check.py` 形状 16×2 / 周期 296.8 ms 全过。

### 阶段 1 验证证据（实机）

| 项              | 值                               | 说明                                       |
| -------------- | ------------------------------- | ---------------------------------------- |
| `g_loop_count` | 3 秒内 6 → 13                     | 主循环每 500ms +1 ⇒ 400MHz 时钟与 `delay_ms` 准确 |
| `g_lcd_id`     | `0x7796`                        | **FMC 读写通路打通** + MPU 强序生效 + ST7796 识别成功  |
| 构建尺寸           | text 42500 / data 108 / bss 800 | 原样烧入即可运行                                 |
| YMGUI 库        | 50 个 .c 全部编译通过                  | 库可用于裸机 GCC                               |

---

## 过程中解决的几个硬问题（写下来避免后来者重踩）

1. **CMSIS 缺 `cmsis_gcc.h`**：厂商资料包的 CMSIS 被裁剪成只剩 Keil 用的  
   `cmsis_armcc.h`/`cmsis_armclang.h`，GCC 编译必然报 fatal error。  
   本机 `/e/STM32Cube_Repository/STM32Cube_FW_H7_V1.12.1` 有完整 CMSIS，已补齐。
2. **厂商源码是 GBK 编码**：`BSP/*` 与 `SYSTEM/*` 共 14 个文件是 GBK，  
   直接编会在中文字符串处报错。已批量转 UTF-8。
3. **CMSIS 只带 Keil 版启动文件**（`Templates/arm/startup_stm32h743xx.s`，ARMCC 语法  
   且调 Keil 的 `__main`）。自写了 `src/startup_gcc.s`：向量表（16 内核 + 150 外部 IRQ）
   - Reset_Handler（设栈 / 开 FPU CP10·CP11 / SystemInit / 搬运 .data / 清 .bss / 进 main）。
4. **缺 `FMC_NORSRAM_*` 符号**：HAL 的 FMC 封装在 LL 层，  
   `HAL_SRAM_Init` 会调 `FMC_NORSRAM_Init` 等，必须把 `stm32h7xx_ll_fmc.c` 加进源列表。
5. **`-nostartfiles` 连带两个符号缺失**：`__libc_init_array` 需要 `_init`（给空实现），  
   库内 `_sbrk` 引用符号 `end`（用 `PROVIDE(end = .)` 补上）。  
   ⚠ 后来发现**这里埋着阶段 3 的根因**：`end` 默认就等于 `_end`，而当时的  
   `_heap0_start` 也设在 `ALIGN(8)` —— 两者同址，stdlib 缓冲与 YMGUI 堆撞在一起。  
   详见下面第 8 条。
6. **`lcd.c` 内部 `#include "lcd_ex.c"`**：不能再把 `lcd_ex.c` 单独加进源列表，否则重复定义。
7. **CMake 的 `POST_BUILD` 自定义命令在本机失败**（`'cmd.exe' 不是内部或外部命令`）：  
   改成独立 `add_custom_target` + `${CMAKE_COMMAND}` 风格，不依赖 shell。
8. **newlib 的 sbrk 堆与 YMGUI 的 heap0 同址**（阶段 3 故障根因，详见上文专节）：  
   修法是链接脚本独立划出 `_sbrk_start.._sbrk_end`（16 KB）并把 `_heap0_start` 后移，  
   同时自写 `_sbrk()` 从保留区取内存、耗尽返回 `-1`；  
   再顺手删掉厂商驱动里 4 处 `printf`，让 stdio 彻底不参与链接。
9. **"只写不读"的诊断全局会被 `--gc-sections` 删掉**：本工程开了  
   `-fdata-sections` + `--gc-sections`。`g_boot_magic` 只在初始化器里出现、运行时代码  
   从不引用它 → 被判为不可达段直接删除，`nm` 查不到符号、SWD 读回来是空，  
   而 README 却把它当"新镜像已在跑"的判据。  
   `__attribute__((retain))` 在本机 ld 上**实测无效**；改成在 `main` 开头写一次  
   （顺带让语义变成"main 已进入"）才稳住。  
   排查手法：`nm <elf> | grep -E " [bBdD] g_"` 与源码里声明的诊断量逐个比对。
10. **QSPI 的"模式码"里数据模式位写错，症状与硬件故障一模一样**：`mode[7:6]` 是数据模式，  
    `0` 表示**无数据阶段**。把"单线读"写成 `(0u << 6)` 后，控制器等一个永远采不到的  
    数据阶段，`SR.BUSY` 恒 1、`SR.FLEVEL` 恒 0，所有读全超时。  
    **区分手法**：看**无数据阶段**的命令（如 `0x06` 写使能 / `0xFF` 退 QPI）是否成功 ——  
    它成功就说明控制器/时钟/引脚都好，问题在数据阶段。
11. **AF9 的引脚↔信号映射是芯片固定的，不能软件调换**：QSPI `IO2`/`IO3` 谁是 PE2、  
    谁是 PD13 由芯片决定。`0xEB` 四线读把 IO0..IO3 当 bit0..bit3，若接反则每字节  
    bit2/bit3 互换 ⇒ 数据必错，所以"逐字节比对失配 = 0"就证明了引脚归属正确。
12. **newlib-nano 的 `snprintf` 不支持 `%lld`，而且失败得很隐蔽**（2026-10-03）：  
    计算器 `apps/calculator.c` 原来用 `%lld` 格式化 `int64_t`，板上显示恒为
    **`ld`**（小写 L + d）——**数值根本没被打印出来**，只吐出两个字面字符；
    大字号下 `l` 与 `1` 几乎同形，肉眼看着就是"按 1 出 1d"。按任何键结果都一样。  
    已改成 `%ld` + `(long)` 强转：计算器入口上限 99999999、结果钳在 ±999999999，
    32 位完全覆盖，不会截断。  
    ⚠ **教训**：host（glibc）跑 `--selftest` 抓不到这类问题 —— 它只在真机 libc 上出现，
    而且**只查数值不查显示串的断言也抓不到**（原 selftest 就是这样漏掉的）。  
    现补两条显示串断言，并加了免触摸的 SWD 探针：写 `g_calc_probe_v` →
    写 1 到 `g_calc_probe` → 读 `g_calc_probe_text`。  
    **真机画面复核**（2026-10-03）：通过 SWD 注入点击钩子（见下）导航到计算器、
    按 `1 2 + =`，抓屏大字显示 **`12`** —— 修复在真实 app 界面上确认，不再只靠探针。

**SWD 注入点击/滑动钩子**（`phone_shell.c` BoardTick，AI 免触摸测试用）：
写 `g_bdtap_x` / `g_bdtap_y`（滑动另写 `g_bdtap_dx` / `g_bdtap_dy`），再把
`g_bdtap_seq` 加 1 触发；下一次 BoardTick 按下→（5 步插值移动）→抬起注入。
行为与 selftest 的 `click_at()`/`drag_pointer()` 一致。常用坐标：
导航键 `(160,464)`（home）、桌面页点 `(160,355|429)`、计算器图标 `(270,80)`
（第 2 页）、计算器键 `1=(50,220) 2=(270,220) +=(120,280) =(195,400) C=(50,400)`。

**文本光标（caret）：下划线 + 闪烁**（2026-10-03 加，改在库里，两个文本控件共用）。

- 原版：`TextInput` / `EditView` 各画一条 **2px 宽、常亮**的竖线，**全库没有任何闪烁实现**
  （`Demo/`、`project_Demo/`、`DEVLOG` 都检索过，别的项目也没有）。
- 现：`YMGUI_Draw_Caret()`（`YMGUI_DrawText.c`）统一画，样式/相位由 HAL 的三个全局变量控制：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `g_ymgui_caret_style` | `1` | `0`=竖线（原版）　`1`=下划线（一个字符宽 × 2px 高，贴行底） |
| `g_ymgui_caret_blink_ms` | `300` | 半周期毫秒数，`0`=不闪（等价原版） |
| `g_ymgui_caret_on` | 翻转 | 当前半周期是否显示；由 `YMGUI_CaretTick()` 自动翻 |

- 宽度取法：优先光标**后**那个字符，串尾取光标**前**刚输入的那个 —— 中文 16px、英文 8px。
  ⚠ **控制字符（`<0x20` / `0x7F`，含 `\n`）不量宽度**，直接用 `cell_w`（见上一条事故）。
- 闪烁不另开定时器：挂在已有的 `YMGUI_Inject_Tick(ms)` 上；翻转时把**焦点控件置脏**，
  下一次 `Refresh` 才重画（不置脏屏幕不会变）。只有编辑态的焦点控件有这点开销。
- 验收（真机，`tools/caret_check.py`）：差分冻结的"亮/灭"两帧 ⇒ 差异区 **16×2 px**（下划线）；
  用设备端 `g_ymgui_caret_ms ÷ g_ymgui_caret_flips` ⇒ **300 ms**。
  ⚠ 不能用墙上时间测周期：**SWD 读变量会暂停内核**，DWT 跟着停，实测会偏快约 30%。
- 半周期累加必须**减法保留余数**（2026-10-03 修）：最初翻转后 `s_acc=0` 清零，把"跨过半周期
  那一帧的超出的部分"丢了 ⇒ 半周期被拉长到 300+一帧（9 分钟混合负载均值实测 **327 ms**）。
  改成 `s_acc -= blink_ms`（上限夹一个半周期防长帧后连翻）后，60 s 采样 60688/202 = **300.4 ms**，
  单次抖动只剩 ±1 帧（短窗实测 296.8）。

---

## 关键设计决定

- **不塞进 `stm32-tetris`**：那是 Rust `#![no_std]` 无 allocator 的工程，而 YMGUI 需要  
  `GY_malloc0/1`。混编会让工具链与内存模型分叉（计划书 §3.1）。独立固件，原工程只作参照。
- **MPU 的 FMC 区保持强序**（`TEX=0/C=0/B=0`）：与 `stm32-tetris` 逐位一致，  
  这是本项目历史上的花屏修复点，**任何情况下不得改成 Device/Normal**（计划书红线 1）。
- **双档堆的落点**：`heap0` = DTCM 剩余（快，放对象头/样式/事件/脏矩形表），  
  `heap1` = AXI SRAM 整块（大，放 draw buffer/图片/字形位图），对应 YMGUI 的  
  `GY_malloc0` / `GY_malloc1`。
- **DTCM 里分成三段，顺序固定**：`.data/.bss` → **sbrk 保留区（16 KB）** → `heap0` → 栈（32 KB）。  
  ⚠ **sbrk 保留区与 heap0 之间绝不能重合**（阶段 3 的故障就出在这里：两者同址，  
  导致 `printf` 的 stdio 行缓冲与 YMGUI 的 `ctx` 撞在一起）。  
  谁要动 `linker/stm32h743vit6.ld` 的这几个符号，先看该文件里的说明。
- **厂商驱动里不放 `printf`**：裸机上 `printf` 会拖进 newlib stdio + malloc + sbrk 一整套，  
  且写入位置由库内部决定、不受本工程控制 —— 它已经踩过一次 heap。  
  调试输出统一走"全局变量 + SWD 读"，见 `src/board_fault.*`、`g_*` 一族的注释。
- **故障不要靠"看现象"判断**：`src/board_fault.c` 会把 CFSR/HFSR/BFAR 等落进 `g_fault`。  
  先读 `g_fault.magic`（非 0 = 发生过故障）再谈别的，避免又把 HardFault 当成死循环。
- **XIP 区（`0x9000_0000`）必须有独立的 MPU 区域**：`PRIVDEFENA=1` 时未映射地址会  
  落到 ARM 默认存储图（可缓存 Normal），会造成"读写都要 cache 维护"的隐性负担。  
  本工程取只读 + 非缓存 + XN，理由与代价写在 `third_party/BSP/MPU/mpu.c` 的注释里。
- **QSPI 稳态停在 memory-mapped 模式**：`0x9000_0000` 就是给只读常量用的，  
  停在映射模式才能直接 `*(const uint8_t*)0x90000000` 读；停在间接模式对该地址的访问  
  是未定义行为。要擦写时不必手工退映射 —— `qspi_read/write/erase_sector` 内部会  
  经 `qspi_ensure_indirect()` 自动先退出。

---

## 第 6 项 + 第 7a 项：拖动局部失效 与 DMA 双缓冲（2026-10-04）

依据是 6/7a/7b/8 四份风险评估里的排序。**本轮只做 6 与 7a。**
`docs/MEMORY_ARCHIVE.md` §九 是完整记录（含两个途中撞到的真 bug），这里只放结论。

### 第 6 项：拖动不再整屏重绘

- 引擎新增**唯一一个公开 API** `YMGUI_Obj_SetPos(obj,x,y)`：改位置并**同时标脏旧位置与新位置**
  （只脏新位置会留拖动残影）。这个两步法引擎内部一直有（动画的 MOVE/RESIZE 就这么做），
  只是没做成公开 API ⇒ app 只能靠脏整屏兜底。
- app 侧**只改一个函数**：`phone_ui.c` 的 `PhoneUI_set_pos` 改成委托 —— 它有 **30 个调用点**
  （图标拖动/桌面滑动/ghost 跟随/shade 滑入…），改一处全受益。
- `brightness_apply` 的整屏标脏**按模式条件化**：只有 `g_dim_mode==1`（软件叠黑）才需要；
  硬件 PWM（默认 `mode=2`）改的是背光占空比，**framebuffer 一个像素都没变**。
- 途中挖出并修掉一个真问题：`settings.c` 的 `scroll_moved()` 在 `scroll_y` **没变**时也把
  320×356 的整个滚动视口标脏（滚动手势的 `Pressing` 分支无条件调它，而处理器挂在视口上、
  是页内所有控件的祖先 ⇒ 设置页里任何拖动都会命中它）。

**实测（`g_flush_pixels` 累积量）**：一次亮度拖动 **113 920 px → 6 144 px**（降 **18.5 倍**；
相对整屏 25 倍），且**每次拖动只产生 1 条 band**，正是预期形态。
竖直滚动仍照常整视口重绘（内容整体位移，这是**必要**的）；滚动中途抓屏目视无残影。

### 第 7a 项：flush 与渲染重叠（DMA 双缓冲）

- 第二块 band buffer 放 **D2 域 SRAM3**（新增链接脚本段 `.bss_sram3`，20 KB/32 KB，**不占 heap1**）。
- DMA1_Stream1、**MEM2MEM 软件触发**（FMC 没有 DMA 请求线）；
  **每笔传输自己写 PAR/M0AR/NDTR + 置 EN、自己轮询 TC 标志**，不用 `HAL_DMA_Start`。
- **等待与交还都由 port 自己做**，不依赖库的 `wait_cb` —— 因为 app 还会再插一层包装
  （`phone_flush`，软件调暗/快照），链路里"谁负责等"并不确定。
- 通路由 **`g_lcd_dma_enable` 开关**（0 = CPU 直推备用 / **1 = DMA，出厂默认**，
  CMake 选项 `YMGUI_PORT_LCD_DMA`）；运行期 SWD 可切，不必重编译。

⚠ **顺序要害（踩过一次，代价是三页花屏）**：`lcd_set_window`(0x2A/0x2B) 与
`lcd_write_ram_prepare`(0x2C) 也是经 FMC 发给面板的，**必须等上一笔 DMA 传完再发**；
否则命令插进像素流，面板把命令当像素收 ⇒ 整条 band 写错位置，且无脏区时 `Refresh` 早返回
⇒ **不会自愈**。修复是把等待提到任何 FMC 访问之前。
证据：新诊断量 `g_lcd_dma_waited`（进 flush_cb 时有 DMA 在途的次数）= **3455 / 3456**，
即旧顺序下**每一次**发窗口命令都有一笔在途 —— 花屏是必然而非偶发。

⚠ **截图看不出这类花屏**：全帧镜像是在 `flush_cb` 里从**源 buffer** 拷的，反映"要画什么"、
压根不经过面板（修复前后桌面页截图 SHA256 完全相同）；而 A2 的 gram 回读是**空闲态**跑的，
那时命令与像素不可能交错。**判据只有 `g_gram_mismatch`**（高频重绘后立刻回读）。

**实测（同场景背靠背 A/B，这是唯一正确的比法）**：

| 通路 | 满负载整帧（桌面页） | cycles |
| --- | ---: | ---: |
| CPU 直推 | 18.14 ms / 55.1 FPS | 7 257 405 |
| **DMA 双缓冲** | **14.71 ms / 68.0 FPS** | 5 883 591 |
| CPU 直推（复测） | 18.14 ms / 55.1 FPS | 7 257 378 |

两次 CPU 复测只差 **27 cyc / 7.26 M** ⇒ 不是噪声，**DMA 真实快 3.43 ms（−18.9%）**；
设置页场景同样更快（20.39 → 17.75 ms）。**顺序修复本身零性能代价**。
验收：A2 ramp=0 / **gram=0**、`g_fault=0`、IME 2/2、caret ✓、`lang_check` 三层 ✓、
`reenable=0`、`timeout=0`。

⚠ **教训：本板帧基准随页面变化**（桌面 18.14 / 设置页 20.39 / 笔记页 23.17），
跨场景比数字会得出**反结论** —— 我第一版就是拿"设置页的 CPU 数字"比"桌面页的旧基线"，
误判成"DMA 没有收益"。


### 决定：7b 与第 8 项**不做**

- **7b（MDMA+中断）**：要改启动文件向量表（当前 150 个 IRQ 槽位是**硬引用**到 `Default_Handler`，
  文件里"都是 weak 符号"那句注释是**错的**）+ 引入 MDMA 驱动全量重建；而增量收益只是
  "发起即返回 + 完成通知"，7a 已经做到。
- **第 8 项（遮挡剔除/子树剪枝/abs 缓存）**：**实测天花板 ≈ 1.8~2.2 ms（10~12%）**
  （`render_scan.py` 解出每 band 固定开销 A = 48 222 cycles × 15 band = 1.81 ms，
  加 root 整屏无效填充 ~0.40 ms），但正确性风险最高（要逐个 draw_cb 审"是否真不透明"、
  在所有改 area/scroll 处接失效）⇒ 收益与风险不成比例，且会被 16 ms 帧节拍吸收。

⚠ 这三项的价值要看清：`FRAME_TARGET_MS = 16` 已把稳态收益封顶，做完 7a 后满负载
**15.98 ms 刚好落进预算**。剩余优化的意义在**最坏帧**（笔记页曾实测 23.17 ms）与交互响应上。

