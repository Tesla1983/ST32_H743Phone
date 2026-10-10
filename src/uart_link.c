/* ===========================================================================
 * ESP32 ↔ STM32H743 串口链路 —— 实现
 * 协议与踩坑说明见 uart_link.h 顶部，这里只写实现细节。
 *
 * 接收为什么用「中断 + 环形缓冲 + 主循环重组」而不是 DMA+IDLE：
 *   对端一帧只有 40~60 字节、10 秒一帧，**速率根本不是瓶颈**；
 *   而 UART4 的 DMA 要走 DMAMUX + 又要多占一路流控，为了这点数据量不值得。
 *   中断只做"读 RDR 塞环形缓冲"这一件事（几十个周期），解析全部放在主循环。
 * =========================================================================== */

#include "uart_link.h"
#include "spi_link.h"           /* SPI 激活时命令改走 SPI 命令环（uart_link_cmd 用） */

#include "stm32h7xx_hal.h"
#include "rtc_clock.h"          /* $DT 到达时校准板载 RTC */
/* BoardNet_* 的声明在本模块的头文件里（uart_link.h），
 * phone_shell_board.h 会把它转给 phone_shell。这里不再 include board 头 ——
 * 那个头依赖 YMGUI 的 GYCTX，本模块是纯板级外设，不该被 UI 类型牵进来。 */

/* 环形缓冲：512 是 2 的幂，可用 & 取模。
 * 一帧 ~60 字节，主循环每拍都会抽干，实际占用远小于容量。 */
/* ⚠ 环形缓冲长度账（2026-10-10 实测踩过，别再改小）：
 *   921600 bps ⇒ 112 500 字节/秒。主循环一帧最坏约 10.9 ms 不来 poll
 *   （渲染 + 触摸 + SD 轮询），这段时间线上能涌进 **约 1226 字节**；
 *   $!BD 的裸负载本身就有 **2048 字节**（对端 CMD_BODY_CAP），加头帧约 2078。
 *   原来只给 512 ⇒ 收 $!BD 时 drop 掉 1045 字节，症状是「超时、len=0」，
 *   而 ORE 只涨 1 ⇒ 不是中断被屏蔽，纯粹是缓冲装不下。
 *   ⇒ 取 4096：同时覆盖上述两种最坏情况，代价是 .bss 多 3.5 KB（SRAM1 够）。 */
#define RING_SIZE   4096u
#define RING_MASK   (RING_SIZE - 1u)

static UART_HandleTypeDef s_hu;

static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint32_t s_head;      /* ISR 写 */
static volatile uint32_t s_tail;      /* 主循环读 */

/* 行重组缓冲：一帧没结束就一直攒，超长直接丢弃重来（坏帧不该拖住后续帧）。 */
static uint8_t  s_line[UART_LINK_LINE_MAX];
static uint32_t s_line_n;

static uint32_t s_now_ms;             /* 最近一次 poll 的时基，BoardNet_* 要用 */

/* ---- 用 $DT 校准板载 RTC（2026-10-09）----
 * 为什么不是每帧无脑写：HAL_RTC_SetTime/SetDate 每次都要进/出 init mode
 * （期间日历暂停几毫秒）。10 s 一帧虽无害，但没必要把网络抖动也照单写进去；
 * 偏差 ≤ 2 s 就认为 RTC 走得好（LSE 精度足够），不动它。
 * ⚠ Rtc_IsValid() 为假（备份域刚复位 / 首次上电）时必须写 —— 这也是把
 *   BKP 标记置起来的唯一路径。 */
static void rtc_calibrate(int y, int mo, int d, int hh, int mm, int ss, int wday)
{
    int cy = 0, cmo = 0, cd = 0, chh = 0, cmm = 0, css = 0, cwd = 0;
    int need = 1;

    if (Rtc_IsValid() && Rtc_GetLocal(&cy, &cmo, &cd, &chh, &cmm, &css, &cwd) == 0)
    {
        if (cy == y && cmo == mo && cd == d)
        {
            int a = chh * 3600 + cmm * 60 + css;
            int b = hh  * 3600 + mm  * 60 + ss;
            int diff = a - b;
            if (diff < 0) diff = -diff;
            if (diff <= 2) need = 0;
        }
    }

    if (need)
        Rtc_SetLocal(y, mo, d, hh, mm, ss, wday);
}

/* ---- 诊断量定义 ---- */
volatile int      g_uart_rc = -99;    /* -99 = 还没初始化（区别于任何真实返回码） */
volatile uint32_t g_uart_baud;
volatile uint32_t g_uart_rx_bytes;
volatile uint32_t g_uart_frames;
volatile uint32_t g_uart_rx_drop;
volatile uint32_t g_uart_err_ore;
volatile uint32_t g_uart_err_ne;
volatile uint32_t g_uart_err_fe;
volatile uint32_t g_uart_err_pe;
volatile uint32_t g_uart_idle_ms;
volatile uint32_t g_uart_last_ms;
volatile uint8_t  g_uart_line[UART_LINK_LINE_MAX];
volatile uint32_t g_uart_line_len;
volatile uint8_t  g_uart_raw[16];
volatile uint32_t g_uart_raw_n;

volatile uint32_t g_net_epoch;
volatile int      g_net_hh, g_net_mm, g_net_ss;
volatile int      g_net_year, g_net_mon, g_net_mday, g_net_wday;
volatile uint32_t g_net_epoch_at;

volatile int      g_net_temp_x10 = -9999;
volatile int      g_net_rh   = -1;
volatile int      g_net_pm25 = -1;
volatile int      g_net_aqi  = -1;
volatile int      g_net_code = -1;
volatile uint8_t  g_net_wx_text[24];

volatile int      g_net_wf_up;
volatile int      g_net_wf_rssi;
volatile uint8_t  g_net_wf_ip[24];

/* $RD：无线电开关状态（2026-10-10，$?RADIO 命令的回报）。
 * 反映的是"开关意图"，与是否连上 WiFi / 是否正在配对无关：
 *   g_net_radio_wifi  = WiFi STA 是否已 start（1=开 0=关）
 *   g_net_radio_bt    = BT 无线电是否已开启（1=开 0=关）
 * STM32 设置页的两个开关直接吃这两个量。 */
volatile int      g_net_radio_wifi;
volatile int      g_net_radio_bt;
volatile uint32_t g_net_rd_pkts;      /* 收到的 $RD 帧数 */

volatile uint32_t g_net_dt_pkts;
volatile uint32_t g_net_wd_pkts;
volatile uint32_t g_net_wf_pkts;
volatile uint32_t g_net_wx_pkts;
volatile uint32_t g_net_xor_fail;
volatile uint32_t g_net_bad_pkts;
volatile uint32_t g_net_rx_bytes;        /* 下行总字节（UART+SPI 合一），命令握手判据用 */

volatile uint32_t g_uart_selftest;

/* 自检**影子结果**：解析完立刻存一份，不被后续真实帧覆盖。
 * 为什么要有影子变量而不是直接读 g_net_*：脚本写完 g_uart_selftest 后要等
 * 0.5 s 才来读，而对端每 10 s 一帧 —— 主变量早就被真实数据盖掉了，
 * 精确断言（22:26:40 / 200 / code=0）就会随机失败。 */
volatile uint32_t g_uart_st_dt, g_uart_st_wd, g_uart_st_wf, g_uart_st_xor;
volatile int      g_uart_st_hh, g_uart_st_mm, g_uart_st_ss;
volatile int      g_uart_st_mon, g_uart_st_mday, g_uart_st_wday;
volatile uint32_t g_uart_st_epoch;
volatile int      g_uart_st_temp, g_uart_st_code;

/* ---- 命令通道诊断量（2026-10-09）---- */
volatile uint32_t g_cmd_tx;
volatile uint32_t g_cmd_rx;
volatile uint32_t g_cmd_xor_fail;
volatile uint32_t g_cmd_ping_ok;
volatile uint32_t g_cmd_weather_req;
volatile uint32_t g_cmd_weather_ack;
volatile uint32_t g_cmd_weather_ok;
volatile uint32_t g_cmd_phase;
volatile uint32_t g_cmd_bd_pkts;
volatile uint32_t g_cmd_bd_id;
volatile uint32_t g_cmd_bd_len;
volatile uint32_t g_cmd_bd_crc_bad;
volatile uint32_t g_cmd_bd_timeout;
volatile uint32_t g_cmd_bd_toobig;
/* 二进制模式超时时的「声明长度 / 实收长度」（2026-10-10 加，用于定位 P2）。 */
volatile uint32_t g_cmd_bd_want;
volatile uint32_t g_cmd_bd_got;
volatile uint8_t  g_cmd_body[UART_LINK_BODY_CAP];
volatile uint32_t g_cmd_body_len;
volatile uint32_t g_cmd_req;

/* ★2026-10-10（P4）负载**收齐**计数与"本块 CRC 结论"。见 uart_link.h 的说明：
 * 头帧计数不能当"块到了"的判据（负载还在 35 笔 SPI 事务的路上）。 */
volatile uint32_t g_cmd_bd_done;
volatile int      g_cmd_bd_last_ok;

/* ---- 蓝牙文件接收（P3/P4，2026-10-10）----
 * 解析层只负责把字段收下来；动作（拉块、写 TF、导入图库）在 src/bt_recv.c。 */
volatile int      g_net_btf_state;
volatile uint8_t  g_net_btf_name[48];
volatile uint32_t g_net_btf_size;
volatile uint32_t g_net_btf_recv;
volatile uint32_t g_net_bt_pkts;
volatile int      g_cmd_btf_rc;
volatile uint32_t g_cmd_btf_rs;

/* ---- 上游 SPP 诊断镜像（$BS 帧，2026-10-11）----
 * 这几个量只活在对端 ESP32 上，而读 ESP32 串口日志**必然把它复位**，计数随之清零，
 * 永远抓不到现场。所以让对端随 $BT 一起把它们回送过来，本板留一份镜像，
 * 之后用 SWD 读（SWD 不动 ESP32）即可无损观测。
 * 判据用法：opens==0 ⇒ 手机根本没连上 SPP；opens>0 而 rx 小 ⇒ 连上了但没发数据。 */
volatile uint32_t g_spp_opens_m;    /* 手机连上 SPP 的次数 */
volatile uint32_t g_spp_closes_m;   /* 断开次数 */
volatile uint32_t g_spp_rx_m;       /* 对端从 SPP 收到的总字节 */
volatile uint32_t g_spp_last_m;     /* 最近一包字节数 */
volatile uint32_t g_spp_err_m;      /* 回调里非成功状态次数 */
volatile int      g_spp_run_m;      /* 1 = SPP 服务端在监听 */
volatile int      g_spp_conn_m;     /* 1 = 手机当前连着 */
volatile uint32_t g_bt_recv_req;
volatile uint32_t g_bt_recv_w;
volatile uint32_t g_bt_recv_h;

/* ---- 逐城天气诊断量（2026-10-10）---- */
volatile uint32_t g_city_req;
volatile uint32_t g_city_ok;
volatile uint32_t g_city_to;
volatile uint32_t g_city_scan;
volatile uint32_t g_city_last_scan;
volatile int      g_city_idx = -1;

/* ---- 二进制收取状态（$!BD 的裸负载）----
 * s_bin_need != 0 即"二进制模式"：此时收到的字节**不参与行重组**，
 * 直接按序填进 g_cmd_body，填满 need 个就收工。
 * 为什么必须有超时：万一负载少发了几个字节（对端异常/线路丢字节），
 * 没有超时就会把后续的推送帧也当成负载吞掉，链路永久错位。 */
static uint32_t s_bin_need;
static uint32_t s_bin_got;
static uint32_t s_bin_id;
static uint16_t s_bin_crc;
static uint32_t s_bin_t0;      /* 进入二进制模式时的时基，用于超时 */
#define BIN_TIMEOUT_MS   4000u

/* ---- 命令状态机 ----
 * IDLE  等板子起来（让对端上电那 ~0.5 s 的 ROM 乱码先过去）
 * LINK  等线上出现字节 —— 证明"收"方向是通的，才值得去试"发"
 * PING  已发 $?PING，等 $!RS
 * WEA   已发 $?WEA，等受理回执
 * WDATA 已受理，等 $WD 真的到（这才是 P1 的最终目的）
 * DONE  结束（成功或放弃）
 * ⚠ PING 3 次都没回就**不再往下走**：发方向不通时 WEA 同样不通，
 *   继续发只是白占阻塞时间。判据也就变得极其干净：
 *   g_cmd_tx=3 且 g_cmd_rx=0 ⇒ 发送方向的线没接。 */
#define CMD_PH_IDLE    0u
#define CMD_PH_LINK    1u
#define CMD_PH_PING    2u
#define CMD_PH_WEA     3u
#define CMD_PH_WDATA   4u
#define CMD_PH_DONE    5u
#define CMD_PH_CITY    6u     /* 逐城天气扫描（开机序列跑完之后才进） */
#define CMD_PH_RADIO   7u     /* 查无线电状态 $?RD（2026-10-10；握手成功之后、问天气之前） */
/* 周期重查 $?RD 的间隔。为什么不能只靠开机问一次：对端 ESP32 会独立复位
 * （与 $WF 加 WIFI_PERIOD_S 是同一个道理），复位后无线电态可能变了，
 * 本板不问就永远停在旧值。30 s 与对端周期重发量级一致。 */
#define RADIO_REFRESH_MS 30000u

static uint32_t s_cmd_phase;
static uint32_t s_cmd_t0;      /* 本阶段起点 */
static uint32_t s_cmd_try;     /* 本阶段已尝试次数 */
static uint32_t s_cmd_wd0;     /* 发 WEA 时的 $WD 计数，用来判断"真的等到了" */
static uint32_t s_cmd_rd0;     /* 发 ?RD 时的 $RD 计数（无线电状态的同类判据） */
static uint32_t s_radio_t0;    /* 上一次周期性重查 $?RD 的时基 */

/* ---- 无线电命令重试（2026-10-10）----
 *
 * 【为什么必须重试】SPI 从机 DMA 未按 4 字节对齐 ⇒ **命令帧会被截断**。
 *   实测抓到：`$?RADIO,BT,ON` 的 `*79\r\n` 被 5 个 0x00 取代，紧接着拼上下一条命令
 *   ⇒ XOR 校验尾丢失 ⇒ 对端 `handle_line` **静默丢弃**（不打任何日志）。
 *   对端持续告警：spi_slave: real trans_len is not 4 bytes aligned, slave may loss data。
 *
 *   天气 / `$?RD` 这类**周期**命令丢了下一轮会自己补上，看不出问题；
 *   但开关是**一次性**命令，丢了就真的没了 —— 表现为"拨了开关没反应"，
 *   而且 `g_cmd_tx` 照样涨（看起来像发出去了），极难定位。
 *
 * ⇒ 判据用 **$RD 里回流的真实状态有没有翻转**，不用"我发没发"。
 *   最多重发 RADIO_RETRY_MAX 次，避免链路真断时无限重发占总线。 */
/* 实测丢帧率不低（3 次里有 1 次命中不了），3 次不够 ⇒ 取 5 次。
 * 5 次 × 500 ms = 2.5 s 内发完，用户拨开关的等待感可接受；
 * 只在"有待确认的开关命令"时才发，平时零开销。 */
#define RADIO_RETRY_MAX  5u
#define RADIO_RETRY_MS   500u
static uint32_t s_radio_pend;      /* 0=无待确认  1=WiFi  2=BT */
static uint32_t s_radio_pend_on;   /* 期望状态 0/1 */
static uint32_t s_radio_pend_t0;   /* 0=尚未装填，由 cmd_tick 首次看到时填 now_ms */
static uint32_t s_radio_pend_tries;

/* ====================== 小工具 ====================== */

static int to_int(const char* s)
{
    int neg = 0, v = 0;
    if (s == NULL)
        return 0;
    if (*s == '-') { neg = 1; ++s; }
    else if (*s == '+') { ++s; }
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return neg ? -v : v;
}

static uint32_t to_u32(const char* s)
{
    uint32_t v = 0;
    if (s == NULL)
        return 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    return v;
}

/* 2 位大写十六进制 → 数值。非十六进制返回 -1（调用方据此判坏帧）。 */
static int hex2(const char* p)
{
    int v = 0;
    for (int i = 0; i < 2; ++i)
    {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9')       d = c - '0';
        else if (c >= 'A' && c <= 'F')  d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')  d = c - 'a' + 10;
        else                            return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* 4 位大写十六进制 → 数值（CRC-16 用）。非十六进制返回 -1。 */
static int hex4(const char* p)
{
    int hi = hex2(p);
    int lo = hex2(p + 2);
    if (hi < 0 || lo < 0)
        return -1;
    return (hi << 8) | lo;
}

/* CRC-16/CCITT-FALSE：poly 0x1021，初值 0xFFFF，无反射、无末异或。
 * ⚠ 与对端 uplink_cmd.c 的 crc16_ccitt() **逐位相同**，改一边必须改另一边。
 * 为什么裸负载不用 XOR8：XOR 对"字节换位""整段偏移"完全无感，
 * 而裸负载没有帧边界可依，只能靠长度 + CRC。 */
/* 形参带 volatile：g_cmd_body 是 volatile 的（脚本会经 SWD 直接读它），
 * C 允许"加限定"转换，因此普通指针也能传进来。 */
static uint16_t crc16(const volatile uint8_t* d, uint32_t n)
{
    uint16_t c = 0xFFFFu;
    for (uint32_t i = 0; i < n; ++i)
    {
        c ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; ++b)
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u)
                              : (uint16_t)(c << 1);
    }
    return c;
}

/* 组一条命令帧 "$<type>[,<body>]*HH\r\n"（HH = 2 位大写十六进制 XOR）。
 * body 为 NULL 或空串时不写逗号（$?PING*HH 而不是 $?PING,*HH）。
 * ⚠ 手写而不用 snprintf：不必为了 6 个字符把 printf 家族拖进固件。
 * 校验范围与对端一致：type（含前导 '?'）+ [','] + body。 */
static int cmd_frame(char* out, int cap, const char* type, const char* body)
{
    static const char HEX[] = "0123456789ABCDEF";
    int      k = 0;
    uint8_t  x = 0;
    int      has_body = (body != NULL && body[0] != 0);

    if (out == NULL || cap < 8 || type == NULL)
        return -1;

    out[k++] = '$';
    for (const char* p = type; *p != 0; ++p)
    {
        if (k >= cap - 6) return -1;
        out[k++] = *p;
        x ^= (uint8_t)*p;
    }
    if (has_body)
    {
        if (k >= cap - 6) return -1;
        out[k++] = ',';
        x ^= (uint8_t)',';
        for (const char* p = body; *p != 0; ++p)
        {
            if (k >= cap - 6) return -1;
            out[k++] = *p;
            x ^= (uint8_t)*p;
        }
    }
    out[k++] = '*';
    out[k++] = HEX[(x >> 4) & 0xFu];
    out[k++] = HEX[x & 0xFu];
    out[k++] = '\r';
    out[k++] = '\n';
    out[k]   = 0;
    return k;
}

/* 原地按 ',' 切分：把分隔符写成 '\0'，返回字段数。
 * ⚠ 会修改 buf。字段数达到 max 就停（多余字段不用，不报错）。 */
static int split_fields(char* buf, char** f, int max)
{
    int n = 0;
    char* p = buf;
    f[n++] = p;
    for (; *p != 0; ++p)
    {
        if (*p == ',' && n < max)
        {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    return n;
}

/* ====================== 逐城天气（2026-10-10）======================
 * 天气 app 要显示多城，而 $WD 只有「默认城市」那一城 —— 所以走命令通道
 * 逐城问（$?WEA,<城市码> → $WX），结果存在这张表里。
 *
 * ⚠ 城市码是 2026-10-10 在**主机上逐个请求 sojson 接口验证过**的：
 *   三个码都 HTTP 200，且响应里 cityInfo.city 与下表的名字一致
 *   （杭州→杭州市 / 上海→上海市 / 成都→成都市）。不是照抄网上的清单。
 *   要加城市：下面两张表各加一项并改 CITY_N，UI 会自动多出一行。
 */
#define CITY_N  3

static const char* const s_city_name[CITY_N] = {"杭州", "上海", "成都"};
static const char* const s_city_key[CITY_N]  = {"101210101", "101020100", "101270101"};

typedef struct
{
    int     valid;      /* 1 = 拿到过一次数据（保留旧值给界面显示，刷新期间不闪空） */
    int     gen;        /* 数据属于第几轮扫描 —— 用来判"本轮这一城是否已回来" */
    int     temp_x10;
    int     rh;
    int     pm25;
    int     aqi;
    int     code;
    uint8_t text[24];   /* UTF-8 天气文本，如 "晴" */
} CityWx;

static CityWx s_city_wx[CITY_N];

/* 扫描状态。s_city_gen 单调递增，每开一轮扫描 +1。
 * ⚠ 为什么用 gen 而不是在扫描开始时清 valid：
 *   清了 valid，界面在每轮刷新的那几秒里会闪成 "--"；留着旧值继续显示，
 *   靠 gen 对不上来判"本轮还没回来"，用户体验和判据两头都站得住。 */
static uint32_t s_city_gen;
static int      s_city_sent;    /* 当前这一城的命令是否已发出 */
static uint32_t s_city_t0;      /* 发出命令时的时基 */
static uint32_t s_city_scan_t0; /* 上一轮扫描结束的时基（用来算重扫间隔） */
static uint32_t s_city_next_ms; /* 距下一轮扫描的间隔（成功=30 min，有城没拿到=60 s） */
static int      s_city_scanned; /* 0 = 从没扫完过一轮 */

/* 逐城超时。
 * ⚠ 为什么是 12 s 而不是"看着够就行"的 8 s：对端 uplink_task 的循环里有一次
 *   `vTaskDelay(1000)`（最坏 1 s 才轮到处理事件位），而它 HTTP 的
 *   `timeout_ms` 是 **10000** —— 也就是说对端自己要 11 s 才会放弃。
 *   我们给 8 s 会出现"对端还在等 HTTP、我们已经判超时并跳去问下一城"，
 *   于是那一城永远少一拍（2026-10-10 实测：第二轮扫描 `g_city_to=1`，
 *   杭州卡在上一代 gen=1 的数据上，要等满 30 min 才补）。
 *   ⇒ 本板的超时必须**大于对端 HTTP 超时 + 一个循环拍**。 */
#define CITY_TIMEOUT_MS     12000u
/* 整轮重扫间隔。与对端 $WD 的 30 min 周期对齐 —— 天气本来就半小时一变，
 * 问得更密只是白白让对端多发几次 HTTP。 */
#define CITY_RESCAN_MS      1800000u
/* 有城市没拿到时的**补扫**间隔。为什么不能也等 30 min：
 *   一次偶发超时就让那一城显示整整半小时的旧值，界面上无从分辨
 *   （它显示的是真数据，只是不是当前值 —— 这比显示"--"更有误导性）。 */
#define CITY_RETRY_MS       60000u

/* ====================== 帧解析 ====================== */

/* 解析一整行（不含 '\r' '\n'）。返回 0 = 识别成功。 */
static int parse_frame(const uint8_t* line, uint32_t len)
{
    char  buf[UART_LINK_LINE_MAX];
    char* f[8];
    int   nf;

    if (len == 0u || len >= UART_LINK_LINE_MAX)
        return -1;

    for (uint32_t i = 0; i < len; ++i)
        buf[i] = (char)line[i];
    buf[len] = 0;

    /* 找 '$' 与 '*'。取**最后一个** '$' 和它之后的第一个 '*'。
     *
     * 为什么要"最后一个"（2026-10-09 已由对端从根上修好，这里保留兜底）：
     *   对端 2026-10-09 之前把**日志和帧打在同一行**（日志里含完整帧），
     *   STM32 收到的整条流其实是 ESP32 的 console 日志，帧是从日志行里抠出来的。
     *   对端改成「帧与日志分口」后，线上只剩纯帧；但上电 ~0.5 s 内对端 IDF
     *   的 banner 仍会漏过来（那几行在它接管日志输出之前打印），
     *   所以"取最后一个 '$'"这个兜底仍然保留，只是不再依赖它。 */
    char* dollar = NULL;
    for (char* p = buf; *p != 0; ++p)
        if (*p == '$')
            dollar = p;
    if (dollar == NULL)
        return -2;                     /* 不是我们的帧（比如对端裸日志），正常现象 */

    char* star = NULL;
    for (char* p = dollar; *p != 0; ++p)
        if (*p == '*') { star = p; break; }
    if (star == NULL || star < dollar + 2)
        return -3;

    /* XOR 校验：对 '$' 之后、'*' 之前的**每一个字节**（含 UTF-8 中文的每一个字节）。 */
    uint8_t x = 0;
    for (char* p = dollar + 1; p < star; ++p)
        x ^= (uint8_t)*p;

    int claimed = hex2(star + 1);
    if (claimed < 0 || claimed != (int)x)
    {
        /* 按方向分开记：推送帧坏 → g_net_xor_fail；响应帧坏 → g_cmd_xor_fail。
         * 分开的意义：命令方向的那根线往往是后补的，坏帧会集中出现在那一侧，
         * 混在一个计数里就看不出是哪根线的问题。
         * ⚠ 两个计数都必须**真的被用到**：本工程开了 --gc-sections，
         *   只声明不使用的全局变量会被整段回收，脚本读符号时直接 KeyError。 */
        if (dollar[1] == '!')
            g_cmd_xor_fail++;
        else
            g_net_xor_fail++;
        return -4;
    }

    *star = 0;                          /* 到此为止是 "<TYPE>,<字段...>" */
    nf = split_fields(dollar + 1, f, 8);

    if (nf >= 5 && f[0][0] == 'D' && f[0][1] == 'T')
    {
        /* $DT,<unix_ts>,<YYYY-MM-DD>,<HH:MM:SS>,<wday> */
        g_net_epoch = to_u32(f[1]);

        /* 日期 "2026-10-08" 与时间 "22:26:40" 都是**已按 CST-8 换算过的本地时间**
         * —— 不要再加减时区，直接拆。 */
        int y = to_int(f[2]);
        int m = to_int(f[2] + 5);
        int d = to_int(f[2] + 8);
        int hh = to_int(f[3]);
        int mm = to_int(f[3] + 3);
        int ss = to_int(f[3] + 6);

        g_net_year = y; g_net_mon = m; g_net_mday = d;
        g_net_hh = hh;  g_net_mm = mm;  g_net_ss = ss;
        g_net_wday = to_int(f[4]);
        g_net_epoch_at = s_now_ms;      /* 记下时基，之后靠板载 tick 自己走秒 */
        g_net_dt_pkts++;

        /* 顺手校准板载 RTC —— 这样 $DT 断流（ESP32 掉电/重启）时界面照样走。
         * ⚠ 有 RTC 之后，"时间"不再依赖本帧：current_clock() 优先读 RTC。 */
        rtc_calibrate(y, m, d, hh, mm, ss, g_net_wday);
        return 0;
    }

    if (nf >= 6 && f[0][0] == 'W' && f[0][1] == 'D')
    {
        /* $WD,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text> */
        g_net_temp_x10 = to_int(f[1]);
        g_net_rh       = to_int(f[2]);
        g_net_pm25     = to_int(f[3]);
        g_net_aqi      = to_int(f[4]);
        g_net_code     = to_int(f[5]);

        /* 文本整段拷贝 —— ⚠ 不要逐字符赋中文：'晴' 这种多字节字符常量会被截断成
         * 最低字节，界面上直接消失（2026-10-08 相册 caption 已经踩过一次）。 */
        uint32_t i = 0;
        const char* t = f[6];
        while (t[i] != 0 && i + 1u < (uint32_t)sizeof(g_net_wx_text))
        {
            g_net_wx_text[i] = (uint8_t)t[i];
            ++i;
        }
        g_net_wx_text[i] = 0;
        g_net_wd_pkts++;
        return 0;
    }

    if (nf >= 8 && f[0][0] == 'W' && f[0][1] == 'X')
    {
        /* $WX,<citykey>,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text>
         * 逐城天气 —— $?WEA,<城市码> 的应答。
         *
         * ⚠ 靠 citykey 对号入座，**不要**按"当前正在问哪一城"来记：
         *   某一城超时后本模块会跳去问下一城，这时上一城的 $WX 才姗姗来迟的话
         *   就会被记到错的槽位上，而且看不出错了。citykey 是当初问的那个码
         *   原样回传的，对不上就是真的对不上。 */
        int idx = -1;
        for (int i = 0; i < CITY_N; ++i)
        {
            const char* a = s_city_key[i];
            const char* b = f[1];
            while (*a != 0 && *a == *b) { ++a; ++b; }
            if (*a == 0 && *b == 0) { idx = i; break; }
        }
        if (idx < 0)
            return -5;                      /* 不是我们问过的城市，忽略 */

        CityWx* w = &s_city_wx[idx];
        w->temp_x10 = to_int(f[2]);
        w->rh       = to_int(f[3]);
        w->pm25     = to_int(f[4]);
        w->aqi      = to_int(f[5]);
        w->code     = to_int(f[6]);

        uint32_t i = 0;
        const char* t = f[7];
        while (t[i] != 0 && i + 1u < (uint32_t)sizeof(w->text))
        {
            w->text[i] = (uint8_t)t[i];
            ++i;
        }
        w->text[i] = 0;

        w->valid = 1;
        w->gen   = (int)s_city_gen;
        g_net_wx_pkts++;
        return 0;
    }

    if (nf >= 3 && f[0][0] == 'W' && f[0][1] == 'F')
    {
        /* $WF,<up>,<ip>,<rssi> */
        g_net_wf_up   = to_int(f[1]);
        g_net_wf_rssi = to_int(f[3]);
        uint32_t i = 0;
        const char* t = f[2];
        while (t[i] != 0 && i + 1u < (uint32_t)sizeof(g_net_wf_ip))
        {
            g_net_wf_ip[i] = (uint8_t)t[i];
            ++i;
        }
        g_net_wf_ip[i] = 0;
        g_net_wf_pkts++;
        return 0;
    }

    if (nf >= 3 && f[0][0] == 'R' && f[0][1] == 'D')
    {
        /* $RD,<wifi>,<bt> —— 无线电开关状态，$?RADIO 命令执行后或开机时回报。
         * 设置页的 WiFi / 蓝牙开关以这一帧为权威来源（见 BoardNet_RadioWifiOn/BtOn）。 */
        g_net_radio_wifi = to_int(f[1]);
        g_net_radio_bt   = to_int(f[2]);
        g_net_rd_pkts++;
        return 0;
    }

    /* ---- 蓝牙文件会话状态：$BT,<state>,<name>,<size>,<recv> ----
     * ⚠ 放在 $!RS 之前判：'B' 与 '!' 不会混，但顺序上先判推送帧更直观。
     * ⚠ name 字段对端用单个 '-' 表示"还没有名字"，这里**原样收下**，
     *   由 bt_recv.c 决定怎么用（解析层不做业务判断）。 */
    if (nf >= 5 && f[0][0] == 'B' && f[0][1] == 'T' && f[0][2] == 0)
    {
        g_net_btf_state = to_int(f[1]);
        g_net_btf_size  = to_u32(f[3]);
        g_net_btf_recv  = to_u32(f[4]);

        uint32_t i = 0;
        const char* t = f[2];
        while (t[i] != 0 && i + 1u < (uint32_t)sizeof(g_net_btf_name))
        {
            g_net_btf_name[i] = (uint8_t)t[i];
            ++i;
        }
        g_net_btf_name[i] = 0;
        g_net_bt_pkts++;
        return 0;
    }

    /* ---- 上游 SPP 诊断：$BS,<opens>,<closes>,<rx>,<last>,<err>,<run>,<conn> ----
     * ⚠ 紧跟在 $BT 之后判：两者都是 'B' 开头，靠第二个字符 'T' / 'S' 区分。 */
    if (nf >= 8 && f[0][0] == 'B' && f[0][1] == 'S' && f[0][2] == 0)
    {
        g_spp_opens_m  = to_u32(f[1]);
        g_spp_closes_m = to_u32(f[2]);
        g_spp_rx_m     = to_u32(f[3]);
        g_spp_last_m   = to_u32(f[4]);
        g_spp_err_m    = to_u32(f[5]);
        g_spp_run_m    = to_int(f[6]);
        g_spp_conn_m   = to_int(f[7]);
        return 0;
    }

    /* ---- 命令响应：$!RS,<CMD>,<rc>[,<附加>] ---- */
    if (nf >= 3 && f[0][0] == '!' && f[0][1] == 'R' && f[0][2] == 'S')
    {
        g_cmd_rx++;
        /* f[1] = 命令名，f[2] = rc。只认"受理成功"（rc > 0）才推进状态机。 */
        if (nf >= 3 && f[1][0] == 'W' && f[1][1] == 'E' && f[1][2] == 'A')
        {
            if (to_int(f[2]) > 0)
                g_cmd_weather_ack++;
        }
        else if (f[1][0] == 'B' && f[1][1] == 'T' && f[1][2] == 'F')
        {
            /* ★P4 蓝牙文件会话：rc 1=已受理/有数据 0=暂时没数据 -1=序号越界。
             * 这里只把 rc 收下来并计数，语义解释在 src/bt_recv.c。 */
            g_cmd_btf_rc = to_int(f[2]);
            g_cmd_btf_rs++;
        }
        return 0;
    }

    /* ---- 大块数据头帧：$!BD,<id>,<len>,<crc16> ----
     * 之后紧跟 len 字节裸负载，由 uart_link_poll 的二进制模式收走。
     * ⚠ 这里**不能**直接 return 后继续按行解析：负载里可能含 '$'、'\r'、'\n'，
     *   按行收必然错位。所以只在这里置状态，收字节的动作交给二进制模式。 */
    if (nf >= 4 && f[0][0] == '!' && f[0][1] == 'B' && f[0][2] == 'D')
    {
        uint32_t n   = to_u32(f[2]);
        int      crc = hex4(f[3]);
        g_cmd_bd_pkts++;

        if (crc < 0)
        {
            g_cmd_bd_crc_bad++;      /* CRC 字段本身坏，不敢进二进制模式 */
            return 0;
        }
        if (n == 0u || n > UART_LINK_BODY_CAP)
        {
            g_cmd_bd_toobig++;       /* 装不下（或对端报 0 字节）：整块放弃 */
            return 0;
        }
        s_bin_need = n;
        s_bin_got  = 0u;
        s_bin_id   = to_u32(f[1]);
        g_cmd_bd_id = s_bin_id;
        s_bin_crc  = (uint16_t)crc;
        s_bin_t0   = s_now_ms;
        return 0;
    }

    g_net_bad_pkts++;
    return -5;
}

/* ====================== 自检 ====================== */

static void run_selftest(void)
{
    /* 这三行是 E:\workbuddy\esp32-com8 日志里的**原样样本**，校验值已独立验算。
     * 喂给的是真正用的 parse_frame()，所以自检过 = 解析链路过（线通不通另算）。 */
    static const char s1[] = "$DT,1791469600,2026-10-08,22:26:40,4*28";
    static const char s2[] = "$WD,200,47,65,111,0,\xe6\x99\xb4*EB";   /* 晴 */
    static const char s3[] = "$WF,1,192.168.1.2,-76*08";

    uint32_t d0 = g_net_dt_pkts, w0 = g_net_wd_pkts, f0 = g_net_wf_pkts;
    uint32_t x0 = g_net_xor_fail;

    parse_frame((const uint8_t*)s1, (uint32_t)(sizeof(s1) - 1u));
    /* ⚠ 必须**立刻**取值存进影子变量，不能等三帧都解析完再读：
     *   真实帧只可能在**上一拍**的 poll 里被处理，同一拍内不会再插入，
     *   所以此刻 g_net_hh 就是样本自己的结果；但如果线上每 10 s 来一帧、
     *   而脚本等 0.6 s 才读，主变量已经被真实帧覆盖了
     *   （2026-10-08 实测：自检读到 $DT +2、时间 23:15:27，就是被真实帧盖了）。 */
    g_uart_st_hh   = g_net_hh;
    g_uart_st_mm   = g_net_mm;
    g_uart_st_ss   = g_net_ss;
    g_uart_st_mon  = g_net_mon;
    g_uart_st_mday = g_net_mday;
    g_uart_st_wday = g_net_wday;
    g_uart_st_epoch = g_net_epoch;

    parse_frame((const uint8_t*)s2, (uint32_t)(sizeof(s2) - 1u));
    g_uart_st_temp = g_net_temp_x10;
    g_uart_st_code = g_net_code;

    parse_frame((const uint8_t*)s3, (uint32_t)(sizeof(s3) - 1u));

    g_uart_st_dt  = g_net_dt_pkts  - d0;
    g_uart_st_wd  = g_net_wd_pkts  - w0;
    g_uart_st_wf  = g_net_wf_pkts  - f0;
    g_uart_st_xor = g_net_xor_fail - x0;
    g_uart_selftest = 0;
}

/* ====================== 初始化 ====================== */

int uart_link_init(uint32_t baud)
{
    /* GPIO 与外设时钟**必须自己开**：厂商 usart.c 里的 HAL_UART_MspInit 只认 USART1，
     * 给 UART4 调 HAL_UART_Init 时它一个脚都不会配（详见 uart_link.h 顶部）。 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_UART4_CLK_ENABLE();

    GPIO_InitTypeDef g;
    g.Pin       = GPIO_PIN_9;                 /* PB9 = UART4_TX（→ 对端命令口 RX） */
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF8_UART4;             /* ⚠ PB8/PB9 的 UART4 是 AF8，不是 AF7 */
    HAL_GPIO_Init(GPIOB, &g);

    g.Pin       = GPIO_PIN_8;                 /* PB8 = UART4_RX（← 对端帧口 TX） */
    HAL_GPIO_Init(GPIOB, &g);

    s_hu.Instance        = UART4;
    s_hu.Init.BaudRate   = baud;
    s_hu.Init.WordLength = UART_WORDLENGTH_8B;
    s_hu.Init.StopBits   = UART_STOPBITS_1;
    s_hu.Init.Parity     = UART_PARITY_NONE;
    s_hu.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    s_hu.Init.Mode       = UART_MODE_TX_RX;
    s_hu.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&s_hu) != HAL_OK)
    {
        g_uart_rc = -1;
        return -1;
    }

    /* 开 RXNE 中断。⚠ 不开 HAL 的 IT 接收（HAL_UART_Receive_IT），
     * 因为收完的回调被厂商代码占了（见头文件说明）—— 这里只开中断位，
     * 字节由我们自己的 ISR 直接读。 */
    UART4->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
    UART4->CR3 |= USART_CR3_EIE;              /* 帧/噪声/溢出错误也要报，用来查波特率 */

    HAL_NVIC_SetPriority(UART4_IRQn, 2, 0);   /* 比 SDMMC1(见 sd_card.c) 更急一点 */
    HAL_NVIC_EnableIRQ(UART4_IRQn);           /* UART4_IRQn = 52（见 startup_gcc.s） */

    g_uart_baud  = baud;
    g_uart_rc    = 0;
    g_uart_last_ms = 0;
    return 0;
}

/* ====================== 命令通道（本板 → 对端） ====================== */

/* 手工触发用的测试 URL（g_cmd_req=3）。选它是因为**本机实测可达**，
 * 而且响应 < 2048 字节时不会被截断，方便脚本和 PC 端抓到的内容逐字节比对。
 * 现在实测响应约 3.5 KB ⇒ 会被截到 2048，脚本按 2048 比对即可。 */
#define CMD_TEST_URL  "http://t.weather.sojson.com/api/weather/city/101010100"

int uart_link_cmd(const char* type, const char* body)
{
    /* ⚠ 缓冲长度账（2026-10-10 修过一个真 bug，别再改小）：
     *   帧长 = 1($) + len(type) + [1(,) + len(body)] + 5(*HH\r\n\0)
     *   $?PING          → 1+5+5      = 11
     *   $?WEA           → 1+4+5      = 10
     *   $?WGET,1,<URL>  → 1+5+1+1+1+len(URL)+5
     *   测试 URL 55 字符 ⇒ **69 字节**；原来 out[64] 时 cmd_frame() 在
     *   k >= cap-6(=58) 处返回 -1 ⇒ **命令静默不发**，症状是 g_cmd_tx 不涨、
     *   对端 hex 里压根没有这一帧、P2 恒 FAIL。
     *   ⇒ 取 256：允许 URL 最长 242 字符，够用；栈上 256 B 对主循环无压力。 */
    char out[256];
    int  n = cmd_frame(out, (int)sizeof(out), type, body);
    if (n <= 0)
        return -1;
    g_cmd_tx++;
    /* SPI 已接管业务链路时，命令改走 SPI 命令环（由 spi_link_poll 在事务里发出）；
     * 否则退回 UART 阻塞发送（UART 线没接时就是静默失败，与历史行为一致）。 */
    if (spi_link_active())
        (void)spi_link_cmd_enqueue(out);
    else
        (void)uart_link_send(out);  /* 阻塞轮询，短帧 <20 字节 ⇒ ~0.2 ms @921600 */
    return n;
}

static void cmd_tick(uint32_t now_ms)
{
    /* ---- 脚本手工触发（g_cmd_req）：写 1/2/3，发完自动清 0 ---- */
    if (g_cmd_req != 0u)
    {
        uint32_t r = g_cmd_req;
        g_cmd_req  = 0u;
        if (r == 1u)
        {
            (void)uart_link_cmd("?PING", NULL);
        }
        else if (r == 2u)
        {
            g_cmd_weather_req++;
            (void)uart_link_cmd("?WEA", NULL);
        }
        else if (r == 3u)
        {
            (void)uart_link_cmd("?WGET", "1," CMD_TEST_URL);
        }
        /* ---- 无线电开关的脚本触发（2026-10-10）----
         * 验"STM32 控 ESP32 WiFi/BT 开关"用：写值后对端执行并回 $RD，
         * 判据是 g_net_rd_pkts 涨 + g_net_radio_wifi/bt 翻转。
         * ⚠ 7(WiFi OFF) 会断网（天气/时间停更），验完务必写 8 恢复。 */
        else if (r == 4u)
        {
            (void)BoardNet_SetBt(1);        /* 蓝牙开 */
        }
        else if (r == 5u)
        {
            (void)BoardNet_SetBt(0);        /* 蓝牙关 */
        }
        else if (r == 6u)
        {
            (void)BoardNet_QueryRadio();    /* 查 $RD */
        }
        else if (r == 7u)
        {
            (void)BoardNet_SetWifi(0);      /* WiFi 关（会断网） */
        }
        else if (r == 8u)
        {
            (void)BoardNet_SetWifi(1);      /* WiFi 开 */
        }
        return;
    }

    /* ---- 无线电命令重试：看 $RD 回流的真实状态，没翻转就补发 ----
     * 只在"有待确认的开关命令"时跑，平时零开销。 */
    if (s_radio_pend != 0u)
    {
        int cur = (s_radio_pend == 1u) ? BoardNet_RadioWifiOn()
                                       : BoardNet_RadioBtOn();

        if (s_radio_pend_t0 == 0u)
        {
            s_radio_pend_t0 = now_ms;       /* 首次巡检：开始计时 */
        }
        else if (cur == (int)s_radio_pend_on)
        {
            s_radio_pend = 0u;              /* 已确认：状态真的翻转了，收工 */
        }
        else if (now_ms - s_radio_pend_t0 > RADIO_RETRY_MS)
        {
            if (++s_radio_pend_tries >= RADIO_RETRY_MAX)
            {
                /* 重发够多次仍未翻转 ⇒ 别再占总线（可能是链路真断或对端拒绝）。
                 * 设置页会停在"等待同步/未生效"的真实状态，不撒谎。 */
                s_radio_pend = 0u;
            }
            else
            {
                s_radio_pend_t0 = now_ms;
                if (s_radio_pend == 1u)
                    (void)uart_link_cmd("?RADIO",
                                        s_radio_pend_on ? "WIFI,ON" : "WIFI,OFF");
                else
                    (void)uart_link_cmd("?RADIO",
                                        s_radio_pend_on ? "BT,ON" : "BT,OFF");
            }
        }
    }

    switch (s_cmd_phase)
    {
    case CMD_PH_IDLE:
        /* 等 2 s：让对端上电那一段 ROM 乱码（74880 波特）先过去，
         * 否则我们发的命令会混在乱码里，对端收到的是脏行。 */
        if (now_ms < 2000u)
            break;
        s_cmd_phase = CMD_PH_LINK;
        s_cmd_t0    = now_ms;
        break;

    case CMD_PH_LINK:
        /* 先确认"收"方向通了（线上有字节），再试"发" —— 收都不通时
         * 发的方向多半也没接，这时发命令只是白占阻塞时间。
         * ⚠ 2026-10-10 SPI 升格后改成看 g_net_rx_bytes（UART+SPI 合一），
         *   否则 SPI 接管、UART 线已拆时这条判据永远为 0、命令永不发出。 */
        if (g_net_rx_bytes != 0u)
        {
            s_cmd_phase = CMD_PH_PING;
            s_cmd_t0    = now_ms;
            s_cmd_try   = 0u;
            (void)uart_link_cmd("?PING", NULL);
        }
        else if (now_ms - s_cmd_t0 > 10000u)
        {
            s_cmd_phase = CMD_PH_DONE;      /* 10 s 一个字节都没有 ⇒ 线不通 */
        }
        break;

    case CMD_PH_PING:
        if (g_cmd_rx != 0u)                 /* 收到任意 $!RS 即算握手成功 */
        {
            g_cmd_ping_ok++;
            /* 2026-10-10：握手成功**先问无线电状态**（设置页的 WiFi/蓝牙开关
             * 要拿真值），拿到或超时后才问天气。
             * ⚠ 不能只指望对端开机广播的那一次 $RD：对端比本板先起来，
             *   那一帧本板还没开始轮询 SPI ⇒ 实测 g_net_rd_pkts 恒 0。
             *   ⇒ 本板必须自己问。 */
            s_cmd_phase = CMD_PH_RADIO;
            s_cmd_t0    = now_ms;
            s_cmd_try   = 0u;
            s_cmd_rd0   = g_net_rd_pkts;
            (void)BoardNet_QueryRadio();
            break;
        }
        if (now_ms - s_cmd_t0 > 1500u)
        {
            if (++s_cmd_try >= 3u)
            {
                s_cmd_phase = CMD_PH_DONE;  /* 发方向不通，不再往下试 */
                break;
            }
            s_cmd_t0 = now_ms;
            (void)uart_link_cmd("?PING", NULL);
        }
        break;

    case CMD_PH_RADIO:
        /*
         * 判据：**$RD 帧数涨了**。
         * ⚠ 不能看 g_cmd_rx —— `$?RD` 只让对端回一个 `$RD` 状态帧，
         *   对端**不会**回 `$!RS`（`$!RS` 是"命令受理"类才有的响应），
         *   所以 g_cmd_rx 在这一相里恒不变，拿它当判据会必定超时。
         *   （与 CMD_PH_WDATA 看 g_net_wd_pkts 而不是看 ack 同理。）
         */
        if (g_net_rd_pkts > s_cmd_rd0)
        {
            s_cmd_phase = CMD_PH_WEA;
            s_cmd_t0    = now_ms;
            s_cmd_try   = 0u;
            s_cmd_wd0   = g_net_wd_pkts;
            g_cmd_weather_req++;
            (void)uart_link_cmd("?WEA", NULL);
            break;
        }
        if (now_ms - s_cmd_t0 > 2000u)
        {
            s_cmd_t0 = now_ms;
            if (++s_cmd_try >= 3u)
            {
                /* 无线电状态查不到**不要卡住开机序列**：天气/时间才是主业，
                 * 开关最多显示"等待同步"，周期重查（CMD_PH_DONE）还会补救。 */
                s_cmd_phase = CMD_PH_WEA;
                s_cmd_try   = 0u;
                s_cmd_wd0   = g_net_wd_pkts;
                g_cmd_weather_req++;
                (void)uart_link_cmd("?WEA", NULL);
                break;
            }
            (void)BoardNet_QueryRadio();
        }
        break;

    case CMD_PH_WEA:
        if (g_cmd_weather_ack != 0u)
        {
            s_cmd_phase = CMD_PH_WDATA;
            s_cmd_t0    = now_ms;
            break;
        }
        if (now_ms - s_cmd_t0 > 2000u)
        {
            if (++s_cmd_try >= 3u)
            {
                s_cmd_phase = CMD_PH_DONE;
                break;
            }
            s_cmd_t0 = now_ms;
            g_cmd_weather_req++;
            (void)uart_link_cmd("?WEA", NULL);
        }
        break;

    case CMD_PH_WDATA:
        /* 真正的判据：受理之后 $WD 帧数**涨了**。
         * 只看 $!RS,WEA,1 不够 —— 那只说明对端答应了，不代表天气真取到了。 */
        if (g_net_wd_pkts > s_cmd_wd0)
        {
            g_cmd_weather_ok++;
            s_cmd_phase = CMD_PH_DONE;
        }
        else if (now_ms - s_cmd_t0 > 20000u)
        {
            s_cmd_phase = CMD_PH_DONE;      /* 20 s 还没等到，放弃（30 min 后对端会自己推） */
        }
        break;

    case CMD_PH_DONE:
        /* 空闲态：只负责"到点重扫一次逐城天气"。
         * ⚠ g_cmd_rx == 0 说明**发送方向根本不通**（线没接/对端没在听），
         *   这时还去问只会每 8 s 往线上吐一帧，既没用又占主循环 —— 直接不动。 */
        if (g_cmd_rx == 0u)
            break;
        /* 周期性重查无线电状态（对端可能独立复位，或本板错过了某帧 $RD）。
         * 与 CMD_PH_RADIO 的开机首查互补：首查保证"快点拿到"，
         * 这里保证"长期不会失同步"。 */
        if (s_radio_t0 == 0u || (now_ms - s_radio_t0 > RADIO_REFRESH_MS))
        {
            s_radio_t0 = now_ms;
            (void)BoardNet_QueryRadio();
        }
        if (s_city_scanned != 0 && (now_ms - s_city_scan_t0 < s_city_next_ms))
            break;
        s_city_gen++;
        g_city_idx  = 0;
        s_city_sent = 0;
        s_cmd_phase = CMD_PH_CITY;
        break;

    case CMD_PH_CITY:
        /*
         * 逐城串行：**一次只问一城**，等它的 $WX 回来（或超时）再问下一城。
         *
         * 为什么不一口气把三城都发出去：对端那边是「一个槽位的邮箱」
         * （见 E:\workbuddy\esp32-com8\main\time_weather.c），连发三帧只会
         * 让后两帧回 $!RS,WEA,0（忙），城市码还被互相覆盖。
         * 串行是协议要求，不是保守。
         */
        if (s_city_sent == 0)
        {
            g_city_req++;
            s_city_sent = 1;
            s_city_t0   = now_ms;
            (void)uart_link_cmd("?WEA", s_city_key[g_city_idx]);
            break;
        }
        /* 本轮这一城回来了？判据是 gen 对得上（不是 valid —— 那可能是上轮旧值） */
        if (s_city_wx[g_city_idx].gen == (int)s_city_gen)
        {
            g_city_ok++;
            s_city_sent = 0;
        }
        else if (now_ms - s_city_t0 > CITY_TIMEOUT_MS)
        {
            g_city_to++;                    /* 对端没回：跳过，问下一城 */
            s_city_sent = 0;
        }

        if (s_city_sent == 0)
        {
            if (++g_city_idx >= CITY_N)
            {
                /* 本轮到齐了几个？到齐才等 30 min，缺了就 60 s 后补扫。
                 * 判据用 gen（不是 valid）：valid 可能是上几轮的旧值。 */
                int got = 0;
                for (int i = 0; i < CITY_N; ++i)
                    if (s_city_wx[i].gen == (int)s_city_gen) ++got;

                g_city_idx       = -1;
                s_city_scanned   = 1;
                s_city_scan_t0   = now_ms;
                s_city_next_ms   = (got >= CITY_N) ? CITY_RESCAN_MS : CITY_RETRY_MS;
                g_city_last_scan = got;
                g_city_scan++;
                s_cmd_phase      = CMD_PH_DONE;
            }
        }
        break;

    default:
        break;
    }

    g_cmd_phase = s_cmd_phase;
}

/* ====================== 中断 ====================== */

void UART4_IRQHandler(void)
{
    USART_TypeDef* u = UART4;
    uint32_t guard = 0;

    /* 迭代上限：真出现"标志清不掉"的意外时宁可退出中断让主循环继续跑，
     * 也不要把 CPU 锁死在 ISR 里（ISR 卡死的表现是整个 UI 冻住，极难定位）。 */
    while (guard++ < 1024u)
    {
        uint32_t isr = u->ISR;

        if (isr & USART_ISR_RXNE_RXFNE)
        {
            uint8_t c = (uint8_t)u->RDR;          /* 读 RDR 即清 RXNE */
            uint32_t next = (s_head + 1u) & RING_MASK;
            if (next == s_tail)
            {
                g_uart_rx_drop++;                 /* 满了：宁丢新字节也不覆盖未读的 */
            }
            else
            {
                s_ring[s_head] = c;
                s_head = next;
                g_uart_rx_bytes++;
            }
            if (g_uart_raw_n < (uint32_t)sizeof(g_uart_raw))
                g_uart_raw[g_uart_raw_n++] = c;   /* 头 16 字节留证 */
            continue;
        }

        if (isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE))
        {
            /* 错误标志必须显式清 ICR，否则会一直触发中断把 CPU 吃满。 */
            if (isr & USART_ISR_ORE) g_uart_err_ore++;
            if (isr & USART_ISR_NE)  g_uart_err_ne++;
            if (isr & USART_ISR_FE)  g_uart_err_fe++;
            if (isr & USART_ISR_PE)  g_uart_err_pe++;
            u->ICR = USART_ICR_ORECF | USART_ICR_NECF | USART_ICR_FECF | USART_ICR_PECF;
            /* ORE 时 RDR 里还压着一字节，读走它，否则 RXNE 恒真、死循环 */
            if (isr & USART_ISR_ORE) { (void)u->RDR; }
            continue;
        }

        break;
    }
}

/* ====================== 主循环 ====================== */

/* ---- 逐字节喂给解析层（UART 与 SPI 共用这一份状态机）----
 * 抽出来是因为 SPI 业务链路也要走同一套行重组 / $!BD 二进制模式 / parse_frame，
 * 不能让两套解析逻辑分叉。传输层（UART 中断 or SPI 事务）只负责把字节送进来。 */
static void feed_byte(uint8_t c)
{
    g_uart_last_ms = s_now_ms;
    g_net_rx_bytes++;                   /* UART + SPI 合一的下行总字节 */

    /* 二进制模式（$!BD 的裸负载）：**按长度收**，不看内容、不参与行重组。 */
    if (s_bin_need != 0u)
    {
        g_cmd_body[s_bin_got++] = c;
        if (s_bin_got >= s_bin_need)
        {
            uint16_t cc = crc16(g_cmd_body, s_bin_need);
            g_cmd_bd_len   = s_bin_need;
            g_cmd_body_len = s_bin_need;
            /* ★P4：CRC 结论**必须在收齐这一拍**落下来 —— 上层（bt_recv）靠
             * "done 变了 + last_ok" 判这一块能不能写卡。g_cmd_bd_crc_bad 是累计
             * 计数，分不清坏的是哪一块，不能拿它当判据。 */
            g_cmd_bd_last_ok = (cc == s_bin_crc) ? 1 : 0;
            if (cc != s_bin_crc)
                g_cmd_bd_crc_bad++;
            s_bin_need     = 0u;
            g_cmd_bd_done++;          /* 置在最后：上层看到它涨时，上面这些都已就绪 */
        }
        return;
    }

    if (c == (uint8_t)'\n')
    {
        if (s_line_n > 0u)
        {
            g_uart_frames++;
            /* 存一份原文留证：看清对端到底发了什么，再改解析器。 */
            uint32_t n = s_line_n;
            if (n > UART_LINK_LINE_MAX - 1u)
                n = UART_LINK_LINE_MAX - 1u;
            for (uint32_t i = 0; i < n; ++i)
                g_uart_line[i] = s_line[i];
            g_uart_line[n] = 0;
            g_uart_line_len = n;

            (void)parse_frame(s_line, s_line_n);
        }
        s_line_n = 0u;
    }
    else if (c == (uint8_t)'\r')
    {
        /* 直接丢弃：'\r\n' 结尾的帧由 '\n' 收尾，'\r' 不进缓冲 */
    }
    else
    {
        if (s_line_n < UART_LINK_LINE_MAX - 1u)
            s_line[s_line_n++] = c;
        else
            s_line_n = 0u;              /* 超长：整行作废，等下一帧 */
    }
}

void uart_link_feed(const uint8_t* buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        feed_byte(buf[i]);
    }
}

void uart_link_poll(uint32_t now_ms)
{
    s_now_ms = now_ms;

    if (g_uart_selftest != 0u)
    {
        run_selftest();
        return;                 /* 自检这一拍不处理线上数据，避免混在一起看不清 */
    }

    while (s_tail != s_head)
    {
        uint8_t c = s_ring[s_tail];
        s_tail = (s_tail + 1u) & RING_MASK;
        g_uart_rx_bytes++;              /* UART 专属计数（SPI 的不计这里） */
        feed_byte(c);
    }

    /* 二进制模式超时兜底：负载少发几个字节时，没有这个会一直吞掉后续的
     * 推送帧（时间/天气），链路永久错位 —— 所以宁可丢这一块也要退出。
     * 用减法比较，天然正确处理 32 位时基回绕。 */
    if (s_bin_need != 0u && (now_ms - s_bin_t0) > BIN_TIMEOUT_MS)
    {
        g_cmd_bd_timeout++;
        /* 2026-10-10 加的诊断量：超时时把「声明要多少 / 实际收到多少」记下来。
         * 没有这两个数就只能看到 len=0 + 超时=1，分不清是
         * 「对端少发」「本板丢字节」「声明长度本身错」哪一种。 */
        g_cmd_bd_want = s_bin_need;
        g_cmd_bd_got  = s_bin_got;
        s_bin_need = 0u;
    }

    cmd_tick(now_ms);

    g_uart_idle_ms = now_ms - g_uart_last_ms;
}

int uart_link_send(const char* text)
{
    if (text == NULL)
        return 0;
    int n = 0;
    for (const char* p = text; *p != 0; ++p)
    {
        while ((UART4->ISR & USART_ISR_TXE_TXFNF) == 0u) { }
        UART4->TDR = (uint8_t)*p;
        ++n;
    }
    while ((UART4->ISR & USART_ISR_TC) == 0u) { }
    return n;
}

/* ====================== 给 UI 用的换算 ======================
 * 为什么要这几个函数：phone_shell 是 third_party，**不认识** g_net_* 这些变量，
 * 也不该认识（与 BoardGallery_* 同一套分层套路）。这里把"原始数值 → 界面字符串"
 * 的换算收在板级，UI 只管拿字符串去显示。 */

/* 当前本地时间。取用优先级：
 *   ① 板载 RTC（2026-10-09 起）—— 复位后立即可用（VDD 不断则备份域保持），
 *      与 $DT 帧的到达节奏无关，$DT 断流也照样走；
 *   ② 回退：以最近一帧 $DT 的 HH:MM:SS 为基准、用板载毫秒时基自己走秒。
 *      （RTC 起不来，或本次上电还没校过时走这条。）
 * 时间帧 10 秒一次，② 靠递推，否则屏幕上的秒会每 10 秒才跳一次。 */
static void current_clock(int* hh, int* mm, int* ss)
{
    if (Rtc_IsValid() && Rtc_GetLocal(NULL, NULL, NULL, hh, mm, ss, NULL) == 0)
        return;

    uint32_t elapsed = (s_now_ms - g_net_epoch_at) / 1000u;
    uint32_t sod = (uint32_t)((g_net_hh < 0 ? 0 : g_net_hh) * 3600
                            + (g_net_mm < 0 ? 0 : g_net_mm) * 60
                            + (g_net_ss < 0 ? 0 : g_net_ss));
    sod = (sod + elapsed) % 86400u;
    *hh = (int)(sod / 3600u);
    *mm = (int)((sod / 60u) % 60u);
    *ss = (int)(sod % 60u);
}

/* 是否有可信时间（没有就让 UI 显示占位而不是 00:00）。
 * 判据两个来源任一成立：板载 RTC 被校准过（跨复位保持），或本次运行收到过 $DT。 */
int BoardNet_HasTime(void)
{
    if (Rtc_IsValid())
        return 1;
    return (g_net_dt_pkts != 0u) ? 1 : 0;
}

int BoardNet_HasWeather(void)
{
    return (g_net_wd_pkts != 0u) ? 1 : 0;
}

/* 拆开的 "HH" / "MM"（桌面大时间用）。
 * 为什么要有拆开版：大时间要**去掉秒位、让冒号每秒闪一次**，而闪烁靠
 * "冒号 ↔ 空"切换实现。若用一个 label 拼成 "HH:MM"，冒号换成空格会改变
 * 文本总宽（位图字体里 ':' 与空格不等宽）⇒ 分钟数字左右跳位。
 * 拆成三个**各自绝对定位**的 label 后，冒号显示与否对两侧毫无影响。 */
void BoardNet_ClockHH(char* out, int n)
{
    if (n <= 0) return;
    if (!BoardNet_HasTime())
    {
        out[0] = '-'; out[1] = '-'; out[2] = 0;
        return;
    }
    int hh, mm, ss;
    current_clock(&hh, &mm, &ss);
    if (n > 1)
    {
        out[0] = (char)('0' + hh / 10);
        out[1] = (char)('0' + hh % 10);
    }
    out[(n > 2) ? 2 : n - 1] = 0;
}

void BoardNet_ClockMM(char* out, int n)
{
    if (n <= 0) return;
    if (!BoardNet_HasTime())
    {
        out[0] = '-'; out[1] = '-'; out[2] = 0;
        return;
    }
    int hh, mm, ss;
    current_clock(&hh, &mm, &ss);
    if (n > 1)
    {
        out[0] = (char)('0' + mm / 10);
        out[1] = (char)('0' + mm % 10);
    }
    out[(n > 2) ? 2 : n - 1] = 0;
}

/* 冒号该不该亮：**秒为奇数时灭、偶数时亮** ⇒ 1 秒一个完整周期（亮 1 s / 灭 1 s）。
 * UI 侧拿它决定画 ":" 还是空串。 */
int BoardNet_ColonBlink(void)
{
    if (!BoardNet_HasTime())
        return 1;                       /* 没同步到时让冒号常亮，别闪（闪会像"没数据"） */
    int hh, mm, ss;
    current_clock(&hh, &mm, &ss);
    return (ss & 1) ? 0 : 1;
}

/* "HH:MM" —— 状态栏用（原硬编码 "9:41"）。 */
void BoardNet_ClockHM(char* out, int n)
{
    if (n <= 0) return;
    if (!BoardNet_HasTime())
    {
        out[0] = '-'; out[1] = '-'; out[2] = ':'; out[3] = '-'; out[4] = '-'; out[5] = 0;
        return;
    }
    int hh, mm, ss;
    current_clock(&hh, &mm, &ss);
    int k = 0;
    out[k++] = (char)('0' + hh / 10);
    out[k++] = (char)('0' + hh % 10);
    out[k++] = ':';
    out[k++] = (char)('0' + mm / 10);
    out[k++] = (char)('0' + mm % 10);
    if (k >= n) k = n - 1;
    out[k] = 0;
}

/* "HH:MM:SS" —— 桌面大时间用（原硬编码 "09:41"）。 */
void BoardNet_ClockHMS(char* out, int n)
{
    if (n <= 0) return;
    if (!BoardNet_HasTime())
    {
        out[0] = '-'; out[1] = '-'; out[2] = ':'; out[3] = '-'; out[4] = '-';
        out[5] = ':'; out[6] = '-'; out[7] = '-'; out[8] = 0;
        return;
    }
    int hh, mm, ss;
    current_clock(&hh, &mm, &ss);
    int k = 0;
    out[k++] = (char)('0' + hh / 10);
    out[k++] = (char)('0' + hh % 10);
    out[k++] = ':';
    out[k++] = (char)('0' + mm / 10);
    out[k++] = (char)('0' + mm % 10);
    out[k++] = ':';
    out[k++] = (char)('0' + ss / 10);
    out[k++] = (char)('0' + ss % 10);
    if (k >= n) k = n - 1;
    out[k] = 0;
}

/* "10月9日 星期五" —— 桌面日期（原硬编码 "9月29日  星期二"）。
 * ⚠ 中文**整段 memcpy**，不要逐字符赋值（多字节字符常量会被截断）。
 * 日期来源同 current_clock()：优先 RTC（能跨复位保持、且会自己进位跨天），
 * 其次是最近一帧 $DT 的日期。 */
void BoardNet_DateText(char* out, int n)
{
    static const char* kWeek[] = {"一", "二", "三", "四", "五", "六", "日"};
    if (n <= 0) return;
    if (!BoardNet_HasTime())
    {
        static const char s[] = "未同步";
        int i = 0;
        while (s[i] != 0 && i < n - 1) { out[i] = s[i]; ++i; }
        out[i] = 0;
        return;
    }

    /* "%d月%d日 " 先用 snprintf 拼 ASCII 部分，再接中文星期 */
    int k = 0;
    int mon = g_net_mon, day = g_net_mday, w = g_net_wday;
    if (Rtc_IsValid())
    {
        int ry = 0, rm = 0, rd = 0, rh = 0, rn = 0, rs = 0, rw = 0;
        if (Rtc_GetLocal(&ry, &rm, &rd, &rh, &rn, &rs, &rw) == 0)
        {
            mon = rm; day = rd; w = rw;
        }
    }
    if (mon >= 10) { if (k < n - 1) out[k++] = '1'; }
    if (k < n - 1) out[k++] = (char)('0' + (mon % 10));
    /* "月" = E6 9C 88 */
    if (k + 3 < n) { out[k++] = (char)0xE6; out[k++] = (char)0x9C; out[k++] = (char)0x88; }
    if (day >= 10)
    {
        if (k < n - 1) out[k++] = (char)('0' + day / 10);
    }
    if (k < n - 1) out[k++] = (char)('0' + (day % 10));
    /* "日" = E6 97 A5 */
    if (k + 3 < n) { out[k++] = (char)0xE6; out[k++] = (char)0x97; out[k++] = (char)0xA5; }
    if (k < n - 1) out[k++] = ' ';

    /* "星期" = E6 98 9F E6 9C 9F */
    if (k + 6 < n)
    {
        out[k++] = (char)0xE6; out[k++] = (char)0x98; out[k++] = (char)0x9F;
        out[k++] = (char)0xE6; out[k++] = (char)0x9C; out[k++] = (char)0x9F;
    }
    if (w >= 1 && w <= 7)
    {
        const char* wk = kWeek[w - 1];
        int i = 0;
        while (wk[i] != 0 && k < n - 1) { out[k++] = wk[i]; ++i; }
    }
    out[k < n ? k : n - 1] = 0;
}

/* "晴 / 20℃" —— 桌面天气（原硬编码 "多云 / 22 C"）。
 * 温度用 ×10 整数拆成整数+一位小数，但**小数位为 0 时省略**（20.0 显示成 20）。 */
void BoardNet_WeatherText(char* out, int n)
{
    if (n <= 0) return;
    if (!BoardNet_HasWeather())
    {
        static const char s[] = "等待天气";
        int i = 0;
        while (s[i] != 0 && i < n - 1) { out[i] = s[i]; ++i; }
        out[i] = 0;
        return;
    }

    int k = 0;
    const char* t = (const char*)g_net_wx_text;
    if (t[0] != 0)
    {
        int i = 0;
        while (t[i] != 0 && k < n - 1) { out[k++] = t[i]; ++i; }
    }
    else
    {
        static const char s[] = "未知";
        int i = 0;
        while (s[i] != 0 && k < n - 1) { out[k++] = s[i]; ++i; }
    }

    /* " / " */
    if (k + 3 < n) { out[k++] = ' '; out[k++] = '/'; out[k++] = ' '; }

    /* 温度：支持负数 */
    int tx = g_net_temp_x10;
    if (tx < 0)
    {
        if (k < n - 1) out[k++] = '-';
        tx = -tx;
    }
    int ip = tx / 10, fp = tx % 10;
    /* 整数部分最多两位（>99℃ 不会发生，真发生了就截断） */
    if (ip >= 100 && k < n - 1) out[k++] = (char)('0' + ip / 100);
    if (ip >= 10  && k < n - 1) out[k++] = (char)('0' + (ip / 10) % 10);
    if (k < n - 1) out[k++] = (char)('0' + (ip % 10));
    if (fp != 0)
    {
        if (k < n - 1) out[k++] = '.';
        if (k < n - 1) out[k++] = (char)('0' + fp);
    }
    /* "℃" = E2 84 83 */
    if (k + 3 < n) { out[k++] = (char)0xE2; out[k++] = (char)0x84; out[k++] = (char)0x83; }

    out[k < n ? k : n - 1] = 0;
}

/* 天气数字码（-1 = 没收到）。UI 拿它选图标，不要比对中文文本。 */
int BoardNet_WeatherCode(void)
{
    return g_net_code;
}

/* ====================== 逐城天气（天气 app 用）====================== */

int BoardNet_CityCount(void)
{
    return CITY_N;
}

const char* BoardNet_CityName(int i)
{
    if (i < 0 || i >= CITY_N) return "";
    return s_city_name[i];
}

int BoardNet_CityHasData(int i)
{
    if (i < 0 || i >= CITY_N) return 0;
    return s_city_wx[i].valid ? 1 : 0;
}

int BoardNet_CityCode(int i)
{
    if (i < 0 || i >= CITY_N || !s_city_wx[i].valid) return -1;
    return s_city_wx[i].code;
}

int BoardNet_CityTempX10(int i)
{
    if (i < 0 || i >= CITY_N || !s_city_wx[i].valid) return 0;
    return s_city_wx[i].temp_x10;
}

int BoardNet_CityRh(int i)
{
    if (i < 0 || i >= CITY_N || !s_city_wx[i].valid) return -1;
    return s_city_wx[i].rh;
}

int BoardNet_CityPm25(int i)
{
    if (i < 0 || i >= CITY_N || !s_city_wx[i].valid) return -1;
    return s_city_wx[i].pm25;
}

int BoardNet_CityAqi(int i)
{
    if (i < 0 || i >= CITY_N || !s_city_wx[i].valid) return -1;
    return s_city_wx[i].aqi;
}

/* 往 out 尾部追加字符串（带容量保护）。返回新的长度。 */
static int app_text(char* out, int n, int k, const char* s)
{
    int i = 0;
    while (s[i] != 0 && k < n - 1) { out[k++] = s[i]; ++i; }
    out[k < n ? k : n - 1] = 0;
    return k;
}

/* 往 out 尾部追加一个十进制整数（含负号）。返回新的长度。 */
static int app_int(char* out, int n, int k, int v)
{
    char b[12];
    int  m = 0;
    if (v < 0) { if (k < n - 1) out[k++] = '-'; v = -v; }
    if (v == 0) b[m++] = '0';
    while (v > 0 && m < (int)sizeof(b)) { b[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0 && k < n - 1) { out[k++] = b[--m]; }
    out[k < n ? k : n - 1] = 0;
    return k;
}

/* "21℃" / "--"。⚠ 没数据时给 "--" 而不是 "0℃" —— 界面上必须能区分
 * "还没取到"和"真的是 0 度"，否则用户会以为杭州现在是 0 度。 */
void BoardNet_CityTempText(int i, char* out, int n)
{
    if (n <= 0) return;
    out[0] = 0;
    if (!BoardNet_CityHasData(i)) { (void)app_text(out, n, 0, "--"); return; }

    int k  = 0;
    int tx = s_city_wx[i].temp_x10;
    if (tx < 0) { if (k < n - 1) out[k++] = '-'; tx = -tx; }
    k = app_int(out, n, k, tx / 10);
    if (tx % 10 != 0)
    {
        if (k < n - 1) out[k++] = '.';
        k = app_int(out, n, k, tx % 10);
    }
    /* "℃" = E2 84 83 */
    if (k + 3 < n) { out[k++] = (char)0xE2; out[k++] = (char)0x84; out[k++] = (char)0x83; }
    out[k < n ? k : n - 1] = 0;
}

/* "晴" / "等待数据" */
void BoardNet_CityCondText(int i, char* out, int n)
{
    if (n <= 0) return;
    out[0] = 0;
    if (!BoardNet_CityHasData(i)) { (void)app_text(out, n, 0, "等待数据"); return; }
    if (s_city_wx[i].text[0] == 0) { (void)app_text(out, n, 0, "未知"); return; }
    (void)app_text(out, n, 0, (const char*)s_city_wx[i].text);
}

/* "湿度 69%  PM2.5 31  AQI 111" / ""（没数据时给空串，界面那一行自然留白）
 *
 * ⚠ 分隔符只用**两个空格**，不用 "·" 之类的分隔点：U+00B7 要落到中文字体里
 *   才画得出来，而这个工程挂的是裁剪过的字体子集，缺字会画成空白或豆腐块。
 *   空格是 ASCII，字体里一定有 —— 这是实测过的安全选择。
 * ⚠ 字段缺失（接口没给）时**整段不显示**，而不是显示 "-1"：对端解析不到时
 *   写的是 -1，那是"无数据"的内部标记，不该直接端上界面。 */
void BoardNet_CitySubText(int i, char* out, int n)
{
    if (n <= 0) return;
    out[0] = 0;
    if (!BoardNet_CityHasData(i)) return;

    static const char u_hum[] = "湿度 ";
    static const char u_pm[]  = "PM2.5 ";
    static const char u_aqi[] = "AQI ";

    int k = 0;
    if (s_city_wx[i].rh >= 0)
    {
        k = app_text(out, n, k, u_hum);
        k = app_int(out, n, k, s_city_wx[i].rh);
        if (k < n - 1) out[k++] = '%';
        out[k < n ? k : n - 1] = 0;
    }
    if (s_city_wx[i].pm25 >= 0)
    {
        if (k > 0 && k + 2 < n) { out[k++] = ' '; out[k++] = ' '; }
        out[k < n ? k : n - 1] = 0;
        k = app_text(out, n, k, u_pm);
        k = app_int(out, n, k, s_city_wx[i].pm25);
    }
    if (s_city_wx[i].aqi >= 0)
    {
        if (k > 0 && k + 2 < n) { out[k++] = ' '; out[k++] = ' '; }
        out[k < n ? k : n - 1] = 0;
        k = app_text(out, n, k, u_aqi);
        k = app_int(out, n, k, s_city_wx[i].aqi);
    }
    out[k < n ? k : n - 1] = 0;
}

int BoardNet_CityUpdatedCount(void)
{
    int c = 0;
    for (int i = 0; i < CITY_N; ++i)
        if (s_city_wx[i].valid) ++c;
    return c;
}

/* 1 = 正在逐城扫描。界面拿它决定副标题写"更新中"还是"已是最新"。 */
int BoardNet_CityBusy(void)
{
    return (g_city_idx >= 0) ? 1 : 0;
}

/* ---- WiFi 链路状态 ----
 * 三态而非两态：界面上"还不知道"（开机到首帧 $WF 之前，约几十秒）
 * 和"已知断开"必须长得不一样，否则用户会把未同步误读成断网。
 * 判据是 g_net_wf_pkts —— $WF 只在状态变化时发，收到过一帧就说明链路说过了话。 */
int BoardNet_WifiUp(void)
{
    if (g_net_wf_pkts == 0u) return -1;
    return g_net_wf_up ? 1 : 0;
}

int BoardNet_WifiRssi(void)
{
    if (g_net_wf_pkts == 0u) return 0;
    return g_net_wf_rssi;
}

/* RSSI → 格数。阈值按常见家用路由器的实际观感取：
 *   ≥ -55 dBm 满格（贴着路由器）
 *   ≥ -70     两格（正常室内）      ← 本板实测 -76 落在下一档
 *   ≥ -85     一格（勉强能连）
 *   < -85     零格（只画底座圆点，连上但很虚）
 * 手机系统一般也是这个量级，不必精确到 dBm。 */
int BoardNet_WifiBars(void)
{
    if (g_net_wf_pkts == 0u) return -1;
    if (!g_net_wf_up)        return 0;
    int r = g_net_wf_rssi;
    if (r >= -55) return 3;
    if (r >= -70) return 2;
    if (r >= -85) return 1;
    return 0;
}

const char* BoardNet_WifiIp(void)
{
    return g_net_wf_ip;
}

/* ---- 无线电开关状态（2026-10-10，$?RADIO / $RD）----
 * 与 BoardNet_WifiUp 同一套三态纪律：没收到过 $RD 之前返回 -1（"还不知道"），
 * 收到过才返回 0/1。设置页的开关据此画"未知 / 关 / 开"。
 * ⚠ g_net_radio_* 反映的是"开关意图"（STA 是否 start / BT 无线电是否开），
 *   不是"是否已连上 WiFi"——那个由 BoardNet_WifiUp 管，别混。 */
int BoardNet_RadioWifiOn(void)
{
    if (g_net_rd_pkts == 0u) return -1;
    return g_net_radio_wifi ? 1 : 0;
}

int BoardNet_RadioBtOn(void)
{
    if (g_net_rd_pkts == 0u) return -1;
    return g_net_radio_bt ? 1 : 0;
}

/* 发 $?RADIO 命令切换无线电（2026-10-10）。on=1 开 / on=0 关。
 * 返回 uart_link_cmd 的发出字节数；对端执行后回 $!RS,RADIO,<rc> 并推 $RD，
 * 设置页的开关由 $RD 回流刷新，不靠本地的"我发了什么"自缓存。
 *
 * ⚠⚠ **`?` 必须自己写进 type**（2026-10-10 实测抓出来的真 bug，别再漏）：
 *   `uart_link_cmd()` **不会**替你补 `?` —— 看它的长度账注释就能确认
 *   （`$?PING` = 1+len("?PING")+5，len 里已含 `?`），现有调用一律写 `"?WEA"/"?PING"`。
 *   当初这里写成 `"RADIO"/"RD"` ⇒ 线上发出的是 `$RADIO,WIFI,ON` 而不是 `$?RADIO,...`，
 *   对端 `handle_line` 比的是 `"?RADIO"` ⇒ **永远匹配不上，开关静默失效**，
 *   而 g_cmd_tx 照样涨、看起来"发出去了"，极难发现。
 *   ⇒ 判断据要看 `$RD` 帧数（g_net_rd_pkts）涨没涨，不能只看 g_cmd_tx。 */
int BoardNet_SetWifi(int on)
{
    s_radio_pend      = 1u;
    s_radio_pend_on   = (uint32_t)(on ? 1 : 0);
    s_radio_pend_tries = 0u;
    s_radio_pend_t0   = 0u;          /* 由 cmd_tick 首次巡检时装填 */
    return uart_link_cmd("?RADIO", on ? "WIFI,ON" : "WIFI,OFF");
}

int BoardNet_SetBt(int on)
{
    s_radio_pend      = 2u;
    s_radio_pend_on   = (uint32_t)(on ? 1 : 0);
    s_radio_pend_tries = 0u;
    s_radio_pend_t0   = 0u;
    return uart_link_cmd("?RADIO", on ? "BT,ON" : "BT,OFF");
}

/* 查询当前无线电状态（发 $?RD，对端回 $RD）。开机序列与周期重查各调一次。 */
int BoardNet_QueryRadio(void)
{
    return uart_link_cmd("?RD", NULL);
}
