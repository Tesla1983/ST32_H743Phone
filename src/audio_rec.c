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

#include "ff.h"
#include "stm32h7xx_hal.h"

#include <string.h>
#include <stdio.h>

/* 每次从采集环取走的半字数：4096 半字 = 8 KB = 2048 帧 ≈ 42 ms。
 * 与音乐播放的半缓冲同量级（那边也是 42 ms），写卡粒度与读卡粒度一致，行为好对照。 */
#define REC_CHUNK_HW 4096u

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
volatile uint32_t g_rec_mic_gain   = 0x24u;    /* 与官方 esp_codec_dev 的默认值一致 */
volatile uint32_t g_rec_max_ms     = 300000u;  /* 5 分钟上限（防止忘关把卡写满）*/
volatile uint32_t g_rec_files      = 0u;
/* ★ 麦克风"到底有没有信号"的判据量。为什么要它：DOUT 没接线时链路**照常跑通**
 *   （DMA 搬、文件生成、进度条走），唯一区别是采到的是悬空电平、峰值极小。
 *   没有这个量，UI 就只能显示"录音中"而实际是空的 —— 那正是本项目最忌讳的假数据。 */
volatile uint32_t g_rec_peak_max   = 0u;       /* 本次录音见过的最大峰值（0..32767）*/
volatile uint32_t g_rec_mic_thresh = 128u;     /* 判"有麦克风信号"的峰值阈值（≈ -48 dBFS）*/
/* ★ 停止时把**卡上文件**的 WAV 头回读进 RAM 并自检 —— 让"到底写没写成一个能播的 WAV"
 *   变成可验证的事实，而不是"我写了所以应该对"。主机读不到 TF 卡，只能这样把它带出来。
 *   g_rec_hdr[0..10] = 卡上前 44 字节（按 32 位小端存，"RIFF" 读出来就是 0x46464952）。*/
volatile uint32_t g_rec_hdr[11];
volatile uint32_t g_rec_hdr_ok  = 0u;          /* 1 = 头自检通过 */
volatile uint32_t g_rec_fsize   = 0u;          /* 卡上文件的真实字节数（f_size）*/
volatile uint32_t g_rec_hdr_bad = 0u;          /* 自检失败时：哪一项不符（位图，便于定位）*/

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
static uint8_t  s_opened   = 0u;

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

    /* ---- ② codec：ADC/麦克风通路（ADC 开、DAC 关、REG17=0xBF）---- */
    if (es8311_adc_start(g_rec_mic_gain) != 0)
    {
        audio_out_capture_stop();
        g_rec_rc = -2;
        rec_set_status("失败：ES8311 ADC 通路没配上");
        return -2;
    }

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

int audio_rec_poll(void)
{
    g_rec_poll++;

    /* 排障钩子：SWD 写 g_rec_test=1/2 就能在**不碰屏**的前提下起/停录音（验收脚本用）。 */
    if (g_rec_test != 0u)
    {
        uint32_t t = g_rec_test;

        g_rec_test = 0u;
        if (t == 1u)      { (void)audio_rec_start(); }
        else if (t == 2u) { audio_rec_stop(); }
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

    /* 维护"见过多大峰值" —— UI 的"有没有麦克风信号"就靠它判（见 g_rec_peak_max 说明）。*/
    {
        uint32_t pk = (g_cap_peak_l > g_cap_peak_r) ? g_cap_peak_l : g_cap_peak_r;

        if (pk > g_rec_peak_max) { g_rec_peak_max = pk; }
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
    (void)rec_scan_max_index();
    return (int)g_rec_files;
}

/* 当前输入电平 0..100（取 L/R 峰值较大者，按 0..32767 线性折算）。
 * ⚠ 这是**采样峰值**，不是分贝；UI 的进度条用它就够了。 */
int BoardRec_Level(void)
{
    uint32_t pk = (g_cap_peak_l > g_cap_peak_r) ? g_cap_peak_l : g_cap_peak_r;

    if (pk > 32767u) { pk = 32767u; }
    return (int)((pk * 100u) / 32768u);
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
