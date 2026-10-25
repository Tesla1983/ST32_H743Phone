#ifndef AUDIO_REC_H
#define AUDIO_REC_H

/* ============================================================================
 * 录音链路（MIC → ES8311 ADC → I2S2 主收 → DMA → RAM → FatFs → WAV）
 * 2026-10-16 补丁18
 *
 * 【整条链路与各段的负责人】
 *   ① 麦克风采集    ES8311 内部：模拟 PGA(REG16) → ADC → 芯片 ASDOUT
 *   ② 电路          **模块 DOUT 脚 → 排针 P2-9（PC2 = I2S2_SDI，AF5）**
 *                   ⚠ 本工程此前只放音（模块 DIN ← STM32 PB15），**DOUT 一直是空着的**；
 *                     不接这根线，后面 ③④⑤ 全都会"跑通但录到的是静默/悬空电平"。
 *   ③ 数字采集      src/audio_out.c：audio_out_capture_*（I2S2 主收 + DMA1_Stream3）
 *   ④ codec 配置    src/es8311.c：es8311_adc_start()（ADC 通路开 / DAC 通路关 / REG17=0xBF）
 *   ⑤ 落盘          本文件：攒块 → f_write → WAV 头（先占位、停止时回填真实长度）
 *
 * 【为什么录音与放音不能同时】I2S2 的 I2SCFG 是"主发/主收"二选一（RM0433 §50.7.6）。
 *   所以开始录音会先要求放音停下；放音走异步淡出，因此 BoardRec_Start() 可能返回
 *   **"准备中"**（下一拍再调即可），而不是把主循环阻塞住。
 *
 * 【格式】16 bit / 48 828 Hz / 立体声（默认）。48 828 是 I2S 的实际 fs（由 MCLK 决定），
 *   不是 48 000 —— WAV 头里据实写 48828，播放端才会用对速率（否则音调会偏 1.7%）。 */

#include <stdint.h>

#define REC_RATE_HZ   48828u    /* I2S 实际采样率（MCLK 12.5 MHz ÷ 256）*/
#define REC_DIR       "/YMGUI/RECORD"

/* ---- 诊断量（全部非 static，SWD 直读；tools/rec_check.py 靠它们判成败）---- */
extern volatile uint32_t g_rec_state;      /* 0=空闲 1=录音中 2=收尾中 */
extern volatile int32_t  g_rec_rc;         /* 最近一次动作的返回码（0=成功）*/
extern volatile uint32_t g_rec_frames;     /* 已写入文件的帧数 */
extern volatile uint32_t g_rec_bytes;      /* 已写入文件的数据字节数 */
extern volatile uint32_t g_rec_poll;       /* poll 调用次数（心跳；不涨=没在跑）*/
extern volatile uint32_t g_rec_werr;       /* f_write 失败次数（>0 = 卡/文件系统有问题）*/
extern volatile uint32_t g_rec_write_ms_max;/* 单次写卡最长耗时(ms) —— 卡顿的直接证据 */
extern volatile uint32_t g_rec_bigblk_ms;  /* 单次填入 chunk 的最长耗时(ms) */
extern volatile uint32_t g_rec_pending;    /* 1 = 正在等放音淡出结束（准备中）*/
extern volatile uint32_t g_rec_test;       /* 排障钩子：1=起录 2=停录（主循环消费一次）*/
extern volatile uint32_t g_rec_mono;       /* 0=立体声(默认) 1=只存左声道（文件减半）*/
extern volatile uint32_t g_rec_mic_gain;   /* REG16 模拟 PGA 增益（默认 0x24，与官方一致）*/
extern volatile uint32_t g_rec_max_ms;     /* 最长录音时长（默认 300000 = 5 分钟）*/
extern volatile uint32_t g_rec_files;      /* 目录里已有的录音文件数（扫描结果）*/
extern volatile uint32_t g_rec_peak_max;   /* 本次录音见过的最大峰值（0..32767）*/
extern volatile uint32_t g_rec_level;      /* ★ 电平条输入值 0..100（dB 刻度，= BoardRec_Level()）*/
extern volatile uint32_t g_rec_gain_req;   /* 写 1 ⇒ 录音中直接改 REG16 为 g_rec_gain_val（扫增益用）*/
extern volatile uint32_t g_rec_gain_val;   /* 要写入 REG16 的值（本板：越小增益越大）*/
/* ★ 补丁27：录音目录扫描的过程量（区分"没扫到"与"扫到了没显示"，SWD 直接读）*/
extern volatile uint32_t g_rec_scan_req;      /* 写 1 ⇒ 主循环跑一次真实扫描（跑完清 0）*/
extern volatile uint32_t g_rec_scan_mount_rc; /* fatfs_ensure_mounted() 返回（0 = 卷可用）*/
extern volatile uint32_t g_rec_scan_rc;       /* 0 = f_opendir 成功，否则 0xFFFFFFFF */
extern volatile uint32_t g_rec_scan_entries;  /* 目录条目数（含子目录）*/
extern volatile uint32_t g_rec_scan_wav;      /* 认成 .WAV 的条目数 */
extern volatile uint32_t g_rec_scan_bad;      /* WAV 头解析失败的条目数 */
extern volatile uint32_t g_rec_scan_n;        /* 入表（卡片）数量 */
extern volatile char     g_rec_scan_name[6][20]; /* 过滤前的头 6 个目录项名字 */
extern volatile uint32_t g_rec_scan_attrib[6];   /* 同序 FAT 属性 */
void audio_rec_scan_poll(void);               /* 主循环里调用：消费 g_rec_scan_req */
extern volatile uint32_t g_rec_mic_thresh; /* 判"有麦克风信号"的峰值阈值（默认 128）*/
/* ★ 停止时从**卡上文件**回读的 WAV 头 + 自检结果 —— "真的录成了一个能播的 WAV"的板上证据 */
extern volatile uint32_t g_rec_hdr[11];    /* 前 44 字节（32 位小端；[0] 应为 0x46464952 = "RIFF"）*/
extern volatile uint32_t g_rec_hdr_ok;     /* 1 = 头自检全部通过 */
extern volatile uint32_t g_rec_hdr_bad;    /* 失败项位图（bit1=RIFF bit2=WAVE bit3=fmt
                                            * bit4=PCM bit5=声道 bit6=采样率 bit7=位深
                                            * bit8=data 长度 bit9=RIFF 长度 bit10=文件实长）*/
extern volatile uint32_t g_rec_fsize;      /* 卡上文件真实字节数 */
/* ★ 补丁18b：试听（放出最近录的那一段） —— 见 audio_rec.c 里的说明 */
extern volatile uint32_t g_rec_play_req;   /* 排障钩子：1=试听最新录音 2=停（主循环消费一次）*/
extern volatile int32_t  g_rec_play_rc;    /* 最近一次试听的返回码（同 wav_play_start）*/
extern volatile uint32_t g_rec_play_n;     /* 试听成功起播的次数（单调）*/
/* ★ 补丁18c：ES8311 那一步的**可区分**诊断（旧实现把所有失败压成 g_rec_rc=-2）*/
extern volatile uint32_t g_rec_adc_retry;  /* ADC 配置整段重试的累计次数 */
extern volatile int32_t  g_rec_es_rc;      /* ADC 配置原始码：0 / -1 回读不符 / -3 无器件 / -4 I2C 起不来 */

/* ---- 引擎（供 main.c 每拍调用与排障钩子使用）---- */
int         audio_rec_start(void);   /* 0=已开始；REC_AGAIN(2)=准备中；<0=失败 */
void        audio_rec_stop(void);
int         audio_rec_poll(void);    /* ★ 每拍调：取数据 + 写卡 + 消费 g_rec_test/g_rec_play_req */

/* ★ 补丁18b：试听 —— 播放目录里编号最大的那条录音（= 最近一段）。
 *   返回码同 wav_play_start（0=成功 / -2 卡 -3 打不开 / -22 音频输出起不来 …）；
 *   另加 -1 = 目录里没有录音且本会话也没录过。若正在录音会**先正常收尾**（回填 WAV 头）。 */
int         audio_rec_play_last(void);
void        audio_rec_play_stop(void);
int         audio_rec_playing(void); /* 1 = 正在试听出声（暂停/停止都算 0）*/

#define REC_AGAIN 2

/* ---- Board 桥接层（给 phone_shell 的录音机 App 用；实现在本文件）----
 * 与 BoardMusic_* / BoardPic_* 同一套分层：UI 只拿"能直接画的字符串"。 */
int         BoardRec_Start(void);       /* 0=已开始；2=准备中；<0=失败 */
int         BoardRec_Stop(void);
int         BoardRec_Active(void);      /* 1=录音中 */
int         BoardRec_Pending(void);     /* 1=正在准备（等放音停）*/
uint32_t    BoardRec_ElapsedMs(void);   /* 已录毫秒（按帧数折算，不依赖 tick）*/
uint32_t    BoardRec_Bytes(void);       /* 已写入的数据字节数 */
const char* BoardRec_Name(void);        /* 落地文件名（不含目录）；无 = "" */
const char* BoardRec_StatusText(void);  /* 可直接画的一行中文 */
int         BoardRec_LastRc(void);
int         BoardRec_Scan(void);        /* 扫一次 /YMGUI/RECORD，返回文件数；<0=目录打不开 */
int         BoardRec_Level(void);       /* 当前输入电平 0..100（取 L/R 采样峰值较大者）*/
int         BoardRec_MicOk(void);       /* 1 = 本次录音采到过明显信号；0 = 一直很小 */
uint32_t    BoardRec_PeakRaw(void);     /* 原始峰值 0..32767（排障/验收用）*/
/* ★ 补丁18b：试听（把最近一段录音放出来听，不用拔卡）。
 *   PlayLast() 的返回码可直接映射文案：0 成功 / -1 没有录音 / -2 卡没挂载 /
 *   -3 打不开文件 / -22 音频输出起不来；其余 <0 = WAV 头有问题。 */
int         BoardRec_PlayLast(void);
void        BoardRec_StopPlay(void);
int         BoardRec_Playing(void);
int         BoardRec_PlayRc(void);
int         BoardRec_PlayName(char* out, int n);
/* ★ 补丁18c：给 UI 的"失败原因"细分码（-1 回读不符 / -3 无器件 / -4 I2C 起不来 / 0 正常）*/
int         BoardRec_EsRc(void);
int         BoardRec_AdcRetry(void);

/* ---- 文件管理器录音卡片（2026-10-16 补丁19）----
 * 在 BoardRec_* 基础上再开一组"按索引枚举 / 播放 / 删除 / 重命名 / 分享"的接口，
 * 让文件管理器把 /YMGUI/RECORD 里的录音当成卡片列出。
 * ⚠ 与 BoardRec_PlayLast() 复用**同一个** wav_play 会话（一次只能放一个）。 */
int         BoardRec_Count(void);                  /* 枚举到的录音条数 */
int         BoardRec_ListName(int i, char* out, int n);  /* 第 i 条文件名（不含目录）*/
int         BoardRec_ListPath(int i, char* out, int n);  /* 第 i 条完整路径（补丁29）*/
int         BoardRec_ListSize(int i, uint32_t* size);    /* 字节数 */
uint32_t    BoardRec_DurationMs(int i);             /* 时长（按 WAV 头 data 区折算；损坏=0）*/
int         BoardRec_DateText(int i, char* out, int n);  /* "YYYY-MM-DD HH:MM"；无日期="无日期" */
int         BoardRec_IsValid(int i);                /* 1 = WAV 头可解析（可播放）；0 = 损坏 */
int         BoardRec_PlayIndex(void);               /* 当前在放/暂停的卡片下标；-1 = 无 */
int         BoardRec_PlayList(int i);               /* 播放第 i 条；返回码同 wav_play_start */
void        BoardRec_PlayStop(void);                /* 停当前播放 */
int         BoardRec_PlayPause(void);
int         BoardRec_PlayResume(void);
int         BoardRec_PlayPaused(void);
uint32_t    BoardRec_PlayElapsedMs(void);
uint32_t    BoardRec_PlayTotalMs(void);
int         BoardRec_PlaySeek(uint32_t ms);
int         BoardRec_Delete(int i, int* next_valid);     /* 删第 i 条；成功写回 next_valid(-1=空) */
int         BoardRec_Rename(int i, const char* newname);  /* 重命名；返回新下标 / <0 失败 */
int         BoardRec_Share(int i);                      /* 复制到 /YMGUI/SHARE；0 = 成功 */

#endif /* AUDIO_REC_H */
