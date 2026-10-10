/* ===========================================================================
 * 蓝牙文件接收会话（P4，2026-10-10）—— STM32 侧
 *
 * 【它在整条链路的哪一段】
 *   手机 --(经典蓝牙 SPP)--> ESP32 --(SPI 从机)--> **本模块** --(FatFs)--> TF 卡
 *                                                          --> img_store 导入
 *                                                          --> 相册可见
 *   本模块负责：开会话、按序号拉块、校验 CRC、写 TF 卡、收尾导入图库。
 *   协议的真源在对端工程 main/bt_file.h 顶部（那边写的是协议本身），
 *   本文件只写"本板这一侧怎么用它"。
 *
 * 【为什么是"本板拉"不是"对端推"】
 *   SPI 上本板是主机、ESP32 是从机。从机只能把数据塞进自己的出站队列等主机来取，
 *   而那个队列只有 4 KB、文件动辄上百 KB ⇒ 推模式必然溢出丢数据。
 *   改成"本板按序号拉"，背压天然成立：本板不拉，对端就不放。
 *
 * 【序号 + 重传（这是这一层最要紧的设计）】
 *   每块 2 KB 的数据要走 SPI 出去，而**一笔事务只搬 61 字节** ⇒ 一块要 35 笔。
 *   任何一笔被截断，整块就废了 —— 而对端的块一旦离开环形缓冲就没了。
 *   所以本模块每次 GET 都带**期望序号**，CRC 不过就**重拉同一序号**；
 *   对端留了"最后发出去那一块"的副本，见到上一序号就原样重发。
 *   ⇒ 代价是两边各 2 KB RAM，换来"坏块可恢复"。
 *
 * 【⚠ 判据必须用 g_cmd_bd_done，不能用头帧计数】
 *   见 uart_link.h 里 g_cmd_bd_done 的说明：头帧到了、负载还在路上（35 笔事务），
 *   拿头帧计数当"块到了"会读到半块数据。CRC 结论同样要等收齐之后才成立，
 *   所以解析层额外给了 g_cmd_bd_last_ok（本块 CRC 是否通过）。
 *
 * 【状态码与 $BT 帧一一对应】
 *   `$BT,<state>,<name>,<size>,<recv>` 由对端在会话每次推进后推来，
 *   本模块的 bt_recv_state() 与它同值（0/1/2/3/4），UI 直接画。
 *   ⚠ 收尾之后本模块**不再跟随** $BT（那之后对端会推 state=0）：
 *     状态停在 DONE，用户才看得见"刚刚收了一个文件"。
 * =========================================================================== */

#ifndef BT_RECV_H
#define BT_RECV_H

#include <stdint.h>

/* 会话状态（与对端 $BT 帧的 state 字段一一对应） */
#define BT_RECV_IDLE   0   /* 没有会话 */
#define BT_RECV_WAIT   1   /* 会话已开，等对端连接 / 等文件头 */
#define BT_RECV_RECV   2   /* 正在接收（已经在写卡了） */
#define BT_RECV_DONE   3   /* 文件收全且已落盘（并已排队导入图库） */
#define BT_RECV_ERROR  4   /* 出错（卡写失败 / 超时 / 序号对不上） */

/* ---- 会话控制（UI 的"接收文件"按钮与验收脚本都调这几个）---- */

/* 开始接收：发 `$?BTF,OPEN`（对端开 BT 无线电 + 起 SPP 服务端 + 开会话）。
 *
 * 返回 0 = 已发起；-1 = 已有会话在跑（对端的会话是全局单例，重叠必然串数据）。
 *
 * ⚠ 这里**没有**"链路没有对端"这种返回码（2026-10-10 修正：头文件以前写的 -2
 *   **代码里从来不会返回**，属文档跑在实现前面）。链路不通的表现是：
 *   begin() 照常返回 0、会话进 WAIT，然后 4 s × OPEN_TRY_MAX 次重试都无人应答，
 *   最终落到 `g_bt_err = BTE_OPEN_TO(2)`。**判"通没通"要看 g_bt_err，不要看返回值。**
 * ⚠ 真实路径（`$?BTF,OPEN` → 对端开经典蓝牙 + 起 SPP 服务端）板上实测耗时
 *   **约 560 ms**（见 docs/DEVELOPMENT_LOG.md 2026-10-10（5）），故 4 s 的
 *   OPEN_ACK_MS 余量充足。 */
int  bt_recv_start(void);

/* 自测：让对端**合成**一张 w×h 的 24 位 BMP，走与手机完全相同的下游管道
 * （ESP32 → SPI → 本模块 → TF → 图库）。不需要手机、不需要蓝牙，
 * 是 P4 验收的主路径。w/h 越界（<8 或 >1024）返回 -1。 */
int  bt_recv_start_test(uint32_t w, uint32_t h);

/* 中止：发 `$?BTF,ABORT`，并**删掉半截文件**（不留坏文件给相册扫）。
 *
 * 返回 0 = 已中止（本板侧已复位）；-1 = 本来就没有会话。
 * ⚠ 返回值只代表**本板**停了，不代表对端收到了 ABORT —— 命令通道有 6%~20% 丢包，
 *   而 ABORT 是一次性的（不像 GET 有无应答重发）。所以这里**连发 3 遍**提高命中率，
 *   并且依赖两条保证让"全丢"也无害：
 *     ① 对端 `bt_file_abort()` 在 IDLE 时直接 return（重复发送幂等）；
 *     ② 对端 `bt_file_open()` 第一行就是 `session_reset()` ⇒ 下次 OPEN 无条件自愈。
 *   ⇒ **判"中止成功"不能看对端 `$BT` 是否变 0**（实测那一轮它没变，因为命令全丢了，
 *     但本板已正确复位、下次能正常再开）。板上验收脚本 `tools/bt_real_check.py`
 *     的 R4 就是这个判据：**中止后能立刻开起一个新会话**（begin() 在 WAIT 时会拒绝，
 *     所以"能开"恰恰证明复位成功了）。 */
int  bt_recv_abort(void);

/* 主循环每拍调一次。
 * ⚠ 必须排在 uart_link_poll / spi_link_poll **之后**：它要读本拍刚被解析出来的
 *   $!BD 收齐标志、$!RS,BTF 响应、$BT 状态帧。 */
void bt_recv_poll(uint32_t now_ms);

/* ---- 给 UI 用（board 层转给 phone_shell；UI 不认识 g_net_btf_*）---- */
int          bt_recv_state(void);        /* BT_RECV_* 之一 */
int          bt_recv_progress(void);     /* 0..100；-1 = 总长未知（进度条画不定长态） */
uint32_t     bt_recv_bytes(void);        /* 已写进 TF 的字节数 */
uint32_t     bt_recv_total(void);        /* 对端声明的总字节数（0 = 未知） */
const char*  bt_recv_name(void);         /* 落地的文件名（不含目录）；无 = "" */
int          bt_recv_last_rc(void);      /* 最近一次会话的结果码：0 = 成功，<0 见 g_bt_err */
const char*  bt_recv_status_text(void);  /* 可直接画的一行状态文字 */
int          bt_recv_busy(void);         /* 1 = 会话在跑（UI 该显示进度而不是按钮） */

/* ---- 诊断量（全部 volatile，SWD 直读；验收脚本按这些判）----
 * ⚠ 每一个都必须在 .c 里被**写过**：本工程开了 --gc-sections，
 *   只声明不使用的全局会被整段回收，脚本读符号时直接 KeyError。 */
extern volatile uint32_t g_bt_sess;      /* 开启过的会话数 */
extern volatile uint32_t g_bt_chunks;    /* 成功写进 TF 的块数 */
extern volatile uint32_t g_bt_bytes;     /* 写进 TF 的总字节数（= 文件大小）*/
extern volatile uint32_t g_bt_crc_bad;   /* 收到的坏块数（CRC 不过）*/
extern volatile uint32_t g_bt_retx;      /* 重取块的次数（>0 = 路上丢过块但被兜住了）*/
extern volatile uint32_t g_bt_get_n;     /* 发出的 $?BTF,GET 次数 */
extern volatile uint32_t g_bt_done;      /* 成功完成的会话数 */
extern volatile int      g_bt_rc;        /* 最近一次会话的结果码，0 = 成功 */
extern volatile uint32_t g_bt_err;       /* 出错阶段（BTE_*，定位失败点）*/
extern volatile uint32_t g_bt_fs_rc;     /* 最后一个 FRESULT */
extern volatile uint32_t g_bt_wr;        /* 最近一块 f_write 实际写出的字节数 */
extern volatile uint32_t g_bt_imp_rc;    /* 收尾"导入图库"的返回码，0 = **已排队**（不代表成功）*/
extern volatile uint32_t g_bt_imp_final; /* ★导入最终结果：0 = 真进相册；非 0 = img_store 的 RC_*；
                                          *   0xFFFFFFFF = 还没出结论。UI 的成功文案只看它。 */
extern volatile uint32_t g_bt_spi_tx;    /* 本次会话新开的 SPI 事务数（算实际吞吐：字节/事务）*/
extern volatile uint32_t g_bt_get_ack_ms;/* GET 应答等待上限（ms），运行期可写，用于扫档 */
extern volatile uint32_t g_bt_wait_max_ms;/* "一字节未到"阶段的总时长上限（ms），可写 */

/* ⚠ 两个"给吞吐判据用"的量（2026-10-10 补）——**必须由固件自己量**：
 *   `bt_file_check.py` 每取一次快照要把 DTCM 逐字节读一遍，实测 ~6 s，期间主循环
 *   被拖慢一个数量级、还会把主机侧的命令吃掉。拿脚本的墙钟算吞吐会把 100 KB
 *   算成 73 s，与真值差一个数量级。 */
extern volatile uint32_t g_bt_ms;        /* 本会话墙钟（ms，主循环 uptime）；0 = 还没跑过 */
extern volatile uint32_t g_bt_get_to;    /* 本次"GET 发了 2 s 无应答"的笔数 */
extern volatile uint32_t g_bt_get_max;   /* 最长**连续**无应答笔数：≥6 就足以判 STALL，
                                          * 是区分"链路真断"与"测量干扰"的判据 */

#endif /* BT_RECV_H */
