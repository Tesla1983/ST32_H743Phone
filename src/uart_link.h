/* ===========================================================================
 * ESP32 ↔ STM32H743 串口链路（USART6 / PC6-TX / PC7-RX，115200 8N1）
 *
 * 接线：
 *   PC6 (USART6_TX) → 对端帧口的 RX
 *   PC7 (USART6_RX) ← 对端帧口的 TX
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
 * ⚠⚠ 三个绕开厂商代码的决定（改这几处前先读懂，否则会原样再踩一遍）：
 *
 * 1. **不能用 HAL 的 HAL_UART_Receive_IT()**。
 *    厂商 third_party/SYSTEM/usart/usart.c 强定义了 `HAL_UART_RxCpltCallback()`
 *    和 `HAL_UART_MspInit()` —— 这俩是**全局唯一**符号，不是 per-instance 的：
 *      - MspInit 里 `if (huart->Instance == USART_UX)`（= USART1）⇒ 给 USART6 调
 *        HAL_UART_Init 时**时钟和 GPIO 一个都不会配**，波特率也算错；
 *      - RxCpltCallback 同样只认 USART1 ⇒ 开 IT 接收的话，收完一字节没人重启
 *        下一轮，链路一帧之后就死。
 *    ⇒ 本模块：GPIO/时钟/波特率**自己配**（不走 MspInit）；
 *      接收**自己写 ISR 直接读 RDR**（不走 HAL_UART_IRQHandler / 回调）。
 *
 * 2. **USART6 挂在 APB2**，时钟源默认 D2PCLK2 = PCLK2 = 100 MHz
 *    （厂商 sys.c：SYSCLK 400M / HCLK 200M / APB2 = 100M）。HAL 的
 *    UART_GETCLOCKSOURCE 认 USART6（stm32h7xx_hal_uart_ex.h:340），未用 RCCEx
 *    改过源时取的就是 PCLK2 ⇒ 115200 的 BRR 算得对。
 *
 * 3. **中断向量要手工开槽**。启动文件原来是 `.rept 150` 全落 Default_Handler
 *    （死循环）。USART6_IRQn = 71，要把 150 拆成 49 + 1 + 21 + 1 + 78
 *    （2026-10-08 已为 SDMMC1/IRQ49 开过一次槽，这是第二次）。
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
 * （115200 收到 0x00/0xFF 或固定乱码，多半是速率或电平不匹配）。 */
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
extern volatile uint32_t g_net_xor_fail;   /* XOR 校验失败的帧数 */
extern volatile uint32_t g_net_bad_pkts;   /* 有 '$' 有 '*' 但字段不对的帧数 */

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

#endif /* UART_LINK_H */
