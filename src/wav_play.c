/* ===========================================================================
 * WAV 播放源（TF 卡 → I2S）— 设计说明见 wav_play.h
 * =========================================================================== */
#include "wav_play.h"
#include "audio_out.h"
#include "es8311.h"           /* 相位锁存要读全部 I2C 诊断量（g_es_*）*/
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

/* 取路径最后一段（文件名）——桌面卡片的"正在播放"标题用。 */
static const char *path_basename(const char *p)
{
    const char *b = p;

    if (p == NULL)
    {
        return "";
    }
    for (; *p != 0; ++p)
    {
        if (*p == '/')
        {
            b = p + 1;
        }
    }
    return b;
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

/* ★ 补丁15d：板上执行的「暂停 → N ms → 续播」突发，专门用来测**快速连点**那条幂等分支。
 *
 * 【为什么必须放板上】`probe-rs write` 每次都要 spawn 一个进程，实测单次 ≈ 400 ms；
 *   而暂停淡出窗口（静音确认后 ≥ AO_PAUSE_MIN_RAMP_MS = 300 ms）比主机两次写之间的
 *   真实间隔更短/相当 ⇒ 主机侧难以稳定落入 `audio_out_start()` 的幂等分支
 *   ⇒ 主机两次写之间的真实间隔**必然**大于淡出窗口 ⇒ 永远落不进 `audio_out_start()`
 *   的幂等分支（`tools/rapid_stop_start_check.py` 实测 5/5 轮"空测"就是这个原因，
 *   不是固件缺陷）。把整段交给板子做，间隔才真的可控。
 * 用法：写 `g_wav_test = 5`；板子下一拍 `wav_play_pause()`，`g_wav_burst_ms` 毫秒后
 *   `wav_play_resume()`，成功则 `g_wav_burst_done++`。判据看 `g_ao_idem_cancel` 增量。 */
volatile uint32_t g_wav_burst_ms   = 30u;   /* ★ 补丁15d：暂停→续播的间隔（毫秒）*/
volatile uint32_t g_wav_burst_done = 0u;    /* ★ 补丁15d：板上突发成功完成的次数 */
static uint8_t    s_burst_stage    = 0u;
static uint32_t   s_burst_deadline = 0u;


/* ====================== 验收用「相位锁存」（2026-10-13）======================
 * 【为什么要有它】版本迭代到补丁 8 后，验收脚本要在"暂停 → 续播"之后的稳态读
 *   十几个诊断量。每个量一次 `probe-rs read` = **一次内核 halt**；而 I2C 是阻塞式
 *   轮询，halt 期间外设不停 ⇒ 芯片应答窗口被错过 ⇒ 读到一堆**假 NACK**（"失败风暴"）。
 *   实测：复位后静置再起播、全程零探针 = 0 失败；密集逐符号读 = 47% 失败。
 *   ⇒ 采样方式本身就是伪影源。修法是把"测量"从主机搬到板上：
 *
 *   主机写 g_wav_snap_req = <相位码>；板子在**下一拍 tick**（绝对无害的上下文）执行
 *   该相位的动作，然后**在同一个函数里**把全部诊断量锁进一张 RAM 表 g_wav_snap[]。
 *   主机只做 **一次** 批量读（probe-rs read -f binary，窗口 ≤ 几百字节）⇒ 一次停机
 *   拿到整张表。表内各量是"同一时刻"的，连时间一致性都顺带解决了。
 *
 *   相位码：1=起播  3=暂停  4=续播  2=停止。与 g_wav_test 同义，但动作后自动锁存。
 *   g_wav_snap[0] 锁存相位码、[1] 锁存"动作完成时的循环计数"，其余见 wav_play.h 索引表。 */
volatile uint32_t g_wav_snap_req = 0u;
volatile uint32_t g_wav_snap[G_WAV_SNAP_N] = {0};
volatile uint32_t g_wav_snap_delay = 40u;     /* 动作后延迟多少拍再锁存（≈0.64 s；主机可改小做瞬态）*/
static uint32_t   s_snap_phase = 0u;
static uint32_t   s_snap_delay = 0u;

extern volatile uint32_t g_loop_count;        /* main.c：主循环拍计数 */

/* 把当前所有诊断量锁进 g_wav_snap[]。索引表与 wav_play.h 的宏一一对应，**顺序不可改**
 * （主机按固定索引解析）。只做 32 位读，不调任何 IO ⇒ 在 tick 里调用绝对安全。
 *
 * 【防"读到半张表"】用 seqlock 手法：进函数先把 G_SNAP_PHASE 填成 `phase | 0x8000_0000`
 *   （"正在写"），全部写完、魔数也落了之后，再把 G_SNAP_PHASE 写回纯 `phase`。
 *   主机只接受"G_SNAP_PHASE == 期望相位码且 bit31 == 0"的表 ⇒ 半张表必被拒。 */
static void wav_snap_latch(uint32_t phase)
{
    g_wav_snap[G_SNAP_PHASE]    = phase | 0x80000000u;  /* seqlock：标记"正在写" */
    g_wav_snap[G_SNAP_LOOP]     = g_loop_count;          /* 板载拍计数（判"动作后过了几拍"）*/
    g_wav_snap[G_SNAP_WSTATE]   = g_wav_state;
    g_wav_snap[G_SNAP_WRC]      = (uint32_t)g_wav_rc;
    g_wav_snap[G_SNAP_WPOS]     = g_wav_pos;
    g_wav_snap[G_SNAP_WTOTAL]   = g_wav_total;
    g_wav_snap[G_SNAP_AOPLAY]   = g_ao_playing;
    g_wav_snap[G_SNAP_AOREADY]  = g_ao_dac_ready;
    g_wav_snap[G_SNAP_AORAMP]   = g_ao_dac_ramp;
    g_wav_snap[G_SNAP_AOWAKE]   = g_ao_wake_cnt;
    g_wav_snap[G_SNAP_AOSKIP]   = g_ao_init_skips;
    g_wav_snap[G_SNAP_AOINITRC] = (uint32_t)g_ao_init_rc;
    g_wav_snap[G_SNAP_AORC]     = (uint32_t)g_ao_rc;
    g_wav_snap[G_SNAP_ESWROK]   = g_es_wr_ok;
    g_wav_snap[G_SNAP_ESWRNG]   = g_es_wr_ng;
    g_wav_snap[G_SNAP_ESRDOK]   = g_es_rd_ok;
    g_wav_snap[G_SNAP_ESRDNG]   = g_es_rd_ng;
    g_wav_snap[G_SNAP_ESRETRY]  = g_es_retry_ok;
    g_wav_snap[G_SNAP_ESFAIL]   = g_es_fail;
    g_wav_snap[G_SNAP_ESFIX]    = g_es_fix_wr;
    g_wav_snap[G_SNAP_ESRB]     = g_es_readback;
    g_wav_snap[G_SNAP_ESPWR]    = g_ao_wake_retry;
    g_wav_snap[G_SNAP_ESREG32N] = g_es_reg32_n;
    g_wav_snap[G_SNAP_ESREG32W] = g_es_reg32_wr;
    g_wav_snap[G_SNAP_ESRBVOL]  = g_es_rb_vol;
    g_wav_snap[G_SNAP_ESRBPWR]  = g_es_rb_pwr;
    g_wav_snap[G_SNAP_ESRB0E]   = g_es_rb_dac0e;
    g_wav_snap[G_SNAP_ESWNCUR]  = g_es_ng_reg;           /* 最近一次写失败的寄存器号 */
    g_wav_snap[G_SNAP_ESWNS]    = g_es_ng_st;            /* 该次 HAL 状态码 1=NACK 2=BUSY 3=TMO */
    g_wav_snap[G_SNAP_ESNAK]    = g_es_ng_nack;
    g_wav_snap[G_SNAP_ESBUSY]   = g_es_ng_busy;
    g_wav_snap[G_SNAP_ESTMO]    = g_es_ng_tmo;
    g_wav_snap[G_SNAP_ESNGHEAD] = g_es_ng_head;
    g_wav_snap[G_SNAP_ESNGIDR]  = g_es_ng_idr;
    g_wav_snap[G_SNAP_VOLUI]    = g_audio_volume;
    g_wav_snap[G_SNAP_DMARSH]   = g_ao_dma_half;
    g_wav_snap[G_SNAP_DMARCP]   = g_ao_dma_cplt;
    g_wav_snap[G_SNAP_DMARERR]  = g_ao_dma_err;
    g_wav_snap[G_SNAP_FRAMES]   = g_ao_frames;
    g_wav_snap[G_SNAP_ESPRETRY] = g_es_probe_retry;      /* probe 首试没探到、延时后探到 */
    g_wav_snap[G_SNAP_PAUSECALL] = g_ao_pause_calls;     /* ★ 暂停动作次数（动作级判据）*/
    g_wav_snap[G_SNAP_PAUSEWR]   = g_ao_pause_volwr;     /* ★ 暂停动作写 REG32 的笔数 */
    g_wav_snap[G_SNAP_RESUMECALL]= g_ao_resume_calls;    /* ★ 起播/续播动作次数 */
    g_wav_snap[G_SNAP_MAGIC]    = G_WAV_SNAP_MAGIC;      /* 最后写魔数 ⇒ 主机凭它判"表是完整的" */
    g_wav_snap[G_SNAP_PHASE]    = phase;                 /* seqlock：清除"正在写"标记 */
}

static FIL  s_f;
static char s_path[IMG_PATH_MAX];
static char s_cur_name[48];   /* 当前曲目显示名（路径末段）——桌面卡片"正在播放"标题用 */
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

    audio_out_set_volume((uint8_t)es8311_ui_volume_effective());
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
    /* 记当前曲目名（路径末段），供桌面卡片实时显示"正在播放"标题。 */
    path_copy(s_cur_name, (int)sizeof(s_cur_name), path_basename(path));
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
    s_cur_name[0] = 0;            /* 停止后没有"当前曲目" ⇒ 桌面卡片回落到占位文案 */
}

/* 暂停：停出音，但**保留文件句柄与 g_wav_pos**（与 stop 的区别就在这儿）。
 * 续播时 feeder 仍从当前文件位置接着读 ⇒ 位置不丢、可从暂停处继续。 */
int wav_play_pause(void)
{
    if (g_wav_state != 1u)
    {
        return -1;                     /* 没在出声，没什么可暂停 */
    }
    audio_out_pause();                 /* ★ 只停出音并静音 codec，**不断电**（见 audio_out_pause
                                        * 的说明：stop 会给 codec 断电，续播撞 NACK 窗口 ⇒ 丢写）。
                                        * s_f 保持打开、g_wav_pos 不动，故位置可续。 */
    g_wav_state = 2u;                  /* 2 = 暂停 */
    return 0;
}

/* 续播：从暂停处（或暂停后拖动到的新位置）重新起播。
 * audio_out_start 会重新铺环缓冲 —— 而 feeder 读的是 s_f 的当前偏移，正是我们要的位置。 */
int wav_play_resume(void)
{
    if (g_wav_state != 2u || s_opened == 0u)
    {
        return -1;
    }
    audio_out_set_volume((uint8_t)es8311_ui_volume_effective());
    audio_out_set_feeder(feeder_cb);   /* 保险：确认喂的仍是本文件 */
    if (audio_out_start() != 0)
    {
        g_wav_state = 2u;              /* 起不来就维持暂停态，别谎报在播 */
        return -1;
    }
    g_wav_state = 1u;
    return 0;
}

int wav_play_paused(void)
{
    return (g_wav_state == 2u) ? 1 : 0;
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

    /* 诊断触发：写 g_wav_test=1 起播固定路径；=2 停播（验收用，见文件头注释）。
     * 3 = 暂停 / 4 = 续播：与 app 的"暂停→播放"按钮**走同一条** wav_play_pause/resume，
     * 供 tools/wav_pause_resume_check.py 用 SWD 复现"续播没声音"那条链路。 */
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
    else if (g_wav_test == 3u)
    {
        g_wav_test = 0u;
        (void)wav_play_pause();
    }
    else if (g_wav_test == 4u)
    {
        g_wav_test = 0u;
        (void)wav_play_resume();
    }
    /* ★ 补丁15d：板上「暂停 → g_wav_burst_ms → 续播」突发（测快速连点的幂等分支）。
     * 分两拍做：本拍发起暂停并记截止时刻，之后每拍检查 → 到点续播。
     * 中间那段**主循环照常跑** ⇒ 淡出 tick 正常推进，与真人连点的时序一致。 */
    else if (g_wav_test == 5u)
    {
        g_wav_test = 0u;
        if (wav_play_pause() == 0)
        {
            s_burst_stage    = 1u;
            s_burst_deadline = HAL_GetTick() + g_wav_burst_ms;
        }
    }
    /* ★ 补丁16（2026-10-16）：g_wav_test = 6 —— 在**停止/暂停态**把当前 UI 音量写进芯片 REG32。
     * 【用途】忠实模拟"用户在没有播放的时候拖过音量滑杆"：main.c 的 app_volume_persist()
     *   正是直接调 es8311_set_volume_ui(g_audio_volume) —— 这是"起播前 REG32 非 0"的
     *   **唯一现实入口**（也是旧版 wav_play_start() 提前写音量的等价场景）。
     *   它让"起播安全栅（g_ao_start_barrier_en）"可以在同一固件上做 A/B：
     *     关掉栅 ⇒ 起播时 g_ao_vol_at_cstart 读到满音量值（爆音条件成立）；
     *     开着栅 ⇒ 读到 0x00。 */
    else if (g_wav_test == 6u)
    {
        g_wav_test = 0u;
        (void)es8311_set_volume_ui(es8311_ui_volume_effective());
    }
    if ((s_burst_stage == 1u) && ((int32_t)(HAL_GetTick() - s_burst_deadline) >= 0))
    {
        s_burst_stage = 0u;
        if (wav_play_resume() == 0)
        {
            g_wav_burst_done++;
        }
    }

    /* 验收"相位锁存"（见文件头说明）：主机写 g_wav_snap_req=<相位码>，这里在**下一拍**
     * 执行同一套动作，随后等 g_wav_snap_delay 拍（默认 40 拍 ≈ 0.64 s，够起播那段
     * "下一拍唤醒 DAC + 8 拍 ramp" 走完）再锁存全部诊断量。主机一次批量读即可拿到
     * "同一时刻、零停机干扰"的完整现场 —— 取代过去"逐符号读十几次"的伪影源。
     *   相位码：1=起播 2=停止 3=暂停 4=续播 9=不动作、只锁存（量 frames 增长用）。
     * ⚠ 放在 g_wav_test 之后、播完收尾之前：此时 audio_out_tick() 已跑完这一拍。 */
    if (g_wav_snap_req != 0u)
    {
        uint32_t phase = g_wav_snap_req;

        g_wav_snap_req = 0u;
        switch (phase)
        {
        case 1u: (void)wav_play_start("/YMGUI/MUSIC/TOKYOHOT.WAV"); break;
        case 2u: wav_play_stop();            break;
        case 3u: (void)wav_play_pause();     break;
        case 4u: (void)wav_play_resume();    break;
        case 9u: break;                      /* 只锁存，不动播放 */
        default: phase = 0u; break;
        }
        if (phase != 0u)
        {
            s_snap_phase = phase;
            s_snap_delay = (g_wav_snap_delay != 0u) ? g_wav_snap_delay : 40u;
        }
    }
    if (s_snap_delay != 0u)
    {
        if (--s_snap_delay == 0u)
        {
            wav_snap_latch(s_snap_phase);
        }
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
    return (g_wav_state == 1u) ? 1 : 0;   /* 仅"正在出声"算在播；暂停(2)/停止(0) = 0 */
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

/* 第 i 首的完整路径（补丁30）：文件管理器「音乐」分类里点一下就交给音乐 App 播，
 * 需要的是绝对路径。UI 不许自己拼 "/YMGUI/MUSIC/"（分层：目录常量只在 src 里）。 */
int BoardMusic_Path(int i, char *out, int n)
{
    return img_scan_path(i, IMG_MUSIC_DIR, out, n);
}

/* 删除第 i 首（补丁30，与 BoardNote_Delete 同一套：真删 + 立刻重扫）。
 * 返回：0=成功 2=下标无效 -1=卡未挂载 -3=路径拼接失败 -4=f_unlink 失败。 */
int BoardMusic_Delete(int i)
{
    char    full[IMG_PATH_MAX];
    FRESULT fr;

    if (i < 0 || i >= img_scan_count()) { return 2; }
    if (fatfs_ensure_mounted() != 0)    { return -1; }
    if (img_scan_path(i, IMG_MUSIC_DIR, full, (int)sizeof(full)) != 0) { return -3; }

    fr = f_unlink(full);
    if (fr != FR_OK) { return -4; }

    (void)BoardMusic_Scan();        /* 重建扫描表（下标会变，UI 必须重新取值）*/
    return 0;
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

/* 播放**任意路径**的 WAV（补丁29）：音乐 App 被文件管理器用"播这段录音"唤起时用。
 * 与 BoardMusic_Play(i) 的唯一区别是**不经过 /YMGUI/MUSIC 的扫描表** ——
 * 录音在 /YMGUI/RECORD，索引语义完全不同，硬套下标只会播错文件。
 * 播放链路完全一致（同一个 wav_play_start ⇒ 起播安全栅 / 音量 ramp / DMA 路径都复用）。 */
int BoardMusic_PlayFile(const char *path)
{
    if (path == NULL || path[0] == 0)
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

int BoardMusic_Pause(void)
{
    return wav_play_pause();
}

int BoardMusic_Resume(void)
{
    return wav_play_resume();
}

int BoardMusic_Paused(void)
{
    return wav_play_paused();
}

/* 当前（在播/暂停）曲目的显示名 —— 桌面卡片"正在播放"标题用。
 * 停止（state==0）后没有当前曲目 ⇒ 返回 <0 且 out 置空串。 */
int wav_play_current_name(char *out, int n)
{
    if (out == NULL || n <= 0)
    {
        return -1;
    }
    if (g_wav_state == 0u || s_cur_name[0] == 0)
    {
        out[0] = 0;
        return -1;
    }
    path_copy(out, n, s_cur_name);
    return 0;
}

int BoardMusic_CurrentName(char *out, int n)
{
    return wav_play_current_name(out, n);
}
