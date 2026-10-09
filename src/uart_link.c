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

volatile uint32_t g_net_dt_pkts;
volatile uint32_t g_net_wd_pkts;
volatile uint32_t g_net_wf_pkts;
volatile uint32_t g_net_xor_fail;
volatile uint32_t g_net_bad_pkts;

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

static uint32_t s_cmd_phase;
static uint32_t s_cmd_t0;      /* 本阶段起点 */
static uint32_t s_cmd_try;     /* 本阶段已尝试次数 */
static uint32_t s_cmd_wd0;     /* 发 WEA 时的 $WD 计数，用来判断"真的等到了" */

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
    (void)uart_link_send(out);      /* 阻塞轮询，短帧 <20 字节 ⇒ ~0.2 ms @921600 */
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
        return;
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
         * 发的方向多半也没接，这时发命令只是白占阻塞时间。 */
        if (g_uart_rx_bytes != 0u)
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
            s_cmd_phase = CMD_PH_WEA;
            s_cmd_t0    = now_ms;
            s_cmd_try   = 0u;
            s_cmd_wd0   = g_net_wd_pkts;
            g_cmd_weather_req++;
            (void)uart_link_cmd("?WEA", NULL);
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
        g_uart_last_ms = now_ms;

        /* 二进制模式（$!BD 的裸负载）：**按长度收**，不看内容、不参与行重组。
         * 这样负载里含 '$' / '\r' / '\n' 也不会把解析搞乱。 */
        if (s_bin_need != 0u)
        {
            g_cmd_body[s_bin_got++] = c;
            if (s_bin_got >= s_bin_need)
            {
                uint16_t cc = crc16(g_cmd_body, s_bin_need);
                g_cmd_bd_len   = s_bin_need;
                g_cmd_body_len = s_bin_need;
                s_bin_need     = 0u;
                if (cc != s_bin_crc)
                    g_cmd_bd_crc_bad++;
            }
            continue;
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
                s_line_n = 0u;          /* 超长：整行作废，等下一帧 */
        }
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
