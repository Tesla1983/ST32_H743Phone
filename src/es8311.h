/* ===========================================================================
 * ES8311 音频 codec 驱动（控制通道：I2C1 / PB6-SCL / PB7-SDA）
 *
 * 硬件背景（2026-10-12，方案 A 接线）
 * ---------------------------------
 * 模块：鹿小班 ES8311 + NS4150B（I2C 配寄存器 + I2S 传音频，NS4150B 直接推喇叭，
 *       无使能脚 —— 不需要像 ESP32 板那样额外拉 PA_EN）。
 * 连线：PB6/PB7 = I2C1_SCL/SDA（AF4，空闲的摄像头脚，摄像头未插）；
 *       I2S2 = PC6(MCLK) / PD3(SCLK) / PB9(LRCK) / PB15 或 PC1(DOUT) / PC2(DIN)，
 *       由 src/audio_test.c 负责，本文件只管 I2C 这一半。
 *
 * 寄存器序列的来源与适配
 * ----------------------
 * 序列移植自 Espressif 官方 esp-adf 的 es8311.c（components/audio_hal/driver/
 * es8311/es8311.c），逐条照抄后只改两处：
 *   ① 8 位地址 0x30/0x32 → 本工程用 HAL 的 7 位地址 0x18/0x19；
 *   ② i2s_iface->mode = SLAVE —— **本板 STM32 是 I2S 主机**（MCLK/SCK/LRCK 全由
 *      STM32 出），所以 REG00 bit6(0x40) 必须清 0（codec 作从机）。
 *      esp-adf 的 ESP32 板默认也是 SLAVE，语义一致。
 * 时钟系数取 esp-adf coeff_div 表里 256fs 的两行（48k/44.1k 完全相同）：
 *   pre_div=1, pre_multi=1, adc_div=1, dac_div=1, fs_mode=0,
 *   lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x10
 *   ⇒ ES8311 侧 fs = MCLK/256、SCK = MCLK/4 = 64fs，
 *     与 STM32 I2S2（MCKOE=1 时 MCLK=256fs、16B_EXTENDED 时 SCK=MCLK/4）**同源同频**，
 *     两边由同一颗 MCLK 推导，不挑 MCLK 绝对精度（本板 MCLK=12.5 MHz ⇒ fs=48.83 kHz，
 *     比标称 48 k 快 1.7%，听感无差，两侧同步所以不会累积滑音）。
 *
 * ⚠ 本文件与 uart_link.c 踩过的同一个坑无关但要照同样的规矩办：
 *   厂商 usart.c 强占了 HAL_UART_MspInit，而 I2C/I2S 的 MspInit 在 HAL 里是
 *   __weak 空函数（third_party 里没有任何模块重定义它们，已全仓 grep 确认）——
 *   所以 GPIO/时钟自己配，HAL_I2C_Init/HAL_I2S_Init 只管外设寄存器。
 *
 * ⚠ I2C 上拉：PB6/PB7 在板上**没有现成上拉**（摄像头座的 SCCB 上拉在 PB8/PB9 上）。
 *   本驱动打开内部上拉（约 40 kΩ）兜底；模块板若自带 4.7k 并联无碍，
 *   若两边都没有、100 kHz 会因上升沿过慢失败 —— 现象是 probe 无应答，先查上拉。
 * =========================================================================== */
#ifndef ES8311_H
#define ES8311_H

#include <stdint.h>

/* 7 位 I2C 从机地址（esp-adf 的 0x30/0x32 是 8 位写法，右移一位即此）*/
#define ES8311_I2C_ADDR_CE0   0x18u
#define ES8311_I2C_ADDR_CE1   0x19u

/* 芯片 ID / 版本寄存器（esp-adf 驱动不读它们，这里读来当"器件真的在"的证据）*/
#define ES8311_REG_ID1        0xFDu
#define ES8311_REG_ID2        0xFEu
#define ES8311_REG_VER        0xFFu

/* ---- 诊断量（SWD 可读；失败路径上也保证被写，防止 --gc-sections 回收）---- */
extern volatile int      g_es_rc;      /* 最近一次 API 返回码：0=OK，<0=失败 */
extern volatile uint32_t g_es_wr_ok;   /* I2C 写成功次数 */
extern volatile uint32_t g_es_wr_ng;   /* I2C 写失败次数 */
extern volatile uint32_t g_es_rd_ok;   /* I2C 读成功次数 */
extern volatile uint32_t g_es_rd_ng;   /* I2C 读失败次数 */
extern volatile uint32_t g_es_bus_rc;  /* es8311_bus_init 的返回码 */
extern volatile uint32_t g_es_reg32_wr;/* REG32（DAC 音量）最后一次**写入**的值；0xFFFF=没写过 */
extern volatile uint32_t g_es_reg32_n; /* REG32 写入次数（每次起播/续播应约 +9：1 静音 + 8 步 ramp）*/
extern volatile uint32_t g_es_rb_vol;  /* REG32 回读值（ramp 走完那一刻，见 es8311_dac_readback）*/
extern volatile uint32_t g_es_rb_pwr;  /* REG12 回读值（0x00 = DAC 上电；0x02 = power-down）*/
extern volatile uint32_t g_es_rb_dac0e;/* REG0E 回读值（0x02 = 正常；0xFF = 挂起后残留）*/
extern volatile uint32_t g_es_rb_mute;  /* REG31 回读值（补丁12）；bit6/5 必须为 0（未静音）*/
/* ★ 2026-10-14（补丁11·D9 配套验证）：**时钟管理寄存器**回读 —— 用来证明"stop 的完整
 *   掉电尾（D9）关掉的时钟（REG00.CSM_ON / REG01 各位使能）在起播时确实被写回来了"。
 *   尤其 g_es_rb_clk1 必须回到 0x3F（MCLK/BCLK/ADCCLK/DACCLK 全开），否则芯片无时钟、
 *   DAC 收不到数据 ⇒ 续播无声；而这件事**音量回读验不出来**（I2C 与音频时钟无关）。 */
extern volatile uint32_t g_es_rb_clk1; /* REG01 回读（期望 0x3F）★ 补丁17 起纳入闭环：不对就补写 */
extern volatile uint32_t g_es_rb_clk0; /* REG00 回读（期望 CSM_ON(bit7)=1）★ 补丁17 起纳入闭环 */
extern volatile uint32_t g_es_rb_clk2; /* REG02 回读（期望 0x00）*/
extern volatile uint32_t g_es_rb_clk6; /* REG06 回读（期望与起播前一致）*/
/* ---- I2C 写失败现场（2026-10-13；暂停→续播"没声音/音量乱"排障）----
 * 每次写失败都先复位 I2C 现场再重试（最多 3 次，g_es_retry_ok 计恢复数）；三次都失败
 * 才计 g_es_fail。失败现场（寄存器号 / HAL 状态 / 总线电平）留在下面这几个量里。 */
extern volatile uint32_t g_es_ng_reg;  /* 最后一次写失败的寄存器号 */
extern volatile uint32_t g_es_ng_st;   /* 该次 HAL_StatusTypeDef：1=NACK 2=BUSY 3=TIMEOUT */
extern volatile uint32_t g_es_ng_nack; /* 三种失败分别累计 */
extern volatile uint32_t g_es_ng_busy;
extern volatile uint32_t g_es_ng_tmo;
extern volatile uint32_t g_es_ng_ring[8];/* 最近 8 次失败：高 8 位寄存器号 / 低 8 位状态码 */
extern volatile uint32_t g_es_ng_head;
extern volatile uint32_t g_es_ng_idr;  /* 失败瞬间 PB6/PB7 电平（IDR bit6/bit7）*/
extern volatile uint32_t g_es_retry_ok;/* 第一次失败、重试成功 的次数 */
extern volatile uint32_t g_es_probe_retry;/* probe 首试没探到、延时后探到的次数 */
extern volatile uint32_t g_es_fail;    /* 重试穷尽仍失败 的次数（>0 = 有写没落进芯片）*/
/* ★ 补丁17：把"哪一支寄存器穷尽了重试"变成可查的数字（旧 g_es_fail 只说笔数、不说对象，
 *   而"丢 0x01/0x00 时钟使能 ⇒ 完全无声"与"丢 0x32 音量 ⇒ 只停在中档"的后果天差地别）。
 *   编码：高 16 位 = 寄存器号；低 16 位 = 最后一次 HAL 状态（0xFF = 故障注入）。*/
extern volatile uint32_t g_es_fail_reg;
/* ★ 补丁17：故障注入 —— 让"对指定寄存器号的下一次写"整段重试全失败一次（默认 0=关）。
 *   只让写返回失败、**不碰总线**；注入一次即自清。用途：证明"时钟寄存器丢写被回读闭环救回"
 *   （验收 tools/es8311_clk_check.py 的 K7）。⚠ 排障用，出货保持 0。 */
extern volatile uint32_t g_es_drop_wr_reg;
/* ★ 补丁17：**状态注入** —— 写 1 ⇒ 主循环下一拍把 REG01 写成 0x00（时钟全关 ⇒ 无声）。
 *   用来复现"起播恢复时钟那一笔写丢了"之后芯片的真实处境（状态级，比 g_es_drop_wr_reg 更贴近
 *   后果）。配合 g_es_clk_loop_en 做 A/B。⚠ 排障用。 */
extern volatile uint32_t g_es_clr_clk_req;
/* ★ 补丁17：回读闭环开关（**默认 1=开**）。0 = 复现旧行为（REG00/REG01 只读不判不补），
 *   仅用于 A/B 证明"把时钟寄存器纳入闭环"这条修法承重。⚠ 出货必须为 1。 */
extern volatile uint32_t g_es_clk_loop_en;
extern volatile uint32_t g_es_fix_wr;  /* 回读校验发现不对、补写 的次数 */

/* ---- 补丁18：录音（ADC / 麦克风）侧 ---- */
extern volatile uint32_t g_es_adc_on;        /* 1 = ADC 通路已按录音配置开好（回读确认通过）*/
extern volatile uint32_t g_es_adc_mic_gain;  /* 最近写入 REG16 的 PGA 增益 */
extern volatile uint32_t g_es_rb_adc0a;      /* REG0A 回读：期望 bit6==0（ADC 通路开）*/
extern volatile uint32_t g_es_rb_adc09;      /* REG09 回读：期望 bit6==1（录音时 DAC 通路关）*/
extern volatile uint32_t g_es_rb_adc17;      /* REG17 回读：期望 0xBF（ADC 音量 0 dB）*/

/**
 * @brief  确保控制通道就绪（I2C1 初始化 + 从机地址探到）。**幂等**，可重复调。
 *
 * 【为什么必须显式调】补丁18 之前只有 audio_out_start() 会建 I2C 总线；而"录音"可能是
 *   开机后第一个音频动作 ⇒ 若不先建通道，所有寄存器写都会 HAL_BUSY 失败。
 * @return 0 = 就绪；-1 = I2C 总线起不来；-2 = 总线上没有 ES8311（没接模块/线错）
 */
int es8311_bus_ready(void);

/**
 * @brief  开 **ADC（麦克风）通路** —— 录音链路第一步（2026-10-16 补丁18）
 *
 * 【为什么必须单独一组】本项目此前只做放音：`dac_path_regs()` 是「DAC 通路开、
 *   **ADC 通路关**（0x0A bit6=1）」+「0x17=0xBF（ADC 侧，本测试不用）」。录音要把这三处反过来，
 *   并且 **ADC 数字音量 REG17 特别容易被静音路径带走**（`es8311_stop_dac()` 会写 0x17=0x00）
 *   ⇒ 走过一次真·掉电后录音就是一片静音，而所有回读都正常 —— 所以这里显式写 0xBF 并回读确认。
 *
 * 【与放音互斥】I2S2 同一时刻只能 TX 或 RX（I2SCFG 二选一）⇒ 录音/放音不能同时进行。
 *   音频侧 I2S 的切换见 src/audio_out.c 的 audio_out_capture_start()。
 *
 * @param  mic_gain_reg  REG16（模拟 PGA 增益）的值；0x24 与官方默认一致
 * @return 0 = 写完且回读确认（g_es_adc_on=1）；<0 = 有写失败或回读值不对
 */
int es8311_adc_start(uint32_t mic_gain_reg);
/**
 * @brief  结束录音，把 codec 恢复成"DAC 静音起播"的干净状态（内部走 dac_path_regs(0x00)）
 * @return 0=OK；<0=有写失败
 */
int es8311_adc_stop(void);
extern volatile uint32_t g_es_recover_pe;/* 因 BUSY/TIMEOUT 做 I2C 软复位(PE 0→1)的次数 */
extern volatile uint32_t g_es_readback; /* 回读体检执行次数（起播后那次 + 播放中每秒那次）*/

/* ★★ 2026-10-14（补丁13）：I2C 阻塞时间 + 静音确认 —— "偶发卡死"与"爆音"的量化判据 ★★
 *
 * 【为什么有这一组量】用户报两个现象：① 音频里仍有咔嗒（爆音）；② 快速点播放/暂停
 *   若干轮后整个 UI 卡住（约 1/3 概率）。本轮实测（tools/ui_freeze_repro.py 40 轮真实点击）
 *   两者是**同一个根因**：I2C 控制通道不可靠 ——
 *     · 复现瞬间 `g_es_fail = 49`（49 个寄存器 5 次重试全灭）、`g_es_breaker_trip` **恒 0**
 *       （熔断器从未生效）；失败类型 100% 是 NACK，失败环里出现最多的是 **0x31（静音）
 *       与 0x32（音量）** —— 恰好就是"切时钟前最后一道防线"那两个寄存器。
 *     · 卡死的直接机制：HAL 的 `HAL_I2C_Master_Transmit` 开头等 BUSY 的时长是
 *       **源文件里的常量 25 ms**，与本项目传的 5 ms 无关 ⇒ 总线烂掉时每笔都白等 25 ms。
 *   于是：静音写丢了 ⇒ 切时钟时 DAC 满音量 ⇒ **爆音**；重试风暴 × 25 ms ⇒ 主循环停转。
 *
 * 【本组量的读法】
 *   · g_es_tx_us_max / g_es_slow_n —— 单笔事务最长耗时与"慢笔"计数：**卡死的直接量化**。
 *   · g_es_busy_stuck_n / g_es_rcc_rst_n —— 命中"外设卡在 BUSY"并绕开 HAL 的次数（每次
 *     命中就省下 25 ms）；外层用 RCC 硬件复位清 BUSY。
 *   · g_es_bc_fail —— bus clear 做完 SDA 仍被钳住的次数（真·卡死 still there 的证据）。
 *   · g_es_silent_n / g_es_silent_ng —— "静音并确认"成功/失败次数。**ng > 0 = 有爆音风险**
 *     （那一刻切时钟时 DAC 可能还挂在有效音量上）。
 *   · g_es_pdown_n —— 真·掉电（es8311_stop_dac）执行次数。补丁13 起**正常播放/停止路径
 *     不再调用它** ⇒ 应恒 0；>0 只可能来自自检或排障钩子。 */
extern volatile uint32_t g_es_tx_us_max;
extern volatile uint32_t g_es_slow_n;
extern volatile uint32_t g_es_busy_stuck_n;
extern volatile uint32_t g_es_rcc_rst_n;
extern volatile uint32_t g_es_bc_fail;
extern volatile uint32_t g_es_silent_n;
extern volatile uint32_t g_es_silent_ng;
extern volatile uint32_t g_es_silent_vol;  /* 那次确认读到的 REG32（应 0x00）*/
extern volatile uint32_t g_es_silent_mute; /* 那次确认读到的 REG31（bit6/5 应为 1）*/
extern volatile uint32_t g_es_pdown_n;
extern volatile uint32_t g_es_reg01_n;   /* REG01（时钟管理寄存器）被写的次数 */
extern volatile uint32_t g_es_reg01_val; /* REG01 最后一次写入的值（应恒 0x3F）*/
/* 排障钩子（SWD 写；纯软件、不碰任何电气）： */
extern volatile uint32_t g_es_pdown_req; /* 写 1 ⇒ 主循环下一拍执行一次"真掉电"（验 D9 配对）*/
extern volatile uint32_t g_es_nack_sim;  /* 0=正常；1=每笔事务直接判失败；2=另假装 SDA 被钳住 */
/* ---- 2026-10-13：UI 卡死修复新增（快速点播放/暂停后主循环被 I2C 拖死）----
 * 板上实测根因：I2C 从机在事务被中途打断后把 SDA 钳低 ⇒ 总线卡死；旧代码对 NACK
 * 直接 return、从不做总线恢复，且单笔超时 50 ms × 5 重试 = 250 ms/笔，一次起播
 * ~19 笔 ⇒ 主循环单拍实测 3921 ms（= 用户看到的"UI 卡住无法滑动"）。
 * 修法：① bus clear（9 SCL 脉冲 + STOP，双线开漏、无对顶风险）；② 单笔超时 50→5 ms；
 * ③ 连续硬失败熔断，冷却期内 I2C 立即快速失败，绝不长时间阻塞 UI。
 * ⚠ 重试次数**保持 5**（一度降到 3，复测后改回）：零探针窗口实测本链路首试 NACK 约 30%
 *   （模块仅靠 MCU 内部 ~40 kΩ 上拉跑 100 kHz 的电气边际），重试 3 次会把"全失败"概率
 *   放大约 13 倍 ⇒ 音频偶发丢写。UI 卡死真因是单笔超时（已 50→5 ms），与重试数无关。 */
extern volatile uint32_t g_es_bus_clear;   /* bus clear 真正执行过的次数（>0 = 发生过总线卡死）*/
extern volatile uint32_t g_es_breaker_trip;/* I2C 熔断触发次数（连续硬失败过多 ⇒ 快速失败）*/
/* ★ 补丁18c：为用户主动动作"放宽一次降级"且真的放宽了的次数（见 es8311_i2c_relax）*/
extern volatile uint32_t g_es_relax_n;
/* ★ 补丁18c：用户动作**开始时**就发现"SDA 被钳住"的次数（总线卡死态的频次统计）*/
extern volatile uint32_t g_es_relax_bus;
/* ★ 补丁18c：因"探不到器件"而强制做总线恢复的次数（补丁9 盲区是否真实发生过）*/
extern volatile uint32_t g_es_bc_after_probe;
extern volatile uint32_t g_es_force_bc;     /* SWD 钩子：写 1 ⇒ 主循环做一次强制总线恢复 */
extern volatile uint32_t g_es_force_bc_n;   /* 上面这个钩子真正执行的次数 */
void es8311_force_bus_clear(void);

/**
 * @brief  为**用户主动触发的一次性动作**放宽一次容错状态（补丁18c，2026-10-16）
 *
 * 只清"降级快速失败"标志（`s_es_fast_fail`），让接下来的这一整段寄存器序列能拿满
 * 5 次重试（扛过本模块固有的 ~30% 首试 NACK）；**不碰熔断**、**不改任何超时上限**。
 * 另外：若此刻 **SDA 被从机钳住**（总线卡死态），会先做一次 `es8311_bus_clear()`
 * 清障（计入 `g_es_relax_bus`）—— 提前介入比"等整段失败后再自愈"便宜得多。
 * 稳态路径（播放中的逐拍回读体检）不受影响 ⇒ 单拍 I2C 阻塞上限不变（UI 不卡、不爆音）。
 *
 * ⚠ 必须**只在用户主动动作的开始**调用（例如录音起播）。若放进逐笔路径或 bus_clear，
 *   等于把降级保护废掉 —— 本轮已用实测证伪过这种写法（见 src/es8311.c 里的说明）。
 * ⚠ 熔断冷却（250 ms）请由调用方**隔拍重试**跨过，不要在这里阻塞等待。
 */
void es8311_i2c_relax(void);
/* ★ 2026-10-13：SCL 频率现场可调（验证"降频消除 ~30% 首试 NACK"的电气边际假设）
 *   写 g_es_i2c_hz=<Hz>（10k~400k）→ 写 g_es_i2c_req=1 → 主循环调 es8311_i2c_retiming()。
 *   一次烧录即可扫多个频率，无需反复 download（每次 ~200 s）。 */
extern volatile uint32_t g_es_i2c_hz;      /* SCL 目标频率（默认 100000）*/
extern volatile uint32_t g_es_i2c_req;     /* 写 1 ⇒ 主循环执行一次重定时 */
extern volatile uint32_t g_es_i2c_timingr; /* 最近生效的 I2C1 TIMINGR（诊断）*/

/**
 * @brief  初始化 I2C1（PB6/PB7，AF4，100 kHz）—— 只做总线，不碰 codec
 * @return 0=OK；<0=HAL 初始化失败
 */
int es8311_bus_init(void);

/**
 * @brief  按 pclk 现场计算 100 kHz 标准模式的 I2C TIMINGR（.c 里有推导过程）
 * @param  pclk I2C1 内核时钟（HAL_RCC_GetPCLK1Freq()）
 * @return TIMINGR 寄存器值
 */
uint32_t es8311_i2c_timing(uint32_t pclk);

/**
 * @brief  按当前 g_es_i2c_hz 重算 TIMINGR 并重初始化 I2C1（SWD 现场调频用，免重烧）
 * @return 生效的 TIMINGR（0 = bus 未初始化，未动作）
 */
uint32_t es8311_i2c_retiming(void);

/**
 * @brief  探测指定 7 位地址上是否有器件应答（20 ms 超时）
 * @param  addr7 0x18 或 0x19
 * @return 0=有应答；<0=无应答
 */
int es8311_probe(uint8_t addr7);

/**
 * @brief  同上，但超时可调（全地址段普查用 2 ms，避免慢总线拖长时间）
 * @return 0=有应答；<0=无应答/超时
 */
int es8311_probe_to(uint8_t addr7, uint32_t timeout_ms);

/**
 * @brief  选定后续读写用的 7 位地址（CE0=0x18 / CE1=0x19，默认 0x18）
 *         模块的 CE 焊法未知 ⇒ 上电先探，探到哪个用哪个。
 */
void es8311_set_addr(uint8_t addr7);

/**
 * @brief  读一个 8 位寄存器（写寄存器号 → repeated start → 读 1 字节）
 * @return 0=OK；<0=失败
 */
int es8311_read_reg(uint8_t reg, uint8_t *val);

/**
 * @brief  写一个 8 位寄存器
 * @return 0=OK；<0=失败
 */
int es8311_write_reg(uint8_t reg, uint8_t val);

/**
 * @brief  读芯片 ID（0xFD/0xFE/0xFF）到 id[3]
 * @return 0=OK；<0=失败
 */
int es8311_read_id(uint8_t id[3]);

/**
 * @brief  完整初始化（esp-adf es8311_codec_init 序列，codec = I2S 从机）
 *         只写寄存器，不启动 DAC —— 启动放在 I2S 时钟起来之后（es8311_start_dac）
 * @return 0=OK；<0=失败（g_es_rc/g_es_wr_ng 可定位到第几包）
 */
int es8311_init(void);

/**
 * @brief  配 16 位 / Philips I2S 格式（REG09/REG0A，esp-adf config_i2s）
 * @return 0=OK；<0=失败
 */
int es8311_config_i2s(void);

/**
 * @brief  启动 DAC 通路（esp-adf es8311_start(ES_MODULE_DAC)），音量取当前 UI 音量
 * @return 0=OK；<0=失败
 */
int es8311_start_dac(void);

/**
 * @brief  启动 DAC 通路，但**音量写 0（静音）**
 *
 * 【为什么要这个版本：起播那一声"咔哒"】
 *   DAC 通路上电是"从 power-down 醒来"的动作，输出会从直流 0 跳到工作点；若同一刻
 *   就把 REG32 一步写到目标音量，跳变幅度 = 目标电平 ⇒ 必然是一声可闻的响。
 *   正确做法（也是 esp-adf/各家 codec 驱动的通例）：**先在静音档把通路接通**，
 *   等 I2S 时钟建立、数据流稳定之后，再把音量逐步 ramp 上去。
 *   调用顺序：I2S 先跑起来（FIFO 有数据）→ es8311_dac_start_muted() →
 *   分若干拍把 REG32 从 0 升到 es8311_volume_reg(g_audio_volume)。
 *   （见 src/audio_out.c 的 s_dac_pending / s_vol_ramp_left。）
 * @return 0=OK；<0=失败
 */
int es8311_dac_start_muted(void);

/**
 * @brief  停止 DAC 并**完整有序掉电**（2026-10-14 补丁11·D8/D9，照抄官方 esp_codec_dev
 *         的 es8311_suspend()："关机"语义，比旧版"半掉电"更干净、掉电态更确定）
 *
 * 【与起播的硬配对（⚠⚠ 改这里必须同步看 dac_path_regs）】
 *   本函数会把 REG00 的 CSM_ON 清 0、把 REG01 的全部时钟使能清 0（MCLK_ON/BCLK_ON/
 *   CLKADC_ON/CLKDAC_ON/ANACLKx_ON），并把 REG0D 写 0xFC（连内部参考 VREF 一起断电）；
 *   而**起播路径 `dac_path_regs()` 开头会重写 REG00=0x80 / REG01=0x3F 把它们恢复**。
 *   本项目续播跳过完整 init（见 es8311_is_inited 的说明）⇒ 若起播不恢复 ⇒ 时钟全关
 *   ⇒ 续播无声。官方 es8311_start() 正是这么与 suspend() 配对的。
 *
 * 【触发场景】仅"真正的停"走它：切歌 / 播完 / 退出 app（audio_out_stop）、自检收尾。
 *   「暂停」**不**走它 —— 暂停走 es8311_dac_mute()：只写 1 笔 REG32=0 静音、**不给芯片
 *   断电**，这样续播时芯片保持可达、不撞"掉电/上电窗口 NACK"（见 es8311_dac_mute 说明）。
 * @return 0=全部写成功；<0=仍有写失败（g_es_fail 会涨）
 */
int es8311_stop_dac(void);

/**
 * @brief  暂停用「静音」：只把 DAC 音量寄存器(REG32)写成 0，**不动电源/通路寄存器**。
 *
 * 【为什么需要它，而不是用 es8311_stop_dac()】
 *   es8311_stop_dac() 是"关机"语义：REG0E=0xFF、REG12=0x02 把 codec 打进
 *   **power-down**。芯片断电/上电的窗口里会对自己的 I2C 地址回 NACK —— 那 9 笔写
 *   里就有若干笔砸在窗口上（2026-10-13 实测：暂停时 wr_ok 增量恰好 +9，且失败寄存器
 *   覆盖 0x0D/0x0E/0x0F/0x12/0x14/0x15/0x32/0x37/0x45）。
 *   而音乐 app 的「暂停」紧接着就是「续播」，**根本没有必要关掉这颗芯片**：
 *   它的 5 V 常亮、寄存器状态本来就在，只要静音 + 停掉 I2S 时钟即可。
 *   ⇒ 暂停改走本函数：只 1 笔 I2C、芯片保持可达，续播那段 ~19 笔 I2C 因此零 NACK 窗口。
 *
 * @return 0=OK；<0=失败（走 es8311_write_reg 的重试，仍失败才返回 <0）
 */
int es8311_dac_mute(void);

/**
 * @brief  ★ 2026-10-14（补丁13）：让 DAC **确定地**变静音，并且**当场回读确认**。
 *
 * 【解决什么】"切 I2S 时钟前必须已经静音"是防爆音的最后一道防线，而 I2C 写是**会失败**的
 *   （实测首试 NACK 5%~30%，失败环里 0x31/0x32 命中最多）。"写了就算"= 有相当概率不静音
 *   ⇒ 切时钟那一刻 DAC 还挂在有效音量 ⇒ 直流阶跃 ⇒ 那声爆音。
 *
 * 【做什么】① REG32 先砍一半（兜底）→ ② REG32=0 → ③ REG31 数字静音
 *   → ④ 回读 0x32/0x31 确认，不对就补写一轮再验（最多 2 轮）。
 *   **完全不碰电源/时钟寄存器**：芯片保持"通电 + VMIDSEL 仍工作"，与「暂停」同构。
 *   （真·掉电是 es8311_stop_dac()：它会把内部参考 VREF 断掉，基准塌掉/重建本身也是
 *    一声爆音，所以正常播放/停止路径已不再调用它。）
 *
 * @return 0 = 已确认静音；-1 = 未能确认（g_es_silent_ng +1，调用方按"有爆音风险"记录）
 */
int es8311_dac_become_silent(void);

/**
 * @brief  REG31 的**专用数字静音位**（on=1 静音 / 0 解静音）—— 官方防爆音手段。
 *
 * 【与 es8311_dac_mute() 的区别】es8311_dac_mute() 是"把音量降到 -95.5 dB"，
 *   慢（硬件软 ramp 160~190 ms 才到听感静音），适合可以等的「暂停」。
 *   本函数直接把 ΣΔ 调制器输入端钳 0，**即时生效**，适合要求"立刻断"的
 *   「停止 / 切歌 / 退出 app」—— 在**时钟仍在跑**的时候静音，随后再切时钟就不会
 *   有直流阶跃（那正是停止时那声"咔"的来源，见 audio_out.c → audio_out_stop）。
 * ⚠ REG31 是芯片内持久状态：置过静音后，**下次起播必须解静音**——已由
 *   dac_path_regs() 末尾统一负责（调用方不需要再管）。
 * @return 0=OK；<0=失败（仍会按复位默认值写一次，不会把状态留在中间态）
 */
int es8311_set_mute(int on);

/**
 * @brief  数字音量 0~255 → REG32（0=静音，0xBF≈75%，0xFF 最大）
 * @return 0=OK；<0=失败
 */
int es8311_set_volume(uint8_t vol);

/* ---- UI 音量（0..100）与 REG32 的换算（2026-10-12）----
 *
 * 【映射】REG32 = vol × 0xBF / 100
 *   0   → 0x00（-95.5 dB，即静音，与"音量拖到 0"的直觉一致）
 *   100 → 0xBF（0 dB 单位增益，模块出厂/自检用的就是这一档）
 * 为什么上限取 0xBF 而不是 0xFF：0xFF 是 +32 dB，这块 5V 小功放推上去只会削顶失真，
 * 而且 UI 上"100"应当是"设计上的最大正常音量"，不是"失真档"。
 *
 * 【谁调用】main.c 的音量持久化回调（控制中心音量滑杆松手时）与
 * es8311_start_dac()（开机/自检起播时把当前 UI 音量灌进 codec）。 */
uint8_t es8311_volume_reg(uint32_t vol0_100);

/**
 * @brief  按 UI 音量 0..100 写 REG32（越界先钳到 0..100）
 * @return 0=OK；<0=失败
 */
int es8311_set_volume_ui(uint32_t vol0_100);

/**
 * @brief  DAC 通路"回读体检"：把 0x32/0x12/0x0E 从芯片读回，存进 g_es_rb_*
 *
 * 【为什么要回读】"写出去了" 不等于 "芯片收下了"。暂停→续播出现"没声音"时的第一
 *   嫌疑是 "REG32 停在 0（静音）"，但只有回读才能区分：
 *     · 我们没写（g_es_reg32_wr 还是旧值 / g_es_reg32_n 没涨）—— 软件时序问题；
 *     · 写了但芯片里不是那个值（g_es_rb_vol ≠ g_es_reg32_wr）—— 复位/上电时序把写吞了。
 *   出货路径在**起播/续播后的"音量确认期"逐拍调**（见 audio_out.c 的 audio_out_tick），
 *   直到本函数回报 0（确认音量已落进芯片）才收工 —— 不是"读一次看一眼"。
 *   为什么必须逐拍确认：ramp 末步写终值若撞上芯片 NACK 窗口会停在中间档（实测 0x78），
 *   而那一拍的回读往往同样失败 ⇒ 只读一次判定不出来（见 .c 里 2026-10-13 的实测记录）。
 * @return 0=**已确认**（读成功且音量 == 目标、REG12 未 power-down）；<0=未确认（调用方可下一拍再试）
 */
int es8311_dac_readback(void);

/**
 * @brief  1 = 本次开机已经成功跑过完整初始化序列（REG00 软复位 + 时钟/格式）
 *
 * 【为什么需要】完整 init 里含 **REG00=0x80 软复位**（整颗芯片重启）。而 codec 的 5V
 *   常亮、寄存器状态本来就在，`es8311_stop_dac()`（暂停）写下的那些寄存器
 *   `es8311_dac_start_muted()`（续播）会原样恢复 ⇒ **续播不需要再复位整颗芯片**。
 *   板上实测（tools/wav_pause_resume_check.py）：复位/电源态切换后的若干毫秒里芯片
 *   会对自己的 I2C 地址**回 NACK**（5 轮回放里 wr_ng 累计 45→77，全部是 NACK、
 *   总线电平正常），而音量那几笔一丢就是"没声音/音量大小混乱"。少做一次复位 = 少一个
 *   NACK 窗口 ⇒ 出货路径改成"只在开机第一次起播时跑完整 init"。
 *   ⚠ 首次 init 失败时 s_codec_ready 不置位 ⇒ 下次起播会自动重试完整序列。
 */
int es8311_is_inited(void);

/**
 * @brief  以当前 UI 音量**重写整条 DAC 通路**（`dac_path_regs` 的对外封装）
 *
 * 回读体检（es8311_dac_readback）发现终态不对时用它补写：只补 REG32/REG12 不够 ——
 * 实测丢写随机落在 0x09/0x0A/0x0E/0x12/0x14/0x17/0x32/0x37/0x45 各处，整段重写才闭合。
 * @return 0=全部写成功；<0=仍有写失败（g_es_fail 会涨）
 */
int es8311_dac_repair(void);

#endif /* ES8311_H */
