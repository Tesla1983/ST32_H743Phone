# 录音链路设计方案（PC2 已接通）

> 2026-10-16 · 补丁18 · 替代原 `docs/TF_CARD_APP_INTEGRATION_FEASIBILITY.md` §3.5/§4.4 中
> "录音硬件上不成立"的结论 —— 该结论写于**模块 DOUT 还没接线**的时候，现在前提已变。

---

## 0. 一句话状态

**数字链路已打通并逐段有板上证据**（ES8311 ADC 通路配置与回读 / I2S2 主收 / DMA 搬运 /
WAV 落卡并自检合格 / 录音机 App 已接真实数据），**唯一未闭环的是"模拟输入没有信号"**
（采到的样本全是 0）。本轮同时修掉两个真实缺陷：**开机 HardFault**（我引入的 heap0 挤爆）
与**录音起不来却说不出原因**（I2C1 从未初始化）。

---

## 1. 设计目标与范围

### 1.1 目标（可验收）

1. 在录音机 App 点"开始录音" ⇒ **真的**把麦克风声音写进 `/YMGUI/RECORD/REC_xxxx.WAV`
   （16 bit PCM / 48 828 Hz / 立体声）。
2. 计时与电平条来自**引擎的真实数据**（已写进文件的帧数、采样峰值），不是 app 自己的动画。
3. 点"停止" ⇒ WAV 头被回填成真实长度，文件可被本机播放器与 PC 正常播放。
4. 采不到信号时**如实报**"未检测到麦克风信号"并给出最可能原因，不假装在录。

### 1.2 范围

| 做 | 不做（本期明确排除） |
|---|---|
| ES8311 ADC（麦克风）通路配置与回读 | 录音回放（复用现成的音乐链路 `wav_play`） |
| I2S2 主收 + DMA1_Stream3 采集 | 暂停/续录、多段拼接、断点续写 |
| WAV（PCM）落卡 + 头回填 + 卡上回读自检 | 压缩编码（MP3/ADPCM）、AGC/降噪/回声消除 |
| 录音机 App 接真实引擎 + 诚实状态 | 蓝牙上传 / 云端同步 |
| 排障钩子 + 可复现的验收工具 | 多麦克风阵列、采样率切换 |

---

## 2. 硬件接口与连接（本次的关键变化）

### 2.1 完整接线表（模块 = 鹿小班 ES8311 + NS4150B）

| 模块脚 | STM32 | AF | 排针 | 状态 |
|---|---|---|---|---|
| SCL | PB6 | I2C1_SCL (AF4) | P1-19 | 已接（控制通道） |
| SDA | PB7 | I2C1_SDA (AF4) | P1-20 | 已接（⚠ 与 SCL 曾交叉，见开发日志） |
| MCLK | PC6 | I2S2_MCK (AF5) | P1-45 | 已接（**录音也靠它**：无 MCLK ⇒ ADC 无时钟） |
| SCLK | PD3 | I2S2_CK (AF5) | P1-27 | 已接 |
| LRCK | PB9 | I2S2_WS (AF5) | P1-18 | 已接（代价：UART4 回退作废） |
| **DIN**（模块输入） | PB15 | I2S2_SDO (AF5) | P2-37 | 已接（放音数据） |
| **DOUT**（模块输出） | **PC2** | **I2S2_SDI (AF5)** | **P2-9** | ★ **本次接通**（录音数据） |
| 5V / GND | — | — | P1-3/4、P1-1/2 | 已接（模块**要 5V**，给 NS4150B 供电） |

### 2.2 与 PC2 的接口要点

* **方向**：`DOUT` 是**模块的输出**（芯片 ASDOUT），PC2 在本方案里只做**输入**
  （`GPIO_MODE_AF_PP` + `GPIO_NOPULL` + AF5）。**同一根线上只有模块在驱动**
  ⇒ 不存在推挽对撞。（对比历史教训：把 STM32 的 SDO 怼在模块 DOUT 上 = 两个输出对撞，
  DAC 从此收不到数据 ⇒ "I2C/时钟/寄存器全对却完全无声"。）
* **绝不做的事**：不要为了让 PC2 "有波形"而给它加推挽驱动或强灌电平（硬件红线）。
  PC2 上只能做**读**（`GPIOC->IDR` 是安全的，见 §7 诊断阶梯第 1 步）。
* **为什么只能用 PC2**：I2S2_SDI 在 LQFP100 上只有 PC2 与 PB14 两个候选，
  而 **PB14 被电容触摸 GT9xxx 占用**（PB11~PB14 四颗一颗都不能动）⇒ 只剩 PC2。
* **连接方式**：单根杜邦线（模块 DOUT → P2-9）；共地；线尽量短（3.125 MHz BCLK）。
* **丝印方向**：模块按**模块自身视角**命名 —— `DIN` = 芯片输入（放音数据）、
  `DOUT` = 芯片输出（录音数据）。这个坑已经栽过两次，接线前先确认。

---

## 3. 数据流与代码地图

```
麦克风 ──┐
         ├─(模拟)─► ES8311 内部：PGA(REG16, 0x24) → ADC → 芯片 ASDOUT
模块 MIC 输入 ──┘
                                    │
                        模块 DOUT ──┴──► 排针 P2-9 (PC2, I2S2_SDI, AF5)
                                    │
        I2S2 主收（I2SCFGR=0x02080407：Philips/16bit-in-32slot/I2SDIV=8/MCKOE=1）
                                    │
              DMA1_Stream3（DMAMUX 请求 39 = SPI2_RX，CIRCULAR，VERY_HIGH）
                                    │
                  s_cap_buf 8 KB（.bss_audio → SRAM1，DMA 可访问）
                                    │
        audio_out_capture_avail() / audio_out_capture_read()  ← 主循环每拍取
                                    │
                  s_chunk 8 KB（.bss_sram2 → SRAM2，**不进 DTCM**）
                                    │
                 f_write → /YMGUI/RECORD/REC_xxxx.WAV（先占位头，停止时回填）
```

| 环节 | 文件 | 关键函数 |
|---|---|---|
| I2S 公共配置（时钟/格式） | `src/audio_out.c` | `i2s_apply(mode)`（放音与录音共用，防两份参数分叉） |
| I2S2 主收 + DMA | `src/audio_out.c` | `audio_out_capture_start/avail/read/stop` |
| ADC/麦克风通路 | `src/es8311.c` | `es8311_adc_start(mic_gain)` / `es8311_adc_stop()` |
| 控制通道就绪 | `src/es8311.c` | `es8311_bus_ready()`（新增：录音可能是开机后第一个音频动作） |
| 引擎 + WAV 落卡 | `src/audio_rec.c` | `audio_rec_start/poll/stop`、`wav_make_header()`（纯函数） |
| App 桥接层 | `tests…` → `third_party/.../phone_shell_board.h` | `BoardRec_*`（UI 只拿"能直接画的东西"） |
| 录音机 App | `third_party/.../apps/recorder.c` | 真实计时/电平/诚实状态 |
| 启动清零 | `src/board_startup.c` | 清 `.bss_sram2` |
| 链接布局 | `linker/stm32h743vit6.ld` | `.bss_sram2 → SRAM2` |

---

## 4. 关键设计决策（每条都有踩坑依据）

1. **I2S2 独占 + 与放音互斥。** `I2SCFG` 是"主发/主收"二选一（RM0433 §50.7.6）
   ⇒ 录音与放音**不能同时**。仲裁点唯一：`audio_out_capture_start()`；若放音还在
   **异步淡出**中就返回 `AUDIO_CAP_AGAIN(2)`，调用方**下一拍重试**（不阻塞主循环，
   单拍仍 ≤60 ms）。放音侧下次起播会自动走 `i2s_init()` 配回主发，不需要额外恢复动作。
2. **放音与录音共用同一个 `i2s_apply()`。** 时钟/格式逐位一致，避免"放音正常、录音变调/错位"
   这类最难查的故障。
3. **采集进度用 DMA 的 NDTR 反推，但必须有合理性闸。**
   ⚠ 踩坑：NDTR 反推法在"DMA 被停/被重配"时会看到**任意跳变**，原写法只要 `adv≠0` 就累加
   ⇒ 实测凭空造出 **80 754 半字/秒**（约 40 k 帧/秒），文件被写出 33.8 MB 零数据。
   现在两道闸：① 只有 `DMA_SxCR.EN==1` 才认；② 单次推进 ≤ 半缓冲；不通过就丢弃并计入
   `g_cap_jump`。**这条属于"假数据零容忍"的落实。**
4. **大缓冲一律不进 DTCM。** ⚠ 踩坑：把 8 KB 暂存放进 DTCM 会把 heap0 从 ~35.6 KB 压到
   26.6 KB，而 YMGUI 建整个界面需要 >26.6 KB ⇒ 分配器返回 NULL 被写穿 ⇒
   `CFSR=0x400(IMPRECISE) / HFSR=0x40000000(FORCED)`，**开机就 HardFault**，
   `g_shell_rc` 停在初值 99。修法：新增 `.bss_sram2`（→ SRAM2）+ 启动清零。
5. **录音时关掉 DAC 通路**（`REG09 bit6=1`）+ `REG32=0`：I2S2 转收以后 PB15 不再驱动
   模块 DIN（悬空输入），把 DAC 通路关掉才能保证喇叭**绝对安静**。
6. **`REG17`（ADC 数字音量）必须显式写 0xBF 并回读确认。**
   `es8311_stop_dac()` 会把它写成 0x00（"ADC 音量→0"）⇒ 走过一次真·掉电之后再录音就是
   **一片静音，而所有其它回读都正常**。
7. **UI 三条不许造假**：计时取 `BoardRec_ElapsedMs()`（按**已写进文件的帧数**折算）、
   电平取 `BoardRec_Level()`（真实采样峰值）、开录 2 s 后仍无信号就如实提示。

---

## 5. 已实现并验证的部分（本轮）

| # | 项目 | 板上证据 |
|---|---|---|
| 1 | 固件可启动、UI 正常 | `g_shell_rc=0`、`g_fault.magic=0`、`g_loop_count` 递增、heap0 free 1 176 B |
| 2 | ES8311 ADC 通路配置 | `g_es_adc_on=1`；回读 `REG0A=0x3C`(bit6=0 通路开)、`REG09=0x5C`(bit6=1 DAC关)、`REG17=0xBF` |
| 3 | I2S2 主收寄存器 | 直读 `I2SCFGR=0x02080407`（master RX / Philips / DATLEN=16bit / CHLEN=32slot / I2SDIV=8 / MCKOE=1）、`CFG1=0x000740EF`（DSIZE=15 / FTHLV=7 / **RXDMAEN=1**）、`CR1=0x201`（SPE+CSTART） |
| 4 | DMA 采集在搬 | `cap_tot` 速率 **91 166 半字/s**（标称 97 656 的 93%）、`g_cap_ovr=0`、`g_cap_lost=0`、`g_cap_jump=5~6`(被丢弃的非法跳变) |
| 5 | WAV 落卡正确 | `g_rec_hdr_ok=1`、`hdr_bad=0`；回读头 = `RIFF / 0xEE024 / WAVE / fmt / 16 / {PCM,2ch} / 48828 / 195312 / {4,16} / data / 974848`，文件实长 = data+44 |
| 6 | 计时正确 | 3 s 录到 **169 984 帧 ≈ 3.48 s** |
| 7 | 放音未被破坏 | `dac_ready=1`、`REG32` 回读=UI 音量、`g_ao_udr=0`、`g_ao_underrun=0`、停止后 `playing=0` |
| 8 | App 已接真实引擎 | `apps/recorder.c` 重写：真实计时/电平、准备中态、无信号如实提示、退出即收尾 |

---

## 6. 本轮修掉的两个真实缺陷（建议单独记档）

### 6.1 开机 HardFault（根因：新增 DTCM 挤爆 heap0）

* 现象：烧录后 `g_loop_count=0`、`g_shell_rc=99`（初值未变 ⇒ 卡在 `PhoneShell_BoardInit` 里）。
* 定位：`g_fault{kind=1( HardFault ), cfsr=0x400( IMPRECISERR ), hfsr=0x4000_0000( FORCED )}`；
  `g_h0{total=used=peak=26560, free_head=NULL}` ⇒ **heap0 用满**。
* 机理：链接脚本 `_heap0_start = _sbrk_end`（DTCM `.bss` 之后）、`_heap0_end = _estack − 32 KB`
  ⇒ **DTCM 里每多一字节，heap0 就少一字节**。`build_ui()` 分配失败返回 NULL，随后被写穿 ⇒
  不精确总线错误。
* 修法：录音的 8 KB 暂存 + FatFs 文件对象 + 路径缓冲全部搬到新增的 `.bss_sram2`（→ SRAM2），
  并在 `board_startup.c` 加清零点。修后 heap0 = **35 424 B**，峰值 34 248，余 1 176 B。
* ⚠ **遗留风险（重要）**：即使修好，heap0 也只有 **~1.2 KB** 余量（峰值占 97%）。
  这是**补丁18 之前就存在**的状态（当时约 1.7 KB）。建议单开任务：
  测得栈的高水位后把 `_heap0_end` 的 32 KB 预留下调（例如 24 KB），或再移出一块 DTCM 消费者。

### 6.2 录音起不来（rc=-2）却说不出原因

* 现象：`g_rec_rc=0xFFFFFFFE`；`g_es_wr_ok=0`、`g_es_wr_ng=12`、`g_es_rd_ok=0`、
  失败环全 `0x02`(**HAL_BUSY**)、回读全 `0xFFFF`。
* 根因：补丁18 之前**只有 `audio_out_start()` 会建 I2C 总线**；而录音可能是**开机后第一个
  音频动作** ⇒ I2C1 的时钟/GPIO/AF/HAL 句柄都没配、`s_addr7` 还是 0 ⇒ 每一笔写都失败。
* 修法：新增**幂等**的 `es8311_bus_ready()`（bus_init + 两个地址都探一次），
  在 `es8311_adc_start()` 最开头调用。

---

## 7. 当前未闭环：模拟输入无信号（含诊断阶梯）

**现象**：`g_cap_peak_l/r = 0/0`、`g_cap_nz = 0`、`g_cap_hist = [4096,0,0,0,0,0,0,0]`
（一个统计窗内 4 096 个样本的绝对值**全落在 bin0 = 恰好 0**）。

**它说明什么**：数字链路本身在正确工作（速率、帧数、WAV 头都对），但**ADC 采到的样本恒为 0**。
"恒为 0"而不是"噪声"这个特征很有信息量：它更像**某个方向被稳定驱动成低电平/或 ADC 无数据**，
而不是"输入悬空"（悬空输入通常会有抖动的非零值）。但**单靠这一个特征无法区分**
"线没通/接错脚"与"模块没有麦克风/偏置没起来"。⇒ 按下面四步阶梯逐级排除。

### 诊断阶梯（按成本从低到高，每一步都有明确判据）

1. **PC2 线活动度采样（只读，零风险，最该先做）**
   采集期间循环读 `GPIOC->IDR` 的 bit2（**纯读，不驱动**），统计 0/1 比例与变化次数：
   * 比例接近 0/100 且**变化次数 ≈ 0** ⇒ PC2 上**没有任何东西在驱动**（线没通/接错脚）。
   * 0/1 都有、变化频繁 ⇒ 模块确实在发送 ⇒ 问题在**模拟侧**（麦克风/偏置/PGA），
     跳到第 2 步。
2. **麦克风与偏置**
   确认模块是否有**板载麦克风**；若无，需要按模块丝印接 2 线驻极体麦克风（MIC±）。
   同时确认 ADC 的内部参考已开（`REG44=0x58` 已写）与 PGA 增益（`REG16=0x24`，可现场扫
   0x00~0x3F 看峰值是否变化 —— 有一处必须注意：**寄存器值改了就应能改变读数**，
   若完全不变说明数据通路根本没到 ADC）。
3. **ADC/SDP 侧补寄存器**
   若第 1 步证明线在动、第 2 步确认有麦克风但仍是 0，则回到 ES8311 的 ADC 输出通路：
   核对 `REG02/REG03/REG04/REG05`（ADC 的 fs/OSR 分频）与 `REG0A` 的其余位
   （SDP 的 word length / 数据对齐，ADC 侧必须与 DAC 侧一致：16 bit I2S）。
   ⚠ 用**一次只改一个寄存器**的方式二分，并把每次结果记进 `g_cap_hist`（本设计已提供该量）。
4. **示波器/逻辑分析仪看 DOUT**
   1、2、3 都排除后，用示波器在模块 DOUT 上看有无波形（有 BCLK/LRCK 的前提下）。
   有波形而 MCU 仍读到 0 ⇒ 回到 I2S2 RX 的采样边沿/对齐（`CKPOL`/`WSINVERSION`/`FirstBit`）。

### 立刻可用的"人耳判据"（最便宜的一步）

在录音中**对着麦克风说话/拍手**，同时读 `g_cap_peak_l/r`：
* **数值明显变化** ⇒ 链路通、问题只是"房间太安静" ⇒ 收工。
* **毫无变化** ⇒ 按上面四步走。

---

## 8. 关键参数表（改动前先看这张表）

| 项 | 值 | 位置/说明 |
|---|---|---|
| I2S 采样率 fs | **48 828 Hz**（MCLK 12.5 MHz ÷ 256） | 由 `i2s_apply()` 从 PLL1DIVR 现场算出 I2SDIV=8；**WAV 头必须写 48828**（写 48000 会偏 1.7%） |
| 数据格式 | 16 bit 数据 / 32 bit 时隙（`I2S_DATAFORMAT_16B_EXTENDED`） | `CFG1.DSIZE=15`；codec 侧 `bclk_div=4` ⇒ SCK=64fs |
| I2S 方向 | 录音=`I2S_MODE_MASTER_RX`（I2SCFG=11） | 与放音主发**二选一** |
| DMA | DMA1_Stream3、请求 **39**(SPI2_RX)、CIRCULAR、VERY_HIGH、半字/半字 | ⚠ 请求号与 `CFG1.RXDMAEN(bit14)` 写错 = NDTR 不动 |
| 采集缓冲 | `s_cap_buf` 8 KB @SRAM1（DMA）；`s_chunk` 8 KB @SRAM2（CPU） | **都不许进 DTCM** |
| 每次取数 | 4 096 半字 = 8 KB = 2 048 帧 ≈ 42 ms | 与放音半缓冲同量级 |
| PGA 增益 | `REG16 = 0x24`（`g_rec_mic_gain` 可现场调） | 官方默认值 |
| ADC 数字音量 | `REG17 = 0xBF`（0 dB） | ⚠ 被 `stop_dac` 写成 0 就是静音 |
| 通路开关 | `REG0A bit6=0`（ADC 开）、`REG09 bit6=1`（DAC 关） | 回读确认后才置 `g_es_adc_on` |
| 有信号判据 | `g_rec_peak_max ≥ g_rec_mic_thresh`（默认 128 ≈ −48 dBFS） | `BoardRec_MicOk()` |
| 最长录音 | `g_rec_max_ms`（默认 300 000 = 5 min） | 忘按停不会写满卡 |
| 目录/命名 | `/YMGUI/RECORD/REC_%04u.WAV` | 序号 = 目录内最大值 +1 |

---

## 9. 验证方案

### 9.1 自动（板上，无需人耳）

```bash
# 1) 起录 → 看链路是否在搬、有没有丢/溢出
#    (g_rec_test=1 起录 / =2 停录；全程可 SWD 触发，不必碰屏)
python tools/rec_check.py            # 见 §9.3 要实现的内容
```

判据（全部要有数字）：
`g_rec_state=1` → `g_es_adc_on=1` → `g_cap_ovr=0` → `g_cap_lost=0` →
`cap_tot` 速率 ∈ [0.9, 1.05]×97 656 半字/s → 停止后 `g_rec_hdr_ok=1` 且
`g_rec_fsize == 44 + g_rec_bytes`。

### 9.2 人工（两步，唯一不可代劳的部分）

1. 打开录音机 App → 点"开始录音" → **对着麦克风说话** → 电平条应当随声音起伏；
   若 2 秒内没信号，界面会**自己**提示"未检测到麦克风信号：请查模块 DOUT → P2-9(PC2) 接线"。
2. 点"停止录音" → 状态行显示真实的 `已保存 REC_xxxx.WAV · N.N 秒 · M KB`。
   把 TF 卡插到 PC 上放这个文件，应当能听到自己刚才说的话。

### 9.3 待补的验收工具

`tools/rec_check.py`（下一步就做）：把 §9.1 的判据写成脚本，并**顺带做 §7 第 1 步**
（PC2 线活动度采样）—— 把"线通不通"也变成一个数字。

---

## 10. 兼容性与影响面

| 场景 | 影响 | 依据 |
|---|---|---|
| 音乐播放 | **无回归**（已实测） | `dac_ready=1`、REG32 回读正确、`udr=0`、`underrun=0`；重构后 `i2s_init()` 只是 `i2s_apply(TX)` 的薄封装 |
| 播放 ↔ 录音 | **互斥**（设计如此） | 录音前自动停音乐，`BoardRec_Start()` 返回 2 让 UI 显示"准备中…"，不阻塞 |
| 其它 App / 桌面 | 无 | 录音机 App 只多了几条 `BoardRec_*` 调用；`app_create`/`destroy` 未变形状 |
| TF 卡布局 | 新增 `/YMGUI/RECORD` | 与 `/YMGUI/MUSIC`、`/YMGUI/PIC`、`/YMGUI/NOTE` 并列；`.WAV` 与音乐播放器同格式 |
| 内存 | DTCM 持平（⚠ heap0 余量本就是瓶颈，见 §6.1） | SRAM1 77%、SRAM2 54% |
| FLASH | 1 673 068 B（79.78%） | 距 2 MB 还有 ~410 KB |
| 中英双语 | App 文案用中文字面量（可被 `phone_lang.c` 翻译表命中）；引擎侧状态串为中文（与 `BoardBt_StatusText` 同惯例） | — |
| UART4 回退 | **无新增代价** | PB9 早已被 I2S2_WS 占用；录音不额外借脚 |

---

## 11. 风险与回滚

| 风险 | 缓解 |
|---|---|
| heap0 只剩 ~1.2 KB，任何新增大块 DTCM 分配都会再次开机 HardFault | ① 大缓冲一律用 `.bss_sram2`/`.bss_audio`；② 建议单开任务下调栈预留（32 KB→24 KB，需先测栈高水位） |
| 录音与放音互斥被误用（例如在播放中直接起录） | 仲裁点唯一、返回 2 + UI"准备中"；`BoardRec_Start()` 幂等 |
| 采集进度反推被误信 | 两道合理性闸 + `g_cap_jump` 计数；`g_cap_lost`/`g_cap_ovr` 都必须为 0 才算合格 |
| 未接线/无麦克风时"看起来在录" | `BoardRec_MicOk()` + UI 如实提示（已实现） |
| 需要回滚 | 只需注释掉 `main.c` 的 `audio_rec_poll()` 并把 `apps/recorder.c` 恢复为演示壳；`src/audio_rec.c` 可整文件移除，链接脚本的 `.bss_sram2` 保留无害 |

---

## 12. 下一步（按优先级）

1. **`tools/rec_check.py`**：把 §9.1 判据 + §7 第 1 步（PC2 线活动度，只读 IDR）做成脚本。
2. **跑 §7 阶梯**，把"模拟输入无信号"闭环（这一步需要人对着麦克风发声配合）。
3. 视结论决定是否补 ADC/SDP 寄存器（§7 第 3 步，一次只改一个）。
4. 闭环后：更新 `ci/firmware_reference.sha256`、同步 `DEVELOPMENT_LOG.md` 与
   `docs/TF_CARD_APP_INTEGRATION_FEASIBILITY.md`（把"录音硬件上不成立"的旧结论改掉）。
5. 单开任务：heap0 余量（§6.1 遗留风险）。
