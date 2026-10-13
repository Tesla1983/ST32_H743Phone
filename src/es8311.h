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
extern volatile uint32_t g_es_fail;    /* 重试 3 次仍失败 的次数（应恒 0）*/
extern volatile uint32_t g_es_fix_wr;  /* 回读校验发现不对、补写 的次数 */
extern volatile uint32_t g_es_recover_pe;/* 因 BUSY/TIMEOUT 做 I2C 软复位(PE 0→1)的次数 */
extern volatile uint32_t g_es_readback; /* 回读体检执行次数（起播后那次 + 播放中每秒那次）*/
/* ---- 2026-10-13：UI 卡死修复新增（快速点播放/暂停后主循环被 I2C 拖死）----
 * 板上实测根因：I2C 从机在事务被中途打断后把 SDA 钳低 ⇒ 总线卡死；旧代码对 NACK
 * 直接 return、从不做总线恢复，且单笔超时 50 ms × 5 重试 = 250 ms/笔，一次起播
 * ~19 笔 ⇒ 主循环单拍实测 3921 ms（= 用户看到的"UI 卡住无法滑动"）。
 * 修法：① bus clear（9 SCL 脉冲 + STOP，双线开漏、无对顶风险）；② 单笔超时 50→5 ms、
 * 重试 5→3；③ 连续硬失败熔断，冷却期内 I2C 立即快速失败，绝不长时间阻塞 UI。 */
extern volatile uint32_t g_es_bus_clear;   /* bus clear 真正执行过的次数（>0 = 发生过总线卡死）*/
extern volatile uint32_t g_es_breaker_trip;/* I2C 熔断触发次数（连续硬失败过多 ⇒ 快速失败）*/

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
 * @brief  挂起/静音（esp-adf es8311_suspend）
 * @return 0=OK；<0=失败
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
