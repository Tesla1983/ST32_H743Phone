/* ===========================================================================
 * 音频输出层（出货构建的常态模块，2026-10-12）
 *
 * 【为什么要有这一层】
 *   src/audio_test.c 是"自检"：SWD 触发、跑完 fid 走人， pin 用完就还回去。
 *   音乐播放是"常态功能"：用户随时点开就要出声，且**必须在默认 CMake option 下编进来**
 *   （出货固件）。两者需求不同，所以拆：
 *     audio_out.c  = 出货常驻（I2C1 + ES8311 + I2S2 出音 + 音量；出音默认走 I2S TX DMA，
 *                   写 `g_ao_use_dma=0` 可回退到 TIM6 中断滴灌）
 *     wav_play.c   = 上层播放源（TF 卡 WAV → 喂给 audio_out）
 *
 * 【接线（方案 A，已出声，权威表见 docs/DEVELOPMENT_LOG.md 2026-10-12 节）】
 *   I2C1  PB6=SCL / PB7=SDA        配 ES8311 寄存器（src/es8311.c）
 *   I2S2  PC6=MCLK / PD3=SCLK / PB9=LRCK / PB15=SDO(→模块 DIN) / PC2=SDI
 *
 * 【DMA 缓冲为什么放 SRAM1】
 *   SRAM1（0x3000_0000）已被 MPU region0 配成 **非缓存**（见 third_party/BSP/MPU/mpu.c），
 *    DMA1/2 也访问得到 D2 域 ⇒ 一致性由 MPU 保证，不必每笔 Clean/Invalidate。
 *   ⚠ SDMMC 的 IDMA 是例外（只认 AXI），但它不参与本模块（卡走 FatFs 轮询读）。
 *
 * 【出音的三条 H7 铁律（见 MEMORY.md）】
 *   ① 置 SPE 不会开始传输，**必须再置 CR1.CSTART**；
 *   ② 欠载 UDR=1 会把 TXP 压在 0 且引擎停吃 FIFO，**必须写 IFCR.UDRC** 才恢复；
 *   ③ **起播防咔哒**：DAC 通路必须等 I2S 时钟与数据流就绪后再唤醒，且音量不能一步
 *      到位 —— 详见 audio_out.c 的「起播防咔哒时序」（先跑 I2S → 静音接通路 → 8 拍 ramp）。
 * =========================================================================== */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

/* 一次给 feeder 的帧数（帧 = 左右各一个 16 bit 样本 = 4 字节）。
 * 半缓冲 4 帧时长 = AUDIO_OUT_HALF_FRAMES / fs：
 *   2048 帧 / 48 828 Hz = 41.97 ms —— 这就是"补数必须在多少时间内完成"的预算。 */
#define AUDIO_OUT_HALF_FRAMES  2048u
#define AUDIO_OUT_HALF_BYTES   (AUDIO_OUT_HALF_FRAMES * 4u)

/**
 * 喂数回调：把 frames 帧交错的立体声 PCM 写进 dst（L,R,L,R...），
 * 返回实际写入的帧数；**返回 <= 0 表示没数据了**（调用方补静音、随后停播）。
 */
typedef int (*audio_out_feeder)(int16_t *dst, uint32_t frames);

/**
 * @brief  上电后的首次出声：配 I2C1 → ES8311 init/config/start → I2S2 → DMA
 * @return 0 = codec 就绪且已起播；<0 = 失败（看 g_ao_rc / g_ao_codec 定位）
 * ⚠ 失败要允许重试：本函数幂等，失败后资源全部回收到"未起播"状态。
 */
int  audio_out_start(void);

/** 停止出声：停 TIM6 心跳 → I2S 挂起 → 回收 PB9 给 UART4_TX → codec 挂起 */
void audio_out_stop(void);

/**
 * @brief  暂停出声：与 audio_out_stop 相同，**但只把 codec 静音、不断电**。
 *
 * 【为什么与 stop 分开】stop 末尾的 es8311_stop_dac() 会把 codec 打进 power-down
 *   （REG0E=0xFF / REG12=0x02），芯片掉电窗口里会 NACK 掉那 9 笔写中的若干笔；
 *   而音乐 app「暂停 → 播放」紧接着续播，续播那 ~19 笔 I2C 就会撞上这个窗口
 *   ⇒ 丢写（音量写一半 = "音量大小混乱"，全丢 = REG32 停在 0 = "没声音"）。
 *   暂停只要静音就够，无需断电 ⇒ 本函数只发 1 笔 REG32=0，芯片保持可达。
 * ⚠ 「切歌 / 播完 / 退出」仍走 audio_out_stop()（那时断电是对的，下次是全新起播）。
 */
void audio_out_pause(void);

/** 1 = 正在出声 */
int  audio_out_playing(void);

/** 挂喂数回调（换歌/停止时可换） */
void audio_out_set_feeder(audio_out_feeder cb);

/** UI 音量 0..100 → ES8311 REG32（0 = 静音，100 = 0 dB）*/
void audio_out_set_volume(uint8_t vol0_100);

/**
 * @brief  内置正弦自检：不碰 TF 卡，直接走常驻出音通路（TIM6 中断滴灌）播 3 秒 977 Hz。
 *         用来把"卡/文件问题"和"音频硬件问题"分开定位（详见 .c 里的注释）。
 * @return 同 audio_out_start
 */
int  audio_out_demo_start(void);

/** SWD 写 1 触发上面那条自检（跑完固件自动清 0）*/
extern volatile uint32_t g_ao_demo;

/**
 * @brief 主循环每拍调一次：TIM6 心跳负责把 FIFO 填到满（中断里做），
 *   本函数负责"重活"——当环形缓冲低于半满时，从 feeder（读 TF 卡/正弦源）
 *   补半个环进来；并处理欠载自恢复、demo 收尾、起播延后启动 TIM6。
 * ⚠ 真正往 TXDR 滴灌的是 TIM6 中断（20 µs 一次，详见 .c）；本函数只补源数据，
 *   因为读 TF 卡这种有不确定延迟的操作不能放在 ISR 里。
 */
void audio_out_tick(void);

/* ---- SWD 可读诊断量 ---- */
extern volatile int32_t  g_ao_rc;        /* 最近一次 start 的结果：0=OK，<0=失败阶段 */
extern volatile uint32_t g_ao_codec;     /* 1 = ES8311 初始化成功（I2C 有应答且序列写完） */
extern volatile uint32_t g_ao_playing;   /* 1 = 正在出声 */
extern volatile uint32_t g_ao_udr;       /* 欠载次数（UDR 置位并被清掉的次数）*/
extern volatile uint32_t g_ao_frames;    /* 累计送出帧数（÷48828 ≈ 秒数）*/
extern volatile uint32_t g_ao_underrun;  /* 补数没跟上、被迫填静音的次数（>0 = 会听到卡顿）*/
extern volatile uint32_t g_ao_push;      /* TIM6 ISR 实际写进 FIFO 的半字数 */
extern volatile uint32_t g_ao_pump;      /* 主循环补过多少次环形缓冲 */
extern volatile uint32_t g_ao_isr;       /* TIM6 中断次数 */
extern volatile uint32_t g_ao_isr_dry;   /* ISR 进来却发现环形缓冲没数据的次数 */
extern volatile uint32_t g_ao_txp_seen;  /* tick 里看到 TXP=1 的次数（时钟在不在跑）*/
extern volatile uint32_t g_ao_stage;     /* 起播阶段进度（1..6，见 .c；不是错误码）*/
extern volatile uint32_t g_ao_dbg[24];   /* 寄存器现场快照（索引表见 .c），probe 读不了外设。
                                          * [16..23] = 欠载分类 + 起播时序（补丁12）*/

/* ---- I2S TX DMA 开关与诊断（2026-10-13；**默认 1 = 走 DMA**）----
 * `g_ao_use_dma` 默认已置 1（用户复听确认 DMA 路径干净）；写 0 回退到 TIM6 滴灌。
 * 置 1 后起播就走"SPI 自己发 DMA 请求、DMA 搬 TXDR"那条路。
 * 判据（tools/i2s_dma_check.py）：
 *   · g_ao_dma_rc == 0 且 g_ao_dma_half / g_ao_dma_cplt 在涨；
 *   · g_ao_frames 在涨（声音在流）而 g_ao_isr **不涨**（TIM6 已不参与）；
 *   · g_ao_dma_err == 0；
 *   · g_ao_dbg2[0] 的 bit15（SPI_CFG1.TXDMAEN）= 1，g_ao_dbg2[8] 低 7 位 = 40（SPI2_TX）；
 *   · g_ao_dbg2[5]（DMA_SxNDTR）应在 0..4096 间**变化**（DMA 真在搬）。 */
extern volatile uint32_t g_ao_use_dma;   /* 1 = 走 I2S TX DMA（默认）；0 = 回退 TIM6 滴灌 */
extern volatile uint32_t g_ao_dma_rc;    /* DMA 起播返回码：0=OK 1=HAL_DMA_Init 失败 2=Start_IT 失败 */
extern volatile uint32_t g_ao_dma_half;  /* 半传中断次数 */
extern volatile uint32_t g_ao_dma_cplt;  /* 全传中断次数 */
extern volatile uint32_t g_ao_dma_err;   /* DMA 错误回调次数 */
extern volatile uint32_t g_ao_dbg2[16];  /* DMA/SPI 现场快照（索引表见 .c）*/
extern volatile uint32_t g_ao_ring_drop; /* 环形缓冲满被丢弃的半字数（应恒 0；>0 = 补数越界、会丢音）*/
extern volatile uint32_t g_ao_dma_gap_max;/* 相邻两次 **半传输(HT)** 中断的最大间隔(µs)。
                                           * ⚠ 口径（2026-10-14 修正）：HT 每个完整 DMA 周期只来
                                           * 一次（HT@21 ms、全传@42 ms）⇒ HT→HT 正常值就是
                                           * **≈42 000**，不是 21 000（旧注释按"半缓冲时长"写，
                                           * 曾把它误读成异常）。真异常判据 = 明显 > 42 000
                                           * （=DMA 引擎被 SPI 的 UDR 压停过）。*/

/* ---- 欠载**分类** + 起播窗口时序（2026-10-14 补丁12）----
 * 【分类的意义】g_ao_underrun 把两条性质完全不同的路混成一个数：
 *   ① dma_fill_half() 环里不够 ⇒ 该半缓冲尾部填**硬静音**直进 DAC ⇒ **听得到"咔/断"**；
 *   ② pump_half() feeder 取不满（卡慢/短读/播到尾巴）⇒ 缺口通常极小，多数听不出来。
 * 只报总数就分不清该修哪条。 */
extern volatile uint32_t g_ao_under_dma;   /* ① 次数：DMA 半缓冲补不满（会听到）*/
extern volatile uint32_t g_ao_under_feed;  /* ② 次数：feeder 取不满（多半无害）*/
extern volatile uint32_t g_ao_under_short; /* 单次最大缺口（半字）；2048 = 整半缓冲全填零 = 21 ms 静音 */
extern volatile uint32_t g_ao_under_first_ms;/* 起播后第一次 ① 距 CSTART 的 ms（0=从未发生）*/
extern volatile uint32_t g_ao_t_cstart_ms; /* CSTART 时刻的 HAL_GetTick()（本次播放的时间原点）*/
extern volatile uint32_t g_ao_t_pump1_ms;  /* 起播后第一次补满环的完成时刻（相对 CSTART）*/
extern volatile uint32_t g_ao_t_dacwake_ms;/* DAC 通路唤醒完成时刻（相对 CSTART）*/
extern volatile uint32_t g_ao_t_ramp_ms;   /* 起播音量 ramp 走完时刻（相对 CSTART）*/
extern volatile uint32_t g_ao_ring_used_min;/* 播放期环形缓冲占用最小值（半字）= 抗卡顿垫子的下限 */

/* ★ 2026-10-14（补丁13）：防爆音闭环的量。
 * 【为什么必须有】"切 I2S 时钟之前 DAC 必须已经静音"是防爆音的最后一道防线，而 I2C 写
 *   是**会失败**的（实测首试 NACK 5%~30%，失败环里 0x31 静音 / 0x32 音量命中最多）。
 *   补丁13 把"写入"升级成"写入 + 回读确认"，这两个量就是"确认失败"的可见计数 ——
 *   没有它们，这个坏状态是**看不出来的**（音量/电源回读全都会显示"正常"）。 */
extern volatile uint32_t g_ao_silent_fail; /* 静音未能确认的次数（>0 = 有爆音风险）*/
extern volatile uint32_t g_ao_csusp_ms_max;/* i2s_stop() 等"先挂起生效"的最长耗时(ms)，正常 0~1 */
extern volatile uint32_t g_ao_csusp_tmo;   /* 等满 100 ms 兜底才走的次数（>0 = 停机路径被拖长）*/

/* ---- 起播"防咔哒"时序（2026-10-13）----
 * 起播那一声响来自"DAC 通路在 I2S 时钟还没建立时就上电，且音量一步到位"。
 * 现在改成三步：I2S 先跑 → 静音接通 DAC → 音量分步 ramp。诊断量：
 *   g_ao_dac_ready  0 = 还没唤醒 DAC（起播初期）/ 1 = 已唤醒
 *   g_ao_dac_ramp   音量 ramp 剩余步数（0 = 已到 UI 音量；ramp 期间 8→1）*/
extern volatile uint32_t g_ao_dac_ready;
extern volatile uint32_t g_ao_dac_ramp;
extern volatile uint32_t g_ao_wake_cnt;  /* "静音接通 DAC + 起 ramp"执行了几次
                                          * （每次起播/续播恰 +1；>1 ⇒ 起播被重复触发）*/
extern volatile int32_t  g_ao_init_rc;   /* 最近一次 codec 初始化序列的返回码（0=全写成功）*/
extern volatile uint32_t g_ao_init_skips;/* 跳过完整 codec init 的次数（续播路径应每次 +1）*/
extern volatile uint32_t g_ao_wake_retry;/* "静音接通 DAC"整段因丢写而重试的次数 */
extern volatile uint32_t g_ao_probe_n;   /* 排障钩子：写 N ⇒ 接下来 N 拍各读 3 个 codec 寄存器
                                          * （量稳态 I2C 失败率用，只读不改）*/
/* ★ 2026-10-13 动作级计数（消除验收脚本 P1 的"窗口跨播放期"伪影）：
 *   暂停/续播各自在**动作本身**累加，与 I2C 流量、探针密度、窗口长度无关。 */
extern volatile uint32_t g_ao_pause_calls;  /* audio_out_pause() 真正执行次数 */
extern volatile uint32_t g_ao_pause_volwr;  /* 暂停动作里对 REG32 的写笔数（应 = pause_calls）*/
extern volatile uint32_t g_ao_resume_calls; /* 起播/续播动作真正执行次数 */
/* ⚠ 2026-10-13 环形缓冲的一次语义修正（影响两条出音路径）：
 *   s_wr/s_rd 改为**单调计数器**（只在取索引时 & RING_MASK）。旧的
 *   `(s_wr - s_rd) & RING_MASK` 占用式在"环满"时读成 0（与"环空"同形），
 *   DMA 路径下会越界补数、覆盖未读数据（实测音源被多吃 1.62 倍）。 */

#endif /* AUDIO_OUT_H */
