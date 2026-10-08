# TF 卡接入 + 缓存一致性 路线图

> **立项**：2026-10-04
> **状态（2026-10-08 更新）**：
>   · L1 已完成 · **L2-1 / L2-2 / L2-3 已完成并上板验证**（实体卡：32 GB，FAT32）
>   · **L3（SRAM1 非缓存 DMA 区）已完成并上板验证**（判据：dma_check 用例② 失配 64 → 0）
>   · **L2-4（IDMA 高速模式）已完成并上板验证**（判据：`sd_dma_check.py` 三项全 0，连跑 5 轮 PASS）
>   · 至此 **L1–L3 与 L2-1~L2-4 全部完成**。剩下的都是"要不要接进应用层"，不是通路问题。
>   · ⚠ 本文档的性质因此变了：下面各节现在主要是**历史记录与坑的留档**，
>     做 L2-4 时新踩到的两条硬约束（IDMA 够不到 SRAM1/2/3、MPU 编号大者优先）见 §2.5。
> **适用范围**：`E:/ymgui-h743`（YMGUI → STM32H743VIT6 移植工程）

本文档记**还没做的事**，以及做它们所需的全部前置信息（地址、编号、判据、坑）。
已完成的 L1 见 `README.md` 的「cache 策略固化 + DMA ↔ D-Cache 一致性台架」一节，
L2 的实测结果与踩坑见下面 §2.0，开发过程全记录见 `docs/DEVELOPMENT_LOG.md`。

---

## 2.0 L2 已完成部分（2026-10-08 上板实测，卡 = 32 GB FAT32）

| 项 | 结果 | 判据/工具 |
| --- | --- | --- |
| 引脚 | SDMMC1 4-bit：D0–D3 = PC8–PC11、CLK = PC12、CMD = PD2，AF12，上拉 | 厂商「实验26」`sdmmc_sdcard.h` + V1.2 原理图（**权威来源，非推测**） |
| 时钟 | `sdmmc_ker_ck` = PLL1_Q = **200 MHz**，`SDMMC_INIT_CLK_DIV=0xFA` ⇒ 初始化 400 kHz；`ClockDiv=4` ⇒ 传输 **25 MHz** | `sys_stm32_clock_init(160,5,2,4)` |
| 卡识别 | `HAL_SD_Init` = HAL_OK；**SDHC/SDXC，61 069 312 块 × 512 B = 29.12 GB** | `g_sd_init_rc` / `g_sd_cap_mb` |
| L2-2 扇区级 | 512 B 写→读失配 **0**；8×512 B 写→读失配 **0**；原内容恢复失配 **0** | `python tools/sd_check.py --repeat N`，**连跑 15 轮全过** |
| L2-3 文件系统 | FatFs 挂上 **FAT32**；建目录 → 写 1024 B → 关 → 重开读回失配 **0** → 删除；整轮 194 ms | `python tools/fs_check.py` |
| 吞吐（轮询 25 MHz） | 写 4 KB ≈ 1.7 MB/s，读 4 KB ≈ 4.1 MB/s | `g_sd_wr_cyc` / `g_sd_rd_cyc` |

**⚠ 实测踩到的坑（都写进了代码注释与工具）**：

1. **间歇失败**：第一轮四项全过、第二轮多块读返回非 OK，且**恢复步骤跟着失败**
   ⇒ 卡尾部 8 个扇区被写成图案没还原。处理：先把 SRAM 里的备份抢救到主机
   （`probe-rs read ... -o build/sd_bss_sd.bin -f binary`），再用 `tools/sd_restore.py` 还原。
2. **修复动作**：启用 **SDMMC 硬件流控**（`SDMMC_HARDWARE_FLOW_CONTROL_ENABLE`）——
   轮询模式是 CPU 搬 FIFO、期间还会被 SysTick 打断，关流控时容易 RXOVER/TXUNDERR；
   另外加了"失败 → 重新初始化 → 重试"与寄存器级导出（`g_sd_sta` / `g_sd_errcode` / `g_sd_step`）。
   改完连跑 15 轮全过、`g_sd_sta = 0`、零重试。
   **根因未 100% 坐实**（失败那次还没导出 STA），所以 L2-4 之前若再见到失败，
   第一手段是降 `ClockDiv`（25 → 12.5 MHz），第二才是怀疑布局/供电。
3. **数据安全**：测试扇区取**卡尾部**（总块数 − 2048），绝不碰扇区 0；且必须
   "备份 → 写图案 → 比对 → **写回原内容** → 再验证"，`g_sd_restore_mis = 0` 才算完。
4. 段内缓冲排列顺序**与源码书写顺序不一致**（段起点是 `s_bak`，`s_wr` 在 +8192），
   所以三个缓冲地址由固件自报（`g_sd_wr_addr` / `g_sd_rd_addr` / `g_sd_bak_addr`），
   **主机不许猜地址**。

**FatFs 相对厂商原配置的改动**：`FF_USE_LFN` 3→1（静态缓冲，不碰 malloc）、
`FF_FS_NORTC` 0→1（本工程没接 RTC）、`FF_USE_MKFS` 1→0（**不在固件里保留格式化能力**）、
`FF_VOLUMES` 2→1；保留 `FF_CODE_PAGE=936`（中文文件名）与 `FF_FS_EXFAT=1`（32 GB 卡可能是 exFAT）。
厂商那份 `ffsystem.c` **故意不编**（它依赖厂商自己的 malloc，且那两个函数在本配置下不会被引用）。

---

## 0. 为什么把这件事单独立项

本工程反复被同一类问题咬：**内存/缓存层面的静默不一致** —— 不报错、不 HardFault、
现象还常常伪装成别的东西。

| 历史事故 | 表现 | 真实根因 |
| --- | --- | --- |
| 阶段 3「注入一调用就卡死」 | 看着像递归死循环 | newlib `_sbrk` 与 YMGUI heap0 **同址**，printf 缓冲覆盖 `ctx->root` → BusFault 被 `while(1)` 的 fault handler 伪装成死循环 |
| 2026-10-04「App 冻结」 | 界面不动、无故障计数器 | 字体 fallback 链**成环**（`chinese_font.fallback` 指向自己） |
| 抓屏读到陈旧值 | 图看起来"不对" | 写回模式下 SWD 绕过 D-Cache |

**共同点**：都是"某项状态在别处被改了，当前观察者不知道"。TF 卡（SDMMC + IDMA）
会引入一个新的总线主设备，正是这类问题的温床 —— 所以**在接卡之前**先把一致性机制定死。

---

## 1. 已完成（有板上证据，不需重做）

### 1.1 L1 · cache 策略固化 ✅

- 删除了 `g_cache_wb`（切写回/透写）与 `g_cache_clean`（手动刷 cache）两个运行期开关。
- 现在是 **D-Cache 常开 + FORCEWT 恒为 1**（厂商 `sys.c` 的 `sys_cache_enable()`）。
- **结构性保证**：CPU 写一定直达内存 ⇒ SWD 抓屏 / 读 AXI 变量永远拿最新值，
  不依赖"谁记得先 clean"。
- 证据：`nm build/ymgui-h743.elf | grep -c "g_cache_wb\|g_cache_clean"` → **0**。

> ⚠ 边界：**FORCEWT 只覆盖"写"方向**。DMA 写内存后 CPU 的 D-Cache 里可能仍是旧副本 ——
> 这就是 L3 要解决的问题，**不可能靠 FORCEWT 消除**。

### 1.2 DMA ↔ D-Cache 双向台架 ✅

- 文件：`src/dma_bench.c` + `src/dma_bench.h`，验收工具 `tools/dma_check.py`。
- 手法：**DMA1 MEM2MEM**（`HAL_DMA_Init` 在 `Direction == DMA_MEMORY_TO_MEMORY` 时
  自动把 Request 强制为 `DMA_REQUEST_MEM2MEM`，见 `stm32h7xx_hal_dma.c:418`）。
- 缓冲区：D2 域 SRAM1，链接脚本 `.bss_dma` 段，实测 `src=0x30000100` / `dst=0x30000000`。

**实测结论（连测 4 轮）**：

| 用例 | 内容 | 失配 | 含义 |
| --- | --- | --- | --- |
| ① | CPU 写 → DMA 读 | **0 / 64** | FORCEWT 保证写直达 ⇒ **DMA 读方向无需任何 cache 维护** |
| ② | DMA 写 → CPU 读（不维护） | **64 / 64** | **风险真实存在，且完全静默** |
| ③ | DMA 写 → CPU 读（invalidate） | **0 / 64** | `SCB_InvalidateDCache_by_Addr` 能完整补救 |

- 交叉验证（直接读 PPB，非固件自报）：`SCB->CCR = 0x00070200`（bit16 DC=1）、
  `SCB->CACR = 0x00000005`（bit2 FORCEWT=1）。
- 性能基线：256 B 搬运 **638 cycles ≈ 1.59 µs ≈ 160 MB/s**。
- ⚠ 首轮曾出现 **56 / 64**（有一条 32 B cache line 被无关流量按组相联换出）。
  **量值有抖动 = 更危险**，不是更安全。这同时是"缓冲区必须 32 B 对齐"的量化依据。

---

## 2. L2 · SDMMC 数据通路（**触发条件：实体 TF 卡到货** —— ✅ 已于 2026-10-08 完成 L2-1~L2-3，见 §2.0）

### 2.1 接卡之前的现状（历史记录，现已改变）

- **没有任何 SD/MMC 驱动代码**：`src/` 下无 `*sd*` / `*mmc*`；
  `third_party/BSP/` 只有 `LCD` / `LED` / `MPU` / `TOUCH`。
- **没有文件系统**：全库无 `fatfs` / `diskio`（`f_open` 只命中未参与构建的 `extern_lib/FFmpeg`）。
- HAL 驱动**已在仓库里、但未加入构建**：
  `stm32h7xx_hal_sd.c` / `stm32h7xx_hal_mmc.c` / `stm32h7xx_ll_sdmmc.c`
  （`CMakeLists.txt` 的 `HAL_SRC` 列表里没有它们）。
- **16 个 APP 全是演示态**，且自陈没有存储：

  | APP | 源码里的原话 |
  | --- | --- |
  | 笔记 | "点击编辑 / **本次运行保留**"（`apps/notes.c:45`） |
  | 文件管理器 | "虚拟目录 · **不读写真实磁盘**"（`apps/files.c:76`） |
  | 录音机 | "未接入麦克风"（`apps/recorder.c:67`） |
  | 相机 | "未调用摄像头，不拍照"（`apps/camera.c:13`） |
  | 短信 | "草稿仅在本次运行保留"（`apps/messages.c:41`） |
  | 相册 | "示例风景"（`apps/gallery.c:35`） |
  | 音乐 | "本演示只有一首曲目的播放状态"（`apps/music.c:88`） |

### 2.2 引脚规划（已核实无冲突）

SDMMC1 · 4-bit · AF12：

| 信号 | 引脚 | 当前占用情况 |
| --- | --- | --- |
| CK | **PC12** | 空闲 |
| CMD | **PD2** | 空闲 |
| D0–D3 | **PC8 / PC9 / PC10 / PC11** | 空闲 |

已占用情况（核实自 `src/` 与 `third_party/BSP/`）：

- 触摸：`PB11`(SCL) `PB14`(SDA) `PB12`(RST) `PB13`(INT)
- QSPI：`PB2`(CLK) `PB10`(NCS) `PD11/PD12/PD13` `PE2`
- LED：`PA2` `PA3`；LCD：`PA8` `PB1` `PD0/1/4/5/7/8/9/10/14/15` `PE3` `PE7–PE15`
- ⇒ **整个 C 口空闲**，D 口还剩 `PD2` / `PD3` / `PD6`。
- 备选：1-bit 模式或 SPI 模式可省引脚，但速度掉一个量级，不建议。

### 2.3 实施顺序（**关键：先用轮询模式**）

| 步 | 内容 | 为什么这个顺序 |
| --- | --- | --- |
| **L2-1** | SDMMC1 初始化（GPIO AF12 + 时钟 + `HAL_SD_Init`），**不启用 IDMA** | 先把"卡能不能识别"与"缓存一致性"两件事解耦 |
| **L2-2** | **轮询模式**读写自检：写一整个 512 B 扇区随机图案 → 读回 `memcmp` 逐字节比对 | 见下方 §2.4 判据 |
| **L2-3** | 接 FatFs（`diskio` 适配 `HAL_SD_ReadBlocks`）+ `f_mount` + 建目录 + 文件读写删 | 文件系统是"APP 有真实数据"的前提 |
| **L2-4** | 再做 IDMA 高速模式 | **必须先完成 L3**，否则 ② 类的静默损坏 |

> **为什么 L2-2 用轮询模式**：`HAL_SD_ReadBlocks()`（不带 `_DMA`）是 CPU 轮询 FIFO、
> 由 **CPU 写内存**。CPU 写在 FORCEWT 下直达内存 ⇒ **完全不存在缓存一致性问题**，
> 可以独立验证"数据通路对不对"。
> 代价：阻塞、慢（一张 320×480 全屏图 = 300 KB，会卡住主循环数百毫秒）。
> 但对"笔记/设置/日志"这类 **KB 级**读写完全够用 —— 足够把最高价值的持久化场景先跑通。
>
> ⚠ 首次接入时按 HAL 实现确认轮询路径确实不过 IDMA；用 §1.2 的台架同法验证
> （写→读比对，不看图）。

### 2.4 验收判据（照抄 QSPI 阶段 4 的做法）

| 判据 | 方法 | 门槛 |
| --- | --- | --- |
| 卡识别 | `HAL_SD_Init` 返回值 + 读 CID/CSD | `HAL_OK`；容量 > 0 |
| **写→读比对** | 写 512 B 随机图案 → 读回 `memcmp` | **失配 = 0**（不是"看着对"） |
| 跨扇区 | 连续写 100 个扇区 → 全部读回比对 | 失配 = 0 |
| 掉电安全 | 写后 `f_sync`，断电重上电再读 | 内容完整 |
| 诊断输出 | 结果挂 `g_sd_*` 全局量，SWD 读 | 能给出**整数结论** |

> 本工程的一贯规矩：**能给出确定整数结论的自测，比"看图对不对"可靠得多。**

### 2.5 L2-4 已完成的实施记录与两条新踩的硬约束（2026-10-08）

**做法**：`startup_gcc.s` 给 IRQ49 单开槽位 → `SDMMC1_IRQHandler`（强定义在 `src/sd_card.c`）→
`HAL_SD_IRQHandler`；`HAL_SD_MspInit` 里 `HAL_NVIC_EnableIRQ(SDMMC1_IRQn)`（优先级 5）。
`g_sd_test` 新增模式 4（IDMA @25 MHz）与模式 5（IDMA @50 MHz，跑完自动退回 25 MHz）。
验收脚本 `tools/sd_dma_check.py [--hs] [--repeat N]`。

**⚠ 硬约束 1：SDMMC1 的 IDMA 访问不到 SRAM1/2/3**
- 现象：缓冲放 SRAM1 时**一个字节都搬不动** —— 写 `TX_UNDERRUN`、读 `RX_OVERRUN`，
  `IDMACTRL=1`、`IDMABASE0` 地址正确、`DCOUNT` 只走 **28/4096**。
- 根因（ST **AN5200**；NuttX `stm32_sdmmc.c` 也引用同一条）：**SDMMC1 在 D1 域，
  IDMA 只能访问 D1 域内存 = AXI SRAM**；SRAM1/2/3(D2)、SRAM4(D3) 都到不了。
  SDMMC2(D2 域) 才支持 SRAM1/2/3 —— 但本板卡座硬件锁死在 SDMMC1 引脚上，换不了。
- 对策：链接脚本新增 `.bss_sd_dma` 段钉在 **AXI 尾部 8 KB = 0x2407E000**
  （`s_rd=0x2407E000`、`s_wr=0x2407F000`），`_heap1_end` 下移 8 KB（212→204 KB，峰值 43%）。
  只走轮询的 `s_bak` 仍留 SRAM1，省 4 KB。

**⚠ 硬约束 2：MPU 是「编号越大优先级越高」**（详见 §3.2 的更正）

**判据设计的坑**：哨兵法（先填 0x5A 再让 IDMA 改写）里，随机图案**本来就含
≈4096/256 = 16 个 0x5A**，直接判 `stale == 0` 必然误报。必须判 **`stale − base == 0`**
（base = 图案里 0x5A 的个数）。实测 15 − 15 = 0。

**实测吞吐（8 扇区 = 4096 B）**

| 方式 | 写 | 读 |
| --- | --- | --- |
| 轮询 @25 MHz | 1.5~4.8 ms（波动大，**卡内编程主导**） | 0.90~1.15 ms |
| IDMA @25 MHz | 1.4~2.5 ms | 0.91~1.06 ms |
| IDMA @50 MHz | 2.1~2.3 ms | **0.75~0.96 ms（≈5.5 MB/s）** |

⇒ **结论：IDMA 对 KB 级小传输的写没有收益**，读在 50 MHz 下有 1.2~1.5×。
所以 FatFs 日常继续走轮询（简单、无中断依赖）；真要做相册/音乐这类大文件再说。

---

## 3. L3 · 专用非缓存 DMA 区（**不依赖 TF 卡，可先做**）—— ✅ 已完成 2026-10-08

### 3.0 完成记录（上板实测）

按 §3.4 的重排方案改 `third_party/BSP/MPU/mpu.c`，实测结果：

| 用例（`python tools/dma_check.py`） | L3 之前 | **L3 之后（实测）** |
| --- | --- | --- |
| ① CPU 写 → DMA 读 | 0 / 64 | **0 / 64** |
| ② DMA 写 → CPU 读（**不做任何维护**） | 64 / 64 | **0 / 64 ← 判据达成** |
| ③ DMA 写 → CPU 读（invalidate） | 0 / 64 | 0 / 64（此时 invalidate 变冗余，但无害） |

- 缓冲区仍在 `src=0x30000100 / dst=0x30000000`（SRAM1）⇒ 段位置没跑偏。
- 属性用的是 **TEX=001 / C=0 / B=0 = Normal 非缓存**（不是厂商公共函数里写死的
  TEX=000 那种 Strongly-ordered），另外 `XN=1`、可写、`SHAREABLE`。
  因为公共的 `mpu_set_protection()` 把 TEX 固定成 LEVEL0，所以这条 region
  在 `mpu_memory_protection()` 里**直接调 `HAL_MPU_ConfigRegion`** 配，不动公共函数。
- 回归全绿：A2 ramp/gram = 0、`g_fault` 全 0、IME 2/2、TF 卡扇区自检 3/3 全过、
  FatFs 写 1024 B 重开读回失配 0。
- ⚠ 用例② 现在是 0，**不是台架失效**：那正是"非缓存区生效"的预期结果。
  `tools/dma_check.py` 已把两种状态（0 = L3 生效 / 64 = 未生效）都判成 PASS 并分别说明。

### 3.1 目标

把一块 SRAM 划成 **MPU `NOT_CACHEABLE`**，此后任何 DMA 缓冲区都放这里 ⇒
**代码里一行 clean / invalidate 都不需要写**，一致性由 MPU 保证而非"调用方记性"。

### 3.2 ⚠ 最大的坑：MPU region 编号优先级

ARMv7-M PMSA：**编号越大的 region 优先级越高**。
（ARMv7-M ARM DDI0403："Where there is an overlap between two regions, the register with
the **highest region number** takes priority."；Cortex-M7 TRM："7 Highest priority when 8
regions are implemented."）

> ⚠ **本文档此前写反了**（曾写"编号小的优先级高"），2026-10-08 做 L2-4 时被实测打脸：
> SRAM1 那条之所以没暴露，是因为它**不与任何其它 region 重叠**（原 region3 已从
> "0x30000000 起 512KB"缩成 SRAM2 单独的 128KB），region0 无论优先级如何都生效；
> 而 AXI 尾部 8 KB 的 IDMA 非缓存区**被 region2（AXI 512KB 可缓存）包住**，
> 放进 region1 压不住 ⇒ 哨兵残留 3438/4096；挪到 region7 后归零。教训：
> **想压住大 region，就取比它更大的编号。**

⇒ 想让某块"被大 region 覆盖"的小区域改属性，它的编号必须**大于**那个大 region。

### 3.3 当前 region 分配（8 个已全部用满）

| region | 基址 | 长度 | 属性 | 用途 |
| --- | --- | --- | --- | --- |
| 0 | `0x90000000` | 256 MB | NOT_CACHEABLE · RO · XN | QSPI XIP（字库） |
| 1 | `0x20000000` | 128 KB | CACHEABLE | DTCM |
| 2 | `0x24000000` | 512 KB | CACHEABLE | AXI SRAM |
| 3 | `0x30000000` | 512 KB | CACHEABLE | SRAM1~SRAM3 |
| 4 | `0x38000000` | 64 KB | CACHEABLE | SRAM4 |
| 5 | `0x60000000` | 64 MB | NOT_CACHEABLE | FMC（LCD） |
| 6 | `0xC0000000` | 32 MB | CACHEABLE | SDRAM —— **本板无器件，可回收** |
| 7 | `0x80000000` | 256 MB | NOT_CACHEABLE | NAND —— **本板无器件，可回收** |

### 3.4 重排方案（推荐）

回收 region6/7 的两个编号，重排如下（**长度必须为 2 的幂，且基址按长度对齐**）：

| region | 基址 | 长度 | 属性 | 用途 |
| --- | --- | --- | --- | --- |
| **0** | `0x30000000` | **128 KB** | **NOT_CACHEABLE** · XN | **← SRAM1 = DMA 专用非缓存区** |
| 1 | `0x20000000` | 128 KB | CACHEABLE | DTCM（不变） |
| 2 | `0x24000000` | 512 KB | CACHEABLE | AXI SRAM（不变） |
| 3 | `0x30020000` | 128 KB | CACHEABLE | SRAM2（IME 静态池） |
| 4 | `0x30040000` | 32 KB | CACHEABLE | SRAM3 |
| 5 | `0x60000000` | 64 MB | NOT_CACHEABLE | FMC（不变） |
| 6 | `0x90000000` | 256 MB | NOT_CACHEABLE · RO · XN | QSPI（**从原 region0 挪来**） |
| 7 | `0x38000000` | 64 KB | CACHEABLE | SRAM4 |

要点：

- SRAM1 整块 128 KB 给 DMA。**不切小**是为了让 DMA 缓冲区有充足空间
  （整帧 300 KB 放不下，但 128 KB 够放"一个 band + 双缓冲 + 文件系统扇区缓存"）。
- region0 编号最小 ⇒ 优先级最高，SRAM1 的非缓存属性**不会被 region3 覆盖**。
- region0 设 `XN=1`（不从 DMA 区取指）；**不要**设 `RO`（DMA 区要能写）。
- QSPI 挪到 region6，属性保持不变（`RO` + `NOT_CACHEABLE`，让 XIP 读每次都真走 QSPI，
  与 indirect 结果逐字节可比 —— 这是阶段 4 验收的基础，**不要顺手改成可缓存**：
  2026-10-03 已验证"改可缓存无收益"，仅增加 cache 维护负担）。
- SRAM4 挪到 region7：本板 SRAM4 未使用，实际可有可无；保留它只为将来留口子。

### 3.5 ⚠ 明确不要用的做法

**不要用 `SubRegionDisable` 来做"给子区配非缓存"。**
它的语义是"该子区**不吃**本 region 的配置"，于是会落到**默认存储图** ——
而 ARMv7-M 默认图里 `0x20000000~0x3FFFFFFF` 是 **Normal Cacheable**，
**方向正好相反**。只能用它配合"低编号 region"来挖洞（即 §3.4 的思路）。

### 3.6 配套改动

| 文件 | 改动 |
| --- | --- |
| `third_party/BSP/MPU/mpu.c` | `mpu_memory_protection()` 按 §3.4 重排（注意：厂商文件，改动要写清理由注释） |
| `linker/stm32h743vit6.ld` | DMA 用的段（现有 `.bss_dma`，将来加 `sdmmc` 缓冲）指向 SRAM1；段长需重算 |
| `src/dma_bench.c` | **不改代码** —— 它已经在 SRAM1，正好用来验收 |

### 3.7 验收判据（天然闭环，强烈推荐）

**复用现有台架**，L3 做完后重跑 `python tools/dma_check.py`：

| 用例 | L3 之前 | L3 之后（**判据**） |
| --- | --- | --- |
| ① CPU写 → DMA读 | 0 / 64 | **0 / 64**（不变，本来就是 0） |
| ② DMA写 → CPU读（不维护） | **64 / 64** | **0 / 64 ← 这就是 L3 成功的唯一判据** |
| ③ DMA写 → CPU读（invalidate） | 0 / 64 | 0 / 64（此时 invalidate 变成冗余，但无害） |

> ② 从 64 → 0 说明"非缓存属性已生效，DMA 的写对 CPU 立即可见"。
> 若 ② 仍非 0 ⇒ 说明 region 优先级没排对（回去查 §3.2）或段没落在 SRAM1。

**回归要求**（同时跑，确认没踩到别的区域）：
A2 `g_ramp_mismatch` / `g_gram_mismatch` = 0、`g_fault[0..8]` 全 0、
中文输入法 2/2、`tools/caret_check.py` 形状 16×2 / 半周期 ≈300 ms。

---

## 4. L4 · 备选：手工 cache 维护（**不推荐，仅记录**）

不改 MPU，缓冲 `__attribute__((aligned(32)))` 独占 cache line，
传 DMA 前 `SCB_CleanDCache_by_Addr`，完成后 `SCB_InvalidateDCache_by_Addr`。

- 优点：不动 MPU，改动最小。
- 缺点：**漏一次就是静默数据损坏**，而台架已证明这类损坏**不报错、不 HardFault**。
  本工程已在阶段 3 吃过"以为在防、实际漏了"的亏 —— **不作为主方案**。

---

## 5. 硬约束汇总（已核实，配错就静默搬不到数据）

| # | 约束 | 原因 / 后果 |
| --- | --- | --- |
| 1 | **DMA1/DMA2 无法访问 DTCM（`0x2000_0000`）** | TCM 是 CPU 专有，不在总线矩阵从设备侧。⇒ DMA 缓冲区**不得**来自 `board_alloc0` |
| 2 | DMA1/DMA2 也**访问不到 D3 域的 SRAM4** | D3 域通常只有 BDMA 能碰。⇒ 用 D2 域的 SRAM1 / SRAM2 / SRAM3 |
| 3 | DMA 缓冲必须 **32 B 对齐** | Cortex-M7 cache line = 32 B；不对齐会让 `by_Addr` 式维护误伤相邻数据 |
| 4 | **MPU region 编号大的优先级高**（DDI0403；2026-10-08 更正，此前写反过） | 想让被大 region 覆盖的小区域改属性，编号必须**大于**那个大 region（§3.2） |
| 5 | MPU region 长度必须是 **2 的幂**，基址按长度对齐 | 硬件限制；512 KB 可以，448 KB 不行 |
| 6 | `SubRegionDisable` **不能**用来"给子区配非缓存" | 语义是"不吃本 region 配置"⇒ 落到默认图 Normal **Cacheable**，方向相反 |
| 7 | MEM2MEM 模式**不能用循环模式** | HAL 头文件明确注明 |
| 8 | NOLOAD 段**必须放在 `.bss` 之前** | 否则被 `*(.bss*)` 通配吃掉，链接器不报错但静静不生效 |
| 9 | 新建 NOLOAD 段要在 `board_startup.c` **手动清零** | `.bss` 的循环不覆盖自定义段 |
| 10 | 改完 `.ld` 有时**不触发重链接** | 删掉 ELF 再构建，否则地址对不上 |
| 11 | `HAL_DMA_PollForTransfer` 用有限超时是安全的 | `stm32h7xx_it.c` 的 `SysTick_Handler` 调了 `HAL_IncTick()` |
| 12 | 每帧预算 **16 ms**（21.64 ms 满负载已超） | SD 读阻塞会掉帧 ⇒ 媒体类功能必须做大块异步 + 显式 loading，**不能让主循环无声停住** |

---

## 6. 优先级与触发条件

| 优先级 | 项 | 触发条件 | 理由 |
| --- | --- | --- | --- |
| **P0** | **L2-1 + L2-2**（SDMMC 轮询通路 + 写读比对自检） | **卡到货即刻做** | 一切后续的前提；轮询模式无缓存风险，可独立验证数据通路 |
| **P0** | **L2-3**（FatFs + 持久化） | L2-2 通过后 | **体验收益最高**：把 16 个 APP 从"演示"变成"有数据" |
| **P1** | ~~**L3**（SRAM1 非缓存 DMA 区）~~ ✅ 已完成 | — | 判据已达成：台架 ② 失配 64 → 0 |
| **P2** | ~~L2-4（IDMA 高速模式）~~ ✅ 已完成（通路） | — | 判据已达成（`sd_dma_check.py` 连跑 5 轮 PASS）。**但没接进 FatFs**：实测 KB 级写无收益，只有大文件场景才值得接 |
| **P3** | L4（手工 invalidate） | 不建议 | 有失败先例 |

**最高价值的三个功能**（都不额外依赖新硬件，做完即"更像手机"）：

1. **笔记 / 待办 / 设置持久化** —— 纯软件 + 卡即可
2. **文件管理器接真实目录** —— 消灭"虚拟目录 · 不读写真实磁盘"
3. **截屏落盘** —— 复用已有 `PhoneHost_Capture()`（现在明确"不落盘"）

⚠ **TF 卡单独解决不了的事**（不要指望它）：

- **不会带来声音**：现在**没有任何音频输出通路**（`recorder.c` 自陈"未接入麦克风"）。
  音乐/录音要真起来还需要 I2S/DAC + 编解码器 **+** 驱动；卡只解决"从哪读写文件"这半条路。
- **不会带来网络**：`browser.c` 依赖联网，与卡无关。
- **不会带来摄像头**：需要接 sensor（DCMI/CSI）。
- **静态素材不必放卡**：QSPI 16 MB 只用了 982 KB，剩约 15 MB；只读素材放 QSPI 更省事
  （不用写盘、不用文件系统、不用管缓存），`font_provision.c` 已有成熟灌写通路。
  **卡不可替代的价值**是：用户数据、可插拔、GB 级容量、真文件语义。

---

## 7. 参考

| 内容 | 位置 |
| --- | --- |
| 已完成部分的完整记录 | `E:/ymgui-h743/README.md` →「cache 策略固化 + DMA ↔ D-Cache 一致性台架」 |
| DMA 台架实现 | `E:/ymgui-h743/src/dma_bench.c` / `dma_bench.h` |
| 一致性验收工具 | `E:/ymgui-h743/tools/dma_check.py` |
| MPU 配置 | `E:/ymgui-h743/third_party/BSP/MPU/mpu.c` |
| 链接脚本 | `E:/ymgui-h743/linker/stm32h743vit6.ld` |
| 归档的原始移植计划（阶段 0–6） | `E:/stm32-tetris/docs/YMGUI_PORT_PLAN.md`（历史背景；当前状态以 ymgui-h743 README / MEMORY_ARCHIVE 为准） |
| 渲染优化可行性 | `E:/stm32-tetris/docs/RENDER_OPT_FEASIBILITY.md` |
