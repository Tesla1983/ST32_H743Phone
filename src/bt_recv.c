/* ===========================================================================
 * 蓝牙文件接收会话（P4，2026-10-10）—— 实现
 * 设计说明、序号/重传语义、判据见 bt_recv.h 顶部。
 *
 * 一句话流程：
 *   发 $?BTF,OPEN → 等 $!RS,BTF,1 → 开 TF 文件 →（循环：发 $?BTF,GET,<seq>
 *   → 等 $!BD 收齐 → CRC 过就写卡、seq++ ；不过就重拉同一 seq）
 *   → 对端推 $BT state=3 且本板字节数够了 ⇒ 关文件、改名、导入图库。
 * =========================================================================== */

#include "bt_recv.h"
#include "uart_link.h"
#include "img_store.h"
#include "fatfs_port.h"
#include "spi_link.h"

#include "ff.h"
#include <string.h>

/* ---- 失败阶段（g_bt_err）---- */
#define BTE_NONE     0u
#define BTE_OPEN     1u   /* $?BTF,OPEN 被对端拒绝（BT 起不来 / 链路不通）*/
#define BTE_OPEN_TO  2u   /* OPEN 发了没人应 */
#define BTE_SINK     3u   /* 开不了落地文件（挂载失败 / 建不出目录 / f_open 失败）*/
#define BTE_WRITE    4u   /* f_write 短写 */
#define BTE_STALL    5u   /* 长时间没有任何进展 */
#define BTE_BADSEQ   6u   /* 序号对不上 / 重取次数耗尽（中间丢过整块）*/

/* ---- 节奏（毫秒）----
 * ⚠ 为什么"刚收到一块就立刻拉下一块"也够安全：GET 本身要走 SPI 命令环
 *   （约 1 笔事务），加上对端任务调度，往返本来就有十几毫秒的天然间隔，
 *   不会把对端打爆。真正需要慢下来的是"对端说暂时没数据"那一支 —— 那说明
 *   手机还没发过来，hammer 它只是白跑 SPI 事务，所以隔 25 ms 再问。 */
#define OPEN_ACK_MS      4000u
#define OPEN_TRY_MAX     3u
#define GET_ACK_MS       300u    /* 发了 GET 之后**一个下行字节都没有**多久 ⇒ 判定 GET 丢了，重发 */
#define GET_ACK_BUSY_MS  1500u   /* 有字节在下行（块正在搬）时的上限：只慢不重发，见 ⑦ 注释 */
#define GET_GAP_MS       2u
#define GET_IDLE_GAP_MS  25u
#define STALL_MS         12000u  /* 这么久没有任何进展（块/响应/状态帧）⇒ 判死 */
#define WAIT_MAX_MS      180000u /* "一个字节都还没收到"时的**总时长**上限（3 分钟），
                                  * 见下面 ⑥ 的说明。运行期可写（g_bt_wait_max_ms）。 */
#define GET_TRY_MAX      8u      /* 同一序号连续无效多少次后放弃 */

#define BT_NAME_FALLBACK "bt_recv.bmp"

/* ---- 会话状态 ---- */
static uint32_t s_state;        /* BT_RECV_* */
static uint32_t s_opening;      /* 1 = 已发 OPEN/TEST，还在等受理回执 */
static uint32_t s_seq;          /* 下一块应有的序号 */
static uint32_t s_recv;         /* 已写进 TF 的字节数 */
static uint32_t s_total;        /* 对端声明的总长（0 = 未知）*/
static uint32_t s_retry;        /* 当前序号的连续无效次数 */
static uint32_t s_need_get;     /* 1 = 该发 GET 了 */
static uint32_t s_get_out;      /* 1 = GET 已发出、还没等到任何应答 */
static uint32_t s_get_t0;       /* GET 发出的时刻 */
static uint32_t s_next_get;     /* 允许下一次 GET 的时基 */
static uint32_t s_t0;           /* 最近一次"有进展"的时基（判 STALL）*/
static uint32_t s_open_try;
static uint32_t s_now;          /* 最近一拍的时基（bt_recv_start 没带时基，用它）*/
static uint32_t s_t_start;      /* 会话开始的时基（算墙钟时长，见 g_bt_ms）*/
static uint32_t s_get_to_run;   /* 连续"GET 发了没人回"的笔数（算 g_bt_get_max）*/
static uint32_t s_rx_base;      /* 发 GET 那一刻的 g_spi_rx_bytes（判"到底有没有字节在下行"）*/
/* 已发出的那条请求（用于超时重发：OPEN 与 TEST 参数不同，得原样再发一遍） */
static uint32_t s_req_test;
static uint32_t s_req_w;
static uint32_t s_req_h;

/* 解析层计数基线：判"自上次以来有没有新东西" */
static uint32_t s_done_base;    /* g_cmd_bd_done */
static uint32_t s_rs_base;      /* g_cmd_btf_rs */
static uint32_t s_bt_base;      /* g_net_bt_pkts */

/* 落地文件 */
static FIL      s_fil;
static uint32_t s_fopened;
static char     s_name[IMG_SCAN_NAME_MAX];   /* 不含目录 */
static char     s_path[IMG_PATH_MAX];        /* 完整路径 */

/* ---- 诊断量定义 ---- */
volatile uint32_t g_bt_sess;
volatile uint32_t g_bt_chunks;
volatile uint32_t g_bt_bytes;
volatile uint32_t g_bt_crc_bad;
volatile uint32_t g_bt_retx;
volatile uint32_t g_bt_get_n;
volatile uint32_t g_bt_done;
volatile int      g_bt_rc = -1;
volatile uint32_t g_bt_err;
volatile uint32_t g_bt_fs_rc;
volatile uint32_t g_bt_wr;
volatile uint32_t g_bt_imp_rc;
volatile uint32_t g_bt_spi_tx;
volatile uint32_t g_bt_ms;
volatile uint32_t g_bt_get_to;
volatile uint32_t g_bt_get_max;
/* "发了 GET 多久没回就当丢了"的等待时长，运行期可写（SWD 改一下即可扫档，
 * 不用重烧固件）。默认 = GET_ACK_MS。理由：丢一笔 GET 的代价就是白等这么久，
 * 实测链路仍有 ~10% 的命令/响应会丢，2 s 太贵；对端是从本地环形缓冲立刻回答的，
 * 几百毫秒足够。⚠ 必须满足 6 × g_bt_get_ack_ms < STALL_MS(12 s)，否则 STALL 会先触发。 */
volatile uint32_t g_bt_get_ack_ms = GET_ACK_MS;
/* "一个字节都还没收到"时的总时长上限（ms），运行期可写 —— 3 分钟太久，验收时要能调小。
 * 详见 poll 里 ⑥ 的注释：修掉 s_retry 语义之后，等待期不再靠重试判死，改用这个兜底。 */
volatile uint32_t g_bt_wait_max_ms = WAIT_MAX_MS;

/* ====================== 小工具 ====================== */

static void copy_str(char* dst, int cap, const char* src)
{
    int i = 0;
    if (dst == NULL || cap <= 0) return;
    if (src == NULL) src = "";
    while (src[i] != 0 && i < cap - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* 无符号十进制 → 字符串（只用于拼序号，不引 snprintf）。
 * 调用方保证 out 至少 11 字节。 */
static void u32_str(uint32_t v, char* out)
{
    char t[12];
    int  n = 0;
    if (v == 0u) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0u) { t[n++] = (char)('0' + (char)(v % 10u)); v /= 10u; }
    for (int i = 0; i < n; ++i) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* 拼 "<dir>/<name>"。dir 已带结尾 '/' 时不重复加。 */
static int join_path(const char* dir, const char* name, char* out, int cap)
{
    int k = 0;
    if (out == NULL || cap < 4) return -1;
    for (int i = 0; dir[i] != 0; ++i) { if (k >= cap - 1) return -1; out[k++] = dir[i]; }
    if (k > 0 && out[k - 1] != '/' && k < cap - 1) out[k++] = '/';
    for (int i = 0; name[i] != 0; ++i) { if (k >= cap - 1) return -1; out[k++] = name[i]; }
    out[k] = 0;
    return 0;
}

/* 把对端给的文件名净化为"能落 FAT32 的 ASCII 名"。
 * ⚠ 三条理由，缺一条都会出问题：
 *   ① 卡是 FAT32 + FF_CODE_PAGE=437（见 fatfs_port.h）：中文名与 PC 不互通，
 *      所以只留 [A-Za-z0-9._-]，其余一律换成 '_'；
 *   ② 必须挡掉 '/' 与 '\'、':' 这些**路径分隔/设备符** —— 名字里带分隔符
 *      就能把文件写到别的目录去（对端是个不可信输入源）；
 *   ③ 空名或全是非法字符时要能回退，不能拼出 "/YMGUI/PIC/" 这种目录路径
 *      去给 f_open（那会得到 FR_INVALID_NAME 或更糟）。 */
static void sanitize_name(const char* src, char* dst, int cap)
{
    int n = 0;
    if (dst == NULL || cap <= 0) return;
    if (src == NULL) src = "";
    for (const char* p = src; *p != 0 && n < cap - 1; ++p)
    {
        char c = *p;
        int  ok = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-');
        dst[n++] = ok ? c : '_';
    }
    dst[n] = 0;
}

/* 挑落地文件名。返回 1 = 用了对端给的真名；0 = 还不知道（用了回退名）。
 * ⚠ 对端把"没有名字"表示成单个 '-'（不是空串），必须单独挡掉，
 *   否则会落地一个叫 "-" 的文件。 */
static int pick_name(char* out, int cap)
{
    const char* src = (const char*)g_net_btf_name;

    if (src[0] == 0 || (src[0] == '-' && src[1] == 0))
    {
        copy_str(out, cap, BT_NAME_FALLBACK);
        return 0;
    }
    sanitize_name(src, out, cap);
    if (out[0] == 0 || out[0] == '.')
    {
        copy_str(out, cap, BT_NAME_FALLBACK);
        return 0;
    }
    /* 没扩展名就补 .bmp —— 文件管理器只扫 .bmp（BoardPic_Scan），
     * 补上才能"相册可见"；对端给的是 btrecv.bin 这类裸流名也走这条。 */
    if (strchr(out, '.') == NULL && (int)strlen(out) < cap - 4)
    {
        int k = (int)strlen(out);
        out[k++] = '.'; out[k++] = 'b'; out[k++] = 'm'; out[k++] = 'p'; out[k] = 0;
    }
    return 1;
}

/* ====================== 文件落地 ====================== */

static void close_sink(void)
{
    if (s_fopened)
    {
        (void)f_sync(&s_fil);
        g_bt_fs_rc = (uint32_t)f_close(&s_fil);
        s_fopened = 0u;
    }
}

/* 关掉并**删掉**落地文件（中止/出错时用：半截文件留在卡上会被相册扫到、
 * 导入必失败，看起来像"程序坏了"）。 */
static void discard_sink(void)
{
    close_sink();
    if (s_path[0] != 0)
    {
        (void)f_unlink(s_path);
    }
}

static int open_sink(void)
{
    FRESULT fr;

    if (fatfs_ensure_mounted() != 0) { g_bt_err = BTE_SINK; return -1; }
    if (img_scan_mkdir(IMG_PIC_DIR) != 0) { g_bt_err = BTE_SINK; return -1; }

    (void)pick_name(s_name, (int)sizeof(s_name));   /* 此刻多半还没有真名 */
    if (join_path(IMG_PIC_DIR, s_name, s_path, (int)sizeof(s_path)) != 0)
    {
        g_bt_err = BTE_SINK; return -1;
    }

    /* CREATE_ALWAYS：同名旧文件直接覆盖。接收会话是可重入的自测/重传场景，
     * 用 CREATE_NEW 会在第二次跑时失败，反而难用。 */
    fr = f_open(&s_fil, s_path, FA_CREATE_ALWAYS | FA_WRITE);
    g_bt_fs_rc = (uint32_t)fr;
    if (fr != FR_OK)
    {
        g_bt_err = BTE_SINK;
        s_path[0] = 0;
        return -1;
    }
    s_fopened = 1u;
    return 0;
}

static int write_chunk(uint32_t len)
{
    UINT    bw = 0;
    FRESULT fr;
    /* g_cmd_body 是 volatile 的（脚本经 SWD 直接读它），显式转换剥掉限定，
     * 免得 -Wall 报 discarded-qualifiers。 */
    const uint8_t* src = (const uint8_t*)g_cmd_body;

    if (!s_fopened || len == 0u || len > UART_LINK_BODY_CAP) { g_bt_err = BTE_WRITE; return -1; }

    fr = f_write(&s_fil, src, (UINT)len, &bw);
    g_bt_fs_rc = (uint32_t)fr;
    g_bt_wr    = (uint32_t)bw;
    if (fr != FR_OK || bw != (UINT)len) { g_bt_err = BTE_WRITE; return -1; }

    s_recv    += len;
    g_bt_bytes = s_recv;
    g_bt_chunks++;
    return 0;
}

/* ====================== 状态切换 ====================== */

static void reset_session(void)
{
    s_state     = BT_RECV_IDLE;
    s_opening   = 0u;
    s_seq       = 0u;
    s_recv      = 0u;
    s_total     = 0u;
    s_retry     = 0u;
    s_need_get  = 0u;
    s_get_out   = 0u;
    s_open_try  = 0u;
    s_fopened   = 0u;
    s_name[0]   = 0;
    s_path[0]   = 0;
    /* 基线取"当前值"：这样后续任何一次自增都能被认出来。
     * ⚠ 必须在发命令**之前**取，否则可能把对端对本命令的响应当基线吞掉。 */
    s_done_base = g_cmd_bd_done;
    s_rs_base   = g_cmd_btf_rs;
    s_bt_base   = g_net_bt_pkts;
    s_t0        = s_now;
    s_next_get  = s_now;
}

static void fail(uint32_t stage)
{
    g_bt_err = stage;              /* 三处一起记，脚本任读一个都能定位 */
    g_bt_rc  = -(int)stage;
    g_bt_ms  = s_now - s_t_start;  /* 失败也记时长：区分"慢"和"卡死" */
    /* 会话开始存的是基线（g_spi_tx_n 快照），收尾这里把它换成**本轮增量**，
     * 脚本侧才能拿它和 ESP32 日志里的 `#N 事务累计` 直接对账 —— 两边差一半
     * 就说明主机发了但从机没看见（2026-10-10 靠这个差值定位了从机裸露窗口）。*/
    g_bt_spi_tx = g_spi_tx_n - g_bt_spi_tx;
    discard_sink();
    s_state   = BT_RECV_ERROR;
    s_opening = 0u;
}

static void finish_ok(void)
{
    char want[IMG_SCAN_NAME_MAX];

    close_sink();

    /* 会话中途才拿到真名 ⇒ 改名。对端在 OPEN 时还不知道名字（手机还没发文件头），
     * 所以落地的多半是回退名；GET 之后对端会把真名推过来，这时补上。
     * ⚠ f_rename 的第二个参数必须给出**同目录**的新路径。 */
    if (pick_name(want, (int)sizeof(want)) == 1 && strcmp(want, s_name) != 0)
    {
        char np[IMG_PATH_MAX];
        if (join_path(IMG_PIC_DIR, want, np, (int)sizeof(np)) == 0
            && f_rename(s_path, np) == FR_OK)
        {
            copy_str(s_name, (int)sizeof(s_name), want);
            copy_str(s_path, (int)sizeof(s_path), np);
        }
    }

    /* 收尾：排队导入图库（异步，由 img_store_poll 的 mode 1 执行）。
     * ⚠ 只有 .bmp/.jpg 导得进去；别的扩展名会在这里失败，rc 记进 g_bt_imp_rc。 */
    g_bt_imp_rc = (uint32_t)img_import_path(s_path);

    g_bt_done++;
    g_bt_rc  = 0;
    g_bt_ms  = s_now - s_t_start;   /* 会话墙钟（毫秒）—— 吞吐判据用它的，不用脚本墙钟：
                                     * 脚本每次 SWD 快照都会停核 ~6 s，把设备拖慢一个数量级，
                                     * 只有固件自己量的才是真值。 */
    /* 基线 → 本轮增量（对账用，理由同 fail()）*/
    g_bt_spi_tx = g_spi_tx_n - g_bt_spi_tx;
    s_state  = BT_RECV_DONE;
    s_opening = 0u;

    /* 通知对端会话结束（让它释放环形缓冲、停 SPP 服务端的"等数据"状态）。
     * 这一步失败无所谓：本模块已经不再跟随 $BT 了。 */
    (void)uart_link_cmd("?BTF", "ABORT");
}

/* ====================== 发命令 ====================== */

static void send_open(void)
{
    (void)uart_link_cmd("?BTF", "OPEN");
}

static void send_test(uint32_t w, uint32_t h)
{
    char body[32];
    char n1[12], n2[12];
    int  k = 0;

    u32_str(w, n1);
    u32_str(h, n2);
    for (const char* p = "TEST,"; *p != 0; ++p) body[k++] = *p;
    for (const char* p = n1; *p != 0; ++p)      body[k++] = *p;
    body[k++] = ',';
    for (const char* p = n2; *p != 0; ++p)      body[k++] = *p;
    body[k] = 0;

    (void)uart_link_cmd("?BTF", body);
}

/* 把"已发出的那条请求"再发一遍（超时重发用）。 */
static void resend_request(void)
{
    if (s_req_test) send_test(s_req_w, s_req_h);
    else            send_open();
}

static void send_get(uint32_t seq)
{
    char body[24];
    char n[12];
    int  k = 0;

    u32_str(seq, n);
    for (const char* p = "GET,"; *p != 0; ++p) body[k++] = *p;
    for (const char* p = n; *p != 0; ++p)      body[k++] = *p;
    body[k] = 0;

    (void)uart_link_cmd("?BTF", body);
    g_bt_get_n++;
}

/* ====================== 会话启动 ====================== */

static int begin(int test, uint32_t w, uint32_t h)
{
    /* 已经有会话在跑就不重叠（对端的会话是全局单例，重叠必然串数据） */
    if (s_state == BT_RECV_WAIT || s_state == BT_RECV_RECV) return -1;
    if (s_opening) return -1;
    /* 上一轮若还开着文件（异常路径），先收干净 */
    if (s_fopened) discard_sink();

    reset_session();
    g_bt_err    = BTE_NONE;
    g_bt_rc     = -1;
    g_bt_wr     = 0u;
    g_bt_imp_rc = 0xFFFFFFFFu;
    g_bt_spi_tx = g_spi_tx_n;        /* 起点：收尾时相减算本轮事务数 */
    g_bt_ms     = 0u;                /* 起点：收尾/判死时写"本会话墙钟" */
    g_bt_get_to = 0u;
    g_bt_get_max = 0u;
    s_t_start   = s_now;
    s_get_to_run = 0u;

    s_state   = BT_RECV_WAIT;
    s_opening = 1u;
    g_bt_sess++;

    s_req_test = (uint32_t)test;
    s_req_w    = w;
    s_req_h    = h;
    if (test) send_test(w, h);
    else      send_open();
    return 0;
}

int bt_recv_start(void)
{
    return begin(0, 0u, 0u);
}

int bt_recv_start_test(uint32_t w, uint32_t h)
{
    if (w < 8u || w > 1024u || h < 8u || h > 1024u) return -1;
    return begin(1, w, h);
}

/* 中止时把 `$?BTF,ABORT` 连发这么多遍。
 *
 * ⚠ 为什么是"连发"而不是"发一次 + 等确认"（2026-10-10 **真实路径**实测后定的）：
 *   实测中止后对端**根本没收到**这条命令（本板 `$BT帧` 不增、对端日志无 ABORT、
 *   对端 state 仍停在 1），因为 SPI 链路的命令通道有 6%~20% 的丢包，
 *   而 ABORT 和 GET 不同 —— GET 丢了有"无应答 300 ms 重发"兜着，
 *   ABORT 是一次性的，丢了就是丢了。
 *   ⇒ 连发 ABORT_N 遍，每遍都是独立的一笔事务，命中率 1−p^N
 *     （p=0.2 时 3 遍即 99.2%）。
 *
 * ⚠ 连发**绝对安全**，有两条独立保证：
 *   ① 对端 `bt_file_abort()` 在 `BT_FILE_IDLE` 时**直接 return**（幂等）；
 *   ② 更彻底的一条：对端 `bt_file_open()` 的**第一行就是 `session_reset()`**
 *      ⇒ 即使所有 ABORT 都丢了，下次「开始接收」的 OPEN 也会无条件清干净旧会话。
 *      （实测印证：那一轮 ABORT 全丢、对端 state 停在 1，紧接着再开会话仍然成功。）
 *   所以这里不做"带状态确认的重发相位"—— 那要给 poll 再加一个尾巴状态，
 *   而上面第 ② 条已经能自愈，代价与收益不成比例。 */
#define ABORT_N          3u

int bt_recv_abort(void)
{
    if (s_state == BT_RECV_IDLE) return -1;
    for (uint32_t i = 0u; i < ABORT_N; i++)
        (void)uart_link_cmd("?BTF", "ABORT");
    discard_sink();
    reset_session();
    g_bt_rc = -1;
    return 0;
}

/* ====================== 主循环 ====================== */

void bt_recv_poll(uint32_t now_ms)
{
    s_now = now_ms;

    /* ---- SWD 手工触发（验收脚本用）：g_bt_recv_req 写在 uart_link.h ---- */
    if (g_bt_recv_req != 0u)
    {
        uint32_t q = g_bt_recv_req;
        g_bt_recv_req = 0u;
        if (q == 1u)
        {
            uint32_t w = (g_bt_recv_w != 0u) ? g_bt_recv_w : 224u;
            uint32_t h = (g_bt_recv_h != 0u) ? g_bt_recv_h : 152u;
            (void)bt_recv_start_test(w, h);
        }
        else if (q == 2u)
        {
            (void)bt_recv_abort();
        }
    }

    if (s_state == BT_RECV_IDLE || s_state == BT_RECV_DONE || s_state == BT_RECV_ERROR)
        return;

    /* ---- ① 等 OPEN / TEST 的受理回执 ---- */
    if (s_opening)
    {
        if (g_cmd_btf_rs != s_rs_base)
        {
            s_rs_base = g_cmd_btf_rs;
            if (g_cmd_btf_rc > 0)
            {
                s_opening  = 0u;
                if (open_sink() != 0) { fail(BTE_SINK); return; }
                s_state    = BT_RECV_WAIT;
                s_retry    = 0u;
                s_need_get = 1u;
                s_next_get = now_ms;
                s_t0       = now_ms;
            }
            else
            {
                /* 对端明确拒绝（BT 起不来 / 尺寸不合法）。重试几次再判死 ——
                 * 无线电刚被打开时偶尔会返回忙。 */
                s_open_try++;
                if (s_open_try >= OPEN_TRY_MAX) { fail(BTE_OPEN); return; }
                s_t0 = now_ms;
                resend_request();
            }
        }
        else if ((now_ms - s_t0) > OPEN_ACK_MS)
        {
            /* 发了没人应：链路不通（或对端正在开蓝牙，那一步要几百毫秒）。 */
            s_open_try++;
            if (s_open_try >= OPEN_TRY_MAX) { fail(BTE_OPEN_TO); return; }
            s_t0 = now_ms;
            resend_request();
        }
        return;
    }

    /* ---- ② 块到了？（必须排在"读状态帧"之前：$BT 与最后一块可能落在同一拍）---- */
    if (g_cmd_bd_done != s_done_base)
    {
        uint32_t jump = g_cmd_bd_done - s_done_base;
        uint32_t id   = g_cmd_bd_id;
        uint32_t len  = g_cmd_bd_len;
        int      ok   = (g_cmd_bd_last_ok != 0);

        s_done_base = g_cmd_bd_done;
        s_get_out   = 0u;
        s_get_to_run = 0u;               /* 应答到了：连续无应答计数清零 */

        if (id != s_seq)
        {
            /* 不是我们等的块（$!BD 也被别的命令通道用着）。当作没发生，
             * 但要把 GET 重新排上 —— 那一笔的响应已经等不到了。 */
            s_need_get = 1u;
            s_next_get = now_ms + GET_GAP_MS;
        }
        else if (jump != 1u)
        {
            /* 一拍之内过了不止一块 ⇒ 中间那块已被后一块覆盖、拿不回来了。
             * 这一块也不收（收了字节流就断了），重取当前序号。 */
            g_bt_retx++;
            s_retry++;
            s_need_get = 1u;
            s_next_get = now_ms + GET_GAP_MS;
        }
        else if (!ok)
        {
            g_bt_crc_bad++;
            s_retry++;
            s_need_get = 1u;
            s_next_get = now_ms + GET_GAP_MS;
        }
        else
        {
            if (write_chunk(len) != 0) { fail(BTE_WRITE); return; }
            s_seq++;
            s_retry    = 0u;
            s_need_get = 1u;
            s_next_get = now_ms + GET_GAP_MS;
            s_t0       = now_ms;
            if (s_state == BT_RECV_WAIT) s_state = BT_RECV_RECV;
        }
    }

    /* ---- ③ $!RS,BTF,<rc> 响应 ---- */
    if (g_cmd_btf_rs != s_rs_base)
    {
        int rc;
        s_rs_base = g_cmd_btf_rs;
        rc        = g_cmd_btf_rc;
        s_get_out = 0u;
        s_get_to_run = 0u;               /* 应答到了 */
        s_t0      = now_ms;

        if (rc < 0)
        {
            /* 序号越界：对端没有这个序号的块了。
             * 对端同时报"完成" ⇒ 本板正好取完，正常收尾；
             * 否则是中间丢过整块、字节序列已经断了 ⇒ 会话失败（不覆水难收地导入半张图）。 */
            if (g_net_btf_state == BT_RECV_DONE) { finish_ok(); return; }
            fail(BTE_BADSEQ);
            return;
        }
        if (rc == 0)
        {
            /* 暂时没有数据（对端还在等手机发 / 环里暂时空）。隔一会儿再问。
             *
             * ⚠⚠ 这里必须把 `s_retry` 清零（2026-10-10 **真实路径**实测暴露的缺陷）：
             *   `s_retry` 的语义是"**同一个序号连续失败**多少次"（见 GET_TRY_MAX 注释），
             *   不是"开机以来总共失败了多少次"。而对端明确回 `$!RS,BTF,0` 说明
             *   **链路是活的、只是还没数据** —— 这必须算进"没有失败"，否则：
             *     用户点「开始接收」后还要去手机上配对 + 挑文件，几十秒很正常；
             *     这段**正常的等待期**里每一笔 GET 丢包都被记成失败，
             *     实测 12 s 内累积到 8 次就判 `BTE_BADSEQ` 死掉
             *     （板上实测：err=6 BADSEQ、get_n=322、**一个字节都还没传**）。
             *   清零之后，"8 次"才真的是"连续 8 次连一句话都没换来"（≈2.4 s）。
             *   等待期不再判死，改由下面的 g_bt_wait_max_ms 兜总时长。 */
            s_retry    = 0u;
            s_t0       = now_ms;          /* 对端活着（响应过），刷新"还有进展"时基 */
            s_need_get = 1u;
            s_next_get = now_ms + GET_IDLE_GAP_MS;
        }
        /* rc > 0：对端是"先数据后状态"，$!BD 随后就到，这里什么都不做。 */
    }

    /* ---- ④ $BT 状态帧（对端在推进会话）---- */
    if (g_net_bt_pkts != s_bt_base)
    {
        s_bt_base = g_net_bt_pkts;
        s_t0      = now_ms;                  /* 光有状态帧也算"还活着" */
        if (s_total == 0u && g_net_btf_size != 0u) s_total = g_net_btf_size;
    }
    if (s_total == 0u && g_net_btf_size != 0u) s_total = g_net_btf_size;

    /* ---- ⑤ 收尾判定 ---- */
    if (g_net_btf_state == BT_RECV_DONE)
    {
        if (s_total == 0u || s_recv >= s_total) { finish_ok(); return; }
        /* 对端说完成、本板还差字节 ⇒ 中间丢过整块。继续 GET 兜底，
         * 补不回来就由下面的 STALL 判死。 */
    }

    /* ---- ⑥ 判死 ---- */
    /* ① "一个字节都还没收到"时的**总时长**上限（2026-10-10 真实路径补）。
     *   为什么不能用 STALL_MS 顶：等待期里对端每 25 ms 回一次 `$!RS,BTF,0`，
     *   那也算"有进展"、会把 s_t0 一直刷新 ⇒ STALL 永远不触发 ⇒ 用户点了
     *   「开始接收」又忘了，会话会**无限挂着**（SPI 上每 25 ms 一笔 GET 白跑）。
     *   也**不能**用 s_retry 顶：那个已经按语义改成"对端明确回了话就清零"。
     *   ⇒ 单独拿"会话开始到现在"计时，只在这一字节都没到的阶段生效；
     *     一旦开始收块（s_recv > 0），保护交给下面那条 STALL。
     *   ⚠ 3 分钟是给"用户去手机上配对 + 挑文件"留的余量；UI 上有「中止接收」
     *     按钮可随时退出，所以这个上限只需防"忘了"。 */
    if (s_recv == 0u && (now_ms - s_t_start) > g_bt_wait_max_ms)
    {
        fail(BTE_STALL);
        return;
    }
    if ((now_ms - s_t0) > STALL_MS) { fail(BTE_STALL); return; }
    if (s_retry > GET_TRY_MAX)     { fail(BTE_BADSEQ); return; }

    /* ---- ⑦ GET 超时重发 / 发下一块 ---- */
    /* ⚠ 判据分两档（2026-10-10 第四轮，用一整轮扫档换来的）：
     *   · 从"发出 GET"到"现在"**一个下行字节都没到**（g_spi_rx_bytes 没变）
     *     ⇒ GET 本身丢了（命令通道丢 ~15%）⇒ 只等 g_bt_get_ack_ms(300 ms) 就重发；
     *   · 有字节在下行 ⇒ 块正在搬，只是慢。这时**绝不能重发** —— 对端会把这
     *     一块再灌一遍，界进正在按长度组装的负载中间 ⇒ 那一块必然 CRC 坏。
     *     原来的单档 2000 ms 把这两种情况混在一起：丢一笔白等 2 s（100 KB 实测
     *     22~44 s 里 8~17 笔白等就占了大头），而 500 ms 单档又会误打断在途的块
     *     （实测每轮必多 1 个坏块）。分开之后两头的毛病都没了。 */
    if (s_get_out)
    {
        uint32_t el  = now_ms - s_get_t0;
        uint32_t lim = (g_spi_rx_bytes != s_rx_base) ? GET_ACK_BUSY_MS : g_bt_get_ack_ms;

        if (el > lim)
        {
            s_get_out  = 0u;
            s_retry++;
            s_need_get = 1u;
            s_next_get = now_ms;
            /* 诊断（2026-10-10）：GET 发出后超时。
             * ⚠ 关键不是"总次数"而是**连续次数** —— 连续 6 笔就把 STALL_MS(12 s) 撑满、
             *   会话判死。值 0 表示命令通道干净（--snap 0 不轮询时应为 0）。
             *   g_bt_get_max 就是"最长连续无应答"，是区分"链路真断"与"测量干扰"的判据。 */
            g_bt_get_to++;
            s_get_to_run++;
            if (s_get_to_run > g_bt_get_max) g_bt_get_max = s_get_to_run;
        }
    }
    if (s_need_get && (int32_t)(now_ms - s_next_get) >= 0)
    {
        send_get(s_seq);
        s_need_get = 0u;
        s_get_out  = 1u;
        s_get_t0   = now_ms;
        s_rx_base  = g_spi_rx_bytes;   /* 起点：⑦ 用它区分"GET 丢了"与"块正在搬" */
    }
}

/* ====================== 给 UI 的查询 ====================== */

int bt_recv_state(void)   { return (int)s_state; }
uint32_t bt_recv_bytes(void) { return s_recv; }
uint32_t bt_recv_total(void) { return s_total; }
int bt_recv_last_rc(void)    { return g_bt_rc; }
const char* bt_recv_name(void) { return s_name; }

int bt_recv_busy(void)
{
    return (s_state == BT_RECV_WAIT || s_state == BT_RECV_RECV || s_opening) ? 1 : 0;
}

int bt_recv_progress(void)
{
    if (s_state == BT_RECV_DONE) return 100;
    if (s_total == 0u)           return -1;         /* 总长未知：进度条画不定长态 */
    if (s_recv >= s_total)       return 100;
    return (int)((s_recv * 100u) / s_total);
}

/* 一行状态文字。产物是**人能读的中文**，与 BoardNet_WeatherText 同一套纪律：
 * 拿不到就明说"还没有数据"，不拿 0 冒充。 */
const char* bt_recv_status_text(void)
{
    switch (s_state)
    {
        case BT_RECV_IDLE:  return "未开始";
        case BT_RECV_WAIT:
            return s_opening ? "正在启动蓝牙…" : "等待对端发送";
        case BT_RECV_RECV:  return "正在接收";
        case BT_RECV_DONE:  return "接收完成";
        default:            return "接收失败";
    }
}

/* ===========================================================================
 * 板级转发（P5，2026-10-10）—— 给 phone_shell 的一组入口
 *
 * 为什么绕一层：phone_shell 是 third_party 里的官方 Demo，**不能直接 include
 * src/ 下的头**。BoardPic_* / BoardGallery_* / BoardNet_* 都是同一套做法。
 * 这里只做"改名 + 转一手"，逻辑全在本文件上半部分，一行都没有重复实现。
 *
 * ⚠ 这一组之所以必须存在而不是让 UI 直接读 g_net_btf_*：UI 需要的是
 *   "进度条画几格 / 这一行显示什么字"，而 g_net_btf_recv 是原始字节数、
 *   bt_recv_state 是会话状态 —— 换算（百分比、状态文字、未知总长的处理）
 *   全在本文件里，UI 不重复算。
 * =========================================================================== */

int         BoardBt_Start(void)      { return bt_recv_start(); }
int         BoardBt_Abort(void)      { return bt_recv_abort(); }
int         BoardBt_Busy(void)       { return bt_recv_busy(); }
int         BoardBt_State(void)      { return bt_recv_state(); }
int         BoardBt_Progress(void)   { return bt_recv_progress(); }
uint32_t    BoardBt_Bytes(void)      { return bt_recv_bytes(); }
uint32_t    BoardBt_Total(void)      { return bt_recv_total(); }
const char* BoardBt_Name(void)       { return bt_recv_name(); }
int         BoardBt_LastRc(void)     { return bt_recv_last_rc(); }
const char* BoardBt_StatusText(void) { return bt_recv_status_text(); }
