/* ===========================================================================
 * UI 设置的持久化存储（阶段 1：界面语言）
 *
 * 【为什么放 W25Q128 而不是内部 Flash】
 *   1. **容量**：W25Q128 共 16 MB，当前只用了字库 982 KB（0x000000~0xEFC00）
 *      加两个扇区（0xFE000 擦写回环、0xFF000 字库记账），**约 15 MB 空闲**。
 *   2. **通路已成熟**：`src/qspi_port.c` 的 `qspi_write()` 已经做到
 *      「自动退出映射 → 整扇区读-改-擦-写回 → 保留扇区内其它数据」，
 *      且**内容相同就跳过擦除**（见其 `need_erase` 判断）——
 *      这正是小配置项最需要的"不变不磨损"特性。
 *   3. **不占内部 Flash**：内部 2 MB 已用 74.7%，且写内部 Flash 会阻塞取指。
 *   4. **寿命**：NOR 每扇区约 10 万次擦写；语言切换是低频操作，
 *      再叠加"值有变才写 + qspi_write 的跳过擦除"，实际等同于无限。
 *
 * 【扇区选址】`0x000F0000`（扇区号 240）
 *   | 地址范围              | 用途                                  |
 *   | 0x000000 ~ 0x0EFBFF   | GB2312 字模（982016 B = 0xEFC00）      |
 *   | **0x0F0000 ~ 0x0F0FFF** | **← 本模块：UI 设置（扇区 240）**    |
 *   | 0x0FE000              | qspi_port 的擦写回环落点（QSPI_VERIFY_LOOP_ADDR）|
 *   | 0x0FF000              | 字库记账头（QSPI_HOUSEKEEP_ADDR）      |
 *   选址落在字库末尾之后、擦写回环之前的空隙里，**不撞任何既有地址**。
 *
 * ⚠ **最大的坑：写完必须重新进入映射模式**
 *   `qspi_read()` / `qspi_write()` 内部都会调 `qspi_ensure_indirect()`，
 *   也就是**先退出 memory-mapped 模式**（映射模式下发不了 indirect 命令）。
 *   而中文字模正是从映射区 `0x9000_0000` 读的 —— 如果这里不把它重新映射回去，
 *   **整屏中文会立刻变成不显示**（读回来是 0 或 0xFF）。
 *   `font_provision.c:167` 就是同样的处理；本模块的 Load/Save 末尾都补了一句。
 *
 * 【损坏处理】带 magic + CRC32 双校验。任一不符即视为"没有有效配置"，
 *   回退编译期默认值（中文），**不阻塞启动、不报错弹窗**。
 * =========================================================================== */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

/* 配置扇区（W25Q128） */
#define APP_CFG_SECTOR_ADDR   0x000F0000u
#define APP_CFG_SECTOR_NO     240u

/* 界面语言取值（与 phone_lang.h 的 PhoneLang 对应） */
#define APP_CFG_LANG_ZH       0u
#define APP_CFG_LANG_EN       1u

/* 上电调用一次：读扇区 → 校验 → 填充当前设置。失败则用默认值。 */
void AppConfig_Load(void);

/* 把当前设置写回扇区。**值同上次写入完全一致时不写**（省一次擦除）。 */
void AppConfig_Save(void);

uint32_t AppConfig_GetLang(void);
void     AppConfig_SetLang(uint32_t lang);

/* 壁纸 ribbon 半透明（2026-10-04 新增，落盘字段 = blob 的 reserved[0]）
 *
 * 【语义】1 = 半透明（壁纸渐变透出来，**出厂默认**，满负载 ~41 FPS）
 *         0 = 不透明（ribbon 实色，满负载 ~48 FPS）
 * ⚠ 与 phone_shell.c 的 g_ribbon_translucent 同语义（1=半透明）。
 *   历史上是 g_ribbon_opaque 且 1=不透明，2026-10-04 翻转并改名，别再混用。 */
uint32_t AppConfig_GetRibbonTranslucent(void);
void     AppConfig_SetRibbonTranslucent(uint32_t on);

/* 屏幕亮度（2026-10-04 启用，落在 blob 的 brightness 字段）
 *
 * 【语义】0..100 整数，**出厂默认 50**（用户 2026-10-04 决策：全亮刺眼）。
 *   100 = 不叠暗罩；<100 时 phone_flush 叠 alpha=(100-v)*160/100 的黑罩。
 * ⚠ 越界（含 0）会被钳回默认 50 —— 0 会造成全屏死黑，与"屏坏了"无法区分。
 * ⚠ 160/100 是屏侧固定系数，所以"亮度值"不是线性感知亮度，别当 gamma 用。 */
void     AppConfig_SetBrightness(uint32_t value);
uint32_t AppConfig_GetBrightness(void);

/* 恢复出厂设置：擦掉配置扇区，下一次上电整套设置回 blob_defaults()。
 * 由 SWD 写 1 到 g_cfg_factory_reset 触发，跑完自动清 0。
 * ⚠ 必须由固件执行 —— 配置在 XIP 只读窗口，主机侧写不进去。 */
void AppConfig_FactoryReset(void);

/* 是否有"改了但还没落盘"的项（供 UI 提示/退出前保存用） */
uint32_t AppConfig_IsDirty(void);

/* ---- SWD 可读诊断量（全在 DTCM，不受 D-Cache 影响）---- */
extern volatile uint32_t g_cfg_rc;          /* Load 的返回码，0 = 成功 */
extern volatile uint32_t g_cfg_loaded;      /* 1 = 读到有效配置；0 = 用默认值 */
extern volatile uint32_t g_cfg_magic_read;  /* 扇区里读到的 magic（期望 0x59434F4E = "YCON"） */
extern volatile uint32_t g_cfg_crc_ok;      /* 1 = CRC 校验通过 */
extern volatile uint32_t g_cfg_lang;        /* 当前语言（0 中 / 1 英） */
extern volatile uint32_t g_cfg_ribbon;      /* 半透明开关当前值（1 半透明 / 0 不透明） */
extern volatile uint32_t g_cfg_brightness;  /* 亮度当前值 0..100（出厂 50，越界已钳回默认） */
extern volatile uint32_t g_cfg_factory_reset; /* SWD 写 1 → 擦配置扇区（恢复出厂），跑完清 0 */
extern volatile uint32_t g_cfg_factory_rc;    /* 上次恢复出厂的结果（0=成功） */
extern volatile uint32_t g_cfg_save_cnt;    /* 本上电周期内实际落盘次数 */
extern volatile uint32_t g_cfg_dirty;       /* 1 = 有未落盘的改动 */
extern volatile uint32_t g_cfg_write_rc;    /* 最近一次 qspi_write 返回码 */
extern volatile uint32_t g_cfg_erase_last;  /* 最近一次保存触发的扇区擦除次数（0 = 内容未变，跳过擦除） */

#endif /* APP_CONFIG_H */
