/* ===========================================================================
 * 音频输出层（出货构建的常态模块，2026-10-12）
 *
 * 【为什么要有这一层】
 *   src/audio_test.c 是"自检"：SWD 触发、跑完 fid 走人， pin 用完就还回去。
 *   音乐播放是"常态功能"：用户随时点开就要出声，且**必须在默认 CMake option 下编进来**
 *   （出货固件）。两者需求不同，所以拆：
 *     audio_out.c  = 出货常驻（I2C1 + ES8311 + I2S2 + TIM6 中断滴灌出音 + 音量）
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
 * 【出音的两条 H7 铁律（2026-10-12 实测，见 MEMORY.md）】
 *   ① 置 SPE 不会开始传输，**必须再置 CR1.CSTART**；
 *   ② 欠载 UDR=1 会把 TXP 压在 0 且引擎停吃 FIFO，**必须写 IFCR.UDRC** 才恢复。
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
extern volatile uint32_t g_ao_dbg[16];   /* 寄存器现场快照（索引表见 .c），probe 读不了外设 */

/* ---- I2S TX DMA 实验开关与诊断（2026-10-13；默认全关 = 走 TIM6 滴灌，零回归）----
 * 置 g_ao_use_dma = 1 再起播，就会改走"SPI 自己发 DMA 请求、DMA 搬 TXDR"那条路。
 * 判据（tools/i2s_dma_check.py）：
 *   · g_ao_dma_rc == 0 且 g_ao_dma_half / g_ao_dma_cplt 在涨；
 *   · g_ao_frames 在涨（声音在流）而 g_ao_isr **不涨**（TIM6 已不参与）；
 *   · g_ao_dma_err == 0；
 *   · g_ao_dbg2[0] 的 bit15（SPI_CFG1.TXDMAEN）= 1，g_ao_dbg2[8] 低 7 位 = 40（SPI2_TX）；
 *   · g_ao_dbg2[5]（DMA_SxNDTR）应在 0..4096 间**变化**（DMA 真在搬）。 */
extern volatile uint32_t g_ao_use_dma;   /* 1 = 起播走 I2S TX DMA；0 = TIM6 滴灌（默认）*/
extern volatile uint32_t g_ao_dma_rc;    /* DMA 起播返回码：0=OK 1=HAL_DMA_Init 失败 2=Start_IT 失败 */
extern volatile uint32_t g_ao_dma_half;  /* 半传中断次数 */
extern volatile uint32_t g_ao_dma_cplt;  /* 全传中断次数 */
extern volatile uint32_t g_ao_dma_err;   /* DMA 错误回调次数 */
extern volatile uint32_t g_ao_dbg2[16];  /* DMA/SPI 现场快照（索引表见 .c）*/
extern volatile uint32_t g_ao_ring_drop; /* 环形缓冲满被丢弃的半字数（应恒 0；>0 = 补数越界、会丢音）*/

/* ⚠ 2026-10-13 环形缓冲的一次语义修正（影响两条出音路径）：
 *   s_wr/s_rd 改为**单调计数器**（只在取索引时 & RING_MASK）。旧的
 *   `(s_wr - s_rd) & RING_MASK` 占用式在"环满"时读成 0（与"环空"同形），
 *   DMA 路径下会越界补数、覆盖未读数据（实测音源被多吃 1.62 倍）。 */

#endif /* AUDIO_OUT_H */
