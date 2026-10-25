/* ============================================================================
 * 录音引擎：MIC → ES8311(ADC) → I2S2 主收 → DMA → RAM → FatFs → WAV（补丁18）
 *
 * 链路分段与各段的负责人见 audio_rec.h 的头注释。本文件只负责：
 *   · 起/停的**状态机**（把"等放音淡出"变成可重入的"准备中"，不阻塞主循环）
 *   · 从 audio_out_capture_* 取数据、攒成块、写进 TF 卡
 *   · WAV 头（先写占位、停止时回填真实长度）
 *   · 给录音机 App 的 BoardRec_* 桥接
 *
 * 【为什么"先写占位头、停止时回填"】录音时长事先不知道，而 WAV 头里必须有两个长度字段。
 *   如果不回填，生成的是"头里写 0 长"的废文件 —— 用播放器打开会显示 0 秒。
 *   回填用 f_lseek(0) 重写 44 字节，代价极小。
 *
 * 【掉电/复位安全】WAV 头回填发生在 stop 里；若此时断电，文件会是"头里长度不对但数据在"，
 *   可被当成裸 PCM 抢救 —— 比"完全没头"好。真正的可靠性做法是分片写 + 定时回填，
 *   本版未做（见 DEVELOPMENT_LOG 遗留）。 */

#include "audio_rec.h"
#include "audio_out.h"
#include "es8311.h"
#include "fatfs_port.h"
#include "wav_play.h"      /* 试听：复用现成的 WAV 播放链路（wav_play_start/stop/busy）*/

#include "ff.h"
#include "stm32h7xx_hal.h"

#include <string.h>
#include <stdio.h>

/* 每次从采集环取走的半字数：4096 半字 = 8 KB = 2048 帧 ≈ 42 ms。
 * 与音乐播放的半缓冲同量级（那边也是 42 ms），写卡粒度与读卡粒度一致，行为好对照。 */
#define REC_CHUNK_HW 4096u

/* ★ 补丁18c：「ES8311 ADC 通路」这一整段的重试次数与间隔（2026-10-16）。
 * 【为什么必须有】本板 I2C 首试 NACK ≈30% 且**成簇**（补丁15d 实测一次涨 2~5 笔；根因=
 *   板上没有外部上拉、只有 MCU 内部 ~40 kΩ，100 kHz 下是电气边际），而这一段要写二十来笔：
 *   "整整一段一笔都不丢"并非必然。**放音路径**（audio_out_start → es8311_dac_start_muted）
 *   早就用"失败就整段重来"扛过去了，**录音路径此前没有** ⇒ 一次偶发丢写就会让用户看到
 *   "录音起不来"（2026-10-16 用户报障即此）。
 * 【为什么是 6 次、而且必须**隔拍**】录音起播是用户主动触发的一次性动作，多试几次的代价
 *   只是几百毫秒；而失败一次就是"点了没反应"。**必须隔拍（≈17 ms/次）而不是背靠背**：
 *   实测第 3 轮出现 `g_es_fail Δ=12`（连续 12 笔真丢写），而当时 3 次背靠背（间隔 2 ms）
 *   全部落空 —— 失败是**成簇**的，且 es8311 还有 250 ms 熔断冷却，2 ms 的间隔跨不过它。
 *   每次尝试都会重跑 es8311_adc_start() 开头的 es8311_bus_ready()（含总线电平自检 +
 *   必要时 bus_clear + 重新探址），所以重试顺带把"从机把 SDA 钳住"这类坏状态再收敛一次。 */
#define REC_ADC_TRIES    6u

/* ---- 诊断量 ---- */
volatile uint32_t g_rec_state      = 0u;
volatile int32_t  g_rec_rc         = 0;
volatile uint32_t g_rec_frames     = 0u;
volatile uint32_t g_rec_bytes      = 0u;
volatile uint32_t g_rec_poll       = 0u;
volatile uint32_t g_rec_werr       = 0u;
volatile uint32_t g_rec_write_ms_max = 0u;
volatile uint32_t g_rec_bigblk_ms  = 0u;
volatile uint32_t g_rec_pending    = 0u;
volatile uint32_t g_rec_test       = 0u;
volatile uint32_t g_rec_mono       = 0u;
/* ★ 模拟 PGA 增益(REG16)：**保留官方 0x24** —— 这是 2026-10-17 现场扫出来的结论。
 * 【扫法】录音**中途**用 g_rec_gain_req/g_rec_gain_val 逐档改 REG16，
 *   同一段连续语音、同一次录音内对比（峰值取自 g_rec_peak_max，改档即清零）。
 * 【实测（用户在旁持续说话）】
 *   0x24 → 峰值 26839（-1.7 dBFS）电平 49%，**不削顶** ← 最佳
 *   0x18 → 峰值   977（很低）
 *   0x0C → 峰值     0（完全没信号）
 *   0x06 → 峰值 32599，**削顶**
 * ⚠⚠ 结论：**本板 REG16 不是单调的**，也不是简单的"值越小增益越大"。
 *   早期一轮（逐档停录/起录、且当时测得的多是毛刺）曾得出"越小越大、0x06 最好"，
 *   用同语音对比法**证伪**了 —— 0x0C/0x18 反而几乎没信号，0x06 直接削顶。
 *   ⇒ 别拿"单调增益"的直觉去调这个值；要调就用本文件说的同语音扫法重扫。
 * ⚠ 26839 已经偏热（余量约 1.7 dB）：大声说话仍有削顶风险，但可选的几档里 0x24 最好。 */
volatile uint32_t g_rec_mic_gain   = 0x24u;
volatile uint32_t g_rec_max_ms     = 300000u;  /* 5 分钟上限（防止忘关把卡写满）*/
volatile uint32_t g_rec_files      = 0u;
/* ★ 麦克风"到底有没有信号"的判据量。为什么要它：DOUT 没接线时链路**照常跑通**
 *   （DMA 搬、文件生成、进度条走），唯一区别是采到的是悬空电平、峰值极小。
 *   没有这个量，UI 就只能显示"录音中"而实际是空的 —— 那正是本项目最忌讳的假数据。 */
volatile uint32_t g_rec_peak_max   = 0u;       /* 本次录音见过的最大峰值（0..32767）*/
volatile uint32_t g_rec_level      = 0u;       /* ★ 电平条的输入值 0..100（dB 刻度；= BoardRec_Level() 的返回值）*/
volatile uint32_t g_rec_gain_req   = 0u;       /* 写 1 ⇒ 录音中直接把 REG16 改成 g_rec_gain_val（扫增益用）*/
volatile uint32_t g_rec_gain_val   = 0x24u;    /* 要写入 REG16 的值（扫增益用，默认与 mic_gain 同步）*/

/* ★ 补丁27：录音目录扫描的过程量 + SWD 钩子（见 audio_rec_scan_poll）。
 *   为什么需要：文件管理器「录音」分类列不出卡上的 REC_xxxx.WAV，可能有两种完全不同的原因
 *     ① 扫描本身没扫到（卷没挂载 / 目录打不开）② 扫到了但 UI 没画出来。
 *   这两种靠看屏幕分不清，按铁律必须**直接计数**。 */
volatile uint32_t g_rec_scan_req      = 0u;    /* 写 1 ⇒ 主循环跑一次真实扫描，跑完自动清 0 */
volatile uint32_t g_rec_scan_mount_rc = 0u;    /* fatfs_ensure_mounted() 的返回（0 = 卷可用）*/
volatile uint32_t g_rec_scan_rc       = 0u;    /* 0 = f_opendir 成功；0xFFFFFFFF = 打开失败 */
volatile uint32_t g_rec_scan_entries  = 0u;    /* 目录里遍历到的条目数（含子目录）*/
volatile uint32_t g_rec_scan_wav      = 0u;    /* 认成 .WAV 的条目数 */
volatile uint32_t g_rec_scan_bad      = 0u;    /* WAV 头解析失败（显示"损坏"）的条目数 */
volatile uint32_t g_rec_scan_n        = 0u;    /* 最终入表（= 卡片）数量 */
volatile char     g_rec_scan_name[6][20];      /* 过滤前的头 6 个目录项名字（原样）*/
volatile uint32_t g_rec_scan_attrib[6];        /* 与上面同序的 FAT 属性（0x10=目录）*/

/* 前置声明：电平换算要在 audio_rec_tick() 里用（定义在文件后面的 BoardRec_Level 附近）。
 * ⚠ 为什么把电平算在**引擎**里而不是只等在 App 调 BoardRec_Level() 时才算：
 *   App 的 tick 只在 state.active 时才会问，而 SWD 钩子（g_rec_test=1）起的录音
 *   根本不过 App ⇒ 那样电平量恒为 0、无法验收（铁律10：要看得到才叫修好）。 */
static int level_from_peak_db(uint32_t pk);
volatile uint32_t g_rec_mic_thresh = 128u;     /* 判"有麦克风信号"的峰值阈值（≈ -48 dBFS）*/
/* ★ 停止时把**卡上文件**的 WAV 头回读进 RAM 并自检 —— 让"到底写没写成一个能播的 WAV"
 *   变成可验证的事实，而不是"我写了所以应该对"。主机读不到 TF 卡，只能这样把它带出来。
 *   g_rec_hdr[0..10] = 卡上前 44 字节（按 32 位小端存，"RIFF" 读出来就是 0x46464952）。*/
volatile uint32_t g_rec_hdr[11];
volatile uint32_t g_rec_hdr_ok  = 0u;          /* 1 = 头自检通过 */
volatile uint32_t g_rec_fsize   = 0u;          /* 卡上文件的真实字节数（f_size）*/
volatile uint32_t g_rec_hdr_bad = 0u;          /* 自检失败时：哪一项不符（位图，便于定位）*/
/* ★ 补丁18b：**试听**（把刚录的那段用同一块板、同一只喇叭放出来）。
 *   为什么要它：主机读不到 TF 卡，而"录到的到底是不是声音、有多大、有没有爆音"
 *   只能靠**耳朵**在这块板上判 —— 拔卡拷到电脑再听是最慢的一条路。
 *   实现上不做新播放器：直接复用 wav_play（它本来就吃任意路径的 16 bit PCM WAV），
 *   所以播放行为（音量 ramp、起播安全栅、DMA/TIM6 路径）与音乐 App 完全一致。 */
volatile uint32_t g_rec_play_req = 0u;         /* 排障钩子：1=试听最新录音 2=停（主循环消费一次）*/
volatile int32_t  g_rec_play_rc  = 0;          /* 最近一次试听的返回码（取 wav_play_start 的口径）*/
volatile uint32_t g_rec_play_n   = 0u;         /* 试听成功起播的次数（单调）*/
/* ★ 补丁18c：把"ES8311 那一步为什么失败"变成能区分的数字。
 *   旧实现把 es8311_adc_start() 的**所有**失败都压成 g_rec_rc=-2 ⇒ App 只能说
 *   "ES8311 状态异常"，用户无从判断该查线还是该重试。 */
volatile uint32_t g_rec_adc_retry = 0u;        /* ADC 配置**整段重试**的累计次数 */
volatile int32_t  g_rec_es_rc     = 0;         /* ADC 配置的**原始**返回码：
                                                *   0 成功 / -1 回读不符（I2C 丢写）/
                                                *  -3 总线上没有 ES8311（没接/线松）/
                                                *  -4 I2C 总线本身起不来 */

/* ---- 内部状态 ---- */
/* ★★ 这个段属性是**必须的**，不是优化（2026-10-16 踩过并记档）：
 *   DTCM 的 .bss 与 **heap0 抢同一块内存**（`_heap0_start = _sbrk_end`，
 *   `_heap0_end = _estack − 32 KB`）。把这块 8 KB 暂存放进 DTCM 会让 heap0
 *   从 ~35.6 KB 掉到 26.6 KB，而 YMGUI 建整个界面需要 >26.6 KB ⇒
 *   `g_h0` 记到 `used == peak == total`、free_head=NULL ⇒ build_ui() 拿到 NULL 写穿
 *   ⇒ CFSR=0x400(IMPRECISE bus error)/HFSR=FORCED ⇒ **开机就 HardFault**。
 *   ⇒ 一律用 REC_BUF_ATTR 指到 SRAM2（见 linker/stm32h743vit6.ld 的 .bss_sram2）。 */
#define REC_BUF_ATTR __attribute__((section(".bss_sram2"), aligned(8)))
/* ⚠ 本段是 NOLOAD（不占 FLASH、不随固件加载）⇒ 只能放**零初始化**的对象，
 *   绝对不能带非零初值（带了链接器不会报错但初值会丢）。带初值的 s_status 因此留在 DTCM。
 * ⚠ 下面这几个**一起移出 DTCM**：DTCM 的每一字节都在吃 heap0 的余量。
 *   实测（2026-10-16）：把 s_chunk 移走后 heap0 仍只剩 648 B（峰值 34 248/34 752），
 *   本模块的静态量合计 ~1.2 KB ⇒ 全部挪到 SRAM2 才能把余量还原成补丁18 之前的水平。*/
static int16_t  REC_BUF_ATTR s_chunk[REC_CHUNK_HW];   /* CPU 专用暂存（不进 DTCM！）*/
static FIL      REC_BUF_ATTR s_f;                     /* FatFs 文件对象（含 512 B 私有窗口）*/
static char     REC_BUF_ATTR s_path[48];
static char     REC_BUF_ATTR s_name[20];
static char     s_status[64] = "未开始";              /* 带初值 ⇒ 必须在 DTCM */
static uint32_t s_start_tick = 0u;
static uint32_t s_data_bytes = 0u;
/* ★ 补丁18c：本次动作还剩几次"ES8311 ADC 配置"尝试（0 = 额度未初始化/已用完）。
 *   用 static 而不是局部变量，是为了让重试能**跨调用**（隔拍）而不是在函数里忙等。 */
static uint32_t s_adc_left  = 0u;
/* ★ 补丁18c：1 = 这次启动是 **SWD 钩子**发起的（g_rec_test=1）。
 *   为什么要它：App 路径有 app_tick 每拍重试，而钩子是一次性消费 ——
 *   返回 REC_AGAIN 之后就没人再调 audio_rec_start() 了，隔拍重试会断链。
 *   有它，主循环就能替钩子把重试驱动完（判据用 g_rec_pending，重试自然结束）。 */
static uint8_t  s_auto_retry = 0u;
static uint8_t  s_opened   = 0u;

/* ---- 文件管理器录音卡片：目录枚举表（2026-10-16 补丁19）----
 * 与 rec_scan_max_index() 不同：后者只为「试听最近一段」找最大编号；这里把
 * /YMGUI/RECORD 里**所有 .WAV** 枚举成一张表，供文件管理器按卡片列出
 * （名称 / 大小 / 时长 / 录制日期 / WAV 头有效性）。
 * ⚠ 大数组放 SRAM2（REC_BUF_ATTR，NOLOAD 零初始化）：7 KB 的表若进 DTCM 的 .bss
 *   会直接吃光 heap0 余量（当前仅 ~1.2 KB），开机即 HardFault —— 与 s_chunk 同款铁律。 */
#define REC_LIST_MAX 64
typedef struct
{
	char     name[32];      /* 含扩展名的文件名（REC_0001.WAV 或重命名后的名字）*/
	char     path[48];
	uint32_t size;          /* 文件总字节 */
	uint32_t dur_ms;        /* 时长（按 WAV 头 data 区折算；损坏/无效 = 0）*/
	uint16_t fdate;         /* FATFs fdate（年/月/日，详见 rec_date_text）*/
	uint16_t ftime;         /* FATFs ftime（时/分/秒）*/
	uint8_t  valid;         /* 1 = WAV 头解析成功（可播放）*/
	uint8_t  bad;           /* 损坏原因位图（valid==0 时有效；便于 UI 说清哪坏）*/
} rec_list_entry_t;
static rec_list_entry_t REC_BUF_ATTR s_rec_list[REC_LIST_MAX];
static int  s_rec_list_n   = 0;
static int  s_rec_scan_rc  = 0;
static int  s_rec_play_idx = -1;   /* 文件管理器当前在放/暂停的卡片下标（复用 wav_play 会话）*/

/* ====================== 纯函数：WAV 头 ======================
 * 只依赖入参、无副作用 ⇒ 可单独复算与验证（本项目要求"换算写成纯函数"）。
 * 布局 = canonical 44 字节 PCM。 */
static void wav_make_header(uint8_t *h, uint32_t data_bytes, uint32_t rate, uint16_t ch)
{
    uint32_t byte_rate = rate * (uint32_t)ch * 2u;
    uint32_t riff      = 36u + data_bytes;

    h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
    h[4]  = (uint8_t)(riff);         h[5]  = (uint8_t)(riff >> 8);
    h[6]  = (uint8_t)(riff >> 16);   h[7]  = (uint8_t)(riff >> 24);
    h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
    h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
    h[16] = 16u; h[17] = 0u; h[18] = 0u; h[19] = 0u;      /* fmt chunk 长度 = 16 */
    h[20] = 1u;  h[21] = 0u;                              /* PCM */
    h[22] = (uint8_t)ch; h[23] = 0u;
    h[24] = (uint8_t)rate;       h[25] = (uint8_t)(rate >> 8);
    h[26] = (uint8_t)(rate >> 16); h[27] = (uint8_t)(rate >> 24);
    h[28] = (uint8_t)byte_rate;  h[29] = (uint8_t)(byte_rate >> 8);
    h[30] = (uint8_t)(byte_rate >> 16); h[31] = (uint8_t)(byte_rate >> 24);
    h[32] = (uint8_t)(ch * 2u);  h[33] = 0u;              /* block align */
    h[34] = 16u; h[35] = 0u;                              /* 位深 16 */
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    h[40] = (uint8_t)data_bytes; h[41] = (uint8_t)(data_bytes >> 8);
    h[42] = (uint8_t)(data_bytes >> 16); h[43] = (uint8_t)(data_bytes >> 24);
}

/* 目录里已有 REC_xxxx.WAV 的最大序号（不能用 atoi：只为这一处引 stdlib 不值当）。*/
static uint32_t rec_scan_max_index(void)
{
    DIR     d;
    FILINFO fi;
    uint32_t maxv = 0u;
    uint32_t cnt  = 0u;

    if (f_opendir(&d, REC_DIR) != FR_OK)
    {
        return 0u;
    }
    for (;;)
    {
        if (f_readdir(&d, &fi) != FR_OK) { break; }
        if (fi.fname[0] == 0)            { break; }
        if ((fi.fattrib & AM_DIR) != 0u) { continue; }
        cnt++;
        if ((fi.fname[0] == 'R') && (fi.fname[1] == 'E') && (fi.fname[2] == 'C') &&
            (fi.fname[3] == '_'))
        {
            uint32_t v = 0u;
            const char *p = &fi.fname[4];

            while ((*p >= '0') && (*p <= '9'))
            {
                v = v * 10u + (uint32_t)(*p - '0');
                p++;
            }
            if (v > maxv) { maxv = v; }
        }
    }
    (void)f_closedir(&d);
    g_rec_files = cnt;
    return maxv;
}

/* 文件管理器：把目录里**所有 .WAV** 枚举成 s_rec_list[]，并解析每份的 WAV 头。
 * 这是「列出录音卡片」唯一的数据来源；与 rec_scan_max_index() 互不干扰
 * （后者只给「试听最近一段」用，仍按 REC_ 前缀取最大编号）。 */
static int rec_list_build(void)
{
	DIR     d;
	FILINFO fi;
	int     n       = 0;
	int     entries = 0;
	int     n_wav   = 0;
	int     n_bad   = 0;

	s_rec_scan_rc = 0;

	/* ★★ 补丁27：必须先确保卷已挂载。★★
	 *   直接 f_opendir 在"卷还没工作区"时返回 FR_NOT_ENABLED(12) ⇒ 本函数
	 *   返回 -1 ⇒ 文件管理器把录音分类画成"暂无录音文件"。
	 *   这正是 img_store.c:1805 在 2026-10-09 踩过并写成注释的同一个坑
	 *   （照片/笔记分类因此一直正常，而录音分类**独独**没有这一步）。
	 *   触发条件很日常：开机后**没先开过音乐/相册**，直接进「文件管理 → 录音」
	 *   —— 此时 FATFS 从未被挂载，卡上明明有 REC_xxxx.WAV 也列不出来。 */
	{
		int mrc = fatfs_ensure_mounted();
		g_rec_scan_mount_rc = (uint32_t)mrc;
		if (mrc != 0)
		{
			s_rec_scan_rc = -2;
			s_rec_list_n  = 0;
			g_rec_scan_rc = 0xFFFFFFFEu;
			g_rec_scan_n  = 0u;
			return -2;
		}
	}

	/* 目录不存在时也建一下（与 img_scan_mkdir 的父目录规则一致：f_mkdir 不级联）。
	 * 建不出来不算错 —— 后面 f_opendir 会如实返回失败。 */
	{
		char parent[16] = "/YMGUI";
		(void)f_mkdir(parent);
		(void)f_mkdir(REC_DIR);
	}

	if (f_opendir(&d, REC_DIR) != FR_OK)
	{
		s_rec_scan_rc = -1;
		s_rec_list_n  = 0;
		g_rec_scan_rc = 0xFFFFFFFFu;      /* f_opendir 失败（FRESULT 见 s_rec_scan_rc）*/
		g_rec_scan_n  = 0u;
		return -1;
	}
	g_rec_scan_rc = 0u;
	for (;;)
	{
		if (f_readdir(&d, &fi) != FR_OK) { break; }
		if (fi.fname[0] == 0)            { break; }
		entries++;
		if ((fi.fattrib & AM_DIR) != 0u) { continue; }

		/* ★ 补丁27：把**过滤之前**的前 6 个名字原样带出来。
		 *   本轮实测：目录 75 个条目、f_opendir 成功，却一个都没认成 .WAV
		 *   ⇒ 必须看清真实文件名/属性，而不是继续猜。 */
		if (entries <= 6)
		{
			(void)snprintf((char*)g_rec_scan_name[entries - 1],
			               sizeof(g_rec_scan_name[0]), "%s", fi.fname);
			g_rec_scan_attrib[entries - 1] = (uint32_t)fi.fattrib;
		}

		/* 只收 .WAV（不区分大小写）；其余文件（临时/碎片）不进卡片列表。
		 *
		 * ★★ 补丁27：修掉"卡上所有录音都不显示"的真根因 ★★
		 *   旧代码取的是**末 4 个字符**，却拿它去比 "WAVE"：
		 *       名字 REC_0001.WAV 的末 4 字符 = '.', 'W', 'A', 'V'
		 *       ⇒ e3='.' 永远 ≠ 'W'，e0='V' 永远 ≠ 'E' ⇒ is_wav 恒为 0
		 *   ⇒ 目录里每一个 .WAV 都被 continue 掉，文件管理器「录音」分类永远是
		 *     "暂无录音文件"。只有叫 *.wave 的文件才碰巧通过（本板不会产生）。
		 *   板上实测证据（补丁27 的 SWD 扫描钩子）：
		 *       mount_rc=0、f_opendir=成功、目录条目=75、**认成 .WAV = 0**
		 *       —— 文件名原样读出来是 REC_0001.WAV（属性 0x20 普通文件）。
		 *   现按"末 4 字符 == .WAV（点+WAV）"判，与 img_store 的 ext_match 同一口径。 */
		{
			int len = 0;
			while (fi.fname[len] != 0) { ++len; }
			if (len < 4) { continue; }
			{
				char e3 = fi.fname[len - 4], e2 = fi.fname[len - 3],
				     e1 = fi.fname[len - 2], e0 = fi.fname[len - 1];
				int is_wav = (e3 == '.') &&
				             (e2 == 'W' || e2 == 'w') &&
				             (e1 == 'A' || e1 == 'a') &&
				             (e0 == 'V' || e0 == 'v');
				if (!is_wav) { continue; }
			}
		}
		n_wav++;

		/* ★ 补丁27：表满时的处理。
		 *   旧代码是 `if (n >= REC_LIST_MAX) break;` —— 目录里按创建顺序排，
		 *   于是**保留最旧的 64 个、把最新的丢掉** ⇒ 录到第 65 段以后，
		 *   文件管理器里"新录音永远不出现"（正是用户报的"找不到录音文件"的另一半）。
		 *   改为：丢掉最旧的一条，把新的放到末尾 —— 始终显示**最新**的 64 条。 */
		if (n >= REC_LIST_MAX)
		{
			memmove(&s_rec_list[0], &s_rec_list[1],
			        sizeof(s_rec_list[0]) * (size_t)(REC_LIST_MAX - 1));
			n = REC_LIST_MAX - 1;
		}

		rec_list_entry_t* e = &s_rec_list[n];
		(void)snprintf(e->name, sizeof(e->name), "%s", fi.fname);
		(void)snprintf(e->path, sizeof(e->path), "%s/%s", REC_DIR, fi.fname);
		e->size  = fi.fsize;
		e->fdate = fi.fdate;
		e->ftime = fi.ftime;
		e->valid = 0u;
		e->bad   = 0u;
		e->dur_ms = 0u;

		/* 解析 WAV 头（只读前 44 字节）：RIFF/WAVE/fmt/PCM + 采样率/声道/位深/data 长 */
		{
			FIL   vf;
			UINT  br = 0u;
			uint8_t h[44];
			if (f_open(&vf, e->path, FA_READ) == FR_OK)
			{
				if ((f_read(&vf, h, 44u, &br) == FR_OK) && (br == 44u) &&
				    h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F' &&
				    h[8] == 'W' && h[9] == 'A' && h[10] == 'V' && h[11] == 'E' &&
				    h[12] == 'f' && h[13] == 'm' && h[14] == 't' && h[15] == ' ' &&
				    h[20] == 1u)
				{
					uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) |
					                ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
					uint16_t ch   = (uint16_t)((uint16_t)h[22] | ((uint16_t)h[23] << 8));
					uint16_t bits = (uint16_t)((uint16_t)h[34] | ((uint16_t)h[35] << 8));
					uint32_t datalen = (uint32_t)h[40] | ((uint32_t)h[41] << 8) |
					                   ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
					if (rate > 0u && ch > 0u && bits >= 8u)
					{
						uint32_t bps = (uint32_t)ch * ((uint32_t)bits / 8u);
						e->dur_ms = (uint32_t)((uint64_t)datalen * 1000u /
						                     ((uint64_t)rate * (uint64_t)bps));
						e->valid = 1u;
					}
					else { e->bad |= 1u; }      /* 头里有 0 率/0 声道/位数过低 */
				}
				else if (br != 44u) { e->bad |= 2u; }   /* 头读不全 */
				else                { e->bad |= 4u; }   /* 不是合法 RIFF/PCM WAV */
				(void)f_close(&vf);
			}
			else { e->bad |= 8u; }                 /* 打不开（卡卸了/文件坏了）*/
		}
		if (e->valid == 0u) { n_bad += 1; }
		++n;
	}
	(void)f_closedir(&d);
	s_rec_list_n = n;

	/* 过程量落 RAM：SWD 可直接读，用于区分"没扫到"和"扫到了没显示" */
	g_rec_scan_entries = (uint32_t)entries;
	g_rec_scan_wav     = (uint32_t)n_wav;
	g_rec_scan_bad     = (uint32_t)n_bad;
	g_rec_scan_n       = (uint32_t)n;
	return n;
}

/* ★ SWD 钩子（补丁27）：写 g_rec_scan_req=1 ⇒ 主循环里跑一次真实的目录扫描，
 *   把 FRESULT/目录项数/.WAV 数/入表数/前 4 个文件名留在 RAM。
 *   为什么需要它：录音分类"列不出文件"有两种完全不同的原因（扫不到 / 扫到了没画出来），
 *   靠肉眼看屏幕分不清；按本项目铁律，分支必须**直接计数**而不是拿别的现象反推。 */
void audio_rec_scan_poll(void)
{
	if (g_rec_scan_req == 0u) { return; }
	g_rec_scan_req = 0u;
	(void)rec_list_build();
}

/* FATFs fdate 折算成「YYYY-MM-DD HH:MM」；年 < 2020（没 RTC / NORTC 默认 1980）视为无日期，
 * 不让假日期混进卡片（本项目假数据零容忍）。 */
static void rec_date_text(const rec_list_entry_t* e, char* out, int n)
{
	uint16_t fd = e->fdate;
	uint16_t ft = e->ftime;
	int year = (int)(fd >> 9) + 1980;
	int mon  = (int)((fd >> 5) & 0x0F);
	int day  = (int)(fd & 0x1F);
	int hh   = (int)((ft >> 11) & 0x1F);
	int mm   = (int)((ft >> 5) & 0x3F);

	if (fd == 0u || year < 2020)
	{
		(void)snprintf(out, (size_t)n, "无日期");
		return;
	}
	(void)snprintf(out, (size_t)n, "%04d-%02d-%02d %02d:%02d",
	               year, mon, day, hh, mm);
}

/* 把一个 chunk 写进文件（立体声直存 / 单声道取左）。返回 0 = 成功。 */
static int rec_write_chunk(uint32_t halfwords)
{
    UINT     bw = 0u;
    FRESULT  fr;
    uint32_t t0, dt;
    uint32_t frames = halfwords / 2u;
    uint32_t bytes;

    if ((halfwords == 0u) || (s_opened == 0u))
    {
        return -1;
    }

    t0 = HAL_GetTick();
    if (g_rec_mono == 0u)
    {
        bytes = halfwords * 2u;                       /* 立体声：原样 */
        fr = f_write(&s_f, s_chunk, bytes, &bw);
    }
    else
    {
        /* 单声道：只取左声道（偶数下标）。就地压缩，省一块缓冲。 */
        for (uint32_t i = 0u; i < frames; ++i)
        {
            s_chunk[i] = s_chunk[2u * i];
        }
        bytes = frames * 2u;
        fr = f_write(&s_f, s_chunk, bytes, &bw);
    }
    dt = HAL_GetTick() - t0;

    if (dt > g_rec_write_ms_max) { g_rec_write_ms_max = dt; }

    if ((fr != FR_OK) || (bw != bytes))
    {
        g_rec_werr++;
        return -1;
    }
    s_data_bytes += bytes;
    g_rec_frames  = s_data_bytes / (uint32_t)((g_rec_mono == 0u) ? 4u : 2u);
    g_rec_bytes   = s_data_bytes;
    return 0;
}

/* 把采集环里**所有**已就绪的数据都取走写掉（停止时也要调，别丢尾巴）。 */
static void rec_drain(void)
{
    uint32_t guard = 0u;

    while (audio_out_capture_avail() >= REC_CHUNK_HW)
    {
        uint32_t t0 = HAL_GetTick();
        uint32_t got = audio_out_capture_read(s_chunk, REC_CHUNK_HW);
        uint32_t dt  = HAL_GetTick() - t0;

        if (dt > g_rec_bigblk_ms) { g_rec_bigblk_ms = dt; }
        if (got == 0u) { break; }
        (void)rec_write_chunk(got);
        if (++guard > 64u) { break; }        /* 防御：绝不在这里死循环 */
    }
}

static void rec_set_status(const char *s)
{
    /* 状态文本必须是**静态字面量或本文件的缓冲**：UI 侧按指针/内容比较，不接临时串。 */
    (void)snprintf(s_status, sizeof(s_status), "%s", s);
}

int audio_rec_start(void)
{
    int      rc;
    uint32_t idx;
    uint8_t  hdr[44];
    UINT     bw = 0u;

    if (g_rec_state == 1u)
    {
        return 0;                            /* 幂等：已在录 */
    }

    /* ---- ① 采集通道：I2S2 切成主收（与放音互斥）----
     * 返回值 2 = 放音还在淡出/发方向还占着 SPI2 ⇒ 让调用方下一拍再来。
     * 这一步放在建文件**之前**：否则每次重试都会在卡上留一个空文件。 */
    rc = audio_out_capture_start();
    if (rc == AUDIO_CAP_AGAIN)
    {
        g_rec_pending = 1u;
        g_rec_rc      = REC_AGAIN;
        rec_set_status("准备中：正在停止播放…");
        return REC_AGAIN;
    }
    if (rc != 0)
    {
        g_rec_pending = 0u;
        g_rec_rc      = -1;
        rec_set_status("失败：I2S 收方向起不来（看 g_cap_err）");
        return -1;
    }
    g_rec_pending = 0u;

    /* ---- ② codec：ADC/麦克风通路（ADC 开、DAC 关、REG17=0xBF）----
     * ★ 补丁18c：**隔拍整段重试**（不是背靠背）。
     *   【为什么不背靠背】本轮实测（build/_reg18c.txt 的 4 轮）里第 3 轮出现
     *   `g_es_fail Δ=12`（**连续 12 笔真丢写**），而当时的 3 次背靠背重试（间隔 2 ms）
     *   全部落空 —— 失败是**成簇**的（同一窗口内连灭），且 es8311 还有 250 ms 熔断冷却，
     *   间隔 2 ms 的重试跨不过它，等于白试。
     *   【隔拍 = 主循环 ≈17 ms/拍】既让开窗口、又不阻塞主循环；更关键的是
     *   **两条调用路径**（App 点击 / SWD 钩子）都自动受益，而不是只在 App 里加重试。
     *   【降级放宽】es8311_i2c_relax() 只清"降级快速失败"、不碰熔断；录音期间必然没有
     *   播放（I2S2 主发/主收互斥），所以放宽它不会影响播放体验。
     *   ⚠ 返回 REC_AGAIN(2) 时**采集通道已经收回**（audio_out_capture_stop），
     *     下一次调用会从头走一遍，因此重试是幂等的。 */
    if (s_adc_left == 0u)
    {
        s_adc_left = REC_ADC_TRIES;          /* 新一次动作开始 */
    }
    es8311_i2c_relax();
    g_rec_es_rc = es8311_adc_start(g_rec_mic_gain);
    if (g_rec_es_rc != 0)
    {
        s_adc_left--;
        audio_out_capture_stop();            /* 让下一拍从干净状态重来 */
        if (s_adc_left > 0u)
        {
            g_rec_adc_retry++;
            g_rec_pending = 1u;
            g_rec_rc      = REC_AGAIN;
            rec_set_status("准备中：ES8311 未就绪，正在重试…");
            return REC_AGAIN;                /* 不阻塞：下一次调用再来 */
        }
        g_rec_pending = 0u;
        g_rec_rc      = -2;                  /* 码表不变：-2 = ES8311 那一步没过 */
        if (g_rec_es_rc == -3)
        {
            rec_set_status("失败：总线上找不到 ES8311（模块没接好/接线松）");
        }
        else if (g_rec_es_rc == -4)
        {
            rec_set_status("失败：I2C 总线起不来（PB6/PB7）");
        }
        else
        {
            rec_set_status("失败：ES8311 回读不符（I2C 丢写，已重试）");
        }
        return -2;
    }
    s_adc_left    = 0u;                      /* 成功 ⇒ 额度清零 */
    g_rec_pending = 0u;
    /* ★ 每次起录都把"本次录音见过的最大峰值"清零 —— 它的注释就是这么写的，
     * 但**从来没有清零过**（跨录音单调累加）⇒ 两个后果：
     *   ① 名字叫"本次录音"、实际是"开机以来"，读到的是历史最大值（假数据）；
     *   ② BoardRec_MicOk() 拿它判"有没有麦克风信号" ⇒ 一旦历史上响过一次就**永久为真**，
     *      以后真没麦克风了它也不报（本项目最忌的"看不出来的坏状态"）。
     * ⇒ 起录清零后，它才是真的"本次录音"，扫增益时也能逐档比较。 */
    g_rec_peak_max = 0u;

    /* ---- ③ 文件：目录 + 命名 + 占位 WAV 头 ---- */
    if (fatfs_ensure_mounted() != 0)
    {
        (void)es8311_adc_stop();
        audio_out_capture_stop();
        g_rec_rc = -3;
        rec_set_status("失败：TF 卡没挂载");
        return -3;
    }
    (void)f_mkdir(REC_DIR);                  /* FR_EXIST 也算正常 */
    idx = rec_scan_max_index() + 1u;
    if (idx > 9999u) { idx = 1u; }
    (void)snprintf(s_name, sizeof(s_name), "REC_%04u.WAV", (unsigned)idx);
    (void)snprintf(s_path, sizeof(s_path), "%s/%s", REC_DIR, s_name);

    if (f_open(&s_f, s_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    {
        (void)es8311_adc_stop();
        audio_out_capture_stop();
        g_rec_rc = -4;
        rec_set_status("失败：建不了录音文件（卡满/写保护？）");
        return -4;
    }
    s_opened = 1u;
    wav_make_header(hdr, 0u, REC_RATE_HZ, (uint16_t)((g_rec_mono == 0u) ? 2u : 1u));
    if (f_write(&s_f, hdr, 44u, &bw) != FR_OK)
    {
        (void)f_close(&s_f);
        s_opened = 0u;
        (void)es8311_adc_stop();
        audio_out_capture_stop();
        g_rec_rc = -5;
        rec_set_status("失败：写不进录音文件");
        return -5;
    }

    /* ---- ④ 起表 ---- */
    s_data_bytes = 0u;
    s_start_tick = HAL_GetTick();
    g_rec_frames = 0u;
    g_rec_bytes  = 0u;
    g_rec_werr   = 0u;
    g_rec_write_ms_max = 0u;
    g_rec_bigblk_ms    = 0u;
    g_rec_state  = 1u;
    g_rec_rc     = 0;
    rec_set_status("录音中");
    return 0;
}

void audio_rec_stop(void)
{
    uint8_t hdr[44];
    UINT    bw = 0u;

    if (g_rec_state != 1u)
    {
        /* ★ 补丁18c：即使没在录，也要把"等 ES8311 就绪"的重试状态收干净 ——
         *   否则残留的 g_rec_pending/s_adc_left 会让下一次调用以为还在重试中。 */
        g_rec_pending = 0u;
        s_adc_left    = 0u;
        s_auto_retry  = 0u;
        return;
    }
    g_rec_state = 2u;                        /* 收尾中：这期间再点"停"不会重入 */

    /* ① 把 DMA 里剩下的数据取干净（尾巴不能丢）*/
    rec_drain();

    /* ② 停采集、把 codec 恢复成"DAC 静音起播"的干净状态 */
    audio_out_capture_stop();
    (void)es8311_adc_stop();

    /* ③ 回填 WAV 头（真实长度）并关闭 —— 不回填的话文件头里是 0，播放器显示 0 秒 */
    if (s_opened != 0u)
    {
        wav_make_header(hdr, s_data_bytes, REC_RATE_HZ,
                        (uint16_t)((g_rec_mono == 0u) ? 2u : 1u));
        (void)f_lseek(&s_f, 0u);
        (void)f_write(&s_f, hdr, 44u, &bw);
        (void)f_sync(&s_f);
        (void)f_close(&s_f);
        s_opened = 0u;
    }

    /* ④ 把卡上那个文件**回读校验**（见 g_rec_hdr 的说明）—— 这是"真的录成了一个能播的
     *    WAV"的**唯一**板上证据：主机读不到 TF 卡，只有把头和长度带回 RAM 才能验。 */
    g_rec_hdr_ok  = 0u;
    g_rec_hdr_bad = 0u;
    g_rec_fsize   = 0u;
    {
        FIL   vf;
        UINT  br = 0u;
        uint8_t vh[44];
        uint16_t ch = (uint16_t)((g_rec_mono == 0u) ? 2u : 1u);

        if (f_open(&vf, s_path, FA_READ) == FR_OK)
        {
            if ((f_read(&vf, vh, 44u, &br) == FR_OK) && (br == 44u))
            {
                for (uint32_t i = 0u; i < 11u; ++i)
                {
                    g_rec_hdr[i] = (uint32_t)vh[i * 4u]
                                 | ((uint32_t)vh[i * 4u + 1u] << 8)
                                 | ((uint32_t)vh[i * 4u + 2u] << 16)
                                 | ((uint32_t)vh[i * 4u + 3u] << 24);
                }
                g_rec_fsize = (uint32_t)f_size(&vf);
            }
            else
            {
                g_rec_hdr_bad |= 1u;                 /* 头读不出来 */
            }
            (void)f_close(&vf);

            /* 逐项自检（每一项对应一个位，失败时能一眼看出是哪一条）。
             * ⚠ 下标千万别数错：44 字节 = 11 个字，word0="RIFF" word1=RIFF长度 word2="WAVE"
             *   word3="fmt " word4=16 word5={PCM,声道} word6=采样率 word7=字节率
             *   word8={块对齐,位深} word9="data" word10=data长度。
             *   2026-10-16 踩过：位深写在 word8 的**高半字**，我一开始取的 word9（那是 "data"
             *   的 ASCII）⇒ hdr_ok 恒 0，报了个**假的不合格**。 */
            if (g_rec_hdr[0] != 0x46464952u)             { g_rec_hdr_bad |= 1u << 1; }  /* "RIFF" */
            if (g_rec_hdr[2] != 0x45564157u)             { g_rec_hdr_bad |= 1u << 2; }  /* "WAVE" */
            if (g_rec_hdr[3] != 0x20746D66u)             { g_rec_hdr_bad |= 1u << 3; }  /* "fmt " */
            if ((g_rec_hdr[5] & 0xFFFFu) != 1u)          { g_rec_hdr_bad |= 1u << 4; }  /* PCM */
            if ((uint32_t)((g_rec_hdr[5] >> 16) & 0xFFFFu) != (uint32_t)ch) { g_rec_hdr_bad |= 1u << 5; }
            if (g_rec_hdr[6] != REC_RATE_HZ)             { g_rec_hdr_bad |= 1u << 6; }  /* 采样率 */
            if (((g_rec_hdr[8] >> 16) & 0xFFFFu) != 16u) { g_rec_hdr_bad |= 1u << 7; }  /* 位深＝16（word8 高半字）*/
            if (g_rec_hdr[9] != 0x61746164u)             { g_rec_hdr_bad |= 1u << 8; }  /* "data" */
            if (g_rec_hdr[10] != s_data_bytes)           { g_rec_hdr_bad |= 1u << 9; }  /* data 长度 */
            if (g_rec_hdr[1] != (36u + s_data_bytes))    { g_rec_hdr_bad |= 1u << 10; } /* RIFF 长度 */
            if (g_rec_fsize != (44u + s_data_bytes))     { g_rec_hdr_bad |= 1u << 11; } /* 文件实长 */
        }
        else
        {
            g_rec_hdr_bad |= 1u;
        }
        g_rec_hdr_ok = (g_rec_hdr_bad == 0u) ? 1u : 0u;
    }

    g_rec_state  = 0u;
    g_rec_rc     = 0;
    g_rec_pending = 0u;

    {
        char t[64];

        (void)snprintf(t, sizeof(t), "已保存 %s · %u.%u 秒 · %u KB",
                       s_name,
                       (unsigned)(g_rec_frames / REC_RATE_HZ),
                       (unsigned)((g_rec_frames % REC_RATE_HZ) * 10u / REC_RATE_HZ),
                       (unsigned)(s_data_bytes / 1024u));
        rec_set_status(t);
    }
}

/* ★ 补丁18b：试听 —— 把**最近录的那一段**从 TF 卡放出来。
 *
 * 【为什么"最近一段"是目录里编号最大的】文件名是 `REC_%04u.WAV`，编号单调递增 ⇒
 *   编号最大 == 最后录的。用扫描（而不是记在 RAM 里的名字）的好处是**复位后也成立**，
 *   而复位是排查时的常态。
 *
 * 【与录音互斥】I2S2 的 I2SCFG 是主发/主收二选一，所以：
 *   · 正在录音 ⇒ 先走完整的 audio_rec_stop()（含回填 WAV 头），否则文件会缺尾巴；
 *   · 放音起不来（<0）时**不改动任何录音状态**，如实把码返回给 UI。
 *
 * 【返回值】就是 wav_play_start() 的码（见 wav_play.h：-1 空路径 / -2 卡没挂载 /
 *   -3 打不开 / -11..-20 WAV 头解析失败 / -21 定位 data 失败 / -22 音频输出起不来）；
 *   另加 -1 = 目录里没有录音且本会话也没录过。同时写进 g_rec_play_rc。 */
int audio_rec_play_last(void)
{
    char     path[48];
    uint32_t idx;
    int      rc;

    /* ① 先收尾（若在录）：这一步会把 WAV 头回填好，之后放的才是完整文件 */
    if (g_rec_state != 0u)
    {
        audio_rec_stop();
    }

    /* ② 选目标文件 */
    idx = rec_scan_max_index();                    /* 顺带刷新 g_rec_files */
    if (idx != 0u)
    {
        (void)snprintf(path, sizeof(path), "%s/REC_%04u.WAV", REC_DIR, (unsigned)idx);
    }
    else if (s_path[0] != 0)                       /* 目录空了但本会话录过：试那条 */
    {
        (void)snprintf(path, sizeof(path), "%s", s_path);
    }
    else
    {
        g_rec_play_rc = -1;                        /* 没有可试听的文件 */
        return -1;
    }

    /* ③ 交给现成的播放链路（内部会 audio_out_start：起播安全栅 / 音量 ramp 全都有） */
    rc = wav_play_start(path);
    g_rec_play_rc = rc;
    if (rc == 0)
    {
        g_rec_play_n++;
    }
    return rc;
}

/* 停止试听（等于停掉共享播放器；不影响已落地的文件）。 */
void audio_rec_play_stop(void)
{
    wav_play_stop();
}

/* 1 = 正在试听出声（暂停/停止都算 0 —— 与 wav_play_busy 同口径）。 */
int audio_rec_playing(void)
{
    return wav_play_busy();
}

int audio_rec_poll(void)
{
    g_rec_poll++;

    /* 排障钩子：SWD 写 g_rec_test=1/2 就能在**不碰屏**的前提下起/停录音（验收脚本用）。 */
    if (g_rec_test != 0u)
    {
        uint32_t t = g_rec_test;

        g_rec_test = 0u;
        if (t == 1u)      { s_auto_retry = 1u; (void)audio_rec_start(); }
        else if (t == 2u) { s_auto_retry = 0u; audio_rec_stop(); }
    }

    /* ★ 补丁18c：替钩子把"隔拍重试"驱动完（App 路径由 app_tick 驱动，钩子路径没有调用方）。
     *   判据用 g_rec_pending —— 它同时覆盖"等放音淡出"与"等 ES8311 就绪"两种准备态；
     *   一旦不再是准备态（成功或额度用尽）就停手。 */
    if ((s_auto_retry != 0u) && (g_rec_pending != 0u))
    {
        if (audio_rec_start() != REC_AGAIN)
        {
            s_auto_retry = 0u;
        }
    }

    /* 排障钩子（补丁18c）：SWD 写 g_es_force_bc=1 ⇒ 立刻做一次**强制总线恢复**。
     *   为什么要有它：补丁9 的总线恢复判据是"SDA 被钳低"，而实测存在"电气空闲但
     *   从机卡在传输中"的状态（IDR=0xC0、probe 全灭、连复位都不恢复）。
     *   用它可以在**不依赖任何业务路径**的前提下验证"9 脉冲 + STOP 能否救回"。 */
    if (g_es_force_bc != 0u)
    {
        g_es_force_bc = 0u;
        es8311_force_bus_clear();
        g_es_force_bc_n++;
    }

    /* 排障钩子（补丁18b）：SWD 写 g_rec_play_req=1/2 就能放/停"最近一段录音"。
     * 用途：让"录到的到底有没有声音"当场用耳朵判，不必拔卡。 */
    if (g_rec_play_req != 0u)
    {
        uint32_t t = g_rec_play_req;

        g_rec_play_req = 0u;
        if (t == 1u)      { (void)audio_rec_play_last(); }
        else if (t == 2u) { audio_rec_play_stop(); }
    }

    if (g_rec_state != 1u)
    {
        return 0;
    }

    /* 超时长上限自动停（忘记按停不会把卡写满）*/
    if ((HAL_GetTick() - s_start_tick) > g_rec_max_ms)
    {
        audio_rec_stop();
        return 0;
    }

    rec_drain();

    /* ★ 在线改增益钩子：录音中直接改 REG16，**不重起录音**。
     *   改完顺手把 g_rec_peak_max 清零 ⇒ 它变成"本档增益下见过的最大峰值"，
     *   扫增益时逐档可比（不清零的话它是跨录音累加的单调最大值，没法按档比较）。 */
    if (g_rec_gain_req != 0u)
    {
        g_rec_gain_req = 0u;
        (void)es8311_adc_set_gain(g_rec_gain_val);
        g_rec_peak_max = 0u;        /* 重开测量窗 */
    }

    /* ★ 麦克风硬件诊断钩子（docs/MIC_RECORD_DESIGN.md §7 阶梯第 1 步）。
     * 写 g_cap_line_req = N ⇒ 本拍录音时连续采样 N 次 PC2（模块 DOUT）的电平。
     * ⚠ 放在 rec_drain() 之后：这时 I2S2 主收已在跑、模块正在往线上发数据。 */
    if (g_cap_line_req != 0u)
    {
        uint32_t n = g_cap_line_req;

        g_cap_line_req = 0u;
        if (n > 400000u) { n = 400000u; }   /* 防御：别把单拍拖到几十毫秒 */
        audio_out_capture_probe_line(n);
    }

    /* 维护"见过多大峰值" —— UI 的"有没有麦克风信号"就靠它判（见 g_rec_peak_max 说明）。
     * ★ 同时把**电平条的输入值**在这里算好：引擎侧算 ⇒ 无论走 App 还是 SWD 钩子起的录音，
     *   这个量都是真实的、SWD 可直接读（否则只有 App 打开时才更新，无法验收）。 */
    {
        /* ★★ 电平只取 **R 声道** —— 这是本板实测的结论（有原始采样为证）★★
         *   板上实测（tools/_mic_dump.py 把 s_chunk 里的原始 int16 直接读出来）：
         *     L = [-1,-1,-1,-1,-1,-1,-1,-1]        ← 恒 -1：空时隙，没有音频
         *     R = [-9,26,-8,45,37,43,32,55,…]      ← 真实音频，随声音变化
         *   ⇒ 本板麦克风是**单声道**，音频落在 R 时隙；L 时隙是空的。
         *   ⚠ 而空时隙**不老实**：它会偶发吐出 0x3FF / 0x1FFF / 0x7FFF 这类
         *     **连续位模式**（2^n−1，典型 I2S 位对齐错位的产物，不是音频）。
         *     若沿用 max(L,R)，这些毛刺会随机把电平条顶到 100%（实测到 level 突然=99
         *     而当时 L/R 峰值才 1/236）⇒ 电平条变成"乱跳"，同样是坏体验。
         *   ⇒ 取 R 声道 = 取真正有音频的那一路。换了立体声输入请把这里改回 max(L,R)。 */
        uint32_t pk = g_cap_peak_r;

        if (pk > g_rec_peak_max) { g_rec_peak_max = pk; }
        g_rec_level = (uint32_t)level_from_peak_db(pk);
    }
    return 0;
}

/* ====================== Board 桥接（phone_shell 录音机 App 用）====================== */

int BoardRec_Start(void)        { return audio_rec_start(); }
int BoardRec_Stop(void)         { audio_rec_stop(); return 0; }
int BoardRec_Active(void)       { return (g_rec_state == 1u) ? 1 : 0; }
int BoardRec_Pending(void)      { return (g_rec_pending != 0u) ? 1 : 0; }
uint32_t BoardRec_Bytes(void)   { return g_rec_bytes; }
int BoardRec_LastRc(void)       { return (int)g_rec_rc; }
/* ---- 补丁18b：试听（把最近一段录音放出来听）---- */
int BoardRec_PlayLast(void)     { return audio_rec_play_last(); }
void BoardRec_StopPlay(void)    { audio_rec_play_stop(); }
int BoardRec_Playing(void)      { return audio_rec_playing(); }
int BoardRec_PlayRc(void)       { return (int)g_rec_play_rc; }
int BoardRec_PlayName(char* out, int n) { return wav_play_current_name(out, n); }
/* 补丁18c：ES8311 那一步的细分码与重试次数（UI 用来给出"该查线还是该重试"的提示）*/
int BoardRec_EsRc(void)         { return (int)g_rec_es_rc; }
int BoardRec_AdcRetry(void)     { return (int)g_rec_adc_retry; }

uint32_t BoardRec_ElapsedMs(void)
{
    /* 按**帧数**折算，不按 tick —— 帧数才是"真的录进去多少声音"的度量；
     * 主循环被卡顿时 tick 会偏大，两者一起用能看出落差（见 rec_check.py）。 */
    return (uint32_t)((uint64_t)g_rec_frames * 1000u / REC_RATE_HZ);
}

const char* BoardRec_Name(void)
{
    return (s_name[0] != 0) ? s_name : "";
}

const char* BoardRec_StatusText(void)
{
    return s_status;
}

int BoardRec_Scan(void)
{
    /* 文件管理器进录音分类时调用：把 /YMGUI/RECORD 里所有 .WAV 枚举成表。
     * 与 rec_scan_max_index() 解耦 —— 后者仍服务「试听最近一段」。 */
    return rec_list_build();
}

/* 峰值(0..32767) → 电平 0..100，**按 dB 对数刻度**（-60 dBFS ⇒ 0%，0 dBFS ⇒ 100%）。
 *
 * ★★ 为什么必须换掉线性折算（这就是用户报的"点击开始录音波形条没有反应"的根因）★★
 *   板上实测（tools/_mic_dump.py 直接把原始采样点读出来看）：
 *       L 声道 = [-1,-1,-1,…] 恒 -1（本板麦克风只落在 R 声道）
 *       R 声道 = [-9,26,-8,45,37,43,32,55,…]  ← **真实音频，确实在随声音变化**
 *   即麦克风硬件**是好的**，但常态峰值只有 **~100（-50 dBFS 左右）**。
 *   而旧实现是线性的 `100*pk/32768` ⇒ 100 算出来是 **0** ⇒ 电平条恒为 0、怎么发声都不动。
 *   ⇒ 麦克风没坏，**是电平表的刻度错了**：音频电平表本来就该用 dB（人耳也是对数响应）。
 *
 * 实现：20*log10(pk/32768) = 6.0206*log2(pk) - 90.31。
 *   用"最高位位置 + 其下 6 位线性插值"近似 log2（≈0.75 dB 步进，对电平表足够），
 *   全程整数运算、无浮点、无 libm。
 * ⚠ 仍然是**采样峰值**（不是 RMS）；UI 的电平条用它，语义与以前一致，只是刻度改对。 */
static int level_from_peak_db(uint32_t pk)
{
    int      bp;        /* 最高位位置 0..14 */
    uint32_t s, frac;
    int      db_x100;   /* dB × 100 */

    if (pk == 0u) { return 0; }
    if (pk > 32767u) { pk = 32767u; }

    bp = 14;
    while (((pk >> bp) & 1u) == 0u) { bp--; }   /* pk>0 ⇒ 一定会停 */

    /* 把 pk 左移到最高位在 bit31，取其下 6 位作插值小数（单位 1/64 倍频程）*/
    s    = pk << (31 - bp);
    frac = (s >> 25) & 0x3Fu;

    /* dB×100 = 602*(bp + frac/64) - 9031 */
    db_x100 = (int)((602u * (uint32_t)((bp * 64) + (int)frac)) / 64u) - 9031;

    /* 映射：-60 dB ⇒ 0%，0 dB ⇒ 100% */
    {
        int lv = (db_x100 + 6000) / 60;

        if (lv < 0)   { lv = 0; }
        if (lv > 100) { lv = 100; }
        return lv;
    }
}

int BoardRec_Level(void)
{
    /* 电平由 audio_rec_tick() 每拍算好（见 g_rec_level 说明），这里只把它交给 UI。 */
    return (int)g_rec_level;
}

/* 1 = 本次录音期间**采到过明显信号**；0 = 一直很小。
 * 用途：UI 在"录音中"超过几秒仍是 0 时，应当如实提示"未检测到麦克风信号"，
 *   并把最可能的原因（DOUT 未接到 P2-9 / 模块没接麦克风）写出来 ——
 *   而不是继续显示"录音中"让用户以为在录（假数据零容忍）。 */
int BoardRec_MicOk(void)
{
    return (g_rec_peak_max >= g_rec_mic_thresh) ? 1 : 0;
}

uint32_t BoardRec_PeakRaw(void)
{
    return g_rec_peak_max;
}

/* ====================== 文件管理器录音卡片接口（补丁19）======================
 * 在 BoardRec_* 基础上再开一组「按索引枚举 / 播放 / 删除 / 重命名 / 分享」的接口，
 * 让文件管理器把录音当成卡片列出。与 BoardRec_PlayLast() 复用**同一个** wav_play
 * 会话（一次只能放一个），所以文件管理器起的播放也会体现在 BoardRec_Playing() 上。 */

int BoardRec_Count(void)
{
    return s_rec_list_n;
}

int BoardRec_ListName(int i, char* out, int n)
{
    if (i < 0 || i >= s_rec_list_n || n <= 0) { return -1; }
    (void)snprintf(out, (size_t)n, "%s", s_rec_list[i].name);
    return 0;
}

/* 完整路径（"/YMGUI/RECORD/REC_0001.WAV"）—— 补丁29 新增。
 * 「播放」按钮要把录音交给音乐播放器，需要的是**绝对值路径**而不是文件名；
 * 让 UI 自己拼 "/YMGUI/RECORD/" 会把目录常量复制到 phone_shell 里（分层破坏），
 * 所以由这一层给出。 */
int BoardRec_ListPath(int i, char* out, int n)
{
    if (i < 0 || i >= s_rec_list_n || out == NULL || n <= 0) { return -1; }
    (void)snprintf(out, (size_t)n, "%s", s_rec_list[i].path);
    return 0;
}

int BoardRec_ListSize(int i, uint32_t* size)
{
    if (i < 0 || i >= s_rec_list_n) { return -1; }
    *size = s_rec_list[i].size;
    return 0;
}

uint32_t BoardRec_DurationMs(int i)
{
    if (i < 0 || i >= s_rec_list_n) { return 0u; }
    return s_rec_list[i].dur_ms;
}

int BoardRec_DateText(int i, char* out, int n)
{
    if (i < 0 || i >= s_rec_list_n || n <= 0) { return -1; }
    rec_date_text(&s_rec_list[i], out, n);
    return 0;
}

int BoardRec_IsValid(int i)
{
    if (i < 0 || i >= s_rec_list_n) { return 0; }
    return (int)s_rec_list[i].valid;
}

int BoardRec_PlayIndex(void)
{
    if (s_rec_play_idx < 0) { return -1; }
    /* 播放会话可能被别处（音乐 App）接手，或已自然结束 ⇒ 卡片下标作废 */
    if (!wav_play_busy() && !wav_play_paused())
    {
        s_rec_play_idx = -1;
        return -1;
    }
    return s_rec_play_idx;
}

int BoardRec_PlayList(int i)
{
    int rc;
    if (i < 0 || i >= s_rec_list_n) { return -1; }
    /* 与录音互斥：若正在录音，先正常收尾（回填 WAV 头）再放，别留半个文件 */
    if (g_rec_state != 0u) { audio_rec_stop(); }
    rc = wav_play_start(s_rec_list[i].path);
    g_rec_play_rc = rc;
    if (rc == 0) { s_rec_play_idx = i; }
    else          { s_rec_play_idx = -1; }
    return rc;
}

void BoardRec_PlayStop(void)
{
    wav_play_stop();
    s_rec_play_idx = -1;
}

int BoardRec_PlayPause(void)
{
    int r = wav_play_pause();
    if (r != 0) { s_rec_play_idx = -1; }
    return r;
}

int BoardRec_PlayResume(void)
{
    return wav_play_resume();
}

int BoardRec_PlayPaused(void)
{
    return wav_play_paused();
}

uint32_t BoardRec_PlayElapsedMs(void)
{
    return wav_play_elapsed_ms();
}

uint32_t BoardRec_PlayTotalMs(void)
{
    return wav_play_total_ms();
}

int BoardRec_PlaySeek(uint32_t ms)
{
    (void)ms;
    return -1;   /* 录音卡片只做进度展示，不提供拖动跳播 */
}

int BoardRec_Delete(int i, int* next_valid)
{
    int rc;
    if (i < 0 || i >= s_rec_list_n) { return 2; }   /* 下标非法 */
    rc = f_unlink(s_rec_list[i].path);
    if (rc != FR_OK) { return 1; }                   /* 删除失败（卡卸了/文件锁）*/
    (void)rec_list_build();                          /* 重建列表 */
    if (next_valid != NULL)
    {
        *next_valid = (i < s_rec_list_n) ? i : (s_rec_list_n > 0 ? s_rec_list_n - 1 : -1);
    }
    return 0;
}

/* 把新文件名收敛成合法的 FAT 文件名：去路径分隔符、限长、强制 .WAV 后缀。
 * 返回 0 = 收敛成功（dst 已填好）；-1 = 输入不可用。 */
static int rec_sanitize_name(const char* src, char* dst, int n)
{
    int k = 0;
    if (src == NULL || src[0] == 0) { return -1; }
    /* 复制可见字符、丢掉 \ / : * ? " < > | 等 FAT 非法字符 */
    for (const char* p = src; *p != 0 && k < n - 6; ++p)
    {
        char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') { continue; }
        dst[k++] = c;
    }
    dst[k] = 0;
    if (k == 0) { return -1; }
    /* 强制 .WAV 后缀（不区分已有扩展名的大小写，缺就补）*/
    {
        int len = k;
        int has_ext = (len >= 4) &&
            (dst[len - 4] == '.' || dst[len - 4] == '.') &&
            ((dst[len - 3] == 'W' || dst[len - 3] == 'w') &&
             (dst[len - 2] == 'A' || dst[len - 2] == 'a') &&
             (dst[len - 1] == 'V' || dst[len - 1] == 'e'));
        if (!has_ext)
        {
            if (k + 4 >= n) { dst[k - 1] = 0; }   /* 太长就先砍一字符腾位置 */
            (void)snprintf(dst + k, (size_t)(n - k), ".WAV");
        }
    }
    return 0;
}

int BoardRec_Rename(int i, const char* newname)
{
    char dst[48];
    if (i < 0 || i >= s_rec_list_n) { return 2; }   /* 下标非法 */
    if (rec_sanitize_name(newname, dst, (int)sizeof(dst)) != 0) { return 3; }
    {
        char full[48];
        (void)snprintf(full, sizeof(full), "%s/%s", REC_DIR, dst);
        if (f_rename(s_rec_list[i].path, full) != FR_OK) { return 1; }  /* 重命名失败 */
    }
    (void)rec_list_build();                          /* 重建列表（位置可能变）*/
    /* 重建后找回新名字的下标，方便 UI 续选 */
    for (int k = 0; k < s_rec_list_n; ++k)
    {
        if (strncmp(s_rec_list[k].name, dst, (size_t)sizeof(dst)) == 0)
        {
            s_rec_play_idx = (s_rec_play_idx == i) ? k : s_rec_play_idx;
            return k;                                /* 返回新下标 */
        }
    }
    return 0;
}

/* 分享：把文件**真实复制**到 /YMGUI/SHARE（用户可拔卡由电脑取走）。
 * 本设备无网络 / 蓝牙发送通道，故「分享」= 复制到导出目录 —— 是实打实的文件操作，不造假。 */
int BoardRec_Share(int i)
{
    static const char* SHARE_DIR = "/YMGUI/SHARE";
    char dst[48];
    FIL  sf, df;
    UINT br = 0u, bw = 0u;
    uint8_t buf[512];
    int  rc = 0;

    if (i < 0 || i >= s_rec_list_n) { return 2; }
    (void)f_mkdir(SHARE_DIR);
    (void)snprintf(dst, sizeof(dst), "%s/%s", SHARE_DIR, s_rec_list[i].name);
    if (f_open(&sf, s_rec_list[i].path, FA_READ) != FR_OK) { return 1; }
    if (f_open(&df, dst, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    {
        (void)f_close(&sf);
        return 1;
    }
    for (;;)
    {
        if (f_read(&sf, buf, sizeof(buf), &br) != FR_OK) { rc = 1; break; }
        if (br == 0u) { break; }
        if (f_write(&df, buf, br, &bw) != FR_OK || bw != br) { rc = 1; break; }
    }
    (void)f_close(&sf);
    (void)f_close(&df);
    return rc;
}

