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
volatile uint32_t g_ao_pause_volwr = 0u;   /* 暂停动作里对 REG32 的**实测写笔数**（补丁13：
                                            * 用 g_es_reg32_n 的动作内前后差，2~3 笔；
                                            * 旧版是"固定 +1"，看不出写丢没写丢）*/
volatile uint32_t g_ao_silent_fail = 0u;   /* ★ 补丁13：**静音未能确认**的次数。
                                            * >0 = 那一刻切时钟时 DAC 可能还挂在有效音量上
                                            * ⇒ 有爆音风险。这是把"听不见的坏状态"变成
                                            * 可读计数的关键量（与 g_es_silent_ng 同源）。*/
/* ★ 补丁21：淡出收尾分支的可直接计数（铁律10：分支只能直接计数）。
 *   - g_ao_mute_retry：暂停/停止时静音未落地、改由 tick 内每拍重试直到确认的次数
 *     （证明"静音晚落地"这条新路径被走到，正是"某一次爆音"的旧根因入口）；
 *   - g_ao_fade_min：静音确认后等满 AO_PAUSE_MIN_RAMP_MS 才切钟的次数（正常路径）；
 *   - g_ao_fade_hardcap：静音始终没确认、靠硬上限 AO_PAUSE_HARD_CAP_MS 到点才切钟的次数
 *     （I2C 死锁兜底，此时有爆音风险，量化不掩盖）。*/
volatile uint32_t g_ao_mute_retry   = 0u;
volatile uint32_t g_ao_fade_min     = 0u;
volatile uint32_t g_ao_fade_hardcap = 0u;
/* ★ 补丁22：淡出期间"写音量"这条危险路径的可直接计数（铁律10）。
 *   - g_ao_vol_during_fade    ：audio_out_set_volume() 在**暂停淡出期间**被调到的次数
 *     （证明"点播放落在淡出窗口内"这种情况真实发生）；
 *   - g_ao_vol_fade_wrote     ：其中**真的把 REG32 写进芯片**的次数
 *     （>0 = 满音量被硬写到仍在跑的传输上 = 用户那声"突然很大声"；修好后应恒 0）；
 *   - g_ao_vol_during_fade_val：那次要写的 UI 音量值。
 *   - g_ao_vol_guard_en       ：A/B 开关。1 = 守卫开（淡出期间不写，修法）；0 = 复现旧行为。*/
volatile uint32_t g_ao_vol_during_fade     = 0u;
volatile uint32_t g_ao_vol_fade_wrote      = 0u;
volatile uint32_t g_ao_vol_during_fade_val = 0u;
volatile uint32_t g_ao_vol_guard_en        = 1u;
volatile uint32_t g_ao_csusp_ms_max = 0u;  /* ★ 补丁13：i2s_stop() 里"等先挂起生效"的**最长耗时(ms)**。
                                            * 正常 0~1；若出现几十 ms ⇒ 主循环单拍被它拖长。*/
volatile uint32_t g_ao_csusp_tmo = 0u;     /* ★ 补丁13：上面那段等满 100 ms 兜底才 break 的次数 */
volatile uint32_t g_ao_resume_calls = 0u;  /* 续播（起播）动作被真正执行的次数 */

/* ================= 2026-10-14（补丁15）：把"咔"变成可读数字 =================
 * 【为什么要新加一组量】前面几轮修"咔"用的都是**间接代理量**：
 *   `g_ao_under_short`（有没有填硬零）、`g_ao_silent_*`（静音确认成没成）、
 *   `g_ao_dma_gap_max`（DMA 停顿多长）。这些量都只能覆盖**某一类**成因。
 *   而"咔"的物理定义只有一条：**送进 DAC 的样本流里出现了不该有的阶跃**。
 *   ⇒ 这一轮直接量它：
 *     `g_ao_seam_max`  DMA 路径：本次半缓冲的**首样本**与上一半缓冲**末样本**之差
 *                      （正常音乐是连续流，这块接缝应当 ≈ 相邻样本差，不会突跳）；
 *     `g_ao_tz_run_max` TIM6 路径：送进 TXDR 的**最长连续零**（半字）——
 *                      TIM6 是逐字流，没有"接缝"，但它若吃到一整块零，一样是阶跃；
 *     `g_ao_feed_short_max` feeder 单次短读的**最大缺口（帧）**——
 *                      旧代码在 got<frames 时会把剩下的**整块 memset 成 0** 再放进环，
 *                      一次就是 1024~2048 帧（21~42 ms）的**人为硬零块**，而这条路径
 *                      **既不计缺口也不报错**（只 +1 次 g_ao_under_feed）⇒ 是"看不见的坏状态"。
 * 【判读口径】
 *   · `g_ao_seam_max` 大（>30000）且 `g_ao_seam_n` 涨 ⇒ 半缓冲接缝处有**人工断口**；
 *   · `g_ao_tz_run_max >= 512`（≈5 ms）⇒ TIM6 吃到成块零；
 *   · `g_ao_feed_short_max == 1024/2048` ⇒ 读卡短读到了"整块"级别，旧代码会合成零块。*/
#define AO_SEAM_BIG 30000   /* 跨半缓冲接缝"人工断口"判据（16 位满幅 32767 的 92%）*/
volatile uint32_t g_ao_stop_calls  = 0u;   /* ★ 补丁15：audio_out_stop() 真正发起淡出的次数 */
volatile uint32_t g_ao_stop_done   = 0u;   /* ★ 补丁15：淡出走完、真正停掉传输的次数 */
/* ★ 补丁15d：「起播落进"取消淡出"幂等分支」的**直接**计数（单调只增）。
 * 为什么需要它：真实用户路径是**快速连点 暂停→播放**（走 wav_play_pause/resume），
 * 而 pause 路径**不** ++g_ao_stop_calls、其淡出收尾也**不** ++g_ao_stop_done
 * ⇒ 用 `stop_calls − stop_done` 反推幂等分支对这条路径**恒为 0**（不是漏测就是误判）。
 * 直接在分支里 ++ 才是无歧义、无竞态的证据。 */
volatile uint32_t g_ao_idem_cancel = 0u;
/* ★★ 补丁16（2026-10-16）：起播安全栅 + 它的可测证据。★★
 *
 * 【为什么需要】（用户报障："点击播放、刚开始播放的瞬间有爆音/破音"）
 *   "停止/暂停"路径**不给 codec 断电**：es8311_dac_become_silent() 只把 REG32 写成 0，
 *   ao_pause_stop_transport() 只关传输。⇒ 停止之后芯片仍是"**通电 + REG31 未静音**"，
 *   只差一个非零的 REG32 就能出声。
 *   而 wav_play_start() / wav_play_resume() 在调 audio_out_start() **之前**就会调
 *   audio_out_set_volume()（本意只是"把当前 UI 音量同步给 codec"）。旧实现里那句会
 *   无条件写 REG32 ⇒ 在 CSTART 之前音量就已是**满值** ⇒ 一开时钟，DAC 立刻以满音量
 *   把音乐前几毫秒送出来，直到下一拍 tick 的 dac_path_regs(0x00) 才把它拉回 0。
 *   实测这段窗口 = g_ao_t_dacwake_ms ≈ 17~33 ms ⇒ 一声短促的爆音 + 一次"掉静音再升起"的顿挫。
 *   ⚠ 首次开机后的第一次播放不受影响（那时 s_codec_ready==0 且通路未上电），
 *     所以现象表现为"**之后每次**点播放都有"。
 *
 * 【两个量的分工】
 *   g_ao_vol_at_cstart    —— CSTART 之前那一刻从芯片**回读**到的 REG32。
 *                            0x00 = 安全；非 0 = 正带着音量开钟（爆音条件成立）。
 *                            高字节置 0x01（值 0x01FF）= I2C 读失败，无法判定。
 *                            这是**直接证据**，不是"我写了 0 所以应该没事"的推断。
 *   g_ao_start_barrier_en —— 起播安全栅开关，**默认 1=开**。写 0 可在同一固件上做
 *                            A/B：关掉它就能复现"带着满音量开钟"的旧行为，
 *                            用来证明这个栅真的是承重的（而不是装饰）。
 *                            ⚠ 仅供排障；出货默认必须是 1。 */
volatile uint32_t g_ao_vol_at_cstart    = 0u;
volatile uint32_t g_ao_start_barrier_en = 1u;
volatile uint32_t g_ao_seam_max      = 0u;   /* DMA：跨半缓冲接缝的最大样本跳变（0~65535）*/
volatile uint32_t g_ao_seam_n        = 0u;   /* DMA：上面这个跳变 > AO_SEAM_BIG 的次数 */
volatile uint32_t g_ao_tz_run_max    = 0u;   /* TIM6：送进 TXDR 的最长连续零（半字）*/
volatile uint32_t g_ao_feed_short_max = 0u;  /* feeder 单次短读最大缺口（帧；0 = 从未短读）*/
volatile uint32_t g_ao_dbg3[16];             /* 上面几个量的"同一时刻"批读窗口 */

/* 起播音量 ramp 的步数：8 步 × 主循环一拍（≈16 ms）≈ 130 ms 完成。
 * 步数太少压不住跳变、太多会让人听出"声音起得慢"，8 是折中。 */
#define AO_VOL_RAMP_STEPS 8u

/* 起播/续播后"音量确认期"的拍数上限：每拍一次回读（含失败重试与补写），
 * 最多 12 拍 ≈ 190 ms。取 12 的理由：ramp 8 拍之后还要跨过一个可能的 NACK 窗口
 * （实测窗口持续到数十毫秒），给足余量又不至于长期占用 I2C 影响播放。 */
#define AO_RB_CONFIRM_TRIES 12u

/* 暂停/停止淡出的"切钟"时机（2026-10-14 修"播放/暂停咔嗒声"，2026-10-16 补丁21 改为时间基准）：
 * 暂停/停止时先把 REG32 写成 0（ES8311 硬件软 ramp：REG37=0x48 = 0.25 dB / 32 LRCK，
 * ≈ 0.655 ms / 0.25 dB）。DAC 音量被硬件平滑降向 -95.5 dB，从 0 dB 降到不可闻约 250~300 ms。
 * ⚠ 旧实现用固定的 12 拍（≈192 ms）倒计时，且从"暂停调用时刻"起算 —— 一旦那笔静音 REG32=0
 *   因 I2C 首试 NACK（实测 5%~30%、风暴下整段唤醒可长达 143 ms）晚落地，HW 软 ramp 没跑满
 *   就被切钟 ⇒ DAC 还挂在可闻音量 ⇒ "多次里某一次"的偶发 pop。
 * ⇒ 补丁21 改为**时间基准、且从"静音真正落地"起算**：
 *   · AO_PAUSE_MIN_RAMP_MS：静音确认后至少再等这么久才切 I2S 时钟（≥ 软 ramp 全程 + 余量）；
 *   · AO_PAUSE_HARD_CAP_MS：静音始终没确认时，最多等这么久也必须切钟，防 I2C 死锁（此时有爆音风险，
 *     但比永远不切钟、链路卡死好 —— 用 g_ao_fade_hardcap 量化）。
 * ⚠ 为什么必须"先静音、后停时钟"：原来 audio_out_pause 先 i2s_stop() 掐时钟、再
 *   es8311_dac_mute()，时钟断的瞬间 DAC 还满音量 ⇒ 输出电平瞬跳 ⇒ 那声"咔嗒"。 */
#define AO_PAUSE_MIN_RAMP_MS 300u   /* 静音确认后至少再等 300 ms 才切钟（软 ramp 到 -95.5 dB ≈ 250 ms，留余量）*/
#define AO_PAUSE_HARD_CAP_MS 900u   /* 静音始终没确认时，最多等 900 ms 也必须切钟（I2C 死锁兜底）*/

/* ★ 补丁15c：**首次起播**时把"DAC 通路唤醒"推迟这么多毫秒（相对 CSTART）。
 * 理由（板上实测，见 audio_out_start 里 s_wake_settle_ms 的注释）：首次起播刚做过
 * REG00 软复位，唤醒那 20 笔 I2C 要重试 22 次、触发 29 次 bus clear、3 次熔断、
 * 单笔最长 31.4 ms ⇒ 整段 110~129 ms，而 DMA 余量只有 ~37.5 ms ⇒ 一整个半缓冲被合成。
 * 这 120 ms 里 I2S 在 REG32=0 下静音送数（听不见），主循环把环补满。
 * ⚠ 只对"跑完整 init 的那一次"生效；续播/二次起播仍走"下一拍就唤醒"（实测 20~60 ms）。 */
#define AO_WAKE_SETTLE_FIRST_MS 120u

/* ---- 环形缓冲 → SRAM1 的 .bss_audio（MPU region0：非缓存）----
 * ★★ 补丁15c：8192 → **16384** 个半字（+16 KB）。 ★★
 * 【为什么必须加深：旧注释里的 ms 数是错的】旧注释写"8192 半字 ≈ 84 ms"是按
 *   "48828 Hz 立体声（97656 半字/s）"算的。板上实测（tools/bench.rd 两端读 + 真实
 *   时间分母）：DMA 实际吃 **327 664 半字/s**（每半传 2048 半字用 **12.47 ms**，
 *   `g_ao_dma_half` 在 6.00 s 里涨 481 次）⇒ 8192 半字只够 **25 ms**，
 *   加上 DMA 乒乓缓冲的 4096 半字（12.5 ms）总共才 **37.5 ms** 余量。
 *   而"起播第一拍唤醒 DAC"那段 codec 通路 I2C 在**首次起播**时实测要
 *   **110~129 ms**（见下）⇒ 环必然被抽干 ⇒ 一整个半缓冲（2048 半字）被合成
 *   （板上实测 ring_min=0、under_short=2048、首个欠载 @105 ms）。
 * ⇒ 加深到 16384（50 ms）+ 起播**铺满整环**：起播余量 50+12.5 = 62.5 ms，
 *   稳态垫子 25→50 ms。⚠ SRAM1 占用 68 KB → 84 KB（128 KB 的 66%）。
 * ⚠ 为什么 RING_HALF_FRAMES/RING_QUARTER_FRAMES **不再**由 RING_HALFWORDS 推导：
 *   它们是"**补数块**大小"（受 s_tmp 尺寸约束），与"环有多深"是两件事。
 *   若继续按 /4 推导，环一加深 s_tmp 就跟着翻倍（+8 KB）——没必要。 */
#define AUDIO_BUF_ATTR   __attribute__((section(".bss_audio"), aligned(32)))
#define RING_HALFWORDS   16384u
#define RING_MASK        (RING_HALFWORDS - 1u)
#define RING_HALF_FRAMES 2048u                      /* 补数块：2048 帧 = 4096 半字 */
#define RING_QUARTER_FRAMES 1024u                   /* 补 1/4 块：1024 帧 = 2048 半字（补丁14）*/
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
static volatile uint8_t  s_pause_fade_ticks = 0u;  /* 暂停/停止淡出态标志（0 = 不在淡出；非 0 = 正在淡出）。
                                          * ★ 补丁21：语义由"12 拍倒计时"改为"态标志"，
                                          *   真正的延时收尾改由下方时间基准判定（见 tick）。*/
static uint32_t          s_mute_t0 = 0u;   /* 淡出：REG32=0 真正落进芯片的时刻（0 = 尚未确认）*/
static uint32_t          s_pause_start_t0 = 0u; /* 淡出：暂停/停止发起时刻（硬上限起点）*/
static int32_t           s_dma_last = 0;      /* 补丁15：上一个半缓冲的末样本（接缝检测的前驱）*/
static uint32_t          s_dma_wr_seq = 0u;   /* 补丁15：已写过的半缓冲个数（第 1 个没有前驱，不计接缝）*/
static uint32_t          s_tz_run = 0u;       /* 补丁15：TIM6 当前连续零的个数（半字）*/
/* ★ 补丁15：淡出走完后是"停"(1) 还是"暂停"(0)。
 * audio_out_stop/pause 共用同一套"发起淡出 → 12 拍 → 停传输"的延时收尾（理由见
 * audio_out_stop 顶部），两者的唯一区别就是这个标志：
 *   暂停 = 保留 feeder 与 codec 通路（续播接着放）；
 *   停止 = 淡出走完后断开 feeder。 */
static volatile uint8_t  s_stop_after_fade = 0u;
static int32_t           s_dma_hold = 0;      /* 补丁15：最后一次真正送出去的样本（缺口"冻结"的取值来源）*/
/* ★ 补丁15c：上一块半缓冲是"合成（冻结）"出来的 ⇒ 本块开头要做 ramp-in 接回真实数据。 */
static volatile uint8_t  s_gap_tail = 0u;
/* ★ 补丁15c：起播后"允许唤醒 DAC"的最早时刻（相对 CSTART 的 ms）。
 *   首次起播（要跑完整 init，内含 REG00 软复位）时非 0 —— 理由见 audio_out_start。
 *   0 = 下一拍就唤醒（续播/二次起播，实测唤醒只要 20~60 ms）。 */
static uint32_t          s_wake_settle_ms = 0u;

static I2S_HandleTypeDef s_hi2s;        /* I2S2 = SPI2 的 I2S 模式 */
static audio_out_feeder  s_feeder = NULL;

/* ---- I2S TX DMA 的乒乓缓冲（放在 .bss_audio：SRAM1、非缓存、DMA1 可达）----
 * 半缓冲 = 1024 帧 = 2048 半字 ≈ 21 ms（48 828 Hz）。DMA 循环模式跑满整个 4096 半字；
 * 半传中断补前半、全传中断补后半。主循环仍负责把卡/正弦源补进 s_ring。 */
#define DMA_HALF_FRAMES   1024u
#define DMA_HALF_HALFWORDS (DMA_HALF_FRAMES * 2u)         /* 2048 */
#define DMA_TOTAL_HALFWORDS (DMA_HALF_HALFWORDS * 2u)     /* 4096 */
/* ★ 补丁15c：缺口之后的第一块开头做 ramp-in 的样本数（半字）。
 *   256 半字 ≈ 3 ms @ 实测吞吐 ⇒ 足够抹掉阶跃，又短到听不出来。 */
#define DMA_RAMPIN        256u
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

    /* 补丁15：把"咔"的直接量度放进第二个批读窗口（一次 read 走 g_ao_dbg3[16]）。 */
    g_ao_dbg3[0] = g_ao_seam_max;
    g_ao_dbg3[1] = g_ao_seam_n;
    g_ao_dbg3[2] = g_ao_tz_run_max;
    g_ao_dbg3[3] = g_ao_feed_short_max;
    g_ao_dbg3[4] = s_dma_wr_seq;
    g_ao_dbg3[5] = g_ao_under_short;
    g_ao_dbg3[6] = (uint32_t)s_dma_last;
    g_ao_dbg3[7] = g_ao_stop_calls;     /* 补丁15：发起了几次"停"（淡出）*/
    g_ao_dbg3[8] = g_ao_stop_done;      /* 补丁15：淡出走完、真正停掉几次 */
    g_ao_dbg3[9] = (uint32_t)s_dma_hold;/* 补丁15：缺口衰减的起点 */
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

/* ★ 2026-10-16（补丁18）：同一个 SPI2 需要两套角色 —— 放音=**主发**、录音=**主收**。
 *   共同部分（标准/数据格式/频率/极性 + I2SDIV/ODD + CFG1.DSIZE + 清挂起标志）抽成本函数，
 *   保证两侧**逐位一致**：两份参数各自维护、慢慢分叉，就是"放音正常、录音变调/错位"这类
 *   最难查的故障。⚠ 只有 Mode 由调用方给（TX/RX 唯一不同之处）。 */
static int i2s_apply(uint32_t mode)
{
    s_hi2s.Instance              = SPI2;
    s_hi2s.Init.Mode             = mode;
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
     * 免得上一次残留的 UDR/SUSP 把 TXP 压在 0；收方向对应的是 OVR（bit3 也在 0x0FF8 里）。 */
    s_hi2s.Instance->IFCR = 0x0FF8u;
    return 0;
}

/* 放音用：主发（时钟/格式的全部说明在 i2s_apply 里）。 */
static int i2s_init(void)
{
    return i2s_apply(I2S_MODE_MASTER_TX);
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
                g_ao_csusp_tmo++;        /* ★ 补丁13：等超了 100 ms 兜底才走 —— 记一笔，
                                          * 它就是"单拍被拖长"的可能来源之一（要用数据说话）*/
                break;
            }
        }
        /* ★ 补丁13：把这次挂起实际等了多久记下来（µs 级用 tick 就够：
         *   正常应是 0~1 ms，若经常看到几十毫秒 ⇒ 主循环单拍被它拖住）。 */
        {
            uint32_t el = HAL_GetTick() - t0;
            if (el > g_ao_csusp_ms_max)
            {
                g_ao_csusp_ms_max = el;
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
    /* ★★ 补丁15c：缺口改成"**冻结**最后一个有效样本"（不再衰减到 0），缺口之后的
     * 第一块开头再做一段 **ramp-in** 接回真实数据。 ★★
     * 【为什么"衰减到 0"仍然不够（补丁15 首版的做法）】衰减在缺口**内部**是平滑的
     *   （一阶差 ≤16 LSB ≈ -66 dB），但缺口的**结束处**是"0 → 音乐值"的跳跃：
     *   |0 − next| 可以是满幅 ⇒ 那一声"咔"只是被推到了缺口尾部，并没有消失。
     * 【为什么"冻结 + ramp-in"是干净的】
     *   · 冻结：缺口首样本 == 缺口前的最后样本 ⇒ 起点**零阶跃**（C0 连续）；
     *   · ramp-in：缺口后头 DMA_RAMPIN 个样本从冻结值线性过渡到真实数据 ⇒ 终点也零阶跃。
     *   整段的听感是"短促的一顿"（这一块 ≈21 ms），而不是"咔"。这正是 TIM6 路径
     *   **天然**的行为（TIM6 缺数据时 ISR 不写 ⇒ H7 的 I2S 重复上一采样 = 冻结），
     *   也正是用户实测"走 TIM6 没有爆音"的物理原因 —— 本轮把 DMA 路径的行为对齐过去。
     *   ⚠ 若这一块一颗数据都没有（avail==0）且此前从未出过声（s_dma_hold==0），
     *     冻结值就是 0 ⇒ 与原来的填零完全等价（不会更差）。 */
    if (avail < DMA_HALF_HALFWORDS)
    {
        uint16_t hold = (uint16_t)(int16_t)s_dma_hold;

        for (uint32_t i = avail; i < DMA_HALF_HALFWORDS; ++i)
        {
            dst[i] = hold;
        }
        s_gap_tail = 1u;                /* 本块是合成的 ⇒ 下一块开头补 ramp-in */

        /* ⚠ 这是**真会被听到**的一类欠载，仍然记账（判据口径不变，便于与历史对照）。 */
        uint32_t short_by = DMA_HALF_HALFWORDS - avail;

        g_ao_underrun++;                /* 供不上：这一半尾部只能冻结补 */
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
    else if (s_gap_tail != 0u)
    {
        /* 上一块是"冻结"出来的 ⇒ 本块开头 DMA_RAMPIN 个样本从冻结值线性过渡到真实数据，
         * 抹掉缺口结束处的阶跃。
         * ⚠ 改的是**本块**（DMA 此刻正在读另一块），本块要等下一次半传才会被读到 ⇒ 安全。 */
        uint32_t n = DMA_RAMPIN;

        if (n > DMA_HALF_HALFWORDS) { n = DMA_HALF_HALFWORDS; }
        if (n >= 2u)
        {
            int32_t h0 = s_dma_hold;

            for (uint32_t i = 0u; i < n; ++i)
            {
                int32_t real = (int32_t)(int16_t)dst[i];
                /* i=0 ⇒ 完全取 h0；i=n-1 ⇒ 完全取真实数据 ⇒ 两端都无阶跃 */
                dst[i] = (uint16_t)(int16_t)((h0 * (int32_t)(n - 1u - i) + real * (int32_t)i)
                                             / (int32_t)(n - 1u));
            }
        }
        s_gap_tail = 0u;
    }
    if (avail > 0u)
    {
        s_dma_hold = (int32_t)(int16_t)dst[DMA_HALF_HALFWORDS - 1u];  /* 供下一块缺口做起点 */
    }
    g_ao_push   += avail;
    g_ao_frames += (avail / 2u);        /* 2 个半字 = 1 帧 */

    /* ★ 补丁15：跨半缓冲接缝检测（数据出口处的"咔"直接量度）。
     * 为什么只测"本次首样本 − 上次末样本"：半缓冲**内部**的大跳变通常是音乐本身
     * （鼓点/瞬态），无法与"故障"区分；而**这一处接缝**在正常播放中一定是连续的
     * 音乐流 —— 出现大跳变只可能来自"人工断口"（块被填零 / 数据被跳过 / 环形缓冲被覆盖）。
     * ⚠ 第 1 个半缓冲没有前驱（s_dma_wr_seq==0），不计。 */
    if (s_dma_wr_seq != 0u)
    {
        int32_t dd = (int32_t)(int16_t)dst[0] - s_dma_last;

        if (dd < 0) { dd = -dd; }
        if ((uint32_t)dd > g_ao_seam_max) { g_ao_seam_max = (uint32_t)dd; }
        if ((uint32_t)dd > (uint32_t)AO_SEAM_BIG) { g_ao_seam_n++; }
    }
    s_dma_wr_seq++;
    s_dma_last = (int32_t)(int16_t)dst[DMA_HALF_HALFWORDS - 1u];
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

/* ====================== 录音采集：I2S2 主收 + DMA1_Stream3（2026-10-16 补丁18）======================
 *
 * 【这条链路在整条录音路径里的位置】
 *   麦克风 → ES8311(模拟 PGA → ADC → 芯片 ASDOUT) → **模块 DOUT 脚**
 *          → 排针 **P2-9（PC2 = I2S2_SDI，AF5）** → I2S2 主收 → DMA1_Stream3
 *          → s_cap_buf（环形）→ src/audio_rec.c 取走 → FatFs → WAV
 *
 * 【⚠⚠ 硬件前提（接线表里这一行写的是"录音才接"）】
 *   模块 **DOUT 必须接到排针 P2-9（PC2）**。本工程放音只用模块 DIN（PB15 驱动），
 *   所以 DOUT 一直是空着的。**没接这根线时**：本模块照常启动、DMA 照常搬数、文件照常生成，
 *   但采到的只是 PC2 的悬空电平 ⇒ `g_cap_peak_l/r` 极小且 `g_cap_dc_*` 恒定 ——
 *   这就是"线没接"的判据，不需要靠猜（tools/rec_check.py 会把它当结论打出来）。
 *
 * 【为什么必须独占 I2S2】I2SCFG 是"主发/主收"二选一（RM0433 §50.7.6）⇒ **录音与放音不能同时**。
 *   所以本函数先要求放音停下：放音走的是**异步淡出**，本拍只能"发起停"，就返回 2
 *   让调用方下一拍再来（见 audio_out_capture_start 的返回值约定）。放音侧下次起播会自动
 *   调 i2s_init() 把同一个 SPI2 重新配回主发，不需要在这里做恢复动作。
 *
 * 【H7 收方向的三个坑（与发方向不同，逐个踩过）】
 *   ① 使能同样是**两段式**：`__HAL_I2S_ENABLE`(SPE) 只配好外设，**还要置 CR1.CSTART** 才真收。
 *   ② 收方向没有 UDR，取而代之的是 **OVR（SR bit6，FIFO 溢出）**——溢出后引擎同样停吃 FIFO，
 *      必须写 **IFCR.OVRC(bit6)** 才恢复。本模块每拍查一次并累加 `g_cap_ovr`（不静默）。
 *   ③ DMA 请求号与使能位都不同：**SPI2_RX = 39**（发方向 40）、**CFG1.RXDMAEN(bit14)**
 *      （发方向 TXDMAEN bit15）。写错任一 ⇒ **NDTR 一动不动**（与当年发方向的症状一模一样）。
 *
 * 【进度从哪来】不用中断：DMA 配 CIRCULAR 后用 `HAL_DMA_Start()`（非 IT）后台跑，
 *   位置由 **NDTR** 反推（`环内位置 = 缓冲大小 − NDTR`）并检测环绕。主循环 17 ms 一拍，
 *   8 KB 缓冲 ≈84 ms ⇒ 5 倍余量；真追不上时 `g_cap_lost` 会涨（不静默丢数）。 */
#define CAP_DMA_HALFWORDS 8192u    /* 8 KB：立体声 48.828 kHz 下 ≈84 ms 的进度垫子 */

volatile uint32_t g_cap_on     = 0u;   /* 1 = 采集在跑 */
volatile uint32_t g_cap_tot    = 0u;   /* DMA 已写入的半字数（单调）*/
volatile uint32_t g_cap_lost   = 0u;   /* 主循环来不及取、被覆盖掉的半字数（0 = 没丢）*/
volatile uint32_t g_cap_ovr    = 0u;   /* SPI RX FIFO 溢出次数（0 = 没溢出）*/
volatile uint32_t g_cap_err    = 0u;   /* 启动/搬运错误位图：1=SPI配置 2=DMA_Init 4=DMA_Start 8=采集期DMA没在跑 */
volatile uint32_t g_cap_jump   = 0u;   /* 被丢弃的"不合理 NDTR 跳变"次数（>0 = 反推不可信，见 cap_poll_hw）*/
volatile uint32_t g_cap_peak_l = 0u;   /* 最近一个统计窗的 L 声道峰值（0..32767）*/
volatile uint32_t g_cap_peak_r = 0u;   /* 同上，R 声道 */
volatile int32_t  g_cap_dc_l   = 0;    /* L 声道直流偏置估计（#0     */
volatile int32_t  g_cap_dc_r   = 0;    /* R 声道直流偏置估计（两者都有一个小而稳的值 = 有模拟源）*/
volatile uint32_t g_cap_nz     = 0u;   /* 非零样本千分比（0 = 整段全 0）*/
volatile uint32_t g_cap_hist[8];       /* |样本| 的 8 级直方图（看"像不像真的模拟信号"）*/
volatile uint32_t g_cap_dbg[8];        /* 同一时刻现场：NDTR/CR/SR/CFG1/I2SCFGR/CR1/tot/lost */
/* ---- 麦克风硬件诊断（docs/MIC_RECORD_DESIGN.md §7 阶梯第 1 步）：PC2 线活动度 ----
 * 【为什么必须有它】"采到的样本全 0"这一个特征**无法区分**：
 *   ① DOUT 线没通 / 接错脚（PC2 上没有任何东西在驱动）
 *   ② 模块确实在发数据，但模拟侧没信号（模块没有麦克风 / 偏置没起来 / PGA 没开）
 * ⇒ 直接看 PC2 这根线本身在不在翻转，就能一刀切开：
 *   · flips ≈ 0 且 hi/lo 一边倒 ⇒ 线上没人驱动 ⇒ 查接线/接脚（硬件问题）
 *   · flips 大量、0/1 都有      ⇒ 模块在发 ⇒ 问题在模拟侧（麦克风/偏置/PGA）
 * ⚠ 硬件红线：只**读** GPIOC->IDR，绝不驱动/强灌/改 PC2 的电气。 */
volatile uint32_t g_cap_line_req   = 0u;  /* 写 N ⇒ 下一拍录音时连续采样 N 次 IDR（一次性）*/
volatile uint32_t g_cap_line_n     = 0u;  /* 已采样次数（累计）*/
volatile uint32_t g_cap_line_hi    = 0u;  /* 其中读到"1"的次数（0 = 线被恒低，=n = 恒高）*/
volatile uint32_t g_cap_line_flips = 0u;  /* 0↔1 翻转次数（≈0 = 线上没信号在动）*/

static uint16_t AUDIO_BUF_ATTR s_cap_buf[CAP_DMA_HALFWORDS];
static DMA_HandleTypeDef s_hdma_rx;
static uint32_t s_cap_ndtr_prev = CAP_DMA_HALFWORDS;  /* 上次 NDTR；初值=满 表示"环内位置 0" */
static uint32_t s_cap_rd        = 0u;                 /* 读游标（单调半字数）*/
static uint32_t s_cap_tot       = 0u;                 /* 写游标（单调半字数）*/
static uint8_t  s_cap_ready     = 0u;

static void cap_dbg_snapshot(void);   /* 前置声明：avail() 里要调它刷新寄存器现场 */

/* 主循环每拍调一次：把 NDTR 反推出来的写入量累进单调计数，并清 OVR。
 *
 * ⚠⚠ 2026-10-16 实测教训（**这里曾经是个假数据源**）：NDTR 反推法在"DMA 被停/被重新
 *   配置"时会看到**任意跳变**。原来的写法只要 adv≠0 就累加 ⇒ DMA 已经停住、NDTR 恒定不动，
 *   而 `_s_cap_ndtr_prev` 因为某种重置被当成了 0 ⇒ 一次就凭空记进 8160 个半字；
 *   实测 `g_cap_tot` 以 80 754 半字/秒增长（约 40 k 帧/秒），文件被写出 33.8 MB 的零数据。
 *   ⇒ 现在两道闸：① 只有 `DMA_SxCR.EN==1` 才认推进；② 单次推进不得超过半个缓冲。
 *   两道闸都不通过时把跳变**丢弃并计数**（`g_cap_jump`），既不造数也不静默。 */
static void cap_poll_hw(void)
{
    uint32_t ndtr = DMA1_Stream3->NDTR;

    if ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0u)          /* ① 通道真在跑 */
    {
        uint32_t written = CAP_DMA_HALFWORDS - ndtr;     /* 环内位置 0..N-1 */
        uint32_t prev    = CAP_DMA_HALFWORDS - s_cap_ndtr_prev;
        uint32_t adv     = (written >= prev) ? (written - prev)
                                             : (CAP_DMA_HALFWORDS - prev + written);

        if (adv > (CAP_DMA_HALFWORDS / 2u))               /* ② 单次推进的合理性上限 */
        {
            g_cap_jump++;                                 /* 记一笔：不是正常推进 */
            adv = 0u;
        }
        s_cap_tot += adv;
    }
    else
    {
        g_cap_err |= 8u;                                  /* bit3：采集期发现 DMA 没在跑 */
    }
    s_cap_ndtr_prev = ndtr;   /* ★ 无论哪种情况都更新基线，免得停住期间的跳变被记账 */

    /* OVR：FIFO 溢出会让引擎停吃 FIFO ⇒ 必须清，否则后面全丢（发方向的 UDR 同款坑）。*/
    if ((SPI2->SR & SPI_SR_OVR) != 0u)
    {
        SPI2->IFCR = SPI_IFCR_OVRC;
        g_cap_ovr++;
    }
}

uint32_t audio_out_capture_avail(void)
{
    if (s_cap_ready == 0u)
    {
        return 0u;
    }
    cap_poll_hw();
    cap_dbg_snapshot();              /* 采集期每拍刷新寄存器现场（主机读不到外设寄存器）*/

    /* 环被写满一圈还没被取走 ⇒ 最旧的数据已被覆盖。把读游标推到"最旧可用"处并计数，
     * 而不是假装没发生（本项目铁律：宁可看见丢数，也不要看不见的坏状态）。 */
    if ((s_cap_tot - s_cap_rd) > CAP_DMA_HALFWORDS)
    {
        uint32_t lost = (s_cap_tot - s_cap_rd) - CAP_DMA_HALFWORDS;

        s_cap_rd  += lost;
        g_cap_lost += lost;
    }
    g_cap_tot = s_cap_tot;
    return s_cap_tot - s_cap_rd;
}

/* 取走已采集的数据，并顺手算"到底像不像有信号"的统计量：
 *   峰值（每声道）、直流均值、非零千分比、|样本| 8 级直方图。
 *   为什么要它：DOUT 没接线时数据不是"0"，而是"悬空电平被采成的近似常数"——
 *   只有峰值/直流/直方图三者一起看才能把"线没接"与"接了但安静"区分开。 */
uint32_t audio_out_capture_read(int16_t *dst, uint32_t halfwords)
{
    uint32_t n, i;
    uint32_t pk_l = 0u, pk_r = 0u, nz = 0u, cnt = 0u;
    int64_t  sum_l = 0, sum_r = 0;
    uint32_t hist[8] = {0};

    if ((s_cap_ready == 0u) || (dst == NULL))
    {
        return 0u;
    }
    n = audio_out_capture_avail();
    if (halfwords < n) { n = halfwords; }
    if (n == 0u)       { return 0u; }

    for (i = 0u; i < n; ++i)
    {
        int16_t  s = (int16_t)s_cap_buf[(s_cap_rd + i) & (CAP_DMA_HALFWORDS - 1u)];
        uint32_t a = (uint32_t)((s < 0) ? -(int32_t)s : (int32_t)s);
        uint32_t b;

        if (a == 0u)            { b = 0u; }
        else if (a < 16u)       { b = 1u; }
        else if (a < 128u)      { b = 2u; }
        else if (a < 1024u)     { b = 3u; }
        else if (a < 4096u)     { b = 4u; }
        else if (a < 8192u)     { b = 5u; }
        else if (a < 16384u)    { b = 6u; }
        else                    { b = 7u; }
        hist[b]++;
        if (a != 0u) { nz++; }

        if ((i & 1u) == 0u)
        {
            dst[i] = s;
            sum_l += s;
            if (a > pk_l) { pk_l = a; }
        }
        else
        {
            dst[i] = s;
            sum_r += s;
            if (a > pk_r) { pk_r = a; }
        }
        cnt++;
    }
    s_cap_rd += n;

    if (cnt != 0u)
    {
        uint32_t cn = (cnt + 1u) / 2u;                 /* 每声道的样本数 */

        g_cap_peak_l = pk_l;
        g_cap_peak_r = pk_r;
        g_cap_dc_l   = (int32_t)(sum_l / (int64_t)(cn ? cn : 1u));
        g_cap_dc_r   = (int32_t)(sum_r / (int64_t)(cn ? cn : 1u));
        g_cap_nz     = (uint32_t)((nz * 1000u) / cnt);
        for (i = 0u; i < 8u; ++i) { g_cap_hist[i] = hist[i]; }
    }
    return n;
}

/* ★ 麦克风硬件诊断：连续采样 PC2（模块 DOUT → P2-9）的电平与翻转。
 * ⚠⚠ 硬件红线：**只读** GPIOC->IDR，绝不驱动/强灌/改 PC2 的电气（本函数不碰 MODER/OTYPER）。
 * 必须在**录音中**（I2S2 主收已起、模块正在发数据）调用，否则采到的是静态电平、没有意义。
 * 用途见 docs/MIC_RECORD_DESIGN.md §7 诊断阶梯第 1 步：
 *   flips ≈ 0 ⇒ 线上没人驱动（查接线）；flips 大量 ⇒ 模块在发，问题在模拟侧（麦克风/偏置）。*/
void audio_out_capture_probe_line(uint32_t n)
{
    uint32_t hi = 0u, flips = 0u, prev = 2u;   /* prev=2 = "尚无前值" */

    for (uint32_t i = 0u; i < n; ++i)
    {
        uint32_t b = (GPIOC->IDR >> 2u) & 1u;

        if (b != 0u) { hi++; }
        if ((prev <= 1u) && (b != prev)) { flips++; }
        prev = b;
    }
    g_cap_line_n     += n;
    g_cap_line_hi    += hi;
    g_cap_line_flips += flips;
}

int audio_out_capture_start(void)
{
    if (s_cap_ready != 0u)
    {
        return 0;                       /* 已在采集：幂等 */
    }
    if (g_ao_playing != 0u)
    {
        /* 放音还没停完（停是异步淡出）。发起一次"停"并让调用方下一拍再来 ——
         * 不在这里忙等，免得把主循环单拍拖长（本项目对单拍的判据是 ≤60 ms）。*/
        audio_out_stop();
        return 2;                       /* 2 = 稍后重试（约定见 audio_out.h）*/
    }
    if ((s_dma_ready != 0u) || (s_tim_pending != 0u))
    {
        return 2;                       /* 发方向的 DMA/心跳还占着 SPI2 */
    }

    i2s_gpio_init();                    /* 同一组脚；其中 PC2 = I2S2_SDI 本来就是给录音留的 */
    if (i2s_apply(I2S_MODE_MASTER_RX) != 0)
    {
        g_cap_err |= 1u;
        return -1;
    }

    __HAL_RCC_DMA1_CLK_ENABLE();
    s_hdma_rx.Instance                 = DMA1_Stream3;
    s_hdma_rx.Init.Request             = DMA_REQUEST_SPI2_RX;       /* ★ 39（发方向是 40）*/
    s_hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    s_hdma_rx.Init.Mode                = DMA_CIRCULAR;
    s_hdma_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;    /* 录音不许丢样本 */
    s_hdma_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    s_hdma_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    s_hdma_rx.Init.MemBurst            = DMA_MBURST_SINGLE;
    s_hdma_rx.Init.PeriphBurst         = DMA_PBURST_SINGLE;
    if (HAL_DMA_Init(&s_hdma_rx) != HAL_OK)
    {
        g_cap_err |= 2u;
        return -1;
    }

    /* **非 IT** 启动：收方向靠 NDTR 轮询就够，少一个 ISR、少一处竞态。*/
    if (HAL_DMA_Start(&s_hdma_rx, (uint32_t)(uintptr_t)&SPI2->RXDR,
                      (uint32_t)(uintptr_t)s_cap_buf, CAP_DMA_HALFWORDS) != HAL_OK)
    {
        g_cap_err |= 4u;
        return -1;
    }

    /* 清挂起标志，并**排空 RX FIFO** 里可能残留的旧数据 ——
     * 否则文件最前面几帧是上一段的尾巴（"开头有个怪响"的一种来源）。*/
    SPI2->IFCR = 0x0FF8u;
    for (uint32_t i = 0u; i < 64u; ++i)
    {
        if ((SPI2->SR & SPI_SR_RXP) == 0u) { break; }
        (void)SPI2->RXDR;
    }

    SET_BIT(SPI2->CFG1, SPI_CFG1_RXDMAEN);                        /* ★ 不置它 DMA 不会被叫 */
    MODIFY_REG(SPI2->CFG1, SPI_CFG1_FTHLV, (7u << SPI_CFG1_FTHLV_Pos));

    s_cap_tot       = 0u;
    s_cap_rd        = 0u;
    s_cap_ndtr_prev = CAP_DMA_HALFWORDS;
    g_cap_tot       = 0u;
    g_cap_lost      = 0u;
    g_cap_ovr       = 0u;
    g_cap_err       = 0u;
    g_cap_peak_l    = 0u;
    g_cap_peak_r    = 0u;
    g_cap_dc_l      = 0;
    g_cap_dc_r      = 0;
    g_cap_nz        = 0u;
    for (uint32_t i = 0u; i < 8u; ++i) { g_cap_hist[i] = 0u; }

    __HAL_I2S_ENABLE(&s_hi2s);
    SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART);                /* ★ 两段式：还要 CSTART */

    s_cap_ready = 1u;
    g_cap_on    = 1u;
    return 0;
}

void audio_out_capture_stop(void)
{
    if (s_cap_ready == 0u)
    {
        return;
    }
    /* 关闭顺序与发方向一致：先摘请求源，再让传输自然收尾（CSUSP），最后拆 DMA。 */
    CLEAR_BIT(SPI2->CFG1, SPI_CFG1_RXDMAEN);
    if (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
    {
        uint32_t t0 = HAL_GetTick();

        SET_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSUSP);
        while (READ_BIT(s_hi2s.Instance->CR1, SPI_CR1_CSTART) != 0UL)
        {
            if (HAL_GetTick() - t0 > 100u) { g_ao_csusp_tmo++; break; }
        }
    }
    __HAL_I2S_DISABLE(&s_hi2s);
    (void)HAL_DMA_Abort(&s_hdma_rx);
    HAL_DMA_DeInit(&s_hdma_rx);

    /* 把 CFG1 还原成"发方向"的样子（FTHLV 复位值 + RXDMAEN 已清）：
     * 放音下一次起播本来就会走 i2s_init() 重配，这里显式还原是为了"离开采集态就干净"。 */
    MODIFY_REG(SPI2->CFG1, SPI_CFG1_FTHLV, 0u);
    SPI2->IFCR = 0x0FF8u;
    i2s_gpio_release();                 /* PB9 还给 UART4 */

    s_cap_ready = 0u;
    g_cap_on    = 0u;
}

/* 采集侧的寄存器现场（主机读不到外设寄存器，必须拷进 RAM）。 */
static void cap_dbg_snapshot(void)
{
    g_cap_dbg[0] = DMA1_Stream3->NDTR;
    g_cap_dbg[1] = DMA1_Stream3->CR;
    g_cap_dbg[2] = SPI2->SR;
    g_cap_dbg[3] = SPI2->CFG1;
    g_cap_dbg[4] = SPI2->I2SCFGR;
    g_cap_dbg[5] = SPI2->CR1;
    g_cap_dbg[6] = s_cap_tot;
    g_cap_dbg[7] = g_cap_lost;
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
        /* ⚠ 这里**什么都不写**是正确的、也是 TIM6 相对 DMA 的结构性优势：
         *   写/读指针相等 ⇒ 不往 TXDR 写 ⇒ H7 的 I2S 在 TXF 空时会**重复上一个采样**
         *   ⇒ 输出电平**保持不变**，没有任何阶跃 ⇒ 一声"咔"都产生不了。
         *   （DMA 路径做不到这一点：它一次搬 2048 个半字，缺数只能整块补，所以补什么
         *     就等于输出什么 —— 补硬 0 就是 21 ms 的满幅阶跃。补丁15 已把那里改成
         *     "线性衰减"，见 dma_fill_half。）
         * ⚠ 因此 tz_run_max 量到的大段连续零**通常是曲目本身的开头静音**，不是故障：
         *   判据请看 g_ao_feed_short_max / g_ao_under_short（那是"人为合成"的证据）。 */
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
        {
            uint16_t v = s_ring[s_rd & RING_MASK];

            /* ★ 补丁15：TIM6 路径的"成块零"检测。
             *   TIM6 是**逐半字**流，所以没有 DMA 那种"半缓冲接缝"；但它若吃到一整块零
             *   （feeder 在 pump 里合成的零块 / 环里被灌了零），在 DAC 侧同样是电平阶跃
             *   ⇒ 同样是一声"咔"。有这个量才能判断"换了 TIM6 之后还剩不剩咔"。 */
            if (v == 0u)
            {
                if (++s_tz_run > g_ao_tz_run_max) { g_ao_tz_run_max = s_tz_run; }
            }
            else
            {
                s_tz_run = 0u;
            }
            *((volatile uint16_t *)&SPI2->TXDR) = v;
        }
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

/* 补 N 帧（N ≤ RING_HALF_FRAMES）。feeder 给不满就补静音，绝不留下上一首的残影。
 * ★ 2026-10-14（补丁14）：由 pump_half() 拆出可变量版本 —— 理由见下方补数判据的注释。 */
static void pump_frames(uint32_t frames)
{
    uint32_t got = 0;
    int      n   = -1;

    if (s_feeder != NULL)
    {
        n = s_feeder(s_tmp, frames);
    }
    if (n > 0)
    {
        got = (uint32_t)n;
    }
    if (got > frames)
    {
        got = frames;
    }
    if (got < frames)
    {
        uint32_t miss = frames - got;

        /* ★ 补丁15：把"短读了多少"量化下来。
         * ⚠⚠ 旧代码在这里 `memset` 填零 —— 也就是说：**只要 feeder 少给一个帧，剩下的
         *   全部被合成成 0 再放进环**。feeder 读到文件尾（`feeder_cb` 里 avail==0 直接
         *   `return 0`）或 f_read 失败（`return -1`）时 got 就是 0 ⇒ 一次合成
         *   **整个 frames（1024 或 2048 帧 = 21~42 ms）的硬零块**，直接播出去 = 一声"咔"。
         *   而这条路径**既不计缺口也不报错**（只让 g_ao_under_feed +1，看不出大小）
         *   ⇒ 属于本项目最忌的"看不见的坏状态"。先把它量出来。 */
        if (miss > g_ao_feed_short_max) { g_ao_feed_short_max = miss; }
        if ((s_feeder != NULL) && (s_demo_left == 0u))
        {
            g_ao_underrun++;             /* 数据供不上（卡慢/播到最后一段）*/
            g_ao_under_feed++;           /* ⚠ 与 ① 分开计：多数只是短读/尾巴，听不出来 */
        }
    }
    /* ★★ 补丁15：**只把 feeder 真正给出来的数据放进环，绝不合成零块**。 ★★
     * 【旧行为为什么是错的】旧代码在 got<frames 时 `memset` 把剩下的填 0，再
     *   `ring_put(..., frames*2)` 整块推进环 —— 也就是**人为制造一段硬静音**：
     *     · feeder 读到文件尾（feeder_cb 里 avail==0 直接 return 0）⇒ got=0
     *       ⇒ 一次就是 **1024~2048 帧 = 21~42 ms 的硬零块**；
     *     · f_read 短读 / 失败（return -1）⇒ 同理。
     *   这段零块进环后会被 DMA/TIM6 **原样送进 DAC** ⇒ 电平阶跃 ⇒ 一声"咔"。
     *   而且它**既不计缺口也不报错**（只让 g_ao_under_feed +1，看不出大小）—— 属于
     *   本项目最忌的"看不见的坏状态"。
     * 【新行为】给多少放多少；got==0 时什么都不放。环里**永远不会出现人为零块**。
     *   真的"没数据了"由下游以**平滑衰减**兜底（见 dma_fill_half 与 TIM6 的 dry 分支），
     *   而且"播完"有专门的收尾（wav_play_tick → audio_out_stop → 淡出停钟）。 */
    if (got != 0u)
    {
        ring_put(s_tmp, got * 2u);
    }
    g_ao_pump++;
}

/* 补半个环（2048 帧 = 4096 半字）/ 补 1/4 个环（1024 帧 = 2048 半字）。 */
static void pump_half(void)    { pump_frames(RING_HALF_FRAMES); }
static void pump_quarter(void) { pump_frames(RING_QUARTER_FRAMES); }


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
    audio_out_set_volume((uint8_t)es8311_ui_volume_effective());
    return audio_out_start();
}

/* ====================== 对外接口 ======================= */

void audio_out_set_feeder(audio_out_feeder cb)
{
    s_feeder = cb;
}

void audio_out_set_volume(uint8_t vol0_100)
{
    /* ★★ 2026-10-16（补丁16）：只在**正在播放**时才把音量写进芯片。 ★★
     *
     * 【原实现与自己的注释不符 —— 这就是"刚开始播放那一下爆音"的根因】
     *   原注释写的是"codec 没起来（没接模块 / **未播放**）就只记录"，但守卫只查了
     *   `s_codec_ready`（= 跑过 init），**没有查"是否正在播放"**。
     *   而 wav_play_start() / wav_play_resume() 恰恰在 audio_out_start() **之前**就调本函数
     *   ⇒ 在"停止后芯片仍通电 + REG31 未静音、只差一个非零 REG32"的状态下，
     *     REG32 被提前写成**满音量**；紧接着 audio_out_start() 置 CSTART 开时钟，
     *     DAC 立刻以满音量输出音乐前几毫秒 ⇒ 那声爆音（详见 g_ao_vol_at_cstart 的注释）。
     *
     * 【修法】起播的音量一律交给 audio_out_tick 的 ramp（它从 0 起步、按 g_audio_volume 爬升，
     *   见 s_vol_ramp_left 分支）⇒ 本函数不再需要"替起播预热音量"。
     *   播放中拖着滑杆调音量仍会直写 REG32（即时可听），行为不变。
     * ⚠ 不写 ≠ 丢掉：g_audio_volume 才是权威值，ramp 与 es8311_dac_readback() 都读它。 */
    /* ★★ 2026-10-17（补丁22）：**暂停淡出期间绝不写 REG32**。★★
     *
     * 【这就是用户报的"点暂停/播放偶发突然变得很大声"的根因】
     *   补丁15 起 audio_out_pause() 是**异步淡出**：函数返回时 `g_ao_playing` **仍是 1**，
     *   要等 ≥ AO_PAUSE_MIN_RAMP_MS(300 ms)、由 tick 收尾后才归零。
     *   而 wav_play_resume() 在 audio_out_start() **之前**就先调本函数（wav_play.c），
     *   补丁16 的守卫只查 `g_ao_playing != 0` ⇒ **淡出期间它仍然为真** ⇒ 这一笔把 REG32
     *   **一步写成满音量**（UI 51 ⇒ 0x61），而此刻 I2S 传输还在跑、DAC 还在出声
     *   ⇒ 喇叭立刻以满音量出声，直到下一拍 ramp 把它拉回 target/8 ⇒ 一声响亮的突兀爆音
     *   （先炸一下、再轻、再爬升 —— 正是"音量不正确，突然变得很大声"）。
     *   ⚠ 补丁21 把淡出窗口从 192 ms 拉长到 ≥300 ms，**正好把"下一次点击落在淡出窗口内"
     *     的概率放大** ⇒ 用户觉得"还是有、更频繁了"。这不是补丁21 引入的新 bug，
     *     而是它把原本 192 ms 的既有窗口变宽、让既有缺陷更容易被踩到。
     *
     * 【修法】淡出期间音量一律交给 tick 的 ramp：续播走幂等分支时会重挂
     *   `s_vol_ramp_left = AO_VOL_RAMP_STEPS`，从当前低音量**平滑**爬升到 g_audio_volume。
     *   ⇒ 本函数在淡出期间直接返回，一笔都不写。
     *   淡出期间拖滑杆同样应被忽略（那时正在往静音走，且续播 ramp 会按最新值爬升）。
     * ⚠ 不写 ≠ 丢掉：g_audio_volume 仍是权威值，ramp 与 readback 都读它。 */
    if (s_pause_fade_ticks != 0u)
    {
        g_ao_vol_during_fade++;             /* 铁律10：这条分支被走到的直接证据 */
        g_ao_vol_during_fade_val = (uint32_t)vol0_100;
        if (g_ao_vol_guard_en != 0u)
        {
            return;                         /* 守卫开：淡出期间不写，交给 ramp 平滑处理 */
        }
        /* 守卫关（A/B 复现旧行为）：继续往下写，用 g_ao_vol_fade_wrote 量化后果。 */
    }
    if (g_ao_playing != 0u)
    {
        (void)es8311_set_volume_ui((uint32_t)vol0_100);
        if (s_pause_fade_ticks != 0u)
        {
            g_ao_vol_fade_wrote++;          /* 真的在淡出期间写了满音量 ⇒ 爆音风险成立 */
        }
    }
}

/* ★ 2026-10-14（补丁15b）：把"本次播放"的计数器清零抽成一个函数。
 * 【为什么必须抽出来】补丁15 把 audio_out_stop() 改成了**异步淡出**：函数返回时
 *   `g_ao_playing` 仍为 1，192 ms 后才由 audio_out_tick() 收尾归零。
 *   ⇒ "停止后立刻起播"这一次调用会落进 audio_out_start() 开头的**幂等分支**
 *     （`if (g_ao_playing != 0u)`），而该分支原先**一个诊断量都不重置**：
 *       · `g_ao_frames` 继续在上一次播放的量上累加 ⇒ 探针算出的"平均帧/中断"
 *         从满值 1024 掉到 169.8（分子把上一段全带进来了）；
 *       · `g_ao_t_*` 继续用上一次播放的 `g_ao_t_cstart_ms` 算**绝对**时间 ⇒
 *         板上实测读出 `g_ao_t_ramp_ms = 87783`（87.8 s，约等于开机时长）这种荒诞值，
 *         直接让"起播防咔哒时序表"这条判据失效。
 * 【语义上也应当重置】"暂停/停止被重新起播取消"= 上一段播放到此结束、这里开新的一段。
 * ⚠ 本函数**只清计数器**，不碰"连续样本流"的上下文（s_dma_last / s_dma_wr_seq /
 *   s_tz_run / s_dma_hold）—— 那些是流的一部分，在流中间清零会凭空造出一个阶跃，
 *   正好是本轮（补丁15）要消灭的东西。流状态只在**全新起播**时由调用方重置。 */
static void ao_reset_play_counters(void)
{
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
    /* 补丁15：数据出口"咔"量度（按本次播放清零）*/
    g_ao_seam_max        = 0u;
    g_ao_seam_n          = 0u;
    g_ao_tz_run_max      = 0u;
    g_ao_feed_short_max  = 0u;
}

int audio_out_start(void)
{
    int      rc;
    uint32_t ran_full_init = 0u;   /* ★ 补丁15c：本次起播是否跑了完整 init（含软复位）*/

    if (g_ao_playing != 0u)
    {
        /* 正在播、但"暂停淡出"还在进行（g_ao_playing 尚未归零、s_pause_fade_ticks > 0）
         * ⇒ 取消淡出、把音量重新 ramp 回目标，否则会卡在"正在淡出到静音"且永远回不来。
         * 典型场景：快速连点 暂停→播放，暂停的淡出还没走完就点了播放。 */
        if (s_pause_fade_ticks != 0u)
        {
            s_pause_fade_ticks  = 0u;
            s_stop_after_fade   = 0u;   /* ★ 补丁15：刚发起的"停"被这次起播取消了 */
            s_dac_rb_pending   = (uint8_t)AO_RB_CONFIRM_TRIES;
            s_vol_ramp_left    = (uint8_t)AO_VOL_RAMP_STEPS;  /* 从当前低音量重新 ramp 上去 */
            g_ao_dac_ramp      = s_vol_ramp_left;
            g_ao_wake_cnt++;
            g_ao_resume_calls++;
            g_ao_idem_cancel++;         /* ★ 补丁15d：这条幂等分支被走到的直接证据 */
            /* ★ 补丁15b：这次"续播"同时也是一段**新播放**的开始 ⇒ 诊断量必须重新标定。
             *   理由见 ao_reset_play_counters() 的函数头（异步停止 ⇒ 停→立刻起播走幂等分支
             *   ⇒ 旧代码里 g_ao_t_* / g_ao_frames 会沿用上一段的口径，板上实测
             *   g_ao_t_ramp_ms=87783、平均帧/中断=169.8）。
             *   ⚠ 流状态（s_dma_last / s_dma_wr_seq / s_tz_run / s_dma_hold）**保持不变**：
             *     这是同一路样本流的延续，清零会造阶跃。 */
            g_ao_t_cstart_ms   = HAL_GetTick();   /* 时间原点重新标定到这次续播 */
            ao_reset_play_counters();
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
        ran_full_init = 1u;             /* ★ 补丁15c：本次起播跑了完整 init（含软复位）*/
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

    /* ★★ 补丁15c：首次起播把"允许唤醒 DAC"的时刻推后 AO_WAKE_SETTLE_FIRST_MS。 ★★
     * 【为什么】首次起播刚做过 REG00 软复位，芯片在自己的复位窗口里对 I2C 地址回 NACK，
     *   而唤醒 DAC 那 20 笔只能**同步**跑在主循环里。板上实测（复位后第一次起播，
     *   tools/bench.rd 读 g_es_* 差值）：写 NACK 22 笔、读 NACK 17 笔、重试后成功 13 笔、
     *   重试 3 次仍丢 9 笔、**bus clear 29 次、熔断 3 次、单笔事务最长 31.4 ms**
     *   ⇒ 整段唤醒 110~129 ms（后续起播只要 20~60 ms）。
     *   而 DMA 的余量只有"环 25 ms + 乒乓缓冲 12.5 ms = 37.5 ms"（按实测吞吐折算）
     *   ⇒ 环被抽干，一整个半缓冲被合成（实测 ring_min=0、under_short=2048 @105 ms）。
     * 【修法】这几十毫秒里 I2S 照常送数但 REG32=0（**听不见**），主循环每 5 ms 补一次数、
     *   把环补满；等复位窗口过去再唤醒 ⇒ 唤醒那一段变快，且开跑时手里是满环。
     *   ⚠ "先时钟、后通路"的不变量反而更稳（唤醒时时钟已经跑了 120 ms）。 */
    s_wake_settle_ms = (ran_full_init != 0u) ? AO_WAKE_SETTLE_FIRST_MS : 0u;

    s_wr = 0u;
    s_rd = 0u;
    /* ★ 2026-10-14（补丁12/15c）：起播把环形缓冲**铺满**（整环 16384 半字）。
     *
     * 【为什么原来只铺半环是个坑】DMA 路径起播时，i2s_dma_start() 紧接着会调两次
     *   dma_fill_half() 把**两个半缓冲都取走**（4096 半字）——若此前只铺了半环（4096），
     *   环里就**正好空**。于是从 CSTART 到"主循环下一拍补数完成"这段时间，环里一颗数据
     *   都没有：只要这期间主循环被堵（起播那一拍要塞 codec 通路 ~16 笔 I2C + 两次现场快照
     *   + 读卡），第一个半传中断就会补进一整块硬静音 ⇒ 起播处一声"咔/断"。
     *   板上实测（tools/audio_buf_probe.py，2026-10-14）：从按下播放起算
     *   `g_ao_underrun` 一次就是 +6 ⇒ 起播窗口确实在成片填静音。
     *
     * 【修法】永远铺满整环：DMA 取走 4096 后环里仍剩 RING_HALFWORDS−4096 个半字。
     *   补丁15c 起环深翻倍（8192→16384）⇒ 剩 12288 半字 ≈ 37.5 ms 的垫子（按实测
     *   327 664 半字/s 折算），加上 DMA 乒乓缓冲自身 4096 半字（12.5 ms）≈ 50 ms。
     *   ⚠ 安全性：写/读指针是**单调 32 位计数器**（只在取下标时 & RING_MASK），
     *     "满"表现为差值 == RING_HALFWORDS（不是 0）⇒ 不存在"满空同形"，铺满不会误判。
     *     旧注释担心的 `(wr-rd)&MASK == 0` 早已不适用于当前实现（见 ring_put 的守卫）。 */
    for (uint32_t i = 0u; i < (RING_HALFWORDS / (RING_HALF_FRAMES * 2u)); ++i)
    {
        pump_half();
    }
    g_ao_stage = 3u;

    /* ★★ 2026-10-16（补丁16）：起播**安全栅** —— 先静音，再开时钟。 ★★
     *
     * 【不变量】任何一次起播，在 I2S 真正开始传输（CSTART）之前，芯片的 REG32 必须是 0
     *   （-95.5 dB）。这是"起播不爆音"的必要条件：停止/暂停不断电 ⇒ 芯片可能处在
     *   "通电 + REG31 未静音 + REG32 = 满音量"的状态（例如：用户在**停止态**拖过音量滑杆，
     *   或历史版本里 wav_play_start() 提前写音量）—— 若就这么开时钟，音乐前几毫秒会以满音量
     *   直冲喇叭。放这一笔之后，无论上游是谁把 REG32 弄成非 0，都不可能在开钟前"漏"过去。
     *
     * 【为什么放在这个位置】此刻环缓冲已铺满、DMA 还没跑、I2S 还没 CSTART ⇒ 写 I2C
     *   **碰不到任何正在播的数据**，零副作用；而且只此一处，覆盖全部起播入口
     *   （起播/续播/切歌/demo/停止态改过音量）。
     *
     * 【与下游的关系】下一拍 tick 的 es8311_dac_start_muted() 会把 REG32 再写一次 0（幂等），
     *   然后**从 0 开始** ramp 到 g_audio_volume ⇒ 与本栅不冲突，且顺序上正好构成
     *   "先静音 → 再开时钟 → 等数据稳定 → 再唤醒通路 → 从 0 爬音量"的完整链条。
     *
     * ⚠ g_ao_start_barrier_en 默认 1；写 0 可在同一固件上复现旧行为做 A/B（见其注释）。 */
    if (g_ao_start_barrier_en != 0u)
    {
        (void)es8311_set_volume_ui(0u);
    }

    /* 把"开钟那一刻芯片里的真实 REG32"回读下来 —— 让不变量**可测**，而不是靠推断。
     * （这是一笔读，发生在时钟启动之前 ⇒ 对出音零影响；读失败时高位置 1 标记。） */
    {
        uint8_t vb = 0xFFu;

        if (es8311_read_reg(0x32u, &vb) == 0)
        {
            g_ao_vol_at_cstart = (uint32_t)vb;
        }
        else
        {
            g_ao_vol_at_cstart = 0x100u | 0xFFu;
        }
    }

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
     *   单次播放的精确量。仅 resetting 播放相关的计数，不动 g_ao_stage/g_ao_codec 等状态位。
     * ★ 补丁15b：清零体已抽成 ao_reset_play_counters()，供本路径与"停→立刻起播"的
     *   幂等分支共用（两处口径必须完全一致，否则又是"换个入口就读出荒诞值"的老坑）。 */
    ao_reset_play_counters();
    /* 下面这几个是**流状态**（不属于计数器），只在全新起播时重置：
     *   s_dma_last/s_dma_wr_seq = 接缝检测的前驱与计数（新流从第 1 个半缓冲重新数）；
     *   s_tz_run                = TIM6 连续零的当前游程；
     *   s_dma_hold              = 缺口线性衰减的起点（新流从静音起）；
     *   s_stop_after_fade       = 全新起播，没有待收尾的"停"。 */
    s_dma_last           = 0;
    s_dma_wr_seq         = 0u;
    s_tz_run             = 0u;
    s_dma_hold           = 0;           /* 补丁15：缺口"冻结"的取值来源（新一次播放从 0 起）*/
    s_gap_tail           = 0u;          /* 补丁15c：新流没有待 ramp-in 的缺口 */
    s_stop_after_fade    = 0u;          /* 补丁15：全新起播，没有待收尾的"停" */
    return 0;
}

void audio_out_stop(void)
{
    if (g_ao_playing == 0u)
    {
        return;
    }

    /* ★★ 2026-10-14（补丁15）：停止改成「发起淡出 → 由 tick 在 12 拍后收尾」。 ★★
     *
     * 【为什么必须异步（这才是"停止/切歌/播完"那声咔的真正修法）】
     *   消咔的唯一手段是三步：**REG32 归零（硬件软 ramp 平滑渐弱）→ 等软 ramp 走完 →
     *   再掐 I2S 时钟**。软 ramp 实测约需 160 ms（REG37=0x48 ⇒ 0.25 dB/0.667 ms，
     *   0 dB → 听感静音）。而旧版这里是同步的：`become_silent()` 之后只 `HAL_Delay(4)`
     *   就 `i2s_stop()` —— **4 ms 只够降到 -1.5 dB**，等于在还能听见的音量上掐钟
     *   ⇒ ΣΔ 调制器停摆、输出保住一个直流电平 ⇒ 相对上一刻的波形就是一次阶跃 ⇒ **"咔"**。
     *   （补丁12/13 以为 `REG31` 数字静音那一笔能兜住，但那笔本身就是"把输入钳 0"
     *     的即时硬切 —— 见 es8311_dac_become_silent() 的函数头。）
     *   ⇒ 若在这里同步 `HAL_Delay(160)`：UI 会冻住 160 ms（用户直接摸到"卡"）。
     *   ⇒ 与「暂停」共用同一套延时收尾：本函数只负责**发起淡出**，真正停传输交给
     *     audio_out_tick：待静音确认后 ≥ AO_PAUSE_MIN_RAMP_MS(300 ms) 才切钟（见 tick 淡出分支）。
     *
     * 【对外语义的两点变化（已逐点核对影响面）】
     *   · 本函数返回时 `g_ao_playing` 仍为 1，约 192 ms 后才变 0。
     *     `audio_out_playing()` 在 src 下**没有别的调用点**（UI 用的是
     *     wav_play_busy()/g_wav_state，那两个在 wav_play_stop() 里是立即置位的）⇒ 无影响。
     *   · `wav_play_stop()` 紧随其后会 `audio_out_set_feeder(NULL)` + `s_opened=0` +
     *     `f_close()`：淡出期间 feeder 为 NULL，`feeder_cb` 也不会被调到（先判 s_opened）
     *     ⇒ **不会去读已关闭的文件**（安全性已逐行核对）。
     *   · 「停止 → 立即起播」（切歌）：`audio_out_start()` 里已有"正在播 + 淡出中 ⇒
     *     取消淡出并把音量 ramp 回去"的分支 ⇒ 切歌变成"不断音换源"，比原来硬切更好。 */

    s_stop_after_fade = 1u;             /* 淡出走完后断开 feeder（与暂停的唯一区别） */

    if (s_dac_rb_pending != 0u) { s_dac_rb_pending = 0u; }  /* 取消"起播音量确认期" */
    s_vol_ramp_left   = 0u;             /* 停掉起播 ramp，免得与淡出抢 REG32 */
    g_ao_dac_ramp     = 0u;
    g_ao_stop_calls++;

    /* 平滑淡出的第一步：REG32 → 0（不写 REG31）。★ 补丁21：捕获返回值，把静音落地
     * 时刻记进 s_mute_t0；没确认就留 0，tick 里继续重试（见 tick 淡出分支）。
     * ⚠ 不再用固定 12 拍倒计时 —— 时间基准从"静音真正落地"起算并带余量，否则 I2C
     * 首试 NACK 把静音那笔推后几十~一百多 ms 时，HW 软 ramp 没跑满就被切钟 ⇒ 偶发爆音。 */
    {
        int rc = es8311_dac_become_silent();
        if (rc != 0) g_ao_silent_fail++;
        s_mute_t0 = (rc == 0) ? HAL_GetTick() : 0u;
    }
    s_pause_start_t0   = HAL_GetTick();
    s_pause_fade_ticks = 1u;            /* 进入淡出态（取代原 12 拍倒计时）*/

    /* ⚠ 这里**不做**任何"立刻停"的动作：传输/A 时钟/GPIO 的收尾全部在 tick 里
     *   （ao_pause_stop_transport）。也**不调** es8311_stop_dac()：理由见下方注释。 */
}
    /* ---- 以下两条是补丁12/13 的历史结论，**仍然成立，勿回退** ---- */
    /* 【收口一】切钟前必须让"音量确实归零"。原来只"写一笔就算"，而 I2C 首试 NACK
     *   约 5%~30%（实测失败环里 0x31/0x32 命中最多）⇒ 写了没落进芯片 = 切钟时 DAC 还
     *   挂在有效音量上。现在由 es8311_dac_become_silent() 负责"写 + 回读确认 + 补写"，
     *   未确认就 +1 g_es_silent_ng（把"有爆音风险"变成可见计数）。
     * 【收口二】本路径**不调** es8311_stop_dac()：那是**真·掉电**（REG0D=0xFC 连内部参考
     *   VREF/VMIDSEL 一起断），而"基准塌掉 → 下次上电重建"会在输出耦合电容上产生一次
     *   低频台阶（"咚/噗"）—— 同一声爆音的另一个来源，且发生在模拟域，数字静音挡不住。
     *   实测旁证：走"通电不动电源"的暂停路径（补丁10 起）没有爆音抱怨，而走断电的
     *   停止/切歌一直有。⇒ 停止后芯片保持"通电 + 无 MCLK + 音量 0"，与暂停同一个状态。
     *   ⚠ 真·掉电路径没有消失：audio_test.c 的一次性自检 + 排障钩子 g_es_pdown_req
     *     （主循环消费一次）仍在用它 ⇒ 由 tools/es8311_clk_check.py 显式触发来保覆盖。
     * ⚠⚠ 补丁15 更正了上面【收口一】的一个误解：当时以为"REG31 数字静音"是消爆音的
     *   手段，**恰恰相反 —— 数字静音本身就是那声咔**（即时把调制器输入钳 0 ⇒ 输出瞬跳
     *   到中点）。所以 become_silent() 现在**只写 REG32**，靠硬件软 ramp 平滑渐弱；
     *   而"等软 ramp 走完"这件事由本函数与 tick 的延时收尾共同保证。 */

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

    /* ★ 补丁21·Fix2：取消任何进行中的音量上 ramp。否则"淡出中途又点播放"留下的
     *   s_vol_ramp_left>0 会在 tick 里把音量重新 ramp 回高音量，之后切钟时音量根本没静
     *   ⇒ 一声 pop（确定性竞态，快速连点时必现）。 */
    s_vol_ramp_left = 0u;
    g_ao_dac_ramp   = 0u;

    /* ★ 修"暂停咔嗒"：先静音（REG32=0），此刻 I2S 传输**仍在跑**、数据**仍在流** ——
     *   ES8311 硬件软 ramp（REG37=0x48）会把音量平滑降向 -95.5 dB（≈0.655 ms/0.25 dB），
     *   不会爆音。等 HW 软 ramp 落静音后，再由 audio_out_tick 的 s_pause_fade_ticks
     *   分支停传输（见下方）。
     *   原来：先 i2s_stop() 掐时钟、再 es8311_dac_mute() —— 时钟断的瞬间 DAC 还满音量
     *   ⇒ 输出电平瞬跳 ⇒ 那声"咔嗒"。
     * ⚠ 只静音、**不** es8311_stop_dac()（断电）：芯片保持可达，续播跳过完整 init。 */
    /* ★ 2026-10-14（补丁13）：改成"静音并**确认**"（es8311_dac_become_silent）。
     *   【为什么"写了就算"不行】暂停这条路的失败模式与停止完全一样：REG32 那一笔若丢了
     *   （实测首试 NACK 5%~30%，失败环里 0x32 命中最多），12 拍后停传输时 DAC 仍挂在
     *   有效音量 ⇒ 那声"咔嗒"照旧。而 P1 那条验收判据历史上只看"写了几笔"，
     *   看不出"写了但没落进芯片"—— 属于本项目最忌的"看不出来的坏状态"。
     *   ⇒ 现在：REG32 半步降 → 归零 → REG31 数字静音 → **回读确认**（不对就补写再验）。
     *     `g_ao_pause_volwr` 改为**动作内实测**的 REG32 写笔数（用 g_es_reg32_n 的前后差，
     *     窗口完全落在本次动作里，不受播放期后台流量影响）。 */
    {
        uint32_t n0 = g_es_reg32_n;
        int      rc = es8311_dac_become_silent();

        if (rc != 0)
        {
            g_ao_silent_fail++;         /* 未能确认静音：这一刻有爆音风险（量化，不掩盖）*/
        }
        g_ao_pause_volwr += (g_es_reg32_n - n0);
        /* ★ 补丁21·Fix1：把静音落地的时刻记进 s_mute_t0；没确认就留 0，tick 的淡出分支
         *   会从此刻起算 ≥ AO_PAUSE_MIN_RAMP_MS 才切钟，而不是从"暂停调用时刻"算固定的
         *   12 拍 —— 这样无论静音那笔因 I2C 重试晚落地多少，HW 软 ramp 都跑满，杜绝
         *   "某一次"的偶发 pop。 */
        s_mute_t0 = (rc == 0) ? HAL_GetTick() : 0u;
    }
    s_dac_rb_pending  = 0u;             /* 取消起播后的"音量确认期"，别回头把音量写回去 */
    s_pause_start_t0  = HAL_GetTick();  /* 暂停发起时刻（硬上限起点）*/
    s_pause_fade_ticks = 1u;            /* 进入淡出态（取代原 12 拍倒计时）*/
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

    /* ★ 补丁13 排障钩子：g_es_pdown_req=1 ⇒ 本拍执行一次**真·掉电**（es8311_stop_dac）。
     * 用途：补丁13 起正常播放/停止路径**不再**给 codec 断电（见 audio_out_stop 的「收口二」），
     *   于是"掉电 → 起播恢复时钟"这条配对的验收必须**显式触发**才还保持覆盖 —— 由
     *   tools/es8311_clk_check.py 写这个钩子来验（K2），而不是靠"每次停止都断电"顺带覆盖。 */
    if (g_es_pdown_req != 0u)
    {
        g_es_pdown_req = 0u;
        (void)es8311_stop_dac();
    }

    /* ★ 补丁17 排障钩子：g_es_clr_clk_req=1 ⇒ 本拍把 REG01 写成 0x00（时钟全关）。
     *   用途：**状态级**复现"起播时恢复时钟那一笔写丢了"之后芯片的真实处境
     *   （REG01 停在 0x00 ⇒ 无 MCLK/BCLK/DACCLK ⇒ 无声，而音量/电源/静音回读全都是正常的）。
     *   配合 g_es_clk_loop_en 做 A/B：闭环关掉 ⇒ 它永远不回来；闭环开着 ⇒ 下次回读体检补回。
     *   由 tools/es8311_clk_check.py 的 K7 触发。 */
    if (g_es_clr_clk_req != 0u)
    {
        g_es_clr_clk_req = 0u;
        (void)es8311_write_reg(0x01u, 0x00u);
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
     *   等 ≥ AO_PAUSE_MIN_RAMP_MS(300 ms)（自静音确认起算）、音量基本落静音后再停传输 —— 切断 I2S 时钟时 DAC
     *   已静音，不会爆音（见 audio_out_pause 顶部说明）。
     *   ⚠ 淡出期间**不 return**：仍让下方逻辑照常补数、DAC 继续出（渐弱）的声音，
     *   直到淡出走完才真正停传输。 */
    if (s_pause_fade_ticks != 0u)
    {
        uint32_t now = HAL_GetTick();

        /* ★ 补丁21·Fix1：淡出计时从"静音真正落地"起算，而不是从暂停/停止发起起算。
         *   - 静音尚未确认（s_mute_t0==0）：每拍继续重试静音，直到落地（g_ao_mute_retry++）；
         *   - 静音已确认：从确认时刻起 ≥ AO_PAUSE_MIN_RAMP_MS 才切 I2S 时钟，确保 HW 软 ramp
         *     把音量降到听感静音，不会爆音；
         *   - 静音始终没确认：硬上限 AO_PAUSE_HARD_CAP_MS 到点也必须切，防 I2C 死锁
         *     （此时有爆音风险，但比永远不切钟、链路卡死好；用 g_ao_fade_hardcap 量化）。 */
        if (s_mute_t0 == 0u)
        {
            if (es8311_dac_become_silent() == 0)
            {
                s_mute_t0 = now;
                g_ao_mute_retry++;
            }
        }
        int cut = 0;
        if ((s_mute_t0 != 0u) && ((now - s_mute_t0) >= AO_PAUSE_MIN_RAMP_MS))
        {
            cut = 1;
            g_ao_fade_min++;
        }
        else if ((now - s_pause_start_t0) >= AO_PAUSE_HARD_CAP_MS)
        {
            cut = 1;
            g_ao_fade_hardcap++;
        }

        if (cut != 0)
        {
            uint8_t was_stop = s_stop_after_fade;

            s_stop_after_fade = 0u;
            ao_pause_stop_transport();   /* ← 内部把 g_ao_playing 归零 */
            if (was_stop != 0u)
            {
                /* ★ 补丁15：淡出走完 ⇒ 真正"停"：断开喂数并记账。
                 *   ⚠ 只有"停"才断 feeder；"暂停"必须留着它，否则续播接不上。 */
                audio_out_set_feeder(NULL);
                g_ao_stop_done++;
            }
            /* ★ 补丁15b：**在 playing 归零之后补一次快照**。
             *   dbg_snapshot() 受 `if (g_ao_playing != 0u)` 保护，而上面刚把 playing 清了
             *   ⇒ 若不补这一次，`g_ao_dbg3` 会**冻结在"淡出最后一拍开头"的值**，读出来就是
             *   "停: 发起=N、收尾完成=N-1"这种差一拍的假象（板上实测踩到：
             *   tools/click_source_probe.py 相位④ 显示 发起=3/收尾=2，下一个相位才变成 3/3）。
             *   修完之后这条不变量（发起 == 收尾）才真的是可读判据。 */
            dbg_snapshot();
            return;                     /* 本拍不再喂数据 / 做别的事 */
        }
        /* 淡出期间不 return：仍照常补数、DAC 继续出（渐弱）的声音 */
    }

    /* ---- ★ 补丁15：**补数块搬到这里（原来是本函数最后一步）** ----
     *
     * 【为什么必须提前】起播后的**第一拍**紧接着要跑 `s_dac_pending` 那一整段 codec 通路
     *   I2C（`dac_path_regs` 约 20 笔写 + 若干读改写）。实测：总线干净时这段只要 ~17 ms，
     *   但在"NACK 风暴"下能长到 **143 ms**（每笔写都要 3 次重试 × 5 ms 超时）。
     *   而 `audio_out_start()` 交给 CSTART 的垫子只有 **4096 半字 = 42 ms**
     *   （环铺满 8192 后被 `dma_fill_half` 取走 4096）。
     *   ⇒ 若补数留在函数末尾，这一整段 I2C 期间**一次补数都不会发生**，DMA 在 42 ms 处
     *     就把环抽干 ⇒ 整块 2048 半字的缺口（实测 `g_ao_under_short=2048`、
     *     `g_ao_under_first_ms≈62`、`g_ao_t_pump1_ms≈84`，板板上可复现）。
     *   ⇒ 提到前面后：进 I2C 之前环已是满的（8192 半字 = 84 ms），加上 DMA 缓冲里的
     *     42 ms，手里共 **126 ms** 余量 —— 覆盖住那段最长 143 ms 的停顿（余下十几毫秒
     *     由补丁15 的"线性衰减"兜底，不会出现阶跃）。
     *   ⚠ 补数只碰环形缓冲，与 DAC 通路/音量状态完全无关 ⇒ 提前执行是安全的。 */
    /* 补数：**只要环里还有空间就补**，把环尽可能维持在"接近满"。
     *
     * 【2026-10-14 补丁14：为什么把判据从"占用 ≤ 半环"改成"剩余空间 ≥ 1/4 环"】
     *   旧判据 `used <= 半环 ⇒ 补半环` 让环占用长期停在 **0~4096 半字（0~42 ms）**，而 DMA 的
     *   消耗是**成批**的（每次半传/全传中断一次性取走 2048 半字）：实测（tools/tim6_vs_dma_check.py，
     *   同一段音乐、同一次干扰）环占用在 0 与 4096 之间摆动、**最低到 0** ⇒ 主循环只要耽误一拍，
     *   下一次 dma_fill_half 就找不到 2048 半字，只能把尾部**填硬 0**（一次 2048 半字 = 21 ms
     *   电平跳变块 = 一声"咔"）。
     *   对照：TIM6 路径的消耗是**逐个半字**的（ISR 每次只写 1 个），同样干扰下环最低水位实测
     *   2304 半字、缺口 0 —— 这就是"同样一次供数失误，两条路径代价差 2048 倍"的由来。
     *   新判据下环稳定在 6144~8192 半字（63~84 ms），扛得住约 3 次连续的半缓冲补数，
     *   即主循环被堵 ~63 ms 也不破音（旧设计余量 = 0）。 */
    {
        uint32_t used  = s_wr - s_rd;
        uint32_t space = RING_HALFWORDS - used;

        /* 补丁12：记录播放期环占用的**最小值** —— 它就是"抗主循环卡顿的垫子"。 */
        if (used < g_ao_ring_used_min)
        {
            g_ao_ring_used_min = used;
        }
        /* 空间够补半环就补半环；只剩 1/4 环的空间就补 1/4 —— 绝不因"没地方放"而放弃补数
         * （放弃补数 = 环就地见底 = 下一拍就可能填硬 0）。 */
        if (space >= (RING_HALF_FRAMES * 2u))
        {
            pump_half();
            if (g_ao_t_pump1_ms == 0u)
            {
                g_ao_t_pump1_ms = HAL_GetTick() - g_ao_t_cstart_ms;
            }
        }
        else if (space >= (RING_QUARTER_FRAMES * 2u))
        {
            pump_quarter();
            if (g_ao_t_pump1_ms == 0u)
            {
                g_ao_t_pump1_ms = HAL_GetTick() - g_ao_t_cstart_ms;
            }
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

        /* ★ 补丁15c：首次起播时再等一个"落定窗口"（s_wake_settle_ms，见 audio_out_start）。
         *   这几十毫秒里 I2S 照常送数但 REG32 还是 0（听不见），主循环把环补满；
         *   顺带避开芯片复位窗口里的 I2C NACK 风暴（那正是唤醒要 110~129 ms 的原因，
         *   而 DMA 的余量只有 ~37.5 ms ⇒ 会合成一整个半缓冲）。 */
        if ((ready != 0) && (s_wake_settle_ms != 0u) &&
            ((uint32_t)(HAL_GetTick() - g_ao_t_cstart_ms) < s_wake_settle_ms))
        {
            ready = 0;                  /* 还没到点：这一拍只补数，不唤醒 */
        }

        if (ready != 0)
        {
            s_dac_pending = 0u;
            s_wake_settle_ms = 0u;      /* 落定窗口一次性用完 */
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
        uint32_t target = es8311_ui_volume_effective();

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

    /* ★ 补丁15：补数块已**上移到本函数前半段**（在 `s_dac_pending` 唤醒 DAC 之前）——
     *   理由见那里的长注释：起播第一拍那段 codec 通路 I2C 在 NACK 风暴下能长达 143 ms，
     *   而补数若留在末尾，这 143 ms 内环不会被补，DMA 在 42 ms 处就把起播垫子抽干。 */
}
