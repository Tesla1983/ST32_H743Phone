/* ===========================================================================
 * ES8311 音频链路自检（方案 A 接线验收）
 *
 * ⚠ 本模块由 CMake option YMGUI_AUDIO_TEST 控制，**默认不编译进出货固件**
 *   （与 YMGUI_XIP_BENCH 同口径）—— 打开：cmake -B build -DYMGUI_AUDIO_TEST=ON。
 *   关闭时 main.c 里的调用点和全部符号一起消失，出货 FLASH 哈希不受影响。
 *
 * 链路（2026-10-12 方案 A 接线，已落地出声；接线表与踩坑见
 * docs/DEVELOPMENT_LOG.md 的「2026-10-12 · ES8311 音频模块接入」一节）
 * ------------------------------------
 *   I2C1  PB6=SCL / PB7=SDA（AF4）        → ES8311 寄存器（src/es8311.c）
 *   I2S2  PC6=MCLK(AF5) / PD3=SCLK(AF5) / PB9=LRCK(AF5) /
 *         PB15=DOUT(AF5) / PC1=DOUT(AF5) / PC2=DIN(AF5)   ← 本文件
 *   NS4150B：模块内常闭功放，无使能脚，不需要 GPIO
 *
 * 触发方式（SWD 写全局量，与 g_micro_test / g_fs_test 同一套路）
 * ------------------------------------
 *   g_audio_test = 1   I2C 扫描 + 读芯片 ID（**不出声**，先跑这个）
 *   g_audio_test = 2   全链路：I2S2 + ES8311 初始化 + 三声提示音
 *                    （244/488/977 Hz，各 350 ms 含 40 ms 淡入淡出，阻塞约 1.2 s，
 *                     期间 UI 冻结 —— 与其它 bench 一致，跑完自动恢复）
 *   tools/audio_check.py 自动完成触发与读数。
 *
 * 结果判据（全部 SWD 可读；失败路径上也保证被写，防 --gc-sections 回收）
 * ------------------------------------
 *   g_at_rc        0=全 OK；<0=失败阶段（见 .c 内各 -N 注释）
 *   g_at_stage     进度：1..6，100=结束
 *   g_at_i2c_addr  探到的 7 位地址（0x18/0x19），0=没探到
 *   g_at_id0/1/2   芯片 ID 寄存器 0xFD/0xFE/0xFF 原值（不预设期望值，只报读数）
 *   g_at_i2s_rc    HAL_I2S_Init 返回码（0=HAL_OK）
 *   g_at_frames    实际送进 I2S 的立体声帧数（>0 证明流跑过）
 *   g_at_mclk_hz / g_at_fs_hz / g_at_sck_hz  由 I2SCFGR 回读分频器算出的期望频率
 *                   （给逻辑分析仪/示波器对照：MCLK≈12.5 MHz、SCK≈3.125 MHz、
 *                    LRCK≈48.8 kHz；两侧同源，绝对差 1.7% 是 200 MHz 内核分频粒度所致）
 *   g_at_hang      1=流写等待超时（I2S 时钟没起来时的卡死保护，不会挂死主板）
 *   g_at_amp       SWD 可写：提示音峰值幅度（1..32000，默认 8000≈24% 满幅）
 * =========================================================================== */
#ifndef AUDIO_TEST_H
#define AUDIO_TEST_H

#include <stdint.h>

extern volatile uint32_t g_audio_test;   /* 触发量：写 1 或 2 */

extern volatile int      g_at_rc;
extern volatile uint32_t g_at_stage;
extern volatile uint32_t g_at_i2c_addr;
extern volatile uint32_t g_at_id0;        /* 芯片 ID 0xFD/FE/FF（uint32 保证 4 字节对齐，SWD b32 可直读）*/
extern volatile uint32_t g_at_id1;
extern volatile uint32_t g_at_id2;
extern volatile int      g_at_i2s_rc;
extern volatile uint32_t g_at_frames;
extern volatile uint32_t g_at_mclk_hz;
extern volatile uint32_t g_at_fs_hz;
extern volatile uint32_t g_at_sck_hz;
extern volatile uint32_t g_at_hang;
extern volatile uint32_t g_at_udr;       /* 流式期间欠载次数（清了就能继续，正常容错）*/
extern volatile uint32_t g_at_amp;       /* 提示音峰值幅度，默认 8000（uint32 便于 SWD 写）*/
/* case 1 附加诊断（2026-10-12 晚：模块不 ACK 时的定位用）*/
extern volatile uint32_t g_at_probe_us;  /* 两个候选地址探测总耗时（µs）：
                                            <1000=快速 NACK（外设在收发）；
                                            ≈120000=等满 6×20ms 超时（时钟没跑/总线卡死）*/
extern volatile uint32_t g_at_scan_n;    /* 全地址段 0x08~0x77 普查：有应答的器件数 */
extern volatile uint32_t g_at_scan_first;/* 第一个有应答的地址（0=一个都没有）*/
/* case 3 交叉位拍普查（SDA/SCL 对调扫描）*/
extern volatile uint32_t g_at_bb_norm;   /* 正常朝向（SDA=PB7,SCL=PB6）扫到的器件数 */
extern volatile uint32_t g_at_bb_cross;  /* 交叉朝向（SDA=PB6,SCL=PB7）扫到的器件数 */
extern volatile uint32_t g_at_bb_first;  /* 交叉朝向第一个有应答的地址（0=没有）*/
extern volatile uint32_t g_at_kclk;      /* I2S2 内核时钟（SPI123SEL=PLL1_Q）实测计算值 */

/**
 * @brief  主循环轮询：g_audio_test 非 0 时跑对应 case，跑完自动清 0
 *         与 run_render_micro / fatfs_bench_poll 同一调用姿势。
 */
void audio_test_poll(void);

#endif /* AUDIO_TEST_H */
