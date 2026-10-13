/* ===========================================================================
 * WAV 播放源：TF 卡上的 16 bit PCM WAV → audio_out（I2S2 + ES8311）
 *
 * 【支持范围（有意收窄，别拿它当通用解码器）】
 *   · RIFF/WAVE，fmt chunk 为 **PCM(16 bit)**，声道 1 或 2；
 *   · 采样率**不限**，但⚠ I2S 的 fs 由 MCLK 固定（本板 12.5 MHz/256 = 48 828 Hz），
 *     ⇒ 文件采样率与它不一致时**播放速度/音高会等比变化**：
 *       48 000 Hz 文件 ⇒ 快 1.7%（听不出来）；44 100 Hz ⇒ 慢 10.7%（明显变调）。
 *     ⇒ 出货配的 WAV 一律做成 **48 828 Hz**（tools/wav_prepare.py 就是这个口径）。
 *
 * 【播放模型】
 *   `wav_play_tick()` 在主循环里驱动 audio_out 的补数（audio_out_tick），
 *   补数时从卡上读一段（f_read）→ 单声道则展开成 L=R 的交错流 → 交给 DMA 缓冲。
 *   半缓冲 2048 帧 ≈ 42 ms，所以**一次 f_read 必须在这个时间内返回**。
 *
 * ⚠ 与图库/文件管理器共用 img_scan_* 那张扫描表（同一时刻只服务一个分类）。
 * =========================================================================== */
#ifndef WAV_PLAY_H
#define WAV_PLAY_H

#include <stdint.h>

/**
 * @brief  打开 TF 卡上的 WAV 并开始播放（内部会 audio_out_start）
 * @param  path 绝对路径，如 "/YMGUI/MUSIC/TOKYOHOT.WAV"
 * @return 0 = 成功；<0 = 失败，细分码写进 g_wav_rc：
 *          -1 空路径  -2 卡没挂载  -3 打不开文件
 *          -11..-20 WAV 头解析失败（-10 + 内部细分码）
 *          -21 定位到 data 失败  -22 音频输出起不来（codec/I2S）
 */
int  wav_play_start(const char *path);

/** 停止播放并关文件 */
void wav_play_stop(void);

/** 主循环每拍调一次：驱动补数 + 播完自动收尾 */
void wav_play_tick(void);

/** 1 = **正在出声**（暂停/停止都返回 0；判"有没有暂停会话"用 wav_play_paused）*/
int  wav_play_busy(void);

/**
 * @brief  暂停：停出音，但**保留文件句柄与播放位置**（区别于 wav_play_stop）
 * @return 0 = 成功；<0 = 没在出声，无物可暂停。
 */
int  wav_play_pause(void);

/**
 * @brief  续播：从暂停处（或暂停后 seek 到的新位置）继续
 * @return 0 = 已续播；<0 = 当前不是暂停态 / 文件没开 / 音频起不来。
 */
int  wav_play_resume(void);

/** 1 = 处于暂停态（文件仍打开、位置保留，可续播）*/
int  wav_play_paused(void);

uint32_t wav_play_elapsed_ms(void);
uint32_t wav_play_total_ms(void);

/**
 * @brief  跳播：把当前播放位置移到 ms 处（f_lseek + 同步 g_wav_pos）
 * @return 0 = 成功；<0 = 失败（文件没开 / 头没解析 / lseek 失败）。
 *         ⚠ 文件已打开即可用（播放中、暂停中都能跳）；落点按帧边界对齐、不落在 data 末尾。
 */
int      wav_play_seek_ms(uint32_t ms);

/**
 * @brief  当前（在播/暂停）曲目的显示名（路径末段，含扩展名）
 * @return 0 = 有当前曲目；<0 = 没有（停止/从未起播），此时 out 置空串。
 */
int      wav_play_current_name(char *out, int n);

/* ---- SWD 可读诊断量 ---- */
extern volatile int32_t  g_wav_rc;       /* 0=OK；<0 = 失败（取值表见 wav_play_start 注释）*/
extern volatile uint32_t g_wav_state;    /* 0 停 / 1 播 */
extern volatile uint32_t g_wav_rate;     /* 文件采样率 */
extern volatile uint32_t g_wav_bits;     /* 位深（本版恒 16）*/
extern volatile uint32_t g_wav_channels; /* 1 或 2 */
extern volatile uint32_t g_wav_total;    /* 数据区总字节 */
extern volatile uint32_t g_wav_pos;      /* 已读字节 */
extern volatile uint32_t g_wav_underrun; /* 卡没供给上的次数 */
extern volatile uint32_t g_wav_read_ms;  /* 最近一次补数耗时（毫秒，判卡够不够快）*/
extern volatile uint32_t g_wav_test;     /* 诊断：写 1 起播 /YMGUI/MUSIC/TOKYOHOT.WAV（SWD 验收用）*/

/* ====================== 验收「相位锁存」表（2026-10-13）======================
 * 目的：把"测量"从主机搬到板上，根治"逐符号读 ⇒ 每次一次内核停机 ⇒ I2C 假 NACK"。
 *   主机写 g_wav_snap_req=<相位码 1起播/2停/3暂停/4续播>，板子在下一拍执行该动作并
 *   立刻把下表全部量锁进 g_wav_snap[]（同一时刻）。主机用**一次**批量读（二进制）取回。
 *   g_wav_snap[G_SNAP_MAGIC] 最后写 ⇒ 主机凭魔数判"这一拍的表是完整的"。
 * ⚠ 索引顺序即内存布局，**改动会同时废掉 tools/wav_pause_resume_check.py**。 */
#define G_WAV_SNAP_MAGIC 0x5A4D4731u   /* 'ZMG1' */
#define G_SNAP_MAGIC    39u   /* 魔数 G_WAV_SNAP_MAGIC（最后写）*/
#define G_SNAP_ESPRETRY 40u   /* g_es_probe_retry  probe 首试没探到、延时后探到 */
#define G_WAV_SNAP_N     41u  /* 表长（含 MAGIC）—— 主机按此字节数批量读 */

#define G_SNAP_PHASE     0u   /* 本次锁存对应的相位码（1/2/3/4）*/
#define G_SNAP_LOOP      1u   /* 锁存瞬间的 g_loop_count（判"动作后又过了几拍"）*/
#define G_SNAP_WSTATE    2u   /* g_wav_state：0 停 / 1 播 / 2 暂停 */
#define G_SNAP_WRC       3u   /* g_wav_rc（<0 见 wav_play_start 取值表）*/
#define G_SNAP_WPOS      4u   /* g_wav_pos 已读字节 */
#define G_SNAP_WTOTAL    5u   /* g_wav_total 数据总字节 */
#define G_SNAP_AOPLAY    6u   /* g_ao_playing */
#define G_SNAP_AOREADY   7u   /* g_ao_dac_ready */
#define G_SNAP_AORAMP    8u   /* g_ao_dac_ramp 剩余 ramp 步数（终态应 0）*/
#define G_SNAP_AOWAKE    9u   /* g_ao_wake_cnt（起播+续播各 +1）*/
#define G_SNAP_AOSKIP   10u   /* g_ao_init_skips（续播跳过完整 init 应 +1）*/
#define G_SNAP_AOINITRC 11u   /* g_ao_init_rc（0 = codec 序列全写成功）*/
#define G_SNAP_AORC     12u   /* g_ao_rc */
#define G_SNAP_ESWROK   13u   /* g_es_wr_ok    I2C 写成功 */
#define G_SNAP_ESWRNG   14u   /* g_es_wr_ng    I2C 写失败 */
#define G_SNAP_ESRDOK   15u   /* g_es_rd_ok */
#define G_SNAP_ESRDNG   16u   /* g_es_rd_ng */
#define G_SNAP_ESRETRY  17u   /* g_es_retry_ok  首试失败、重试成功 */
#define G_SNAP_ESFAIL   18u   /* g_es_fail      重试 3 次仍失败（应恒 0）*/
#define G_SNAP_ESFIX    19u   /* g_es_fix_wr    回读发现不对、整段补写 */
#define G_SNAP_ESRB     20u   /* g_es_readback  回读体检执行次数 */
#define G_SNAP_ESPWR    21u   /* g_ao_wake_retry 唤醒 DAC 整段重试次数 */
#define G_SNAP_ESREG32N 22u   /* g_es_reg32_n   REG32 写入次数 */
#define G_SNAP_ESREG32W 23u   /* g_es_reg32_wr  REG32 最后写入值（0 = 静音！）*/
#define G_SNAP_ESRBVOL  24u   /* g_es_rb_vol    REG32 回读值 */
#define G_SNAP_ESRBPWR  25u   /* g_es_rb_pwr    REG12 回读（bit1=power-down）*/
#define G_SNAP_ESRB0E   26u   /* g_es_rb_dac0e  REG0E 回读 */
#define G_SNAP_ESWNCUR  27u   /* g_es_ng_reg    最近一次写失败的寄存器号 */
#define G_SNAP_ESWNS    28u   /* g_es_ng_st     该次 HAL 状态（1=NACK 2=BUSY 3=TMO）*/
#define G_SNAP_ESNAK    29u   /* g_es_ng_nack */
#define G_SNAP_ESBUSY   30u   /* g_es_ng_busy */
#define G_SNAP_ESTMO    31u   /* g_es_ng_tmo */
#define G_SNAP_ESNGHEAD 32u   /* g_es_ng_head  失败环累计（= wr_ng + rd_ng 不变量）*/
#define G_SNAP_ESNGIDR  33u   /* g_es_ng_idr   失败瞬间 PB6/PB7 电平（0xC0 = 总线空闲）*/
#define G_SNAP_VOLUI    34u   /* g_audio_volume UI 音量档位 */
#define G_SNAP_DMARSH   35u   /* g_ao_dma_half */
#define G_SNAP_DMARCP   36u   /* g_ao_dma_cplt */
#define G_SNAP_DMARERR  37u   /* g_ao_dma_err（应恒 0）*/
#define G_SNAP_FRAMES    38u  /* g_ao_frames（÷48828 ≈ 已出声秒数）*/
#define G_SNAP_MAGIC    39u   /* 魔数 G_WAV_SNAP_MAGIC（最后写）*/
#define G_SNAP_ESPRETRY 40u   /* g_es_probe_retry  probe 首试没探到、延时后探到 */
#define G_WAV_SNAP_N     41u  /* 表长（含 MAGIC）—— 主机按此字节数批量读 */

extern volatile uint32_t g_wav_snap_req;         /* 写相位码触发锁存（板上用后清 0）*/
extern volatile uint32_t g_wav_snap[G_WAV_SNAP_N];
extern volatile uint32_t g_wav_snap_delay;       /* 动作后延迟多少拍再锁存（默认 40 ≈ 0.64 s）*/

/* ---- board 层（phone_shell 的音乐 app 用，声明见 phone_shell_board.h）---- */
int         BoardMusic_Scan(void);
int         BoardMusic_Count(void);
int         BoardMusic_Name(int i, char *out, int n);
int         BoardMusic_Size(int i, uint32_t *size);
int         BoardMusic_Play(int i);
void        BoardMusic_Stop(void);
int         BoardMusic_Playing(void);
int         BoardMusic_LastRc(void);   /* 上一次动作的返回码（UI 直接显示）*/
uint32_t    BoardMusic_ElapsedMs(void);
uint32_t    BoardMusic_TotalMs(void);
int         BoardMusic_Seek(uint32_t ms);  /* 跳播到指定毫秒（拖动进度条用）*/
int         BoardMusic_Pause(void);        /* 暂停（保留文件/位置，可续播）*/
int         BoardMusic_Resume(void);       /* 从暂停处续播 */
int         BoardMusic_Paused(void);       /* 1 = 有暂停会话待续播 */
int         BoardMusic_CurrentName(char *out, int n); /* 当前在播/暂停曲目名（停止返回 <0）*/

#endif /* WAV_PLAY_H */
