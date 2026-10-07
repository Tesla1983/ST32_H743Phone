/* QSPI 分块灌库 —— 见 qspi_provision.h 的长注释。
 *
 * 本文件**只有在 CMake option YMGUI_QSPI_PROVISION=ON 时才编进实际逻辑**；
 * OFF 时全部退化成空壳，但符号仍然导出（g_prov_enabled = 0），这样
 * tools/qspi_provision.py 能明确区分"没编进来"和"编进来但失败了"，
 * 而不是读到一个恒为 0 的状态就猜。 */

#include "qspi_provision.h"
#include "qspi_port.h"
#include "font_provision.h"

#if defined(YMGUI_QSPI_PROVISION)

/* 暂存区放 **SRAM1（0x3000_0000，D2 域）**，不用 DTCM（.bss 拥挤）也不用 AXI
 * （帧镜像 + heap1 占满）。链接脚本的 .bss_prov 段指到这里，NOLOAD。
 * ⚠ 与 .bss_dma 同段区：两者都在 SRAM1，互不重叠由链接器排布保证。 */
static uint8_t s_prov_buf[QSPI_PROV_CHUNK]
	__attribute__((section(".bss_prov"), aligned(32)));

volatile uint32_t g_prov_state;
volatile uint32_t g_prov_cmd;
volatile uint32_t g_prov_addr;
volatile uint32_t g_prov_len;
volatile uint32_t g_prov_rc;
volatile uint32_t g_prov_done;
volatile uint32_t g_prov_chunks;
volatile uint32_t g_prov_erase;
volatile uint32_t g_prov_verify;
volatile uint32_t g_prov_crc;
/* ⚠ 这三个**只被 SWD 读、固件代码本来不引用它们**，所以必须在代码里真的出现一次：
 *   开着 -fdata-sections + --gc-sections 时，链接器会把它们当垃圾段丢掉 ——
 *   实测它们在 nm 里**直接消失**（`__attribute__((used))` 也救不回来，
 *   used 只挡编译器、挡不住 --gc-sections），脚本于是误报"没编入灌库支持"。
 *   修法见 qspi_provision_poll 开头那三行。 */
volatile uint32_t g_prov_buf_addr;
volatile uint32_t g_prov_buf_size;
volatile uint32_t g_prov_enabled;

void qspi_provision_poll(void)
{
	if (g_prov_enabled == 0u)
	{
		g_prov_buf_addr = (uint32_t)(uintptr_t)s_prov_buf;
		g_prov_buf_size = (uint32_t)sizeof(s_prov_buf);
		g_prov_enabled  = 1u;
	}

	if (g_prov_verify == 1u)
	{
		g_prov_verify = 0u;
		g_prov_crc = font_crc32_xip(g_prov_addr, g_prov_len);
	}

	if (g_prov_cmd != 1u) return;
	g_prov_cmd = 0u;

	uint32_t len = g_prov_len;
	if (len == 0u || len > (uint32_t)sizeof(s_prov_buf) ||
	    g_prov_addr + len > QSPI_XIP_SIZE) {
		g_prov_state = 3u;
		return;
	}

	g_prov_state = 1u;

	uint32_t erase_before = g_qspi_erase_cnt;
	int rc = qspi_write(s_prov_buf, g_prov_addr, len);
	g_prov_rc = (uint32_t)rc;
	g_prov_erase += g_qspi_erase_cnt - erase_before;

	if (rc != QSPI_OK) {
		g_prov_state = 3u;
	} else {
		g_prov_done += len;
		g_prov_chunks++;
		g_prov_state = 2u;
	}

	/* ⚠ 擦写会把 QSPI 踢出映射模式，而**整屏中文都靠映射模式取字模** ——
	 * 不重新进映射，灌完库的第一帧就是空白。font_provision.c:167 是同样处理。 */
	(void)qspi_enter_mmap();
}

#else

volatile uint32_t g_prov_state;
volatile uint32_t g_prov_cmd;
volatile uint32_t g_prov_addr;
volatile uint32_t g_prov_len;
volatile uint32_t g_prov_rc;
volatile uint32_t g_prov_done;
volatile uint32_t g_prov_chunks;
volatile uint32_t g_prov_erase;
volatile uint32_t g_prov_verify;
volatile uint32_t g_prov_crc;
volatile uint32_t g_prov_buf_addr;
volatile uint32_t g_prov_buf_size;
volatile uint32_t g_prov_enabled;

/* 空壳：不搬数据。但**要读一下 g_prov_enabled** —— 否则它会被 --gc-sections
 * 丢掉，host 脚本就分不清"没编进来"和"编进来了但状态是 0"。 */
void qspi_provision_poll(void)
{
	if (g_prov_enabled != 0u) return;
}

#endif /* YMGUI_QSPI_PROVISION */
