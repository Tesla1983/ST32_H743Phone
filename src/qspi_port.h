/* ===========================================================================
 * 阶段 4：W25Q128（16 MB SPI NOR）经 QUADSPI 接入
 *
 * 两件事：
 *   1. **indirect 模式**：按指令收发（读/写/擦除），用来给 Flash 灌数据。
 *   2. **memory-mapped（XIP）模式**：把整颗 16 MB 映射到 0x9000_0000，
 *      CPU 直接当内存读 —— 只读常量（字模/图片/词典载荷）走这条。
 *
 * 引脚（厂商 qspi.h，全部 AF9）：
 *   CLK = PB2   NCS = PB10   IO0 = PD11   IO1 = PD12   IO2 = PE2   IO3 = PD13
 * ⚠ NCS 必须**自己配成 AF9**：HAL 的 QSPI 初始化不管片选，漏了它片选悬空、
 *   读回永远是 0xFF。
 * ⚠ 这几个脚与 LCD 的 FMC 不冲突（本板 FMC 只用 PD0/1/8/9/10/14/15 与 PE7..PE15，
 *   LCD 控制线在 PA8/PB1/PD4/PD5/PD7/PE3）。
 *
 * 内存属性：XIP 区 0x9000_0000 的 MPU 区域在 BSP/MPU/mpu.c 里（region 0，
 * 只读 + 非缓存 + XN）。**不加那条 MPU 区域就会走默认存储图（可缓存 Normal）**。
 * =========================================================================== */

#ifndef QSPI_PORT_H
#define QSPI_PORT_H

#include <stdint.h>

/* ---- XIP 窗口 ---- */
#define QSPI_XIP_BASE   0x90000000u
#define QSPI_XIP_SIZE   (16u * 1024u * 1024u)   /* W25Q128 = 2^24，DCR.FSIZE = 23 */

/* ---- 内容布局（计划书 §6.4）----
 *   0x000000 ~ 0x0FFFFF  字模：第 i 字在 i×128 偏移（GB2312 16×16 4bpp）
 *   0x100000 ~ 0x2FFFFF  图片/壁纸常量
 *   0x300000 ~ 0x5FFFFF  IME 词典载荷
 *   0x600000 ~ 0xFEFFFF  文件系统区
 *   0x00FFF000           最后 1 个扇区：**留空**（早期参照工程自检占用，避开）
 *
 * 字模区尾部 0xEFC00~0xFFFFF 是字库本身**不占用**的余量（字库正好 0xEFC00），
 * 本工程在这里放两个记账扇区，从而不动 §6.4 指定的任何区域。 */
#define QSPI_FONT_REGION_BASE   0x00000000u
#define QSPI_FONT_REGION_SIZE   0x00100000u                     /* 1 MB */
#define QSPI_FONT_BLOB_SIZE     (982016u)                       /* gb2312_glyphs.bin 实测大小 = 0xEFC00 */
#define QSPI_VERIFY_LOOP_ADDR   0x000FE000u                     /* 阶段 4 擦写回环落点 */
#define QSPI_HOUSEKEEP_ADDR     0x000FF000u                     /* 记账/配置扇区（font_provision 用） */

/* ---- 芯片识别 ---- */
#define QSPI_W25Q128_ID      0xEF17u            /* 0x90 读回（厂商 norflash.h） */
#define QSPI_JEDEC_W25Q128   0x00EF4018u        /* 0x9F 读回：EFh + 40h + 18h */

/* ---- 返回码 ---- */
#define QSPI_OK          0
#define QSPI_ERR_COMM    1      /* 通信超时（片选悬空 / 芯片没焊 / 时序不对） */
#define QSPI_ERR_ID      2      /* ID 不符 */
#define QSPI_ERR_PARAM   3      /* 参数非法 */
#define QSPI_ERR_STATE   4      /* 状态不对（如映射模式下不能发 indirect 命令） */

/* 初始化：配引脚/时钟/QSPI 寄存器 → 退 QPI → 置 QE → 读 ID。
 * 返回 QSPI_OK / QSPI_ERR_COMM / QSPI_ERR_ID。失败不致命，显示与触摸照常。 */
int qspi_port_init(void);

/* ---- indirect 读写（0xEB 四线快读 / 0x32 四线页写 / 0x20 扇区擦除）---- */
int qspi_read (uint8_t *buf, uint32_t addr, uint32_t len);
int qspi_write(const uint8_t *buf, uint32_t addr, uint32_t len);
int qspi_erase_sector(uint32_t sector);        /* 参数是**扇区号**，不是字节地址 */

/* ---- memory-mapped（XIP）进出 ----
 * 进出成对使用：映射模式下**不能**发 indirect 命令（含擦写），必须先 exit。 */
int qspi_enter_mmap(void);
int qspi_exit_mmap(void);

/* 阶段 4 验收（计划书 §6.6）：
 *   ① 全地址空间抽样：每 64 KB 块首 256 B，XIP 与 indirect 逐字节比对；
 *   ② 擦写回环：最后一个扇区擦除→写图案→indirect 读回→**退出映射再重进**→XIP 读回，
 *      两次都要与写入图案一致。
 * 返回 QSPI_OK 表示 ② 通过且 ① 的失配数为 0。 */
int qspi_port_verify(void);

/* ---- 供 SWD 阅读的量 ---- */
extern volatile uint32_t g_qspi_rc;          /* qspi_port_init 返回值 */
extern volatile uint32_t g_qspi_jedec;       /* 0x9F 三字节，期望 0x00EF4018 */
extern volatile uint32_t g_qspi_id90;        /* 0x90 两字节，期望 0xEF17 */
extern volatile uint32_t g_qspi_sr_snap;     /* SR1 | SR2<<8（SR2 的 bit1 = QE） */
extern volatile uint32_t g_qspi_cr;          /* 最终 CR 实测值 */
extern volatile uint32_t g_qspi_dcr;         /* 最终 DCR 实测值 */
extern volatile uint32_t g_qspi_ker_sel;     /* RCC_D1CCIPR.QUADSPISEL（0 = HCLK3） */
extern volatile uint32_t g_qspi_mm_ok;       /* 最近一次 enter/exit mmap 是否成功 */
extern volatile uint32_t g_qspi_verify_rc;   /* qspi_port_verify 返回值 */
extern volatile uint32_t g_qspi_cmp_blocks;  /* 已比对的 64 KB 块数（目标 256） */
extern volatile uint32_t g_qspi_cmp_bytes;   /* 已比对的字节数（目标 65536） */
extern volatile uint32_t g_qspi_cmp_mismatch;/* XIP vs indirect 失配字节数（目标 0） */
extern volatile uint32_t g_qspi_first_bad;   /* 首个失配偏移（0xFFFFFFFF = 无） */
extern volatile uint32_t g_qspi_loop_ok;     /* 擦写回环：0=未跑 1=两次读都一致 2=不符 */
extern volatile uint32_t g_qspi_loop_w0;     /* 写入的第 1 个字（小端 4 字节） */
extern volatile uint32_t g_qspi_loop_r0;     /* indirect 读回的第 1 个字 */
extern volatile uint32_t g_qspi_loop_x0;     /* 重进映射后 XIP 读回的第 1 个字 */
extern volatile uint32_t g_qspi_erase_cnt;   /* 本工程执行的扇区擦除次数 */
extern volatile uint32_t g_qspi_busy_snap;   /* 最近一次超时瞬间的 QUADSPI->SR */
extern volatile uint32_t g_qspi_err_cnt;     /* indirect 读失败次数 */

#endif /* QSPI_PORT_H */
