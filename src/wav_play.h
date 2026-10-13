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
