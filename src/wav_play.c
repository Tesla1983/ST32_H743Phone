/* ===========================================================================
 * WAV 播放源（TF 卡 → I2S）— 设计说明见 wav_play.h
 * =========================================================================== */
#include "wav_play.h"
#include "audio_out.h"
#include "fatfs_port.h"
#include "img_store.h"        /* IMG_MUSIC_DIR / img_scan_* 扫描表 */

#include "ff.h"                /* FIL / FRESULT / f_open…（FatFs 本体，见 third_party/FatFs）*/
#include "stm32h7xx_hal.h"     /* HAL_GetTick（补数耗时计时）*/

#include <string.h>

/* UI 音量的权威值在 main.c（audio_out 那边不起播时也要知道当前档位）。 */
extern volatile uint32_t g_audio_volume;

/* 单声道文件展开成交错立体声时的一级暂存：2048 个半字 = 4 KB。
 * ⚠ 与 audio_out 的 DMA 缓冲同段（SRAM1 .bss_audio）—— 非缓存区，
 *   CPU 读写它没有任何一致性问题（DMA 不碰这块，只碰 audio_out 那块）。 */
#define WAV_BUF_ATTR   __attribute__((section(".bss_audio"), aligned(4)))
static int16_t WAV_BUF_ATTR s_mono[2048];

static void path_copy(char *dst, int n, const char *src)
{
    int i = 0;

    if (dst == NULL || n <= 0)
    {
        return;
    }
    while (src[i] != 0 && i < n - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* ---- 诊断量 ---- */
volatile int32_t  g_wav_rc       = 99;   /* 0=OK；<0 = 打开/播放失败（见 wav_play.h 的取值表）*/
volatile uint32_t g_wav_state    = 0;    /* 0 停 / 1 播 / 2 暂停 */
volatile uint32_t g_wav_rate     = 0;    /* 文件本身的采样率（不是 I2S 的 fs）*/
volatile uint32_t g_wav_bits     = 0;    /* 位深（本版只支持 16）*/
volatile uint32_t g_wav_channels = 0;    /* 声道数（1 或 2）*/
volatile uint32_t g_wav_total    = 0;    /* 数据总字节数 */
volatile uint32_t g_wav_pos      = 0;    /* 已读数据字节数 */
volatile uint32_t g_wav_underrun = 0;    /* 卡没供给上的次数 */
volatile uint32_t g_wav_read_ms  = 0;    /* 最近一次半缓冲补数的耗时（毫秒）*/

/* 诊断触发：写 1 起播固定路径 /YMGUI/MUSIC/TOKYOHOT.WAV（不依赖 UI、不依赖扫描表）。
 * 与 audio_out.c 的 g_ao_demo 同构，供 tools/wav_play_check.py 用 SWD 触发验收。 */
volatile uint32_t g_wav_test = 0;

static FIL  s_f;
static char s_path[IMG_PATH_MAX];
static uint8_t s_opened;
static uint32_t s_data_off;   /* data chunk 在文件里的起始偏移（seek 用，0 = 还没解析头）*/

/* ====================== WAV 头解析 =======================
 * 只认最朴素的 RIFF/WAVE + fmt + data。做法是**逐个 chunk 走**，
 * 而不是按"fmt 一定在 12 字节处"的假设跳偏移 —— 中间夹 LIST/INFO 的文件很常见。 */
static uint32_t rd_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd_le16(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8);
}

static int wav_read_header(uint32_t *data_off, uint32_t *data_len,
                           uint32_t *rate, uint32_t *bits, uint32_t *ch)
{
    uint8_t  hdr[64];
    UINT     br = 0;
    FRESULT  fr;
    uint32_t pos = 12u;                 /* 跳过 "RIFF"+size+"WAVE" */

    fr = f_read(&s_f, hdr, 12u, &br);
    if (fr != FR_OK || br != 12u)                    { return -1; }
    if (memcmp(hdr, "RIFF", 4) != 0)                 { return -2; }
    if (memcmp(hdr + 8, "WAVE", 4) != 0)             { return -3; }

    *rate = 0; *bits = 0; *ch = 0; *data_off = 0; *data_len = 0;

    for (;;)
    {
        uint32_t cid, csz;

        if (f_lseek(&s_f, pos) != FR_OK)             { return -4; }
        fr = f_read(&s_f, hdr, 8u, &br);
        if (fr != FR_OK || br != 8u)                 { return -5; }

        cid = rd_le32(hdr);
        csz = rd_le32(hdr + 4);

        if (cid == 0x20746D66u)                      /* "fmt " */
        {
            uint32_t n = (csz < 40u) ? csz : 40u;

            fr = f_read(&s_f, hdr, n, &br);
            if (fr != FR_OK || br < 16u)             { return -6; }
            *ch   = rd_le16(hdr + 2);
            *rate = rd_le32(hdr + 4);
            *bits = rd_le16(hdr + 14);
        }
        else if (cid == 0x61746164u)                 /* "data" */
        {
            *data_off = pos + 8u;
            *data_len = csz;
            break;                                   /* 拿到 data 就够了 */
        }
        pos += 8u + csz;
        if ((csz & 1u) != 0u)
        {
            pos += 1u;                               /* chunk 按偶数字节对齐，必要时补 1 */
        }
        if (pos > (uint32_t)f_size(&s_f))            { return -7; }
    }

    if (*bits != 16u)                                { return -8; }   /* 本版只支持 16 bit PCM */
    if (*ch != 1u && *ch != 2u)                      { return -9; }
    if (*rate == 0u)                                 { return -10; }
    return 0;
}

/* ====================== feeder：audio_out 的喂数回调 ======================= */

static int feeder_cb(int16_t *dst, uint32_t frames)
{
    UINT    br = 0;
    FRESULT fr;
    uint32_t want, avail;
    uint32_t t0 = HAL_GetTick();

    if (s_opened == 0u)
    {
        return -1;
    }

    avail = (g_wav_total > g_wav_pos) ? (g_wav_total - g_wav_pos) : 0u;

    if (avail == 0u)
    {
        return 0;                       /* 播完了 */
    }

    if (g_wav_channels == 1u)
    {
        /* 单声道：读 frames 个半字，展开成 L=R 的交错立体声。 */
        uint32_t n = frames;

        if (n > avail / 2u)
        {
            n = avail / 2u;
        }
        fr = f_read(&s_f, s_mono, n * 2u, &br);
        if (fr != FR_OK)
        {
            g_wav_underrun++;
            return -1;
        }
        want = br / 2u;
        for (uint32_t i = 0; i < want; ++i)
        {
            dst[2u * i]     = s_mono[i];
            dst[2u * i + 1] = s_mono[i];
        }
        /* 尾巴不足一个半缓冲时，超出部分填静音凑够帧数（audio_out 也会再补一次 0）。 */
        for (uint32_t i = want; i < frames; ++i)
        {
            dst[2u * i]     = 0;
            dst[2u * i + 1] = 0;
        }
        g_wav_pos += br;
        g_wav_read_ms = HAL_GetTick() - t0;
        return (int)want;
    }
    else
    {
        /* 立体声：文件里本身就是交错的，直接读。 */
        uint32_t nb = frames * 4u;

        if (nb > avail)
        {
            nb = avail;
        }
        fr = f_read(&s_f, dst, nb, &br);
        if (fr != FR_OK)
        {
            g_wav_underrun++;
            return -1;
        }
        g_wav_pos += br;
        g_wav_read_ms = HAL_GetTick() - t0;
        return (int)(br / 4u);
    }
}

int wav_play_start(const char *path)
{
    int rc;
    uint32_t off, len, rate, bits, ch;

    wav_play_stop();

    if (path == NULL || path[0] == 0)
    {
        g_wav_rc = -1;
        return -1;
    }
    if (fatfs_ensure_mounted() != 0)
    {
        g_wav_rc = -2;                 /* 卡没挂载：文件读不了，别赖 WAV 解析 */
        return -2;
    }

    if (f_open(&s_f, path, FA_READ) != FR_OK)
    {
        g_wav_rc = -3;
        return -3;
    }
    s_opened = 1u;

    rc = wav_read_header(&off, &len, &rate, &bits, &ch);
    if (rc != 0)
    {
        f_close(&s_f);
        s_opened = 0u;
        g_wav_rc = -10 + rc;           /* -11..-20 区间：头解析失败的细分码 */
        return rc;
    }
    g_wav_rate     = rate;
    g_wav_bits     = bits;
    g_wav_channels = ch;
    g_wav_total    = len;
    g_wav_pos      = 0u;
    s_data_off     = off;        /* 记住 data chunk 起点，供 seek 换算绝对偏移 */

    path_copy(s_path, (int)sizeof(s_path), path);

    if (f_lseek(&s_f, off) != FR_OK)
    {
        f_close(&s_f);
        s_opened = 0u;
        g_wav_rc = -21;
        return -21;
    }

    audio_out_set_volume((uint8_t)g_audio_volume);
    audio_out_set_feeder(feeder_cb);
    if (audio_out_start() != 0)
    {
        f_close(&s_f);
        s_opened = 0u;
        g_wav_rc = -22;
        return -22;
    }

    g_wav_state = 1u;
    g_wav_rc    = 0;
    return 0;
}

void wav_play_stop(void)
{
    audio_out_stop();
    audio_out_set_feeder(NULL);
    if (s_opened != 0u)
    {
        (void)f_close(&s_f);
        s_opened = 0u;
    }
    g_wav_state = 0u;
    g_wav_pos   = 0u;
}

int wav_play_seek_ms(uint32_t ms)
{
    uint32_t bps;
    uint32_t frame;
    uint64_t b;

    /* 必须文件已打开且头已解析，否则没法把毫秒换算成字节偏移。 */
    if (s_opened == 0u || s_data_off == 0u ||
        g_wav_total == 0u || g_wav_rate == 0u || g_wav_channels == 0u)
    {
        return -1;
    }

    bps   = g_wav_rate * g_wav_channels * 2u;   /* 每秒字节数（16 bit PCM）*/
    frame = g_wav_channels * 2u;                /* 一帧字节数（对齐用）*/
    if (bps == 0u || frame == 0u)
    {
        return -1;
    }

    /* 目标数据字节（相对 data chunk 起点）。 */
    b = (uint64_t)ms * (uint64_t)bps / 1000u;
    if (b >= (uint64_t)g_wav_total)
    {
        /* 别正好落在末尾 —— 否则会被"播完"判据立刻收尾。留一帧余量。 */
        b = (g_wav_total >= frame) ? ((uint64_t)g_wav_total - frame) : 0u;
    }
    b -= (b % frame);                            /* 对齐到帧边界，避免半样本爆音 */

    if (f_lseek(&s_f, s_data_off + (uint32_t)b) != FR_OK)
    {
        return -1;
    }
    g_wav_pos = (uint32_t)b;                     /* 同步进度量，elapsed/total 立刻反映新位置 */
    return 0;
}

void wav_play_tick(void)
{
    audio_out_tick();

    /* 诊断触发：写 g_wav_test=1 起播固定路径；=2 停播（验收用，见文件头注释）。 */
    if (g_wav_test == 1u)
    {
        g_wav_test = 0u;
        (void)wav_play_start("/YMGUI/MUSIC/TOKYOHOT.WAV");
    }
    else if (g_wav_test == 2u)
    {
        g_wav_test = 0u;
        wav_play_stop();
    }

    /* 播完（数据读完且最后一个半缓冲也出去了）就收尾。
     * ⚠ 判据用"文件读到尾"，不是"某帧数为 0" —— 后者在卡慢时会误停。 */
    if ((g_wav_state == 1u) && (s_opened != 0u) &&
        (g_wav_pos >= g_wav_total) && (g_wav_pos != 0u))
    {
        /* 让最后一截缓冲播完再停：等音频侧的 DMA 把剩余部分走一轮。
         * 这里用"已读字节 == 总字节 且 已经又过了约一个半缓冲的时间"粗略判断。 */
        static uint32_t s_tail_wait;

        /* 半缓冲是 42 ms，主循环一拍约 16 ms ⇒ 至少等 3 拍，取 4 拍留余量。
         * 期间 feeder 一直返回"没数据"，audio_out 补静音 ⇒ 尾巴不会爆音。 */
        s_tail_wait++;
        if (s_tail_wait > 4u)
        {
            s_tail_wait = 0u;
            wav_play_stop();
        }
    }
}

int wav_play_busy(void)
{
    return (g_wav_state != 0u) ? 1 : 0;
}

uint32_t wav_play_elapsed_ms(void)
{
    uint32_t bytes_per_sec = g_wav_rate * g_wav_channels * 2u;

    if (bytes_per_sec == 0u)
    {
        return 0u;
    }
    return (uint32_t)(((uint64_t)g_wav_pos * 1000u) / bytes_per_sec);
}

uint32_t wav_play_total_ms(void)
{
    uint32_t bytes_per_sec = g_wav_rate * g_wav_channels * 2u;

    if (bytes_per_sec == 0u)
    {
        return 0u;
    }
    return (uint32_t)(((uint64_t)g_wav_total * 1000u) / bytes_per_sec);
}

/* ====================== board 层：音乐目录扫描 =======================
 * 与 BoardPic_* / BoardNote_* 完全同构：扫一次、之后只读缓存表。
 * ⚠ 与照片共用 img_scan_* 那张表：同一时刻只服务一个分类，所以**不要**
 *   在相册/文件管理器已经开着的时候调这里（它们进页面时会自己重扫）。 */

int BoardMusic_Scan(void)
{
    (void)img_scan_mkdir(IMG_MUSIC_DIR);
    return img_scan_dir(IMG_MUSIC_DIR, ".wav", 3);
}

int BoardMusic_Count(void)
{
    return img_scan_count();
}

int BoardMusic_Name(int i, char *out, int n)
{
    return img_scan_name(i, out, n);
}

int BoardMusic_Size(int i, uint32_t *size)
{
    return img_scan_size(i, size);
}

int BoardMusic_Play(int i)
{
    char path[IMG_PATH_MAX];

    if (img_scan_path(i, IMG_MUSIC_DIR, path, (int)sizeof(path)) != 0)
    {
        return -1;
    }
    if (wav_play_start(path) != 0)
    {
        return -1;
    }
    return 0;
}

void BoardMusic_Stop(void)
{
    wav_play_stop();
}

int BoardMusic_Playing(void)
{
    return wav_play_busy();
}

/* 上一次播放动作的返回码（UI 显示"起播失败 rc=N"用），取值表见 wav_play.h。 */
int BoardMusic_LastRc(void)
{
    return (int)g_wav_rc;
}

uint32_t BoardMusic_ElapsedMs(void)
{
    return wav_play_elapsed_ms();
}

uint32_t BoardMusic_TotalMs(void)
{
    return wav_play_total_ms();
}

int BoardMusic_Seek(uint32_t ms)
{
    return wav_play_seek_ms(ms);
}
