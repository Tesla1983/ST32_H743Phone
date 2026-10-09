/* ===========================================================================
 * ESP32 ↔ STM32H743 串口链路（**UART4 / PB9-TX / PB8-RX**，**921600** 8N1）
 *
 * ⚠ 2026-10-09 换外设：原先是 USART6(PC6/PC7)。PC6 那根命令线始终不通，
 *   双向发波验证过「PC6 与对端无电气连接」，而 **USART6 在 LQFP100 上只有
 *   PC6/PC7 一组**（PG14/PG9 该封装没有）⇒ 换脚只能换外设，改到 UART4。
 *   PB9/PB8 官方功能是摄像头 SCCB 的 SDA/SCL，摄像头未插即空闲。
 *   （PC7 在排针上紧邻 PC6，故此前"PC6 不在排针"的判断已证伪——真正原因是接错位置。）
 *
 * 接线：
 *   PB9 (UART4_TX) → 对端帧口的 RX
 *   PB8 (UART4_RX) ← 对端帧口的 TX
 *   GND ↔ GND
 *
 * ⚠ 2026-10-09 实测更正：对端**帧口默认是 UART0 = GPIO1(TX)/GPIO3(RX)**，
 *   不是原来以为的 GPIO17。判据 = 板上 g_uart_line 里出现过完全不含 '$' 的
 *   纯日志行（`I (62165694) wifi_sta: [状态] 已连接 ...`），说明 PC7 上跑的
 *   一直是 ESP32 的 console 日志流；界面上的时间靠"取最后一个 '$'"从日志行里
 *   抠出来，属于**靠巧合工作**。对端已重写串口层（帧/日志分口），
 *   详见 E:\workbuddy\esp32-com8\docs\UPLINK-NTP-WEATHER.md 第六节。
 *   修完之后 g_uart_line 收到的是 `$DT,1791533908,2026-10-09,16:18:28,5*25`
 *   这种 39 字节纯帧（此前是 100~148 字节的日志行）。
 *
 * 对端固件与协议出自 E:\workbuddy\esp32-com8（docs/UPLINK-NTP-WEATHER.md）。
 * 帧格式（NMEA 风格，'\r\n' 结尾）：
 *     $<TYPE>,<字段1>,<字段2>,...*<HH>\r\n
 *   HH = 对 "<TYPE>,<字段...>"（不含 '$'、不含 '*'）逐字节 XOR，2 位大写十六进制。
 *
 *   $WX,<citykey>,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text>*HH
 *                                                      逐城天气，$?WEA,<城市码> 的应答。
 *          citykey = 当初问的那个城市码（原样回传）⇒ 靠它对号入座，
 *          某一城超时后后续帧不会错位记到别的城市上。
 *          其余字段与 $WD 相同。
 *   $DT,<unix_ts>,<YYYY-MM-DD>,<HH:MM:SS>,<wday>*HH     时间，每 10 s 一帧
 *          unix_ts=UTC 秒；日期与时间**已按 CST-8 换算** ⇒ 可直接显示，不要再加时区
 *          wday = ISO 8601：1=周一 … 7=周日
 *   $WD,<temp_x10>,<rh>,<pm25>,<aqi>,<code>,<text>*HH   天气，每 30 min 一帧
 *          temp_x10 = 温度×10 的整数（200 = 20.0 ℃，负数带 '-'）
 *          code = 天气数字码（0=晴 1=多云 2=阴 3=小雨 … 99=未识别）
 *          text = UTF-8 原始文本（可能是中文，如"晴"）⇒ 参与 XOR，别按字符数算
 *   $WF,<up>,<ip>,<rssi>*HH                            WiFi 状态，变化时各发一次
 *
 * 实测样本（对端日志，校验值已独立验算）：
 *   $DT,1791469600,2026-10-08,22:26:40,4*28
 *   $WD,200,47,65,111,0,晴*EB
 *   $WF,1,192.168.1.2,-76*08
 *
 * ---- 命令通道（2026-10-09 新增，把对端 ESP32 当成"外挂 WiFi"用）----
 *   上面那一组是**推送**（对端主动，10 s / 30 min 一帧）。命令通道把方向反过来：
 *
 *     命令（本板 → 对端）  $?<CMD>[,<参数>]*HH\r\n
 *     响应（对端 → 本板）  $!RS,<CMD>,<rc>[,<附加>]*HH\r\n      单行响应
 *                          $!BD,<id>,<len>,<crc16>*HH\r\n      头帧
 *                          <紧跟 len 字节裸负载>                二进制负载
 *
 *   $?PING                 → $!RS,PING,1,<uptime_ms>   握手（判据：g_cmd_tx/rx 都涨）
 *   $?WEA                  → $!RS,WEA,1（已受理，随后推 $WD）/ -1（对端未联网）
 *                            解决"STM32 复位后天气最多要等 30 min"的问题：开机问一次。
 *   $?WEA,<城市码>         → $!RS,WEA,1（已受理，随后推 **$WX**）/ 0（对端忙，稍后重试）
 *                            逐城天气（2026-10-10 新增，给天气 app 的多城列表用）。
 *                            ⚠ 应答是 $WX 不是 $WD：$WD 是**默认城市**的周期推送，
 *                              桌面/控制中心按它显示；逐城结果若也挤进 $WD，
 *                              那些界面会把杭州当成默认城市显示 —— 那是骗人。
 *                            ⚠ **一次只能问一城**：对端邮箱只有一个槽位（见对端
 *                              time_weather.c 的说明），连发会回 $!RS,WEA,0。
 *                              本模块因此做成"发一城 → 等 $WX 或超时 → 再下一城"。
 *   $?WGET,<id>,<url>      → $!BD,<id>,<len>,<crc16> + len 字节裸负载；失败回 $!RS,WGET,0
 *                            通用 HTTP 通道，给将来下图片 / 下固件用。
 *
 *   ⚠ 大数据为什么要"头帧 + 定长裸负载"：本模块单行上限只有 200 字节
 *     （UART_LINK_LINE_MAX，超了整行作废），而 HTTP 响应随便几 KB。
 *     所以头帧仍是普通 ASCII 行（有 XOR，长度与 CRC 可信），
 *     之后**按长度收**恰好 len 字节 —— 负载里含 '$' 或换行都不会乱。
 *     本模块由此多出一个"二进制模式"：进入后字节不再参与行重组。
 *
 *   ⚠ 发送方向要接一根线：PB9(UART4_TX) → 对端命令口的 RX。
 *     默认推荐接到 **GPIO16**（对端 UART2 的 RX，纯 GPIO）；
 *     **不要**接到 GPIO3 —— 那是开发板上标 "RX" 的脚，已被板载 CH340 的 TX
 *     驱动着，两个推挽驱动同一条线会形成直通电流。
 *     判据：g_cmd_tx 涨而 g_cmd_rx 不涨 ⇒ 这根线没接（或对端没在听）。
 *
 * ⚠⚠ 三个绕开厂商代码的决定（改这几处前先读懂，否则会原样再踩一遍）：
 *
 * 1. **不能用 HAL 的 HAL_UART_Receive_IT()**。
 *    厂商 third_party/SYSTEM/usart/usart.c 强定义了 `HAL_UART_RxCpltCallback()`
 *    和 `HAL_UART_MspInit()` —— 这俩是**全局唯一**符号，不是 per-instance 的：
 *      - MspInit 里 `if (huart->Instance == USART_UX)`（= USART1）⇒ 给 UART4 调
 *        HAL_UART_Init 时**时钟和 GPIO 一个都不会配**，波特率也算错；
 *      - RxCpltCallback 同样只认 USART1 ⇒ 开 IT 接收的话，收完一字节没人重启
 *        下一轮，链路一帧之后就死。
 *    ⇒ 本模块：GPIO/时钟/波特率**自己配**（不走 MspInit）；
 *      接收**自己写 ISR 直接读 RDR**（不走 HAL_UART_IRQHandler / 回调）。
 *
 * 2. **UART4 挂在 APB1**，时钟源默认 D2PCLK1 = PCLK1 = 100 MHz
 *    （厂商 sys.c：SYSCLK 400M / HCLK 200M / APB1 = APB2 = 100M，两个都是 /2，
 *    所以换成 UART4 之后波特率分频与原来 USART6 完全相同）。HAL 的
 *    UART_GETCLOCKSOURCE 认 UART4（stm32h7xx_hal_uart_ex.h:286），未用 RCCEx
 *    改过源时取的就是 PCLK1 ⇒ 波特率算得对。
 *    2026-10-09 提到 921600：整数分频 BRR=109 ⇒ 实际 917 431（−0.45%），
 *    对端 80 MHz + 小数分频（<0.02%）⇒ 合计约 0.46%，在 8N1/16 倍采样容差内。
 *
 * 3. **中断向量要手工开槽**。启动文件原来是 `.rept 150` 全落 Default_Handler
 *    （死循环）。**UART4_IRQn = 52**，150 现拆成 49 + 1 + 2 + 1 + 18 + 1 + 49 + 1 + 28
 *    （已为 SDMMC1/IRQ49、USART6/IRQ71（停用保留）、JPEG/IRQ121 开过槽）。
 *
 * 判据（SWD 直接读，不需要屏幕也不需要串口助手）：
 *   g_uart_rc       = 0       初始化成功
 *   g_uart_rx_bytes 在涨      线上真有字节进来（恒为 0 ⇒ 线不通或波特率错）
 *   g_uart_frames   在涨      帧重组 + XOR 校验通过
 *   g_uart_err_fe   = 0       帧错误 0 ⇒ 波特率对（波特率错必然 FE 或一堆乱码）
 *   g_net_dt_pkts / g_net_wd_pkts  分别收到几帧时间/天气
 * =========================================================================== */

#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>

#define UART_LINK_LINE_MAX  200u   /* 单行上限，超了丢掉重来 */

/* 命令通道的负载上限（$!BD 的裸数据）。与对端 uplink_cmd.c 的 CMD_BODY_CAP
 * **必须一致**：不一致时会出现"对端发 2048、本板判超限丢弃"。 */
#define UART_LINK_BODY_CAP  2048u

/* ---- 链路诊断量（全部 volatile，SWD 可读）---- */
extern volatile int      g_uart_rc;         /* 0 = 初始化成功；<0 见 .c 里的返回码 */
extern volatile uint32_t g_uart_baud;       /* 实际生效波特率 */
extern volatile uint32_t g_uart_rx_bytes;  /* 累计收到字节数（关键：为 0 = 线没通） */
extern volatile uint32_t g_uart_frames;    /* 累计完成帧数（见到 \n） */
extern volatile uint32_t g_uart_rx_drop;   /* 环形缓冲满而丢弃的字节 */
extern volatile uint32_t g_uart_err_ore;   /* 溢出：主循环没及时取走 */
extern volatile uint32_t g_uart_err_ne;    /* 噪声 */
extern volatile uint32_t g_uart_err_fe;    /* 帧错误（波特率不对的典型症状） */
extern volatile uint32_t g_uart_err_pe;    /* 奇偶错误（8N1 下不该出现） */
extern volatile uint32_t g_uart_idle_ms;   /* 距上次收到字节的毫秒数 */
extern volatile uint32_t g_uart_last_ms;   /* 上次收到字节时的毫秒时基 */

/* 最后一帧原文（NUL 结尾）—— 排障第一手证据：先看清对端到底发了什么，
 * 再改解析器，不要凭猜测写协议。 */
extern volatile uint8_t  g_uart_line[UART_LINK_LINE_MAX];
extern volatile uint32_t g_uart_line_len;

/* 开机后最早的 16 个字节。对端若**不发换行符**，g_uart_line 会一直是空的，
 * 这时靠这 16 个字节也能看出它在发什么；还能看波特率对不对
 * （收到 0x00/0xFF 或固定乱码，多半是速率或电平不匹配）。 */
extern volatile uint8_t  g_uart_raw[16];
extern volatile uint32_t g_uart_raw_n;

/* ---- 帧解析结果 ---- */
extern volatile uint32_t g_net_epoch;      /* $DT 的 unix 秒（0 = 还没同步过） */
extern volatile int      g_net_hh, g_net_mm, g_net_ss;   /* $DT 给的本地时间（CST-8） */
extern volatile int      g_net_year, g_net_mon, g_net_mday, g_net_wday;
extern volatile uint32_t g_net_epoch_at;   /* 收到该帧时的板载毫秒时基 */

extern volatile int      g_net_temp_x10;   /* $WD 温度×10（200 = 20.0 ℃） */
extern volatile int      g_net_rh;         /* 相对湿度 % */
extern volatile int      g_net_pm25;
extern volatile int      g_net_aqi;
extern volatile int      g_net_code;       /* 天气码，-1 = 没收到过 */
extern volatile uint8_t  g_net_wx_text[24];/* $WD 的 UTF-8 天气文本（"晴"） */

extern volatile int      g_net_wf_up;      /* $WF：1=已连上 */
extern volatile int      g_net_wf_rssi;
extern volatile uint8_t  g_net_wf_ip[24];

extern volatile uint32_t g_net_dt_pkts;    /* 收到的 $DT 帧数 */
extern volatile uint32_t g_net_wd_pkts;    /* 收到的 $WD 帧数 */
extern volatile uint32_t g_net_wf_pkts;    /* 收到的 $WF 帧数 */
extern volatile uint32_t g_net_wx_pkts;    /* 收到的 $WX（逐城天气）帧数 */
extern volatile uint32_t g_net_xor_fail;   /* XOR 校验失败的帧数 */
extern volatile uint32_t g_net_bad_pkts;   /* 有 '$' 有 '*' 但字段不对的帧数 */

/* ---- 命令通道诊断量（2026-10-09）----
 * 判链路是否双向通：**只看 g_cmd_tx 与 g_cmd_rx**。
 *   g_cmd_tx 涨、g_cmd_rx 不涨  ⇒ 发送方向的线没接 / 对端没在听（最常见）
 *   两者都涨                    ⇒ 双向通
 */
extern volatile uint32_t g_cmd_tx;           /* 已发出的命令帧数 */
extern volatile uint32_t g_cmd_rx;           /* 已收到的 $! 响应帧数 */
extern volatile uint32_t g_cmd_xor_fail;     /* 响应校验失败（说明线路有噪声） */
extern volatile uint32_t g_cmd_ping_ok;      /* PING 握手成功次数 */
extern volatile uint32_t g_cmd_weather_req;  /* 发出的 $?WEA 次数（默认城市） */
extern volatile uint32_t g_cmd_weather_ack;  /* 收到 $!RS,WEA,1 的次数 */
extern volatile uint32_t g_cmd_weather_ok;   /* 请求之后确实等到 $WD 的次数 */
extern volatile uint32_t g_cmd_phase;        /* 命令状态机当前阶段（见 .c 里的枚举） */

/* ---- 逐城天气（2026-10-10，天气 app 用）----
 * 判据一句话：**g_city_ok 达到城市数（3）** 即三个城市都拿到真数据。
 *   g_city_req  已发出的 $?WEA,<城市码> 次数（每城一次，逐城串行）
 *   g_city_ok   本轮已拿到数据的城市数（0..CITY_N）
 *   g_city_to   逐城请求超时次数（对端没回 $WX；连着涨 = 对端没联网或不认这个命令）
 *   g_city_scan 完成的整轮扫描次数
 *   g_city_last_scan 上一轮**到齐**的城市数（== 3 才算全绿；< 3 则 60 s 后自动补扫）
 *   g_city_idx  当前正在问的城市下标（-1 = 当前没在扫描）
 * ⚠ 复位后要先让开机序列跑完（PING→WEA→WDATA，几秒），扫描才开始。 */
extern volatile uint32_t g_city_req;
extern volatile uint32_t g_city_ok;
extern volatile uint32_t g_city_to;
extern volatile uint32_t g_city_scan;
extern volatile uint32_t g_city_last_scan;
extern volatile int      g_city_idx;

/* $!BD 大块数据 */
extern volatile uint32_t g_cmd_bd_pkts;      /* 收到 BD 头帧数 */
extern volatile uint32_t g_cmd_bd_id;        /* 最后一次的 id（回传给请求方） */
extern volatile uint32_t g_cmd_bd_len;       /* 最后一次收到的负载长度 */
extern volatile uint32_t g_cmd_bd_crc_bad;   /* CRC 校验失败次数 */
extern volatile uint32_t g_cmd_bd_timeout;   /* 等负载超时（长度对不上）次数 */
extern volatile uint32_t g_cmd_bd_toobig;    /* 长度超过本板缓冲，直接丢弃 */
extern volatile uint32_t g_cmd_bd_want;      /* 上次超时：对端声明的负载字节数 */
extern volatile uint32_t g_cmd_bd_got;       /* 上次超时：本板实际收到的字节数 */
extern volatile uint8_t  g_cmd_body[UART_LINK_BODY_CAP];  /* 最近一次收到的负载 */
extern volatile uint32_t g_cmd_body_len;

/* 手工触发（脚本用）：写 1 = 发一次 $?PING；写 2 = 发一次 $?WEA；
 * 写 3 = 发一次 $?WGET 取 httpbin 的 1 KB 测试数据。固件发完自动清 0。 */
extern volatile uint32_t g_cmd_req;

/* ---- 自检（没有对端也能验证「解析 → 换算 → 显示」这条链）----
 * 写 1：把上面三行实测样本逐字节喂进**真正的解析函数**（不是另写一份），
 *       跑完固件清 0。链路本身通不通仍然要看 g_uart_rx_bytes 有没有在涨 ——
 *       自检只证明下游对，不证明线通。
 *
 *   结果落在 **g_uart_st_*** 这一组"影子变量"里，不读 g_net_*：
 *   脚本写完 g_uart_selftest 要等半秒才来读，而对端每 10 s 一帧，
 *   主变量早就被真实数据盖掉了（2026-10-08 实测：自检读到 $DT +2、23:15:27，
 *   正是被真实帧覆盖）。 */
extern volatile uint32_t g_uart_selftest;
extern volatile uint32_t g_uart_st_dt, g_uart_st_wd, g_uart_st_wf, g_uart_st_xor;
extern volatile int      g_uart_st_hh, g_uart_st_mm, g_uart_st_ss;
extern volatile int      g_uart_st_mon, g_uart_st_mday, g_uart_st_wday;
extern volatile uint32_t g_uart_st_epoch;
extern volatile int      g_uart_st_temp, g_uart_st_code;

/* ---- 接口 ---- */
int  uart_link_init(uint32_t baud);        /* 返回 0 成功 */
void uart_link_poll(uint32_t now_ms);      /* 主循环每拍调一次 */
int  uart_link_send(const char* text);     /* 发送（回显/握手用），返回发出字节数 */

/* 发一条命令帧 $?<type>[,<body>]*HH\r\n（body 为 NULL 或空串时不带逗号）。
 * 返回发出的字节数；<0 = 参数非法或缓冲不足。
 * ⚠ 发送是**阻塞轮询**（115200 下每字节 ~87 µs，921600 下 ~11 µs），命令帧都很短，
 *   但别在渲染路径里高频调用。 */
int  uart_link_cmd(const char* type, const char* body);

/* ---- 给 UI 的换算结果 ----
 * 为什么要这几个：phone_shell 不认识 g_net_* 这些变量，也不该认识（与
 * BoardGallery_* 同一套分层）。这里只给"界面能直接画的字符串"，
 * 温度×10 → "20℃"、wday → "星期四" 这类换算全部收在本模块里。
 * ⚠ 没同步到时返回**占位串**（"--:--" / "等待天气"）而不是全零 ——
 *   界面上要能一眼区分"没数据"和"真的是 0 点"。
 * 声明同时被 phone_shell_board.h 转给 phone_shell（那里 include 了本头）。 */
int  BoardNet_HasTime(void);
int  BoardNet_HasWeather(void);
void BoardNet_ClockHM(char* out, int n);      /* "HH:MM"     状态栏 */
void BoardNet_ClockHMS(char* out, int n);     /* "HH:MM:SS"  （诊断/备用） */
void BoardNet_ClockHH(char* out, int n);      /* "HH"   ┐ 桌面大时间用，拆开是为了 */
void BoardNet_ClockMM(char* out, int n);      /* "MM"   ┘ 让冒号闪烁不把分钟挤位 */
int  BoardNet_ColonBlink(void);               /* 1 = 冒号该亮（秒为偶数时亮，1 s 周期） */
void BoardNet_DateText(char* out, int n);     /* "10月8日 星期四" */
void BoardNet_WeatherText(char* out, int n);  /* "晴 / 20℃" */
int  BoardNet_WeatherCode(void);              /* 天气码；-1 = 没收到，图标按它选 */

/* ---- 逐城天气（给天气 app 用，2026-10-10）----
 * 与上面同一套分层：UI 只拿"能直接画的字符串"，温度×10 → "21℃" 这类换算
 * 收在本模块里；phone_shell 不认识 g_city_* / s_city_*。
 *
 * ⚠ 数据来自 ESP32 的 $?WEA,<城市码> → $WX，是**真网络数据**；
 *   拿不到时返回占位串（"--" / "等待数据"），界面上要能一眼区分
 *   "没取到"和"真的是 0℃"（与 BoardNet_WeatherText 同一条纪律）。
 *
 * ⚠ 城市表在这里硬编码（3 城），城市码是 2026-10-10 在主机上逐个请求
 *   sojson 接口验证过的（HTTP 200 且 cityInfo.city 与名字一致），
 *   不是照抄网上的清单。要加城市：往 s_city_name / s_city_key 里各加一项，
 *   并把 CITY_N 改掉（UI 会自动多出一行）。 */
int  BoardNet_CityCount(void);                     /* 城市数（3） */
const char* BoardNet_CityName(int i);              /* "杭州"；i 越界返回 "" */
int  BoardNet_CityHasData(int i);                  /* 1 = 已拿到真数据；0 = 还没有 */
int  BoardNet_CityCode(int i);                     /* 天气数字码；-1 = 无数据 */
int  BoardNet_CityTempX10(int i);                  /* 温度×10；无数据返回 0 且 HasData=0 */
int  BoardNet_CityRh(int i);                       /* 湿度 %；-1 = 无数据 */
int  BoardNet_CityPm25(int i);                     /* PM2.5；-1 = 无数据 */
int  BoardNet_CityAqi(int i);                      /* AQI；-1 = 无数据 */
void BoardNet_CityTempText(int i, char* out, int n);   /* "21℃" / "--" */
void BoardNet_CityCondText(int i, char* out, int n);   /* "晴" / "等待数据" */
void BoardNet_CitySubText(int i, char* out, int n);    /* "湿度 69% · PM2.5 31" / "" */
int  BoardNet_CityUpdatedCount(void);              /* 已拿到数据的城市数（0..N） */
int  BoardNet_CityBusy(void);                      /* 1 = 正在扫描（界面可显示"更新中"） */

/* ---- WiFi 链路状态（给状态栏图标用）----
 * ⚠ 数据来源是 ESP32 的 `$WF,<up>,<ip>,<rssi>` —— **WiFi 射频在对端**，
 *   本板没有 WiFi。这里报的是"经 ESP32 网关的联网状态"，不是本芯片的无线状态。
 *   界面上画 WiFi 图标是诚实的（链路确实存在且已通），但语义要写清楚，
 *   别让后来者以为 H743 自带 WiFi。
 * ⚠ 2026-10-10 之前这几个量解析了却没人用（UI 层一处都没引用），
 *   状态栏那 4 格信号柱是硬画的假信号 —— 现在换成接真值的 WiFi 图标。 */
int  BoardNet_WifiUp(void);                   /* 1=已连  0=已断开  -1=还没收到过 $WF */
int  BoardNet_WifiRssi(void);                 /* dBm（负数）；没收到过返回 0 */
int  BoardNet_WifiBars(void);                 /* 0..3 信号格数；-1 = 还没收到过 $WF */
const char* BoardNet_WifiIp(void);            /* "192.168.1.2"；没收到过返回 "" */

#endif /* UART_LINK_H */
