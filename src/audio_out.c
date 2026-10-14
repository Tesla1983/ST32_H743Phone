/* ===========================================================================
 * 音频输出层实现 —— 设计说明见 audio_out.h
 *
 * 【出音用哪条路：默认 I2S TX DMA（2026-10-13 起）；TIM6 滴灌为回退路径】
 *   ★ 现状：出货构建默认走 **SPI2(I2S2) TX 的 DMA1_Stream4 + DMAMUX（请求 40）**，
 *     8 KB 乒乓缓冲，CPU 只在半/全传中断里补数（约 95 次/秒，而不是 5 万次/秒）。
 *     运行期开关 `g_ao_use_dma`（默认 1）；写 0 回退到下面的 TIM6 滴灌。
 *   ★ 两条路径的**起播时序都已做"防咔哒"处理**（见下方「起播防咔哒时序」）。
 *
 * 【为什么当初先做的是 TIM6 路（历史，保留因为踩坑结论仍有价值）】
 *   第一版走的是 DMA1_Stream2 循环双缓冲，板上实测**送不出数据**：
 *     SPI_CR1=0x201（SPE+CSTART 都在）、I2SCFGR 分频正确、内核时钟 200 MHz 正常、
 *     DMA 的 CR/CIRC/MINC/PSIZE 都对，但 SR 恒为 0、NDTR 一秒只动几十项
 *     ⇒ I2S 的 TXP（FIFO 有空位）一次都没出现，DMA 请求也就无从谈起。
 *   追了两轮没拿到更细的证据（主机侧 probe-rs 读不到外设寄存器，只能靠 RAM 快照），
 *   ⇒ 改成**已被验证过的那条路**：audio_test.c 里出过声的做法是"轮询 TXP → 写 TXDR"
 *     （at_stream→at_tx_wait，case 4 实测出声），这里把它搬进 **TIM6 的周期中断**
 *     （20 µs），ISR 里把 FIFO 填到满——与 at_stream 的内层循环是同一套逻辑，只是
 *     从"主循环阻塞轮询"变成"定时器中断滴灌"，从而**不冻结 UI**。
 *     主循环只负责把 TF 卡/正弦源的数据补进环形缓冲（RING），不在 ISR 里碰文件系统。
 *
 *   ⚠ 为什么必须靠中断滴灌：I2S TX FIFO 只有 16 个半字（HAL 的 FTHLV 上限即 16 可证），
 *     48 828 Hz × 2 声道下 1 个半字 = 10.24 µs ⇒ 整 FIFO 也就 ~164 µs；主循环一帧
 *     ~16 ms，远慢于 164 µs，纯主循环补数会让 FIFO 长期枯竭
 *     ⇒ 连续出声**只能**靠"高频中断"或"DMA"。
 *
 *   ⚠ 为什么 20 µs：取 FIFO 预算（82 µs）的 ~1/4 做余量。ISR 每次最多填 8 个半字，
 *     开销可忽略；万一 TXP 因"时钟没起"而恒为 1，ISR 内的 8 次上限也保证它不会被
 *     拖成死循环（否则会饿死主循环 —— 2026-10-12 实测过这个后果）。
 *
 * 【接线 / H7 两条铁律】见 audio_out.h 顶部与 docs/DEVELOPMENT_LOG.md 的
 * 「2026-10-12 · ES8311 音频模块接入」一节。
 * =========================================================================== */
#include "audio_out.h"
#include "es8311.h"

#include "stm32h7xx_hal.h"

#include <string.h>
#include <math.h>

/* ---- 诊断量 ---- */
volatile int32_t  g_ao_rc       = 99;
volatile uint32_t g_ao_codec    = 0;
volatile uint32_t g_ao_playing  = 0;
volatile uint32_t g_ao_udr      = 0;
volatile uint32_t g_ao_frames   = 0;     /* 累计送出帧数（÷48828 ≈ 秒数）*/
volatile uint32_t g_ao_underrun = 0;     /* 主循环补数没跟上、被迫填静音的次数 */
volatile uint32_t g_ao_push     = 0;     /* ISR 实际写进 FIFO 的半字数 */
volatile uint32_t g_ao_pump     = 0;     /* 主循环补过多少次环形缓冲 */
volatile uint32_t g_ao_txp_seen = 0;     /* tick 里看到 TXP=1 的次数（时钟在不在跑）*/
volatile uint32_t g_ao_isr      = 0;     /* TIM6 中断次数 */
volatile uint32_t g_ao_isr_dry  = 0;     /* ISR 进来却发现环形缓冲没数据的次数 */
/* 起播阶段进度（排障用，**不是**错误码）：
 *   1 = codec 序列写完   2 = I2S 初始化 OK   3 = 环形缓冲已铺满
 *   4 = SPE/CSTART 已置  5 = TIM6 心跳已起   6 = 全部就绪（此时 g_ao_rc=0）
 * ⚠ 为什么单独要一个阶段量：上一版出现过"起播函数没走完、系统就停摆"的情况，
 *   而 g_ao_rc 只在失败路径上写 ⇒ 靠它无法区分"卡在第几步"。 */
volatile uint32_t g_ao_stage    = 0;

/* ---- 2026-10-13：I2S TX 走 DMA 的开关与诊断（**默认 1 = 走 DMA**）----
 * 2026-10-13 用户复听确认 DMA 路径干净（无断音/咔哒）⇒ 默认已从 TIM6 切到 DMA。
 * 写 0 可回退到 TIM6 滴灌（对照/排障用；两条路径的代码都保持可用）。
 * 第一版 DMA（DMA1_Stream2 循环双缓冲）当年结论是"SPI_SR 恒为 0、NDTR 不涨"。
 * 但那个结论有个**方法论隐患**：当时是主机侧 probe-rs 直接读外设寄存器 —— 而本工程
 * 自己的记录写着"probe-rs 读 0x4000_xxxx 不可信，只能读 RAM"。所以这次：
 *   ① 外设寄存器一律**拷进 RAM（g_ao_dbg2）再用 SWD 读**，不再直接读外设；
 *   ② 显式补上最可能漏的一步 —— **SPI_CFG1.TXDMAEN**（H7 上不置这一位，SPI 根本
 *      不产生 TX DMA 请求，NDTR 自然一动不动；HAL 在 HAL_I2S_Transmit_DMA 里是
 *      单独 SET_BIT 的，见 stm32h7xx_hal_i2s.c:1467-1471）。 */
volatile uint32_t g_ao_use_dma   = 1;    /* 1 = 起播走 I2S TX DMA（默认）；0 = 回退 TIM6 滴灌 */
volatile uint32_t g_ao_dma_rc    = 99;   /* DMA 起播返回码（0=OK）*/
volatile uint32_t g_ao_dma_half  = 0;    /* 半传中断次数 */
volatile uint32_t g_ao_dma_cplt  = 0;    /* 全传中断次数 */
volatile uint32_t g_ao_dma_err   = 0;    /* DMA 错误回调次数 */
volatile uint32_t g_ao_dbg2[16];         /* DMA/SPI 现场快照（见 dbg2_snapshot）*/
volatile uint32_t g_ao_ring_drop = 0;    /* 环形缓冲满而被丢弃的半字数（应为 0；>0 = 补数越界）*/
volatile uint32_t g_ao_dma_gap_max = 0;  /* 相邻两次"半传输中断"之间的**最大间隔(µs)**：
                                          * ⚠ 口径修正（2026-10-14）：半传中断(HT)每个完整 DMA
                                          * 周期只来一次（HT@21 ms、全传@42 ms）⇒ **HT→HT 的
                                          * 正常值本来就是 ≈42 000 µs，不是 21 000**。旧注释按
                                          * "半缓冲时长"写，曾把它误读成异常。真正的异常判据是
                                          * 明显 **超过 42 000**（=DMA 引擎被 UDR 压停过）。*/

/* ---- 2026-10-14（补丁12）：欠载**分类** + 起播窗口时序量化 ----
 * 【为什么必须分类】`g_ao_underrun` 把两条完全不同的路混在一个数里：
 *   ① `dma_fill_half()` 发现环形缓冲不足 ⇒ 该半缓冲尾部填**硬静音**、直接进 DAC
 *      ⇒ 阶跃 ⇒ **听得到"咔/断音"**；
 *   ② `pump_half()` 的 feeder 取不满（卡慢 / 播到尾巴 / f_read 短读）⇒ 缺口一般很小，
 *      多数时候听不出来。
 * 只报一个总数 ⇒ 无法判断该修哪条。⇒ 拆开计数，并把"最大缺口有多大"记下来
 * （缺口 = 缺多少个半字；2048 = 整整半个缓冲全被填零 = 21 ms 静音）。
 * 【为什么还要起播时序表】用户报的"按下播放那一声咔"集中在起播窗口。有了
 * CSTART 时刻与"第一次补满环 / DAC 唤醒 / ramp 走完"的时刻差，就能直接回答
 * "起播后主循环被堵了多久、堵到有没有把环里的垫子耗光"。 */
volatile uint32_t g_ao_under_dma   = 0u;   /* ① 次数：DMA 半缓冲补不满（**会听到**）*/
volatile uint32_t g_ao_under_feed  = 0u;   /* ② 次数：feeder 取不满（多半无害）*/
volatile uint32_t g_ao_under_short = 0u;   /* 单次最大缺口（半字数）；2048 = 整半缓冲全填零 */
volatile uint32_t g_ao_under_first_ms = 0u;/* 起播后第一次 ① 距 CSTART 的 ms（0=从未发生）*/
volatile uint32_t g_ao_t_cstart_ms  = 0u;  /* CSTART 那一刻的 HAL_GetTick()（本次播放的时间原点）*/
volatile uint32_t g_ao_t_pump1_ms   = 0u;  /* 起播后第一次补满环形缓冲的完成时刻（相对 CSTART）*/
volatile uint32_t g_ao_t_dacwake_ms = 0u;  /* DAC 通路唤醒（静音接通）完成时刻（相对 CSTART）*/
volatile uint32_t g_ao_t_ramp_ms    = 0u;  /* 起播音量 ramp 走完时刻（相对 CSTART）*/
volatile uint32_t g_ao_ring_used_min = 0xFFFFFFFFu; /* 播放期环形缓冲占用最小值（半字）
                                          * ⇒ 起播/播放中"垫子"最少还剩多少（4096 ≈ 42 ms）*/

/* ---- 起播"防咔哒"时序（2026-10-13；诊断量声明见 audio_out.h）----
 * 【那一声"咔哒"从哪来】起播序列原来把 DAC 通路（es8311_start_dac）放在 I2S **之前**
 *   调用，且 REG32 一步写到目标音量 ⇒ 两个跳变叠在一起：
 *     ① DAC 从 power-down 醒来时，MCLK/BCLK 还没建立 ⇒ 时钟一到就产生一次直流跳变；
 *     ② 音量从"静音"瞬间跳到目标值 ⇒ 幅度再跳一次。
 * 【修法】把动作拆成三步、按物理顺序排：
 *     1) 先起 I2S（时钟跑起来、FIFO 有数据）—— 起播函数只做这一步；
 *     2) 下一拍 tick 再 `es8311_dac_start_muted()`：通路接通，音量仍是 0（静音）；
 *     3) 随后 8 拍把 REG32 从 0 分步升到 UI 音量（≈ 8 × 16 ms ≈ 130 ms）。
 *   ⚠ 为什么 DAC 唤醒要交给 tick 而不是在 audio_out_start() 里直接做：TIM6 路径
 *     在 CSTART 之后 FIFO 还是空的，要等 tick 里起心跳、ISR 喂上第一口才有数据。
 *     统一"起播第二拍再开 DAC"，两条路径（TIM6 / DMA）都不必各写一套。 */
volatile uint32_t g_ao_dac_ready = 0u;   /* 1 = DAC 通路已唤醒（起播初期为 0）*/
volatile uint32_t g_ao_dac_ramp  = 0u;   /* 音量 ramp 剩余步数（0 = 已到目标音量）*/
volatile uint32_t g_ao_wake_cnt  = 0u;   /* "唤醒 DAC + 起 ramp"这个动作执行过几次
                                          * （每次起播/续播应当恰好 +1；>1 ⇒ 起播被重复触发）*/
volatile int32_t  g_ao_init_rc   = 0;    /* 最近一次 codec 初始化序列的返回码（0=全写成功）*/
volatile uint32_t g_ao_init_skips = 0u;  /* 跳过了多少次完整 codec init（续播路径应当每次 +1）*/
volatile uint32_t g_ao_wake_retry = 0u;  /* "静音接通 DAC"这一整段因有写丢失而重试的次数 */
volatile uint32_t g_ao_probe_n   = 0u;   /* 排障钩子：写 N ⇒ 接下来 N 拍各做一次 codec 回读体检
                                          * （用于量"稳态下 I2C 失败率"，不依赖有没有在播放）*/
/* ★ 2026-10-13：动作级计数器 —— 消除验收脚本 P1 的"测量窗口伪影"。
 * 【为什么需要】wav_pause_resume_check 的 P1 用 `B.wr_ok − A.wr_ok` 反推"暂停写了几笔"，
 *   而 A(起播)→B(暂停) 的窗口**跨了播放期**，播放中"每秒回读体检"补写的 REG32 会被
 *   误计进去 ⇒ 间歇报"暂停发了 2/16 笔"（实测复跑全部证伪为伪影）。窗口无论如何收紧都
 *   无法根除（播放期必须存在）。⇒ 改由**动作本身**计数：暂停/续播各自在同一处累加，
 *   与 I2C 流量、探针密度、窗口长度都无关，是确定性判据。 */
volatile uint32_t g_ao_pause_calls = 0u;   /* audio_out_pause() 被真正执行（非 early-return）的次数 */
volatile uint32_t g_ao_pause_volwr = 0u;   /* 暂停动作里对 REG32 的写笔数（应恒 = g_ao_pause_calls）*/
volatile uint32_t g_ao_resume_calls = 0u;  /* 续播（起播）动作被真正执行的次数 */

/* 起播音量 ramp 的步数：8 步 × 主循环一拍（≈16 ms）≈ 130 ms 完成。
 * 步数太少压不住跳变、太多会让人听出"声音起得慢"，8 是折中。 */
#define AO_VOL_RAMP_STEPS 8u

/* 起播/续播后"音量确认期"的拍数上限：每拍一次回读（含失败重试与补写），
 * 最多 12 拍 ≈ 190 ms。取 12 的理由：ramp 8 拍之后还要跨过一个可能的 NACK 窗口
 * （实测窗口持续到数十毫秒），给足余量又不至于长期占用 I2C 影响播放。 */
#define AO_RB_CONFIRM_TRIES 12u

/* 暂停淡出等待拍数（2026-10-14 修"播放/暂停咔嗒声"）：
 * 暂停时先把 REG32 写成 0（ES8311 硬件软 ramp：REG37=0x48 = 0.25 dB / 32 LRCK，
 * ≈ 0.655 ms / 0.25 dB）。DAC 音量被硬件平滑降向 -95.5 dB。从 0 dB 降到听感静音
 * （~-60 dB）约需 240 步 ≈ 157 ms ⇒ 取 12 拍（≈192 ms）再停 I2S 传输，此时已基本
 * 无声，切断时钟不再可闻。
 * ⚠ 为什么必须"先静音、后停时钟"：原来 audio_out_pause 先 i2s_stop() 掐时钟、再
 *   es8311_dac_mute()，时钟断的瞬间 DAC 还满音量 ⇒ 输出电平瞬跳 ⇒ 那声"咔嗒"。 */
#define AO_PAUSE_FADE_TICKS 12u

/* ---- 环形缓冲 → SRAM1 的 .bss_audio（MPU region0：非缓存）----
 * 8192 个半字 = 16 KB = 4096 帧 ≈ 84 ms 的音频；一次补半个环（2048 帧 ≈ 42 ms）。 */
#define AUDIO_BUF_ATTR   __attribute__((section(".bss_audio"), aligned(32)))
#define RING_HALFWORDS   8192u
#define RING_MASK        (RING_HALFWORDS - 1u)
#define RING_HALF_FRAMES (RING_HALFWORDS / 4u)      /* 2048 帧 = 4096 半字 */
static uint16_t AUDIO_BUF_ATTR s_ring[RING_HALFWORDS];
/* feeder 的落脚点（连续缓冲，补完再拷进 ring）—— 2048 帧 × 4 B = 8 KB */
static int16_t  AUDIO_BUF_ATTR s_tmp[RING_HALF_FRAMES * 2u];

/* ⚠⚠ 2026-10-13 修正：s_wr/s_rd 改成**单调递增计数器**（不再每次 & RING_MASK，
 *   只在取数组索引时才掩码）。原因：`(s_wr - s_rd) & RING_MASK` 的占用式在
 *   **环满（差 = 8192）时会读成 0 —— 与"环空"同形**。旧代码靠"每拍只补半个环 +
 *   消费慢"侥幸没暴露；换成 DMA 后消费变成确定性排空，一次越界补数就会把
 *   未读数据覆盖掉，实测音源被多吃 1.62 倍（163 255 B/s vs 应有 97 656 B/s）。
 *   单调计数器下：差 = 0 才是空、差 = 8192 就是满，语义唯一。
 *   ⚠ 回绕在 2^32，是 8192 的整数倍 ⇒ 掩码索引仍一致（96k 半字/s 下约 12 h 才回绕）。 */
static volatile uint32_t s_wr;          /* 写计数（半字，单调），主循环推进 */
static volatile uint32_t s_rd;          /* 读计数（半字，单调），ISR 推进 */
static volatile uint8_t  s_tim_pending = 0u;   /* 起播延后到下一拍再起 TIM6（见下）*/
static volatile uint8_t  s_dac_pending = 0u;   /* 起播后延到下一拍再唤醒 DAC（防咔哒，见上）*/
static volatile uint8_t  s_vol_ramp_left = 0u; /* 音量 ramp 剩余步数（0 = 不动）*/
static volatile uint8_t  s_dac_rb_pending = 0u;/* "确认期"剩余尝试次数（见 tick；0 = 不在确认期）*/
static volatile uint32_t s_rb_tick = 0u;       /* 播放中定期回读体检的节流计数（~1 s 一次）*/
static volatile uint8_t  s_pause_fade_ticks = 0u;  /* 暂停淡出：写完 REG32=0 后，延几拍等 HW 软 ramp 落静音再停传输（0 = 不在淡出）*/

static I2S_HandleTypeDef s_hi2s;        /* I2S2 = SPI2 的 I2S 模式 */
static audio_out_feeder  s_feeder = NULL;

/* ---- I2S TX DMA 的乒乓缓冲（放在 .bss_audio：SRAM1、非缓存、DMA1 可达）----
 * 半缓冲 = 1024 帧 = 2048 半字 ≈ 21 ms（48 828 Hz）。DMA 循环模式跑满整个 4096 半字；
 * 半传中断补前半、全传中断补后半。主循环仍负责把卡/正弦源补进 s_ring。 */
#define DMA_HALF_FRAMES   1024u
#define DMA_HALF_HALFWORDS (DMA_HALF_FRAMES * 2u)         /* 2048 */
#define DMA_TOTAL_HALFWORDS (DMA_HALF_HALFWORDS * 2u)     /* 4096 */
static uint16_t AUDIO_BUF_ATTR s_dma_buf[DMA_TOTAL_HALFWORDS];
static DMA_HandleTypeDef s_hdma_tx;
static uint8_t  s_dma_ready = 0u;
static uint32_t s_last_half_cyc;      /* 上一次半传输中断的 DWT 周期数（量间隔用）*/

/* 内置正弦自检（不依赖 TF 卡），见 audio_out.h 的 audio_out_demo_start。
 * 用 200 样本 LUT + 每帧步进 4 ⇒ 频率 = 4 × fs / 200 = 4 × 48828 / 200 ≈ 977 Hz，
 * 与 audio_test.c case 4 同一口径（已实测出声）。 */
#define DEMO_HZ      977u
#define DEMO_SEC     3u
#define DEMO_LUT_N   200u
#define DEMO_AMP     28000           /* 峰值 ≈85% 满幅（先排除"太轻听不见"）*/
static int16_t  s_demo_lut[DEMO_LUT_N];
static uint32_t s_demo_left;
static uint32_t s_demo_acc;           /* LUT 相位累加（样本数）*/
static uint8_t  s_demo_done;
volatile uint32_t g_ao_demo = 0;

/* UI 音量的权威值在 main.c（es8311 起播时会读它写 REG32）。 */
extern volatile uint32_t g_audio_volume;

/* ---- 寄存器现场快照（2026-10-12 排障用）----
 * ⚠⚠ 为什么必须有它：主机侧 probe-rs **读不到外设寄存器**（只对 RAM 地址有效，
 *   读 0x4000_xxxx 返回空）—— 要拿外设现场只能把寄存器**拷进 RAM** 再让 SWD 读。
 * 索引：0 SPI_CR1 / 1 SPI_SR / 2 SPI_CFG1 / 3 SPI_I2SCFGR / 4 TIM6_ARR
 *      5 ring 已用半字 / 6 g_ao_push / 7 ISR 空转次数 / 8 g_ao_isr
 *      9 NVIC ISER0 / 10 ring 起始地址低 16 位 / 11 txp_seen
 *     12 RCC->APB1LENR / 13 RCC->D2CCIP1R
 *     14/15 保留
 *     16..23（2026-10-14 补丁12 新增）：欠载分类与起播时序 —— 见 g_ao_under_* 的注释。
 *      16 g_ao_underrun   17 g_ao_under_dma   18 g_ao_under_feed  19 g_ao_under_short
 *      20 g_ao_under_first_ms  21 g_ao_t_pump1_ms  22 g_ao_t_dacwake_ms  23 g_ao_ring_used_min
 * ⚠ 主机侧一次 `probe-rs read b32 &g_ao_dbg 24` 就能整块拿走 —— 这几个量都是"同时刻"的，
 */
volatile uint32_t g_ao_dbg[24];

static void dbg_snapshot(void)
{
    g_ao_dbg[0]  = s_hi2s.Instance->CR1;
    g_ao_dbg[1]  = s_hi2s.Instance->SR;
    g_ao_dbg[2]  = s_hi2s.Instance->CFG1;
    g_ao_dbg[3]  = s_hi2s.Instance->I2SCFGR;
    g_ao_dbg[4]  = TIM6->ARR;
    g_ao_dbg[5]  = s_wr - s_rd;
    g_ao_dbg[6]  = g_ao_push;
    g_ao_dbg[7]  = g_ao_isr_dry;
    g_ao_dbg[8]  = g_ao_isr;
    g_ao_dbg[9]  = NVIC->ISER[0u];
    g_ao_dbg[10] = (uint32_t)((uintptr_t)&s_ring[0] & 0xFFFFu);
    g_ao_dbg[11] = g_ao_txp_seen;
    g_ao_dbg[12] = RCC->APB1LENR;
    g_ao_dbg[13] = RCC->D2CCIP1R;
    g_ao_dbg[14] = 0u;
    g_ao_dbg[15] = 0u;
    g_ao_dbg[16] = g_ao_underrun;
    g_ao_dbg[17] = g_ao_under_dma;
    g_ao_dbg[18] = g_ao_under_feed;
    g_ao_dbg[19] = g_ao_under_short;
    g_ao_dbg[20] = g_ao_under_first_ms;
    g_ao_dbg[21] = g_ao_t_pump1_ms;
    g_ao_dbg[22] = g_ao_t_dacwake_ms;
    g_ao_dbg[23] = g_ao_ring_used_min;
}

/* ---- DMA 现场快照（2026-10-13）----
 * 与 dbg_snapshot 同一理由：主机读不到外设寄存器，必须拷进 RAM。
 * 索引：0 SPI_CFG1（bit15 = TXDMAEN ← 重点看这一位）
 *      1 SPI_CR1（bit0 SPE / bit9 CSTART）  2 SPI_SR（bit1 TXP）  3 SPI_I2SCFGR
 *      4 DMA_SxCR  5 DMA_SxNDTR  6 DMA_SxPAR  7 DMA_SxM0AR
 *      8 DMAMUX 通道 CCR（低 7 位 = 请求号，SPI2_TX 应为 40）
 *      9 dma_half 10 dma_cplt 11 dma_err  12 ring 已用半字
 *     13 NVIC ISER[0]（bit15 = DMA1_Stream4）14 RCC->AHB1ENR  15 s_dma_ready */
static void dbg2_snapshot(void)
{
    g_ao_dbg2[0]  = SPI2->CFG1;
    g_ao_dbg2[1]  = SPI2->CR1;
    g_ao_dbg2[2]  = SPI2->SR;
    g_ao_dbg2[3]  = SPI2->I2SCFGR;
    g_ao_dbg2[4]  = DMA1_Stream4->CR;
    g_ao_dbg2[5]  = DMA1_Stream4->NDTR;
    g_ao_dbg2[6]  = DMA1_Stream4->PAR;
    g_ao_dbg2[7]  = DMA1_Stream4->M0AR;
    g_ao_dbg2[8]  = (s_hdma_tx.DMAmuxChannel != NULL) ? s_hdma_tx.DMAmuxChannel->CCR : 0u;
    g_ao_dbg2[9]  = g_ao_dma_half;
    g_ao_dbg2[10] = g_ao_dma_cplt;
    g_ao_dbg2[11] = g_ao_dma_err;
    g_ao_dbg2[12] = s_wr - s_rd;
    g_ao_dbg2[13] = NVIC->ISER[0u];
    g_ao_dbg2[14] = RCC->AHB1ENR;
    g_ao_dbg2[15] = s_dma_ready;
}

/* ====================== I2S2（SPI2）====================== */

static void i2s_gpio_init(void)
{
    GPIO_InitTypeDef g;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;      /* 全部 AF5，DS12110 Table 9 逐脚核对过 */

    g.Pin = GPIO_PIN_6;               /* PC6 = I2S2_MCK */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_3;               /* PD3 = I2S2_CK */
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_9;               /* PB9 = I2S2_WS ⚠ 会顶掉 UART4_TX，stop 时还原 */
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_15;              /* PB15 = I2S2_SDO → 模块的 DIN */
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_1;               /* PC1 = I2S2_SDO（兼容另一种接线，用户只接一根即可）*/
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;               /* PC2 = I2S2_SDI（录音用，本版不用）*/
    HAL_GPIO_Init(GPIOC, &g);
}

/* 把 PB9 还给 UART4_TX（uart_link.c 开机时的配置）：借出去的脚用完必须还回去。 */
static void i2s_gpio_release(void)
{
    GPIO_InitTypeDef g;

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF8_UART4;
    g.Pin       = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &g);
}

static int i2s_init(void)
{
    s_hi2s.Instance              = SPI2;
    s_hi2s.Init.Mode             = I2S_MODE_MASTER_TX;
    s_hi2s.Init.Standard         = I2S_STANDARD_PHILIPS;
    /* ⚠ 必须是 EXTENDED（16 位数据装 32 位时隙 ⇒ SCK = 64fs），
     *   ES8311 侧 esp-adf 系数表也是 bclk_div=4（64fs）。改成 16B 会错位。 */
    s_hi2s.Init.DataFormat       = I2S_DATAFORMAT_16B_EXTENDED;
    s_hi2s.Init.MCLKOutput       = I2S_MCLKOUTPUT_ENABLE;
    s_hi2s.Init.AudioFreq        = I2S_AUDIOFREQ_48K;
    s_hi2s.Init.CPOL             = I2S_CPOL_LOW;
    s_hi2s.Init.FirstBit         = I2S_FIRSTBIT_MSB;
    s_hi2s.Init.WSInversion      = I2S_WS_INVERSION_DISABLE;
    s_hi2s.Init.Data24BitAlignment = I2S_DATA_24BIT_ALIGNMENT_RIGHT;
    s_hi2s.Init.MasterKeepIOState  = I2S_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_I2S_Init(&s_hi2s) != HAL_OK)
    {
        return -1;
    }

    /* 显式写一遍 I2SDIV/ODD：不依赖 HAL 的时钟查询，从 PLL1DIVR 现场推内核时钟。
     * 200 MHz / 16 = 12.5 MHz MCLK ⇒ fs = 12.5M/256 = 48 828 Hz。
     * ⚠ I2SDIV 在 I2SCFGR bits 23:16、ODD 在 bit 24（曾按 bits 7:0 解码读出假值）。 */
    {
        uint32_t sysclk = SystemCoreClock;
        uint32_t p1     = ((RCC->PLL1DIVR >> 9) & 0x7Fu) + 1u;
        uint32_t q1     = ((RCC->PLL1DIVR >> 16) & 0x7Fu) + 1u;
        uint32_t kclk   = (sysclk * p1) / q1;
        uint32_t target = 48000u * 256u;
        uint32_t div    = (kclk + target / 2u) / target;
        uint32_t odd    = div & 1u;
        uint32_t i2sdiv = (div - odd) / 2u;

        if (i2sdiv < 1u)   { i2sdiv = 1u;  odd = 0u; }
        if (i2sdiv > 255u) { i2sdiv = 255u; odd = 1u; }
        MODIFY_REG(s_hi2s.Instance->I2SCFGR,
                   (SPI_I2SCFGR_I2SDIV | SPI_I2SCFGR_ODD),
                   ((i2sdiv << SPI_I2SCFGR_I2SDIV_Pos) | (odd << SPI_I2SCFGR_ODD_Pos)));
    }

    /* ⚠ 显式把 CFG1.DSIZE 设成 **16 位**（复位值是 7 = 8 位，HAL 在 I2S 模式下
     *   根本不碰这个字段）。ISR 里按 16 位往 TXDR 写，两边必须一致。 */
    MODIFY_REG(s_hi2s.Instance->CFG1, SPI_CFG1_DSIZE, (15u << SPI_CFG1_DSIZE_Pos));

    /* 清全部挂起标志（与 HAL_I2S_Init 的 WRITE_REG(IFCR, 0x0FF8) 同口径），
     * 免得上一次残留的 UDR/SUSP 把 TXP 压在 0。 */
    s_hi2s.Instance->IFCR = 0x0FF8u;
    return 0;
}

static void i2s_stop(void)
{
    /* 先挂起再关：CSUSP 让当前帧自然结束、硬件自清 CSTART，不在时钟中途硬切。 */
    if (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
    {
        uint32_t t0 = HAL_GetTick();

        SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSUSP);
        while (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
        {
            if (HAL_GetTick() - t0 > 100u)
            {
                break;                 /* 100 ms 兜底，不等死 */
            }
        }
    }
    __HAL_I2S_DISABLE(&s_hi2s);
    /* 还原 TX FIFO 的 DMA 请求阈值到复位值（阈值 1）：DMA 路径会把它抬到"半满"，
     * 这里在 SPE=0 之后写回，保证 TIM6 路径拿到的仍是它一直在用的默认值。 */
    MODIFY_REG(s_hi2s.Instance->CFG1, SPI_CFG1_FTHLV, 0u);
    HAL_I2S_DeInit(&s_hi2s);
}

/* ====================== TIM6：喂 FIFO 的心跳 ======================= */

static void tim6_start(void)
{
    /* ⚠ 周期取 20 µs：FIFO 只有 8 个半字、48.8 kHz 立体声下够撑约 82 µs，
     *   取 1/4 当余量。APB1 定时器时钟 = 200 MHz（APB1=100 MHz × 2）⇒
     *   PSC=0、ARR=3999 ⇒ 4000 个周期 = 20 µs。
     *   即使实际只有 100 MHz（40 µs）也仍在 82 µs 预算内 —— 这就是留 4 倍余量的意义。 */
    __HAL_RCC_TIM6_CLK_ENABLE();

    TIM6->CR1  = 0u;
    TIM6->PSC  = 0u;
    TIM6->ARR  = 3999u;
    TIM6->CNT  = 0u;
    TIM6->SR   = 0u;                    /* 清更新中断标志 */
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->CR1  = TIM_CR1_CEN;

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

static void tim6_stop(void)
{
    HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
    TIM6->DIER = 0u;
    TIM6->SR   = 0u;
    TIM6->CR1  = 0u;
}

/* ====================== I2S TX DMA（2026-10-13 实验路径）======================
 * 目标：把"每个 20 µs 一次中断、ISR 里 CPU 写 TXDR"换成"SPI 自己发 DMA 请求、
 * DMA 从内存搬 16 位半字到 TXDR"。CPU 只在"半缓冲发完"时被叫一次（每 21 ms），
 * 中断频率从 5 万次/秒降到约 95 次/秒。
 *
 * ⚠ 两个必须先讲清的前提：
 *   ① 缓冲放在 .bss_audio（SRAM1，D2 域）：DMA1 能访问、且 MPU 配成**非缓存**
 *      ⇒ 不需要任何 cache 维护（SDMMC/IDMA 那条路的经验）。
 *   ② DMA 的"SIZE"是**半字个数**（不是帧、不是字节）：16 位数据格式下 1 帧 = 2 半字。 */

/* 把一个半缓冲（2048 半字 ≈ 21 ms）从环形缓冲填过去；不够就补静音。
 * ⚠ 只做内存拷贝，**绝不碰文件系统** —— 它会在 DMA 中断里被调用。 */
static void dma_fill_half(uint32_t half)
{
    uint16_t *dst   = &s_dma_buf[half * DMA_HALF_HALFWORDS];
    uint32_t  avail = s_wr - s_rd;

    if (avail > DMA_HALF_HALFWORDS)
    {
        avail = DMA_HALF_HALFWORDS;
    }
    for (uint32_t i = 0u; i < avail; ++i)
    {
        dst[i] = s_ring[s_rd & RING_MASK];
        s_rd++;
    }
    for (uint32_t i = avail; i < DMA_HALF_HALFWORDS; ++i)
    {
        dst[i] = 0u;
    }
    if (avail < DMA_HALF_HALFWORDS)
    {
        /* ⚠ 这是**真会被听到**的一类欠载：尾部这些半字是硬 0，紧跟着前面真实音乐进 DAC
         *   ⇒ 电平从任意值瞬跳到 0（再跳回来）⇒ 一声"咔"。缺口越大越明显。 */
        uint32_t short_by = DMA_HALF_HALFWORDS - avail;

        g_ao_underrun++;                /* 供不上：这一半尾部填静音 */
        g_ao_under_dma++;
        if (short_by > g_ao_under_short)
        {
            g_ao_under_short = short_by;
        }
        if ((g_ao_under_first_ms == 0u) && (g_ao_t_cstart_ms != 0u))
        {
            g_ao_under_first_ms = HAL_GetTick() - g_ao_t_cstart_ms;
        }
    }
    g_ao_push   += avail;
    g_ao_frames += (avail / 2u);        /* 2 个半字 = 1 帧 */
}

static void dma_half_cb(DMA_HandleTypeDef *h)
{
    (void)h;
    g_ao_dma_half++;

    /* 量"相邻两次半中断的间隔"：正常 = 半缓冲播放时长 ≈ 1024 帧 ≈ 21 ms。
     * 出现 UDR 的机理是"DMA 补数被总线/仲裁推迟"，推迟越久、间隔越大 ⇒ 这个最大值
     * 直接能量出停顿幅度（TIM6 路径不涉及 DMA 端口，可作反证）。 */
    {
        uint32_t now = DWT->CYCCNT;
        uint32_t gap = now - s_last_half_cyc;
        uint32_t us  = (uint32_t)(((uint64_t)gap * 1000000u) / (SystemCoreClock ?
                                                               SystemCoreClock : 1u));
        if (s_last_half_cyc != 0u && us > g_ao_dma_gap_max)
        {
            g_ao_dma_gap_max = us;
        }
        s_last_half_cyc = now;
    }

    dma_fill_half(0u);                  /* 前半刚发完 ⇒ 补前半 */
}

static void dma_cplt_cb(DMA_HandleTypeDef *h)
{
    (void)h;
    g_ao_dma_cplt++;
    dma_fill_half(1u);
}

static void dma_err_cb(DMA_HandleTypeDef *h)
{
    (void)h;
    g_ao_dma_err++;
}

/* 起 I2S TX DMA。顺序照抄 HAL_I2S_Transmit_DMA：
 *   HAL_DMA_Start_IT → 置 CFG1.TXDMAEN → 使能 SPE → 置 CR1.CSTART */
static int i2s_dma_start(void)
{
    /* ⚠ DMAMUX1 与 DMA1 同在 D2-AHB1、**没有独立时钟使能位**（RCC_AHB1ENR 里只有
     *   DMA1EN/DMA2EN）—— 所以开 DMA1 时钟就等于开 DMAMUX1。这里不要写
     *   __HAL_RCC_DMAMUX1_CLK_ENABLE()：H7 的 HAL 根本没这个宏。 */
    __HAL_RCC_DMA1_CLK_ENABLE();

    s_hdma_tx.Instance                 = DMA1_Stream4;
    s_hdma_tx.Init.Request             = DMA_REQUEST_SPI2_TX;      /* = 40 */
    s_hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    s_hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    s_hdma_tx.Init.Mode                = DMA_CIRCULAR;
    /* ⚠⚠ 必须 **VERY_HIGH**：LCD 的 band 推送也在 DMA1（Stream1，优先级 HIGH，MEM2MEM）。
     * 两者同为 HIGH 时走轮转仲裁，界面重绘（进度条/时钟每 250 ms~1 s 一动）期间的
     * MEM2MEM 传输会把 SPI2_TX 的补数推迟到 FIFO 垫子之外 ⇒ UDR ⇒ 引擎停吃 FIFO
     * ⇒ 约 16 ms 的"断音"。SPI2_TX 每个请求只搬 1 个半字、间隔 ~10 µs，提到 VERY_HIGH
     * 不会饿死 LCD（2026-10-13 实测 UDR ≈ 1 次/s 时加的这条）。 */
    s_hdma_tx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    s_hdma_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;     /* 直接模式 */
    s_hdma_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    s_hdma_tx.Init.MemBurst            = DMA_MBURST_SINGLE;
    s_hdma_tx.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    if (HAL_DMA_Init(&s_hdma_tx) != HAL_OK)
    {
        g_ao_dma_rc = 1u;
        return -1;
    }

    s_hdma_tx.XferHalfCpltCallback = dma_half_cb;
    s_hdma_tx.XferCpltCallback     = dma_cplt_cb;
    s_hdma_tx.XferErrorCallback    = dma_err_cb;

    dma_fill_half(0u);                  /* 先铺满两个半缓冲，起播第一拍就有数据 */
    dma_fill_half(1u);

    if (HAL_DMA_Start_IT(&s_hdma_tx, (uint32_t)(uintptr_t)s_dma_buf,
                         (uint32_t)(uintptr_t)&SPI2->TXDR,
                         DMA_TOTAL_HALFWORDS) != HAL_OK)
    {
        g_ao_dma_rc = 2u;
        return -2;
    }

    /* ★★ H7 关键一步 ★★：不置 TXDMAEN，SPI **根本不会发出 TX DMA 请求**，
     *   DMA 的 NDTR 就一动不动 —— 与当年"NDTR 不涨"的现象完全吻合。
     *   HAL 在 HAL_I2S_Transmit_DMA 里是单独 SET_BIT 的（见 hal_i2s.c:1467-1471），
     *   自己手搓 DMA 时最容易漏掉这一步。 */
    SET_BIT(SPI2->CFG1, SPI_CFG1_TXDMAEN);

    /* ★★ 第二个关键位：TX FIFO 的 DMA 请求阈值 FTHLV ★★
     *   HAL 的 I2S 初始化**从不设置** FTHLV（HAL_I2S_Init / HAL_I2S_Transmit_DMA 都不碰），
     *   复位值 = 0 ⇒ 阈值只有 **1 个数据**：DMA 要等 FIFO 几乎空了才被叫来补。
     *   一个半字 @48828Hz 立体声 = 10.24 µs，只要总线被 SDMMC IDMA / 渲染争用超过这么点，
     *   TX FIFO 就断供 → 置 UDR → 而 H7 的 UDR 会**让引擎停吃 FIFO**，直到主循环
     *   下一拍写 IFCR.UDRC 清掉（最坏 ~16 ms）⇒ 一声可闻的"断音/咔哒"。
     *   2026-10-13 实测：FTHLV=0 时 DMA 路径 g_ao_udr 稳定 +1/s（TIM6 路径为 0）。
     *   设成**半个 FIFO（16 深 ⇒ 阈值 8 个数据）**给 DMA 留 ~82 µs 垫子。
     *   ⚠ 2026-10-13 实测教训：把阈值抬到 15 个数据时 DMA **完全不搬**（NDTR 不动、
     *     半中断=0、SR.TXC 置位）—— 说明 FTHLV 并非"≤ 阈值就请求"这么简单，
     *     阈值取太靠近满会抑制请求。8（半满）是实测可搬运的值，别乱改大。
     *   ⚠ SPI2.cfg1 只能在 SPE=0 时写 —— 这里在 __HAL_I2S_ENABLE 之前，合法。
     *   ⚠ 复位值(阈值 1)由 i2s_stop() 在关掉 SPE 后还原，免得影响 TIM6 路径。 */
    MODIFY_REG(SPI2->CFG1, SPI_CFG1_FTHLV,
               (7u << SPI_CFG1_FTHLV_Pos));      /* 阈值 = 8 个数据（半满，实测可搬） */

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    __HAL_I2S_ENABLE(&s_hi2s);

    /* ★ 第三个细节：**先让 DMA 把 TX FIFO 灌满，再置 CSTART**。
     *   CSTART 之后 SCK 立刻开始跑；若那一刻 FIFO 还是空的，第一个时钟就取不到数据
     *   ⇒ 置 UDR ⇒ 起播第一声"啪"。SPE 一置位 TXP 即为 1、DMA 请求随即产生，
     *   灌满 16 深的 FIFO 只要几 µs。这里带计数上限，异常时不至于死等。 */
    for (uint32_t t = 0u; t < 100000u; ++t)
    {
        if ((READ_BIT(SPI2->SR, SPI_SR_TXP) & SPI_SR_TXP) == 0UL)
        {
            break;                       /* TXP=0 ⇒ FIFO 已满 */
        }
    }

    SET_BIT(SPI2->CR1, SPI_CR1_CSTART);
    g_ao_t_cstart_ms = HAL_GetTick();   /* 起播时间原点（后面几个 g_ao_t_* 都相对它）*/

    s_dma_ready = 1u;
    g_ao_dma_rc = 0u;
    return 0;
}

static void i2s_dma_stop(void)
{
    if (s_dma_ready == 0u)
    {
        return;
    }
    HAL_NVIC_DisableIRQ(DMA1_Stream4_IRQn);
    CLEAR_BIT(SPI2->CFG1, SPI_CFG1_TXDMAEN);   /* 先关请求源，再拆 DMA */
    (void)HAL_DMA_Abort(&s_hdma_tx);
    HAL_DMA_DeInit(&s_hdma_tx);
    s_dma_ready = 0u;
}

/* DMA1_Stream4（IRQ15）：强定义（弱别名在 src/startup_gcc.s 兜底）。
 * ⚠ 与 TIM6 同样的坑：向量表若不给 IRQ15 开槽，这里定义了也接不上。 */
void DMA1_Stream4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_tx);
}

/* ⚠ 这个 handler 定义在**本文件**而不是 stm32h7xx_it.c：让模块自带 handler，
 * 保证"编进来就一定接得上"。startup 的向量表该项是 weak，本定义直接覆盖。
 * ⚠⚠ 同时也必须在 linker 里 EXTERN 出来 —— 否则 --gc-sections 会把只有 weak
 *   引用的函数整段删掉（2026-10-12 实测：handler 消失了但编译链接全绿）。 */
void TIM6_DAC_IRQHandler(void)
{
    /* 先清更新标志（防重入/累计）；写 1 清 0，TIM6 只有 UIF 这一位。 */
    TIM6->SR = (uint32_t)~TIM_SR_UIF;
    g_ao_isr++;

    /* 防御：心跳理论上只在播放期间开，但万一 stop 与 ISR 竞态，多一道闸。 */
    if (g_ao_playing == 0u)
    {
        return;
    }

    if (s_wr == s_rd)
    {
        g_ao_isr_dry++;                 /* 缓冲空了：这次什么都不做（保持上一次的值/静音）*/
        return;
    }

    /* 把 TX FIFO 填到满 —— 与 audio_test.c 出声时同一手法。
     * ⚠ 显式上限 8 次：FIFO 就 8 个半字，写超了没有意义；加计数上限是为了
     *   万一 TXP 因为"时钟没起而恒为 1"时，ISR 不会被拖成死循环（那样会
     *   把整个主循环饿死 —— 2026-10-12 实测过这个后果）。 */
    for (uint32_t k = 0u; k < 8u; ++k)
    {
        if (s_wr == s_rd)
        {
            break;
        }
        if ((READ_BIT(SPI2->SR, SPI_SR_TXP) & SPI_SR_TXP) == 0UL)
        {
            break;
        }
        *((volatile uint16_t *)&SPI2->TXDR) = s_ring[s_rd & RING_MASK];
        s_rd++;
        g_ao_push++;
        if ((g_ao_push & 1u) == 0u)      /* 2 个半字 = 1 帧 */
        {
            g_ao_frames++;
        }
    }
}

/* ====================== 环形缓冲补数 ======================= */

static void ring_put(const int16_t *src, uint32_t halfwords)
{
    for (uint32_t i = 0; i < halfwords; ++i)
    {
        if ((s_wr - s_rd) >= RING_HALFWORDS)
        {
            g_ao_ring_drop++;               /* 环已满：丢弃，绝不覆盖未读数据 */
            return;
        }
        s_ring[s_wr & RING_MASK] = (uint16_t)src[i];
        s_wr++;
    }
}

/* 补半个环（2048 帧 = 4096 半字）。feeder 给不满就补静音，绝不留下上一首的残影。 */
static void pump_half(void)
{
    uint32_t got = 0;
    int      n   = -1;

    if (s_feeder != NULL)
    {
        n = s_feeder(s_tmp, RING_HALF_FRAMES);
    }
    if (n > 0)
    {
        got = (uint32_t)n;
    }
    if (got > RING_HALF_FRAMES)
    {
        got = RING_HALF_FRAMES;
    }
    if (got < RING_HALF_FRAMES)
    {
        memset(&s_tmp[got * 2u], 0, (RING_HALF_FRAMES - got) * 4u);
        if ((s_feeder != NULL) && (s_demo_left == 0u))
        {
            g_ao_underrun++;             /* 数据供不上（卡慢/播到最后一段）*/
            g_ao_under_feed++;           /* ⚠ 与 ① 分开计：多数只是短读/尾巴，听不出来 */
        }
    }
    ring_put(s_tmp, RING_HALF_FRAMES * 2u);
    g_ao_pump++;
}

/* ====================== 内置正弦自检（不碰 TF 卡）=======================
 * 【为什么需要它】"codec + I2S + 心跳" 能不能响，与"卡上有没有 WAV、WAV 解析对不对"
 *   是两件事。先有一个不碰卡的声源，就能把两类问题分开定位：
 *     demo 响、播 WAV 不响 ⇒ 播放器/文件的问题；demo 也不响 ⇒ 音频硬件/接线的问题。 */

static void demo_lut_fill(void)
{
    int32_t peak = DEMO_AMP;

    if (peak > 32000) peak = 32000;     /* 留 1 LSB 余量，防包络乘爆 */
    if (peak < 0)     peak = 0;
    for (uint32_t i = 0u; i < DEMO_LUT_N; i++)
    {
        double s = sin(6.283185307179586 * (double)i / (double)DEMO_LUT_N);
        s_demo_lut[i] = (int16_t)(s * (double)peak);
    }
}

/* 每帧步进 LUT_STEP 个样本 ⇒ 频率 = LUT_STEP × fs / LUT_N。
 * LUT_STEP=4 ⇒ 4 × 48828 / 200 = 977 Hz（与 audio_test case 4 同口径）。 */
#define DEMO_LUT_STEP  4u
static int demo_feeder(int16_t *dst, uint32_t frames)
{
    uint32_t n = (frames < s_demo_left) ? frames : s_demo_left;

    for (uint32_t i = 0; i < n; ++i)
    {
        if (s_demo_acc >= DEMO_LUT_N) s_demo_acc -= DEMO_LUT_N;
        int16_t v = s_demo_lut[s_demo_acc];
        dst[2u * i]      = v;
        dst[2u * i + 1u] = v;
        s_demo_acc += DEMO_LUT_STEP;
    }
    s_demo_left -= n;
    if (s_demo_left == 0u)
    {
        s_demo_done = 1u;
    }
    return (int)n;
}

int audio_out_demo_start(void)
{
    demo_lut_fill();
    s_demo_left = 48828u * (uint32_t)DEMO_SEC;
    s_demo_acc  = 0u;
    s_demo_done = 0u;
    audio_out_set_feeder(demo_feeder);
    audio_out_set_volume((uint8_t)g_audio_volume);
    return audio_out_start();
}

/* ====================== 对外接口 ======================= */

void audio_out_set_feeder(audio_out_feeder cb)
{
    s_feeder = cb;
}

void audio_out_set_volume(uint8_t vol0_100)
{
    /* codec 没起来（没接模块 / 未播放）就只记录 —— g_audio_volume 是权威值，
     * es8311_start_dac() 起播时会读它写一次 REG32。 */
    (void)es8311_set_volume_ui((uint32_t)vol0_100);
}

int audio_out_start(void)
{
    int rc;

    if (g_ao_playing != 0u)
    {
        /* 正在播、但"暂停淡出"还在进行（g_ao_playing 尚未归零、s_pause_fade_ticks > 0）
         * ⇒ 取消淡出、把音量重新 ramp 回目标，否则会卡在"正在淡出到静音"且永远回不来。
         * 典型场景：快速连点 暂停→播放，暂停的淡出还没走完就点了播放。 */
        if (s_pause_fade_ticks != 0u)
        {
            s_pause_fade_ticks = 0u;
            s_dac_rb_pending   = (uint8_t)AO_RB_CONFIRM_TRIES;
            s_vol_ramp_left    = (uint8_t)AO_VOL_RAMP_STEPS;  /* 从当前低音量重新 ramp 上去 */
            g_ao_dac_ramp      = s_vol_ramp_left;
            g_ao_wake_cnt++;
            g_ao_resume_calls++;
        }
        return 0;                       /* 已在播：幂等 */
    }
    g_ao_resume_calls++;                /* ★ 动作级计数：本次起播/续播真的执行了 */

    /* 1) 控制通道：I2C1 + ES8311 */
    rc = es8311_bus_init();
    if (rc != 0)
    {
        g_ao_rc = -1;
        return -1;
    }
    /* CE 脚焊法未知 ⇒ 两个地址都探一次，探到哪个用哪个。 */
    if (es8311_probe(ES8311_I2C_ADDR_CE0) == 0)
    {
        es8311_set_addr(ES8311_I2C_ADDR_CE0);
    }
    else if (es8311_probe(ES8311_I2C_ADDR_CE1) == 0)
    {
        es8311_set_addr(ES8311_I2C_ADDR_CE1);
    }
    else
    {
        g_ao_rc = -2;                   /* 总线上没有 ES8311：没接模块或线错了 */
        g_ao_codec = 0u;
        return -2;
    }

    rc  = 0;
    /* ⚠⚠ 只在**本次开机第一次**起播时跑完整初始化序列（内含 REG00 软复位）。
     *   为什么：软复位 = 整颗芯片重启；而 codec 的 5 V 常亮、寄存器状态本来就在，
     *   暂停时 es8311_stop_dac() 写的那些寄存器，续播时 dac_path_regs() 会原样恢复
     *   ⇒ **续播根本不需要再复位一次芯片**。板上实测（tools/wav_pause_resume_check.py）：
     *   复位/电源态切换后的若干毫秒里芯片会对自己的 I2C 地址回 NACK（实测 100% 是
     *   NACK、总线电平正常 C0），而音量那几笔一丢就是用户报的"没声音 / 音量大小混乱"。
     *   少做一次复位 = 少一个 NACK 窗口，顺带把续播的 I2C 事务从 ~55 笔降到 ~19 笔。
     *   ⚠ 首次 init 失败（写丢了）时重试整段一次；仍失败才判起播失败。 */
    if (es8311_is_inited() == 0)
    {
        rc  = es8311_init();
        rc |= es8311_config_i2s();
        if (rc != 0)
        {
            rc  = es8311_init();
            rc |= es8311_config_i2s();
        }
        g_ao_init_rc = (int32_t)rc;
    }
    else
    {
        g_ao_init_skips++;              /* 续播/二次起播：跳过完整 init（诊断量）*/
        /* 格式再确认一次（幂等、只有 2 笔 I2C）。⚠ 失败**不阻断**起播：格式在上次
         * init 时已经配好，这里只是复核；真丢了也还有下面的回读体检兜着 —— 一个瞬时
         * I2C 失败不该让用户"点了播放没反应"。 */
        g_ao_init_rc = (int32_t)es8311_config_i2s();
        rc = 0;
    }
    /* ⚠⚠ 这里**不再**调 es8311_start_dac()：DAC 通路必须等 I2S 时钟与数据流就绪
     *   之后再唤醒（否则就是那一声"咔哒"）。唤醒 + 音量 ramp 交给下一拍 tick
     *   （s_dac_pending 分支）—— 见文件上方「起播防咔哒时序」的说明。 */
    if (rc != 0)
    {
        g_ao_rc = -3;
        g_ao_codec = 0u;
        return -3;
    }
    g_ao_codec = 1u;
    g_ao_stage = 1u;

    /* 2) 数据通道：I2S2 + TIM6 心跳 */
    i2s_gpio_init();
    if (i2s_init() != 0)
    {
        g_ao_rc = -4;
        return -4;
    }
    g_ao_stage = 2u;

    s_wr = 0u;
    s_rd = 0u;
    /* ★ 2026-10-14（补丁12）：起播把环形缓冲**铺满**（两次 pump = 8192 半字 = 84 ms）。
     *
     * 【为什么原来只铺半环是个坑】DMA 路径起播时，i2s_dma_start() 紧接着会调两次
     *   dma_fill_half() 把**两个半缓冲都取走**（4096 半字）——若此前只铺了半环（4096），
     *   环里就**正好空**。于是从 CSTART 到"主循环下一拍补数完成"这段时间，环里一颗数据
     *   都没有：只要这期间主循环被堵（起播那一拍要塞 codec 通路 ~16 笔 I2C + 两次现场快照
     *   + 读卡），第一个半传中断就会补进一整块硬静音 ⇒ 起播处一声"咔/断"。
     *   板上实测（tools/audio_buf_probe.py，2026-10-14）：从按下播放起算
     *   `g_ao_underrun` 一次就是 +6 ⇒ 起播窗口确实在成片填静音。
     *
     * 【修法】永远铺满整环：DMA 取走 4096 后环里仍剩 4096（= 21 ms 垫子），
     *   加上 DMA 缓冲本身的 42 ms，CSTART 瞬间手里共 63 ms 余量 —— 主循环哪怕被堵
     *   ~60 ms 也不会破音。
     *   ⚠ 安全性：写/读指针是**单调 32 位计数器**（只在取下标时 & RING_MASK），
     *     "满"表现为差值 == RING_HALFWORDS（不是 0）⇒ 不存在"满空同形"，铺满不会误判。
     *     旧注释担心的 `(wr-rd)&MASK == 0` 早已不适用于当前实现（见 ring_put 的守卫）。 */
    pump_half();
    pump_half();
    g_ao_stage = 3u;

    if (g_ao_use_dma != 0u)
    {
        /* DMA 路径：SPE / CSTART / TXDMAEN / NVIC 全在 i2s_dma_start 里按 HAL 的顺序做。 */
        if (i2s_dma_start() != 0)
        {
            g_ao_rc = -5;
            return -5;
        }
    }
    else
    {
        /* ⚠⚠ H7 铁律：SPE 只使能外设，必须再置 CSTART 才开始传输（2026-10-12 实测）。 */
        __HAL_I2S_ENABLE(&s_hi2s);
        SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART);
        g_ao_t_cstart_ms = HAL_GetTick();  /* 起播时间原点（两路径统一口径）*/

        /* ⚠⚠ 心跳**不在这里起**：tim6_start 末尾的 NVIC_EnableIRQ 会让中断在
         *   audio_out_start() 还没返回时就进来一次（即便没有 EGR_UG，也使能即可能
         *   响应挂起标志），而那时 g_ao_playing 还是 0 —— 一旦 ISR 里再出点意料之外
         *   的事，整个起播函数就再也回不去了。改成由 tick 在下一拍启动（见 audio_out_tick）。 */
        s_tim_pending = 1u;
    }
    g_ao_stage = 4u;

    g_ao_playing = 1u;
    g_ao_rc      = 0;
    g_ao_stage   = 5u;

    /* DAC 唤醒延到下一拍（此刻 I2S 刚起、FIFO 刚由 DMA 灌上，但 TIM6 路径还没喂过）。
     * 清零"防咔哒"三个状态量，保证每次起播都从"未唤醒 + 音量 0"重新走一遍。 */
    s_dac_pending   = 1u;
    s_vol_ramp_left = 0u;
    g_ao_dac_ready  = 0u;
    g_ao_dac_ramp   = 0u;
    s_rb_tick       = 0u;

    /* ⚠ 诊断量按"本次播放"清零：这些量在 ISR/主循环里只增不减，若不在这里清，
     *   多次播放会累加（上一次 3 s 的 146 k 帧会带进下一次），验收脚本算不出
     *   单次播放的精确量。仅 resetting 播放相关的计数，不动 g_ao_stage/g_ao_codec 等状态位。 */
    g_ao_frames   = 0u;
    g_ao_push     = 0u;
    g_ao_isr      = 0u;
    g_ao_isr_dry  = 0u;
    g_ao_udr      = 0u;
    g_ao_underrun = 0u;
    g_ao_pump     = 0u;
    g_ao_dma_half = 0u;
    g_ao_dma_cplt = 0u;
    g_ao_dma_err  = 0u;
    g_ao_ring_drop = 0u;
    g_ao_dma_gap_max = 0u;
    s_last_half_cyc  = 0u;
    /* 补丁12：欠载分类 + 起播时序表（每次播放重新计时）*/
    g_ao_under_dma       = 0u;
    g_ao_under_feed      = 0u;
    g_ao_under_short     = 0u;
    g_ao_under_first_ms  = 0u;
    g_ao_t_pump1_ms      = 0u;
    g_ao_t_dacwake_ms    = 0u;
    g_ao_t_ramp_ms       = 0u;
    g_ao_ring_used_min   = 0xFFFFFFFFu;
    return 0;
}

void audio_out_stop(void)
{
    if (g_ao_playing == 0u)
    {
        return;
    }
    /* ★ 2026-10-14（补丁12）：**先数字静音、再停时钟**。
     * 【那一声"咔"从哪来】原来这里第一刀就是 tim6_stop/i2s_dma_stop/i2s_stop ——
     *   I2S 的 MCLK/BCLK/LRCK 在 DAC **还挂在当前音量**（REG32 没归零）时被掐断。
     *   ES8311 的 ΣΔ 调制器一没时钟就不再翻转，模拟输出立刻保住/跳到一个直流电平
     *   ⇒ 相对"上一刻的波形值"就是一次阶跃 ⇒ 可闻的"咔/啪"。
     *   暂停路径（补丁10）早就改成"先静音、后停钟"，但**停止/切歌/退出 app** 这条路
     *   一直漏着 —— 而它比暂停更常见（点停止、切下一首、退出音乐 app）。
     * 【修法】照官方 esp_codec_dev 的上下电次序：`es8311_enable(false)` =
     *   set_mute(true) → 关 pa → suspend()。即**先把 DAC 数字静音**（REG31 专用静音位，
     *   官方防爆音机制），让调制器在**时钟仍在跑**的时候把输入钳到 0、经重建滤波平滑
     *   落向中位；给约 4 ms 落定，再停时钟、最后才给芯片断电。
     *   ⚠ 为什么不复用暂停那套"REG32=0 + 硬件软 ramp + 等 12 拍"：停止是要求**立刻断**
     *     的动作（切歌/退出），等 190 ms 会让人以为卡了。数字静音是即时的，
     *     只多花一个 4 ms 落定时间（人耳完全感知不到）。
     *   ⚠ 静音后**必须**在下次起播解静音 —— 由 dac_path_regs() 负责，
     *     否则"停过一次之后再也出不了声"。 */
    (void)es8311_set_mute(1);
    HAL_Delay(4);                       /* 等模拟真正落到中位再切时钟 */
    tim6_stop();
    i2s_dma_stop();                     /* 没起 DMA 时内部直接返回 */
    i2s_stop();
    i2s_gpio_release();                 /* PB9 还给 UART4 */
    (void)es8311_stop_dac();
    g_ao_playing = 0u;
    g_ao_codec   = 0u;
    s_wr = 0u;
    s_rd = 0u;
    s_tim_pending = 0u;
    s_dac_pending   = 0u;               /* 起播防咔哒状态一并归零 */
    s_vol_ramp_left = 0u;
    s_dac_rb_pending = 0u;
    s_pause_fade_ticks = 0u;            /* 万一停传输时正卡在暂停淡出，清掉免得 tick 又来停一次 */
    g_ao_dac_ready  = 0u;
    g_ao_dac_ramp   = 0u;
}

/* 暂停：停出音链路，但**不给 codec 断电**。
 *
 * 【为什么必须与 audio_out_stop 分开（2026-10-13 根因）】
 *   音乐 app 的「暂停 → 播放」是一对紧接着的操作。原来暂停走 audio_out_stop()，
 *   末尾的 es8311_stop_dac() 会写 9 笔 I2C 并把 codec 打进 **power-down**：
 *     REG0E=0xFF（关内部块）、REG12=0x02（power-down）。
 *   芯片在掉电/上电窗口里**不对自己的 I2C 地址应答** ⇒ 那 9 笔写里总有几笔砸在窗口上
 *   变成 NACK（板上实测：暂停增量 wr_ok 恰 +9，失败寄存器覆盖
 *   0x0D/0x0E/0x0F/0x12/0x14/0x15/0x32/0x37/0x45）；而丢写又被调用方 `(void)` 忽略，
 *   于是"音量只写进去一半"⇒ 用户报的**"音量大小混乱"**；极端情况连 REG32 都丢
 *   ⇒ 芯片停在 0（静音）⇒ **"进度条在走但没声音"**。
 *
 *   但暂停根本不需要关这颗芯片：它 5 V 常亮、寄存器状态本来就在，暂停只要
 *   **静音 + 停 I2S 时钟/心跳** 即可，续播时 dac_path_regs() 再把通路写回来。
 *   ⇒ 本函数只发 1 笔 REG32=0（静音），芯片保持可达；实测暂停零失败，
 *     续播那段 ~19 笔 I2C 也因此不再有 NACK 窗口。
 *
 * ⚠ 与 audio_out_stop 的另一处差别：**不 clear s_codec_ready**（es8311 侧的静态），
 *   这样续播时 es8311_is_inited()==1 ⇒ 跳过完整 init（含 REG00 软复位），
 *   又省掉一次"软复位 → 芯片不应答"的窗口。
 * ⚠ 真正的"停"（切歌 / 播完 / 退出）仍走 audio_out_stop()：那时把 codec 断电是对的，
 *   下次是全新起播，本来就要完整 init。 */
/* 停"出音传输"但**保留 codec 通电与通路**（暂停淡出末步用）：与 audio_out_stop 的
 * 区别是**不**调 es8311_stop_dac()（不给 codec 断电）、也**不**清 es8311 侧的
 * s_codec_ready ⇒ 续播能跳过完整 init、不再撞 NACK 窗口（见 audio_out_pause 顶部说明）。 */
static void ao_pause_stop_transport(void)
{
    tim6_stop();
    i2s_dma_stop();                     /* 没起 DMA 时内部直接返回 */
    i2s_stop();
    i2s_gpio_release();                 /* PB9 还给 UART4 */
    g_ao_playing = 0u;
    g_ao_codec   = 0u;
    s_wr = 0u;
    s_rd = 0u;
    s_tim_pending      = 0u;
    s_dac_pending      = 0u;
    s_vol_ramp_left    = 0u;
    s_dac_rb_pending   = 0u;
    s_pause_fade_ticks = 0u;
    g_ao_dac_ready     = 0u;
    g_ao_dac_ramp      = 0u;
}

void audio_out_pause(void)
{
    if (g_ao_playing == 0u)
    {
        return;
    }
    if (s_pause_fade_ticks != 0u)
    {
        return;                         /* 已在淡出，忽略重复请求（g_ao_pause_calls 不重复 +1）*/
    }
    g_ao_pause_calls++;                 /* ★ 动作级计数：本次暂停真的执行了 */

    /* ★ 修"暂停咔嗒"：先静音（REG32=0），此刻 I2S 传输**仍在跑**、数据**仍在流** ——
     *   ES8311 硬件软 ramp（REG37=0x48）会把音量平滑降向 -95.5 dB（≈0.655 ms/0.25 dB），
     *   不会爆音。等 HW 软 ramp 落静音后，再由 audio_out_tick 的 s_pause_fade_ticks
     *   分支停传输（见下方）。
     *   原来：先 i2s_stop() 掐时钟、再 es8311_dac_mute() —— 时钟断的瞬间 DAC 还满音量
     *   ⇒ 输出电平瞬跳 ⇒ 那声"咔嗒"。
     * ⚠ 只静音、**不** es8311_stop_dac()（断电）：芯片保持可达，续播跳过完整 init。 */
    (void)es8311_dac_mute();            /* ★ 只写 REG32=0（静音），HW 软 ramp 接管 */
    g_ao_pause_volwr++;                 /* ★ 本次暂停对 REG32 的写笔数（与上面一行 1:1）*/
    s_dac_rb_pending = 0u;              /* 取消起播后的"音量确认期"，别回头把音量写回去 */
    s_pause_fade_ticks = (uint8_t)AO_PAUSE_FADE_TICKS;  /* 延几拍再停传输 */
}

int audio_out_playing(void)
{
    return (g_ao_playing != 0u) ? 1 : 0;
}

void audio_out_tick(void)
{
    /* 排障钩子（与播放状态无关）：g_ao_probe_n = N ⇒ 接下来 N 拍各读 3 个 DAC 寄存器。
     * 用途：量**稳态**下 I2C 的失败率（不依赖有没有在播放），把"芯片电源态切换窗口里的
     * NACK"和"平时也 NACK"分开 —— 两者的处置完全不同。只读、不留痕（失败率看
     * g_es_rd_ng / g_es_retry_ok 的增量），也不改任何寄存器。
     * ⚠ 必须等 I2C1 初始化过才能碰（g_es_bus_rc==0），否则 s_hi2c.Instance 还是空指针。 */
    if ((g_ao_probe_n != 0u) && (g_es_bus_rc == 0u))
    {
        uint8_t v;
        g_ao_probe_n--;
        (void)es8311_read_reg(0x32, &v);
        (void)es8311_read_reg(0x12, &v);
        (void)es8311_read_reg(0x0Eu, &v);
    }

    /* dbg 无条件刷新：起播卡住时 playing 往往是 0，那时才最需要看现场
     * （上一版正是"dbg 因 playing=0 而全 0"，白白多烧了一遍固件）。 */
    if (g_ao_playing != 0u)
    {
        dbg_snapshot();
        dbg2_snapshot();                /* DMA/SPI 现场（含 CFG1.TXDMAEN、DMA_SxNDTR）*/
    }

    /* 心跳延后到这一拍再起（理由见 audio_out_start 里的注释）。 */
    if (s_tim_pending != 0u)
    {
        s_tim_pending = 0u;
        tim6_start();
        g_ao_stage = 6u;
    }

    if (g_ao_playing == 0u)
    {
        return;
    }

    /* 欠载自恢复（H7 第二个坑）：UDR 置位后引擎停吃 FIFO，必须写 UDRC 清掉才继续。 */
    if ((READ_BIT(s_hi2s.Instance->SR, SPI_SR_UDR) & SPI_SR_UDR) != 0UL)
    {
        SET_BIT(s_hi2s.Instance->IFCR, SPI_IFCR_UDRC);
        g_ao_udr++;
    }
    if ((READ_BIT(s_hi2s.Instance->SR, SPI_SR_TXP) & SPI_SR_TXP) != 0UL)
    {
        g_ao_txp_seen++;
    }

    /* ---- 暂停淡出：REG32 已被写成 0（HW 软 ramp 正在把音量降向 -95.5 dB），
     *   等 AO_PAUSE_FADE_TICKS 拍、音量基本落静音后再停传输 —— 切断 I2S 时钟时 DAC
     *   已静音，不会爆音（见 audio_out_pause 顶部说明）。
     *   ⚠ 淡出期间**不 return**：仍让下方逻辑照常补数、DAC 继续出（渐弱）的声音，
     *   直到淡出走完才真正停传输。 */
    if (s_pause_fade_ticks != 0u)
    {
        if (--s_pause_fade_ticks == 0u)
        {
            ao_pause_stop_transport();
            return;                     /* 本拍不再喂数据 / 做别的事 */
        }
    }

    /* ---- 起播防咔哒：第二拍才唤醒 DAC，随后把音量分步 ramp 上去 ----
     * 判据"数据流稳了没有"分两条路：
     *   DMA 路径：i2s_dma_start 已经把两个半缓冲都铺满 ⇒ 起播即就绪；
     *   TIM6  路径：CSTART 时 FIFO 是空的，得等 ISR 喂过（g_ao_isr > 0）。
     * ⚠ 必须在 I2S 真的在流的时候才唤醒 DAC —— 这就是"先时钟、后通路"的全部意义。 */
    if (s_dac_pending != 0u)
    {
        int ready = (g_ao_use_dma != 0u) ? (s_dma_ready != 0u) : (g_ao_isr > 0u);

        if (ready != 0)
        {
            s_dac_pending = 0u;
            /* 整段"静音接通 DAC"重试一次：I2C 写是**会失败**的（见 es8311.c），
             * 而这一段的返回值以前是直接丢掉的 —— 丢一笔就可能 DAC 没上电/音量没升，
             * 正是"续播没声音"。失败就整段重来（第二遍基本都能过）。 */
            if (es8311_dac_start_muted() != 0)
            {
                g_ao_wake_retry++;
                (void)es8311_dac_start_muted();
            }
            s_vol_ramp_left = (uint8_t)AO_VOL_RAMP_STEPS;
            g_ao_dac_ready  = 1u;
            g_ao_stage      = 7u;
            g_ao_wake_cnt++;
            g_ao_t_dacwake_ms = HAL_GetTick() - g_ao_t_cstart_ms;  /* 补丁12：起播时序表 */
            /* 进入"音量确认期"：不是"ramp 走完读一次"就完事，而是**逐拍确认**
             * 直到 es8311_dac_readback() 明确回报"音量已落进芯片"（最多 AO_RB_CONFIRM_TRIES 拍）。
             * 理由见 es8311_dac_readback 的说明与 tools/wav_pause_resume_check.py 的实测：
             * ramp 末步写终值恰好撞上芯片 NACK 窗口时会停在中间档，而那一拍的回读
             * 往往也失败 ⇒ 只读一次会漏掉，必须跨过窗口重试。 */
            s_dac_rb_pending = (uint8_t)AO_RB_CONFIRM_TRIES;
        }
    }

    /* 音量 ramp：8 步从 0 升到 UI 音量（每拍一步 ≈ 16 ms）。
     * 目标每步重读 g_audio_volume：ramp 途中用户拖了滑杆也能跟上，不把音量锁在旧值。 */
    if (s_vol_ramp_left > 0u)
    {
        uint32_t step   = (uint32_t)(AO_VOL_RAMP_STEPS - s_vol_ramp_left) + 1u;
        uint32_t target = g_audio_volume;

        if (target > 100u)
        {
            target = 100u;
        }
        (void)es8311_set_volume_ui((s_vol_ramp_left == 1u) ? target
                                                          : (target * step) / AO_VOL_RAMP_STEPS);
        s_vol_ramp_left--;
        g_ao_dac_ramp = s_vol_ramp_left;
        if (s_vol_ramp_left == 0u)
        {
            g_ao_t_ramp_ms = HAL_GetTick() - g_ao_t_cstart_ms;  /* 补丁12：起播时序表 */
        }
    }

    /* ---- 起播后「音量确认期」：逐拍确认音量真的落进芯片（2026-10-13 关键修复）----
     * 【为什么不能"ramp 走完读一次"】板上实测（tools/wav_pause_resume_check.py）：
     *   ramp 是 8 拍、每拍写一次 REG32；第 8 拍写终值（如 UI 72 ⇒ 0x89）时若正好落在
     *   芯片"电源态/复位窗口"里，那 5 次重试可能全灭 ⇒ REG32 停在**中间档**（实测 0x78
     *   = 第 7 步值）。此时用户听到的正是"音量大小混乱"（比设定值轻一档、每次轻重不一），
     *   或极轻时像"没声音"。而**同一拍的回读也常失败**（同一窗口）⇒ 只读一次判定不出来。
     * 【做法】ramp 走完后不只看一眼，而是**每拍读一次、不对就补写（readback 内分层补）**，
     *   最多 AO_RB_CONFIRM_TRIES 拍，直到读到"音量 == 目标"才收工。确认通过后清计数,
     *   退回原来的"每 ~1 s 体检"。
     * ⚠ 只在"刚起播/续播后的窗口期"多花这几拍 I2C；后台稳态仍是 1 次/s，不影响播放。 */
    if ((s_dac_rb_pending != 0u) && (s_vol_ramp_left == 0u))
    {
        if (es8311_dac_readback() == 0)
        {
            s_dac_rb_pending = 0u;      /* 确认到手：退出确认期 */
            s_rb_tick = 0u;
        }
        else if (--s_dac_rb_pending == 0u)
        {
            /* 用尽次数仍未确认（芯片长时间不可达）：保留现场量，退回稳态体检，
             * 不再纠缠 —— 下一次 1 s 体检还会再试。 */
            s_rb_tick = 0u;
        }
    }
    else if ((s_vol_ramp_left == 0u) && (g_ao_dac_ready != 0u) && (s_pause_fade_ticks == 0u))
    {
        /* 播放中的定期体检（每 ~1 s 一次）：以**回读值**为准确认"音量还在芯片里、
         * DAC 还上着电"，不对就补写。为什么必须做：I2C 写是会失败的（见 es8311.c
         * 的失败统计），漏掉一笔 REG32 就是用户那头"进度条在走但没声音 / 音量忽大忽小"，
         * 而且自己不恢复的话，只有退出 app 重进才能好。代价 = 每秒 3 次单字节 I2C 读。 */
        if (++s_rb_tick >= 64u)
        {
            s_rb_tick = 0u;
            (void)es8311_dac_readback();
        }
    }

    dbg_snapshot();

    /* demo 播完：feeder 已把剩余帧数走完，这里负责收尾（停心跳 + 挂 codec）。 */
    if (s_demo_done != 0u)
    {
        s_demo_done = 0u;
        g_ao_demo   = 0u;
        audio_out_stop();
        return;
    }

    /* 补数：环形缓冲里剩的不超过半个环就补半个环（≈42 ms 的量，主循环一拍 16 ms 足够）。 */
    {
        uint32_t used = s_wr - s_rd;

        /* 补丁12：记录播放期环占用的**最小值** —— 它就是"抗主循环卡顿的垫子"。
         * 起播铺满整环 + DMA 拿 4096 后，这里应当稳定 ≥ 4096（42 ms）；
         * 若掉到接近 0，说明某一拍被堵得太久，下一次半传中断就会填静音。 */
        if (used < g_ao_ring_used_min)
        {
            g_ao_ring_used_min = used;
        }
        if (used <= (RING_HALFWORDS / 2u))
        {
            pump_half();
            if (g_ao_t_pump1_ms == 0u)
            {
                g_ao_t_pump1_ms = HAL_GetTick() - g_ao_t_cstart_ms;
            }
        }
    }
}
