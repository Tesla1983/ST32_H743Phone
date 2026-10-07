# ymgui-h743 —— YMGUI 在 STM32H743VIT6 上的手机上原型

把国产 GUI 库 [YMGUI](https://github.com/Yao-Mi/YMGUI) 移植到一块 STM32H743VIT6 小系统板上，
跑它自带的中文手机 Shell（`project_Demo/phone_shell`）：320×480 并口屏 + 电容触摸 +
16 MB QSPI Flash，裸机（无 RTOS），CMake + GCC 构建，全部资源近距离地上板实测。

> **本 README 只说明"这个项目是什么、怎么构建、怎么验收"。**
> 阶段过程、每一轮优化的实测与结论、踩坑修复、决策依据，全部在
> **[`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md)**。

---

## 1. 硬件目标板

| 部件 | 型号 / 参数 | 接法 |
|---|---|---|
| MCU | **STM32H743VIT6**，Cortex-M7 @ **400 MHz**，2 MB Flash / 1 MB SRAM | — |
| 显示屏 | **320×480 ST7796**，RGB565 | **8080 并口经 FMC**（NE1，`LCD_RAM @ 0x6010_0000`，A19 作 RS） |
| 触摸屏 | **GT1158**（GT9xxx 系）电容触摸 | I²C，产品 ID 读出 `"1158"` |
| 外部 Flash | **W25Q128**，16 MB | **QSPI**，memory-mapped（XIP）@ `0x9000_0000` |
| FMC 内核时钟 | **220 MHz**（PLL2_R） | 现写时序 `ADDSET=2 / DATAST=4` |
| LCD 背光 | 定时器 PWM 调光 | 调暗不再占用 CPU 画像素 |

## 2. 软件构成

```
裸机主循环（无 RTOS）
 ├─ 自建 board 层：启动文件 / 双档堆 / 故障快照 / 各外设 port
 ├─ STM32H7xx HAL + LL（含 LL_FMC）+ 厂商 BSP·SYSTEM（已转 UTF-8）
 ├─ YMGUI 库（third_party/YMGUI）：显示、触控注入、控件、绘制图元、字体
 └─ project_Demo/phone_shell：桌面 · 启动器 · 状态栏 · 快捷面板 · 16 个应用 · 中文输入法
```

关键数据布局：**只读大资源**上 QSPI 用 memory-mapped 方式就地访问（XIP），内部 Flash 只留代码与少量索引。

| 资源 | 位置 | 大小 |
|---|---|---|
| GB2312 字模（7672 字 × 128 B，16×16 4bpp） | QSPI 偏移 `0x000000` → `0x9000_0000` | 982 016 B |
| 记账扇区（字模 CRC，避免每次上电重灌） | QSPI 偏移 `0x000FF000` | — |
| 全量词组词典 `phrases_xip.bin` | QSPI 偏移 `0x300000` → `0x9030_0000` | 1 303 456 B |
| 拼音单字词典 `pinyin_gb2312.bin` | 内部 Flash `.rodata.ime_chars` | 116 350 B |

## 3. 目录结构

```text
E:\ymgui-h743\
├─ CMakeLists.txt                # 唯一的顶层构建脚本（库 + 厂商代码 + 板级代码一起编）
├─ cmake\arm-none-eabi.cmake     # 交叉编译工具链文件
├─ linker\stm32h743vit6.ld       # 链接脚本：显式划分 DTCM / AXI / SRAM1-4 / sbrk 区
├─ src\
│   ├─ startup_gcc.s             # GCC 版启动 + 向量表（CMSIS 只带 Keil 版，故自写）
│   ├─ board_startup.c           # .data 搬运 / .bss 清零 / _init·_fini 桩 / 自写 _sbrk
│   ├─ board_alloc.c/.h          # 双档堆 heap0(DTCM) / heap1(AXI)，对应 GY_malloc0/1
│   ├─ board_fault.c/.h          # 故障现场快照（CFSR/HFSR/MMFAR/BFAR），SWD 事后读
│   ├─ ymgui_port.c/.h           # GYdisp 初始化 + flush_cb + 全帧镜像缓冲 + 显示自检
│   ├─ touch_port.c/.h           # GT9xxx → YMGUI_Inject_Pointer / Inject_Tick
│   ├─ qspi_port.c/.h            # W25Q128：indirect 读写擦 + memory-mapped XIP + 验收
│   ├─ qspi_provision.c/.h       # 产线用：主机分块灌写 QSPI（默认不编入）
│   ├─ font_provision.c/.h       # GB2312 字库首次上电自灌 + 记账扇区校验
│   ├─ dma_bench.c/.h            # DMA/MDMA 搬运微基准
│   └─ main.c                    # 主循环：各模块 tick + 产线动作调度 + 渲染
├─ tools\                        # 宿主机侧 Python 工具（读板上变量、抓屏、回归、灌库）
├─ ci\                           # 构建输入检查 + 固件可复现性闸门
├─ docs\                         # 专项报告与开发过程记录
└─ third_party\
    ├─ YMGUI\                    # YMGUI 仓库根（库本体在再下一级 YMGUI\）
    ├─ CMSIS\                    # 内核头（已从官方 CubeH7 补齐 cmsis_gcc.h）
    ├─ STM32H7xx_HAL_Driver\
    ├─ BSP\{LCD,LED,MPU}\        # 厂商外设驱动
    └─ SYSTEM\{sys,delay,usart}\ # 厂商系统层
```

## 4. 构建与烧录

**依赖**：`arm-none-eabi-gcc 14.3.rel1`、`Ninja`、`CMake ≥ 3.20`、`probe-rs 0.29.1`、Python 3 + `pyserial`。

```bash
cd /e/ymgui-h743
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
cmake --build build --target ymgui-bin          # 生成 bin / hex 并打印尺寸

P="--probe 0416:5021:0123456789AB --chip STM32H743VITx"
probe-rs download $P build/ymgui-h743.elf
probe-rs reset    $P
```

> ⚠ **`probe-rs download` 跑完内核仍然 halt**，不会自己跑起来。只 download 不 reset，
> RAM 里还是上一份固件的残余，用新固件的符号地址去读会得到一串莫名其妙的数据——
> 看起来像"新功能没生效"，其实是**压根没跑**。每次烧完都要跟一条 `probe-rs reset`。

> ⚠ **构建目录必须叫 `build`**：CMakeLists 里 linker map 的输出路径写死成 `${ROOT}/build/ymgui-h743.map`。

### 构建选项

| CMake option | 默认 | 作用 |
|---|---|---|
| `YMGUI_IME_PHRASES_XIP` | **ON** | 词组词典走外部 Flash XIP（需先灌库，见 `$6` 输入法一节） |
| `YMGUI_QSPI_PROVISION` | OFF | 编入 QSPI 分块灌库支持（一次性产线动作） |
| `YMGUI_XIP_BENCH` | OFF | XIP / 内部 Flash 读延迟台架（`micro.py` case 20~26） |
| `YMGUI_DIAG` | OFF | 渲染诊断计数/计时（明显拖慢帧率，仅供测量） |
| `YMGUI_PORT_FRAME_MIRROR` | OFF | flush 时同步 300 KB 全帧镜像（抓屏与 A2 需要） |
| `YMGUI_ANIM` | ON | UI 动画模块 |

出货形态 = 全部取默认值。

## 5. 固件能力

**桌面 Shell**：三页桌面（可滑动切换）、应用抽屉、最近任务、快捷下拉面板、状态栏、设置页。

**内置 16 个应用**：任务 · 音乐 · 相册 · 设置 · 时钟 · 天气 · 笔记 · 计算器 · 电话 ·
短信 · 联系人 · 录音 · 文件 · 陀螺仪 · 相机 · 浏览器。

**中文输入法**：拼音 → 候选 → 上屏，走真实软键盘路径。
单字 **7291** 条 + 词组 **47 280** 条；词组词典常驻外部 Flash，**零 RAM 索引**（按桶游标解码）。

```bash
python tools/ime_probe.py zhongguo xianzai zhongguoren
# [OK] 'zhongguo'     候选(4)=中国 中过 中果 中郭       上屏 '中国'
# [OK] 'xianzai'      候选(4)=现在 想在 先在 现在爱     上屏 '现在'
# [OK] 'zhongguoren'  候选(3)=中国人 中国仍 中国任      上屏 '中国人'
```

**文本光标**：下划线样式 + 闪烁，改在库里，两个文本控件共用；宽度取光标处那个字符（中文 16 px / 英文 9 px）。

**亮度**：定时器 PWM 硬件调背光，不占用 CPU 逐像素叠加。

**多语言**：中 / 英两套词条，运行时可切，选择落 QSPI 持久保存并带 CRC 校验。

## 6. 内存布局与硬性约束

```text
DTCM   0x2000_0000  128 KB   栈 + heap0 + 部分关键数据（最快，不放 DMA）
AXI    0x2400_0000  512 KB   _frame_buf 307200 B(0x24000000~0x2404AFFF) + heap1
SRAM1  0x3000_0000  128 KB   非缓存 DMA 区
SRAM2  0x3002_0000  128 KB   常规可缓存数据
SRAM3  0x3004_0000   32 KB
SRAM4  0x3800_0000   64 KB
QSPI   0x9000_0000   16 MB   memory-mapped XIP（GB2312 字模 / 词组词典）
LCD    0x6010_0000           FMC NE1 + A19 作 RS
```

必须知道的几条：

- **抓屏 / A2 依赖全帧镜像**，而它在出货构建里默认关闭。`tools/*` 的抓屏脚本会自己
  写 `g_frame_mirror = 1`，不用手工开。
- **画面静止时 `YMGUI_Refresh` 因脏区为空直接返回**，镜像里留的是上一帧。
  抓屏前必须先写 `g_force_redraw = 1` 再等约 0.3 s（≥ 2 拍），否则抓到老帧。
- **`GY_MixPx` 在存储域混合**（先算加权再量化一次），避免系统性偏暗；`opa=255` 时
  结果与 `GY_ColorToPx` 逐位相同。
- sbrk 有自己的 16 KB 保留区，**不与 heap0 重叠**（重叠会造成极难排查的内存踩踏）。
- 被 `--gc-sections` 吃掉的变量：本工程开了 `-fdata-sections` + `--gc-sections`，
  只读不写的诊断全局会被删掉（`__attribute__((retain))` 在本机 ld 上无效），
  要保留就得在代码里真的读写一次。

## 7. 板上工具与验收

工具都在 `tools/`，一般用 SWD 读写全局变量，**不需要重新编译或复位**。

| 脚本 | 用途 |
|---|---|
| `tools/read_vars.py <符号…>` | 读一组全局变量（自动从 ELF 取地址、合并相邻读） |
| `tools/ime_probe.py <拼音…>` | 输入法端到端：拼音 → 候选 → 上屏 |
| `tools/flash_and_verify.py` | **完整回归**：烧录 + 语言三层验收 + A2 + IME + 光标 |
| `tools/heap_peak_check.py` | 双档堆峰值 |
| `tools/micro.py <case>` | 圆/半透明填充/位图搬运等图元微基准 |
| `tools/bench.py <帧数>` | 满负载整帧基准（强制每帧整屏标脏） |
| `tools/grab_ymgui.py` `tools/frame_grab.py` | 读 `_frame_buf` 还原 PNG |
| `tools/caret_check.py` | 光标形状与闪烁周期（差分两帧 + 设备端计时） |
| `tools/lang_check.py` | 中/英切换：交互生效 · 落盘 CRC · 掉电重启保持 |
| `tools/verify_board_image.py` | 把板内 flash 全量读回，与本地 `.bin` 逐字节比对 |
| `tools/check_ribbon_equiv.py` | 两种壁纸算法逐像素等价性（主机侧） |

### 验收判据与当前基线

| 编号 | 判据 | 实测（出货形态，2026-10-08） |
|---|---|---|
| **A1** | 满负载整帧刷新 | **10.89 ms = 91.8 FPS**（60 帧整屏标脏，桌面场景，两次复现一致） |
| **A2** | 显示通路逐像素自检 | `g_ramp_mismatch = 0`、`g_gram_mismatch = 0` |
| **A3** | 触摸识别 | `g_tp_pid = 0x31313538`（`"1158"`） |
| **A4** | XIP 与 indirect 读一致 | 比对覆盖全部 16 MB，失配 0 |
| **A5** | UI 交互 + 中文输入法可用 | 合成滑动/单击通过；IME 3/3 |
| — | 故障计数器 | `g_fault.magic = 0`（从未发生内核故障） |
| — | 文本光标 | 16×2 下划线，周期 300 ms |

跑一遍 A2（不用重编译）：写 1 到 `g_gram_recheck`，等一帧后读那两个变量。

> ⚠ **帧率依赖场景**：同一份固件在不同页面会差很多（整表都按"重置后的桌面页"量）。
> 跨场景比数字没有意义，比之前先确认场景。
>
> ⚠ **堆峰值是开机以来的累计最大值**：先跑过别的操作（如 `lang_check.py` 的英文模式 +
> flash 落盘）再读会读到偏大的值。要干净基线就先 `probe-rs reset` 再单独测。
>
> 上电闪一下的彩色波纹（约 86 ms）是 **A2 的 ramp 自检图案**，不是花屏：它把整屏 GRAM
> 写成「像素 i = i & 0xFFFF」再读回比对，证明 8080 读通路可信，随后自动恢复画面。
> 该自检默认不随上电运行（`src/ymgui_port.h` 的 `YMGUI_PORT_BOOT_SELFTEST = 0`）。

## 8. 资源占用（出货形态）

```text
FLASH   1 579 052 B  / 2 MB   75.30 %
DTCM       23 936 B  / 128 KB  18.26 %
SRAM1         512 B  / 128 KB   0.39 %
SRAM2      62 000 B  / 128 KB  47.30 %
SRAM3       20 KB    /  32 KB  62.50 %
text 1 575 132 / data 3 912 / bss 103 016
heap0 峰值  32 688 B /  56.6 KB  56 %
heap1 峰值  71 064 B / 212.0 KB  33 %
```

## 9. 持续集成

`.github/workflows/firmware-build.yml`，**push 到 `main` 或 PR 到 `main` 触发**（也可手动 `workflow_dispatch`），
跑在 `ubuntu-latest`，工具链钉死 **arm-gnu 14.3.rel1**。

四个闸门：

1. **构建输入完整性** `ci/check_build_inputs.py` —— 两个 objcopy 进固件的 `.bin` 必须在库里且字节数精确相等；
2. **构建** `cmake -B build` + `ymgui-bin`；
3. **可复现性** `ci/check_firmware_hash.py` —— CI 产物与 `ci/firmware_reference.sha256`
   **逐字节比对**。那个参考哈希的来源不是随手一算：本机出货形态构建 → 烧录复位 →
   跑完 `flash_and_verify.py` 全套回归后，把 elf / bin 的 sha256 记下来，即是板上在跑的那份字节；
4. **产物留存**（`if: always()`，红灯也能下载排查），保留 30 天。

> 参考哈希只在三种情况下允许更新，都要在 commit message 里写清原因：
> ① 有意为之的功能改动；② 工具链版本升级；③ 构建参数调整。
> **没人改代码却变红 = 环境漂移，去查环境，不要改哈希。**
> 另外 `check_firmware_hash.py` 把**所有未注释行**都当参考项，换基准时记得把旧行注释掉。

## 10. 已知问题与未做项

| 项 | 说明 |
|---|---|
| `shijie` 打不出"世界" | 引擎打分问题：单字"是"频率 1 180 957，`composeQuality()` 对 2 段复合除 16 后"是接"仍 74 920 分 > 世界 30 808。改打分公式会连带影响其他输入，需单独一轮做回归比对 |
| GB2312/CJK 字模生成参数未覆盖验证 | CJK 用 `cell_w=16 / PT=16`（Noto Sans CJK），本机无该字体，只验证了 ASCII 那部分 |
| `InvalidateOldNew` API 未做 | 现在拖一下滑块就 `Invalidate(root)` 变全屏重绘 |
| MDMA + 第二块 band buffer | 可让 flush 与渲染重叠，未做 |
| 遮挡剔除 / 子树剔除 | 未做 |

## 11. 文档索引

| 文档 | 内容 |
|---|---|
| **[`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md)** | **开发过程全记录**：阶段进度、每轮优化实测、踩坑与修复、决策依据 |
| `docs/CI_REPRODUCIBILITY.md` | CI 与构建可复现性的建立过程与排查方法 |
| `docs/IME_PHRASE_DICT_FEASIBILITY.md` | 词组词典上外部 Flash 的可行性论证、取舍与落地结果 |
| `docs/FONT_GLYPH_CLIPPING.md` | 字母 W 渲染残缺的根因、证据与上板结果 |
| `docs/EXTERNAL_FLASH_OFFLOAD_FEASIBILITY.md` | 只读大资源外挂到 QSPI 的可行性论证与布局 |
| `docs/MEMORY_ARCHIVE.md` | 内存布局、历史结论与已归档论证 |
| `docs/BOOT_HANG_VOSRDY_LESSONS.md` | 上电卡死的定位过程与教训 |
| `docs/PREVIEW_BRIGHTNESS_ACCEPTANCE.md` | 亮度/预览的验收标准与目视结论 |
| `docs/SETTINGS_RIBBON_ACCEPTANCE.md` | 设置页 ribbon 外观的验收记录 |
| `ci/README.md` | 参考固件哈希的来源、更新规则与复现步骤 |
| `third_party/YMGUI/project_Demo/phone_shell/APP_GUIDE.md` | phone_shell 应用开发指南 |
