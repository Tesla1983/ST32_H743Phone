# ===========================================================================
# YMGUI → STM32H743VIT6 构建脚本
#
# 配置：
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#
# 设计要点：
#   - YMGUI 库按官方 ymgui_app.cmake 的同一口径 GLOB 12 个可移植层目录建静态库；
#   - 厂商 BSP/SYSTEM 源码用 `#include "./SYSTEM/sys/sys.h"` 这种**相对工程根**的写法，
#     所以把 third_party/ 整个加进头文件搜索路径；
#   - lcd.c 内部 `#include "lcd_ex.c"`，**不能**再把 lcd_ex.c 单独加进源列表（会重复定义）。
# ===========================================================================

cmake_minimum_required(VERSION 3.20)
project(ymgui_h743 C ASM)

# ---- YMGUI 编译配置（与 project_Demo/ymgui_app.cmake 同口径）----
# ---- 优化等级（2026-10-03 新增）----
# 本工程此前**从未设置过** CMAKE_BUILD_TYPE，导致整个固件以 gcc 默认的 **-O0** 编译。
# 对于 LCD 并口 flush 这种逐像素热循环，-O0 会保留真实的 BL 函数调用与内存里的循环计数器，
# 每像素多付约 30 个周期 —— 这是原先 `g_flush_cyc` 高达 20.5 ms 的首要原因，而非总线不够快。
#
# 定点选 **-O2**（不用 Release 默认的 -O3）：
#   · 厂商 `lcd.c` 里那句 `data = data;` 的注释明确写着"使用 -O2 优化的时候必须插入的延时"，
#     作者的验证基线就是 -O2；
#   · -O3 的激进内联/循环展开会明显撑大 text，本工程 flash 不紧张但 I-Cache 只有 16 KB，
#     代码变大反而容易掉进取指瓶颈。
# 已确认的安全前提：`third_party/SYSTEM/delay/delay.c` 的延时是**读 SysTick->VAL 的硬件计时**，
# 不是空循环，因此 -O2 不会把它优化掉。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "构建类型" FORCE)
endif()
# Release 自带 -O3，这里显式降级到 -O2，避免不同工具链的默认档位不一致。
add_compile_options(-O2)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # YMGUI 是 C99 + GNU 扩展

set(YMGUI_COLOR_DEPTH "16" CACHE STRING "Framebuffer color depth: 1, 8, 16 or 24")
set_property(CACHE YMGUI_COLOR_DEPTH PROPERTY STRINGS 1 8 16 24)
if(NOT YMGUI_COLOR_DEPTH MATCHES "^(1|8|16|24)$")
	message(FATAL_ERROR "YMGUI_COLOR_DEPTH must be 1, 8, 16 or 24")
endif()
add_compile_definitions(YMGUI_COLOR_DEPTH=${YMGUI_COLOR_DEPTH})

option(YMGUI_ANIM "Build the optional UI animation module" ON)
if(YMGUI_ANIM)
	add_compile_definitions(YMGUI_ANIM=1)
else()
	add_compile_definitions(YMGUI_ANIM=0)
endif()

add_compile_definitions(USE_HAL_DRIVER STM32H743xx)

# phone_shell 的中文字体走 QSPI memory-mapped（计划书 §6.5）：
# phone_locale.c 见到这个宏就走 bitmap=0x90000000 + glyph_read=NULL 那条路，
# 否则（桌面构建）走 fopen(gb2312_glyphs.bin) 那条路。
add_compile_definitions(PHONE_LOCALE_XIP_BASE=0x90000000)

# phone_shell 用板级入口（phone_shell_board.h + phone_shell.c 里的 BoardInit/BoardTick）
# 代替依赖 SDL/stdio 的宿主 main()。
add_compile_definitions(PHONE_SHELL_BOARD=1)

# ---- 拼音输入法（阶段 5 / T6）----
# 手机级中文软键盘（phone_ime.c）开启；词典模式：
#   单字词典 pinyin_gb2312.bin（116 KB）用 objcopy 内嵌进片上 flash，
#   ime_candidates.inc 的 YMGUI_IME_CHARS_ROM 分支**原地解析**（零拷贝），
#   entries 索引在大对象池构建（7291 × 12 ≈ 85 KB，heap1 放得下）。
#   词组词典 phrases_rime.bin 的 RAM 索引要 739 KB（47280 × 16B），
#   本板任何 RAM 都放不下 → OFF（记录为已知缺口，见 README 阶段 5）。
#   英文词典同样 OFF（走库的 direct English input 模式）。
add_compile_definitions(
	YAOMI_IME=1
	YMGUI_IME_CHARS_ROM=1
	YMGUI_IME_PHRASES_OFF=1
	# IME 的大静态池（候选词池等 ≈60 KB）放进 SRAM2 的 .bss_ime 段，
	# 否则 DTCM 的 .bss 会被挤到 heap0 区间倒挂（实测）。
	# 段名不带引号地传（Windows 上 CMake/Ninja 会把带引号定义的引号吃掉，实测），
	# .inc 里自己拼 "." 前缀。
	YMGUI_IME_BSS_SECTION_NAME=bss_ime
)

# 库内剪贴板静态缓冲上限。默认 32768，占 32 KB 的 **DTCM .bss** ——
# DTCM 一共 128 KB（.bss + sbrk 16 KB + heap0 + 栈 32 KB），
# 32 KB 的剪贴板会把 YMGUI 的小对象堆（heap0）从 79 KB 挤到 39 KB。
# 库自己在 YMGUI_Hal.c 的注释里就写明"裸机可用编译宏改小省 RAM"，本板取 2 KB
# （剪贴板文本用不到 30 KB，且本工程没有编入 IME）。省下的 30 KB 全部给 heap0。
add_compile_definitions(GY_CLIP_MAX=2048)

# ---- 渲染诊断：**默认关**（2026-10-04 步骤 2b）----
#
# 【为什么从"默认编入"改成"默认关"】这三项都曾经默认开着，而它们**都站在热路径上**，
# 于是每一次量出来的帧时间都含着诊断自己的成本：
#
#   · 全帧镜像（YMGUI_PORT_FRAME_MIRROR，见下面第二个 option）
#       每帧 300 KB memcpy，而且在 flush_cb **内部**，所以既进 g_flush_cb_cyc
#       也进 g_bench_cyc —— README 记过它值 ≈2.5 ms/帧（≈12%）。
#   · YMGUI_DIAG_FILLPX
#       GY_PutPx / GY_BlendPx 每写一个像素就做一次全局变量读-改-写。这两个函数
#       正是线条/圆弧/圆形羽化环的逐像素出口。
#   · YMGUI_DIAG_OBJTIME
#       每个对象**每条 band**做一次线性表查找（表 72 项）。全屏一帧 = 15 条 band，
#       所以真实成本是"每帧 15×对象数 次查找"，不是注释里说的"每帧 0.05 ms"。
#
# 【怎么用】要测量时显式打开（`-DYMGUI_DIAG=ON`），量完关掉再测产线数字。
# 关掉后 main.c 里读这些计数的代码也被 `#if` 摘掉了，链接不会缺符号。
option(YMGUI_DIAG "编译渲染诊断计数/计时（会明显拖慢帧率，仅供测量）" OFF)
if(YMGUI_DIAG)
	add_compile_definitions(YMGUI_DIAG_FILLPX=1 YMGUI_DIAG_OBJTIME=1)
	message(STATUS "渲染诊断: ON（诊断站在热路径上，帧率会明显偏低，仅供测量）")
else()
	message(STATUS "渲染诊断: OFF（出货形态）")
endif()

# 全帧镜像缓冲（flush_cb 里把每条 band 拷进 AXI SRAM 的 300 KB 副本）。
# 它不是显示链路的一部分，只给两个诊断当参照物：
#   · A2 的 gram_verify（整屏回读面板与镜像逐像素比对）
#   · tools/grab_ymgui.py 抓屏还原 PNG
# 默认 0 = **不做这次拷贝**。注意 g_frame_mirror 仍是可读写全局量，
# 所以 SWD 写 1 就能临时打开（跑 A2/抓屏前写 1、并让它跑满一帧让镜像追上）。
option(YMGUI_PORT_FRAME_MIRROR "flush 时同步 300KB 全帧镜像（抓屏/A2 需要）" OFF)
if(YMGUI_PORT_FRAME_MIRROR)
	add_compile_definitions(YMGUI_PORT_FRAME_MIRROR=1)
else()
	add_compile_definitions(YMGUI_PORT_FRAME_MIRROR=0)
endif()

# 开机自检（A2 双自检 ramp + gram_verify）—— **默认关**（默认值在 src/ymgui_port.h）。
# 打开后每次上电会跑一遍，代价是面板闪一下彩色波纹（约 86 ms，i & 0xFFFF 图案，
# 之后自动恢复）。**正常开发不需要打开**：主循环里有 g_gram_recheck 钩子，
# SWD 写 1 即可随时重跑同一套自检，不用重编译。
# 需要编译进固件（例如要验证"冷启动第一帧的读通路"）时取消下一行注释：
# add_compile_definitions(YMGUI_PORT_BOOT_SELFTEST=1)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(TP   ${ROOT}/third_party)
# ⚠ 注意层级：third_party/YMGUI 是 **git 仓库根**（含 YMGUI/、SDL_LCD/、project_Demo/、docs/），
#   真正的库本体在再下一级的 YMGUI/ 里。写错一层会让 GLOB 一个文件都收不到。
set(YMGUI_REPO ${TP}/YMGUI)
set(YMGUI_ROOT ${YMGUI_REPO}/YMGUI)

# ---- 头文件搜索路径 ----
include_directories(
	${ROOT}/src
	${TP}                                        # 厂商源码的 "./SYSTEM/..." 相对根
	${TP}/CMSIS/Device/ST/STM32H7xx/Include
	${TP}/CMSIS/Include
	${TP}/STM32H7xx_HAL_Driver/Inc
	${TP}/BSP/LCD ${TP}/BSP/LED ${TP}/BSP/MPU
	${TP}/SYSTEM/sys ${TP}/SYSTEM/delay ${TP}/SYSTEM/usart
	${YMGUI_ROOT}/CONFIG
	${YMGUI_ROOT}/DEBUG
	${YMGUI_ROOT}/COMMON/GEOM
	${YMGUI_ROOT}/COMMON/MATH
	${YMGUI_ROOT}/OPOBJ
	${YMGUI_ROOT}/CORE
	${YMGUI_ROOT}/GUI
	${YMGUI_ROOT}/ANIM
	${YMGUI_ROOT}/WIDGET
	${YMGUI_ROOT}/STATE
	${YMGUI_ROOT}/PLUGIN
	${YMGUI_ROOT}/HAL
	${YMGUI_REPO}/project_Demo/phone_shell   # phone_shell 的应用层头（phone_locale.h 等）
	${YMGUI_REPO}/project_Demo/chinese_ime   # ime_symbols.h（phone_ime.c 用）
)

# ---- 应用层（我们自己写的板级代码）----
set(APP_SRC
	src/startup_gcc.s
	src/board_startup.c
	src/board_alloc.c
	src/board_fault.c
	src/ymgui_port.c
	src/touch_port.c
	src/qspi_port.c
	src/font_provision.c
	# UI 设置持久化（2026-10-04）：把界面语言存到 W25Q128 的扇区 240（0x000F0000）。
	# 16 MB 里目前只用了字库 982 KB + 两个扇区，约 15 MB 空闲；
	# 且 qspi_write 本身带"内容未变则跳过擦除"，语言这种低频改动等于零磨损。
	src/app_config.c
	# DMA ↔ D-Cache 一致性双向台架（2026-10-04）：接 TF 卡/任何 DMA 外设前先把
	# "总线主设备绕过 D-Cache"这条风险量成板上实测证据。只需要 HAL DMA（已编入），
	# 不需要 SD 卡、不占引脚、不用中断。
	src/dma_bench.c
	# ---- phone_shell 应用层（阶段 5）----
	# 与官方 project_Demo/phone_shell/CMakeLists.txt 同一组源；区别只有：
	#   加 PHONE_SHELL_BOARD 宏 → 用板级入口代替宿主 main（不依赖 SDL/stdio）；
	#   phone_ime.c 已编入（YAOMI_IME=1，词典模式见文件顶部的 IME 说明）。
	${YMGUI_REPO}/project_Demo/phone_shell/phone_shell.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_font.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_locale.c
	# 界面语言（中/英）：翻译表 + 语言状态。不依赖 flash，桌面构建同样可用；
	# 持久化靠 PhoneLang_SetPersistCallback() 由板级注入（见 src/app_config.c）。
	${YMGUI_REPO}/project_Demo/phone_shell/phone_lang.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_launcher.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_ui.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_app.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_shade.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_desktop.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_quick.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_quick_defaults.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_quick_builtin.c
	${YMGUI_REPO}/project_Demo/phone_shell/phone_ime.c
	src/main.c
	src/stm32h7xx_it.c
	${TP}/CMSIS/Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c
)

# ---- GB2312 字库载荷：tools/gb2312_glyphs.bin → objcopy 成 .rodata 目标文件 ----
# 为什么要 objcopy 而不是生成 C 数组：982 KB 的二进制写成 C 源码约 5 MB 文本，
# 编译要几十秒且吃内存（phone_shell 自带的 phone_font.c 就是那种写法，1.8 MB 文本，
# 只装 78 个 ASCII 字形）。objcopy 直接把二进制包成 .rodata，秒级完成，
# 且符号名固定为 _binary_gb2312_bin_start / _end，不必猜。
#
# objcopy 的符号名来自**传给它的那个路径字符串**（非字母数字都换成下划线），
# 所以这里必须在 build/ 里用**相对文件名** gb2312.bin 调用，符号才会是
# `_binary_gb2312_bin_start`。传绝对路径会得到
# `_binary_E__ymgui_h743_build_gb2312_bin_start` 这种没法引用的名字（踩过）。
set(FONT_BIN  ${YMGUI_REPO}/tools/gb2312_glyphs.bin)
set(FONT_OBJ  ${ROOT}/build/gb2312_blob.o)
set(FONT_COPY ${ROOT}/build/gb2312.bin)

add_custom_command(
	OUTPUT ${FONT_OBJ}
	COMMAND ${CMAKE_COMMAND} -E copy ${FONT_BIN} ${FONT_COPY}
	COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B armv7e-m
	        --rename-section .data=.rodata.gb2312,alloc,load,readonly,data,contents
	        gb2312.bin gb2312_blob.o
	WORKING_DIRECTORY ${ROOT}/build
	DEPENDS ${FONT_BIN}
	COMMENT "打包 GB2312 字库载荷（objcopy -I binary）"
)
	set_source_files_properties(${FONT_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# ---- 拼音单字词典载荷（T6）----
# 与字库同理：objcopy 打包 .bin；ime_candidates.inc 的 YMGUI_IME_CHARS_ROM 分支
# 引用符号 _binary_ime_chars_bin_start/_end（名字由这里的相对文件名 ime_chars.bin 决定）。
set(IME_BIN  ${YMGUI_REPO}/project_Demo/chinese_ime/pinyin_gb2312.bin)
set(IME_OBJ  ${ROOT}/build/ime_chars_blob.o)
set(IME_COPY ${ROOT}/build/ime_chars.bin)

add_custom_command(
	OUTPUT ${IME_OBJ}
	COMMAND ${CMAKE_COMMAND} -E copy ${IME_BIN} ${IME_COPY}
	COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B armv7e-m
	        --rename-section .data=.rodata.ime_chars,alloc,load,readonly,data,contents
	        ime_chars.bin ime_chars_blob.o
	WORKING_DIRECTORY ${ROOT}/build
	DEPENDS ${IME_BIN}
	COMMENT "打包拼音单字词典（objcopy -I binary）"
)
set_source_files_properties(${IME_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# ---- phone_shell 的 16 个应用（apps/*.c）----
# 与官方 phone_shell/CMakeLists.txt 同口径：从 phone_app_catalog.def 解析出源文件名。
file(STRINGS "${YMGUI_REPO}/project_Demo/phone_shell/phone_app_catalog.def" PHONE_APP_ENTRIES REGEX "^PHONE_APP\\(")
set(PHONE_APP_SRC)
foreach(_entry IN LISTS PHONE_APP_ENTRIES)
	if(_entry MATCHES "^PHONE_APP\\([A-Z_]+, *([a-z0-9_]+)\\)$")
		list(APPEND PHONE_APP_SRC "${YMGUI_REPO}/project_Demo/phone_shell/apps/${CMAKE_MATCH_1}.c")
	else()
		message(FATAL_ERROR "Invalid phone app entry: ${_entry}")
	endif()
endforeach()
list(LENGTH PHONE_APP_SRC PHONE_APP_N)
message(STATUS "phone_shell 应用数: ${PHONE_APP_N}")
list(APPEND APP_SRC ${PHONE_APP_SRC})

# ---- 厂商 BSP + SYSTEM（已转 UTF-8）----
set(VENDOR_SRC
	${TP}/SYSTEM/sys/sys.c
	${TP}/SYSTEM/delay/delay.c
	${TP}/SYSTEM/usart/usart.c
	${TP}/BSP/LED/led.c
	${TP}/BSP/MPU/mpu.c
	${TP}/BSP/LCD/lcd.c
	# 触摸：软件 I2C + GT9xxx 电容屏（引脚已按本机实测改为 SCL=PB11/SDA=PB14/RST=PB12/INT=PB13）
	${TP}/BSP/TOUCH/ctiic.c
	${TP}/BSP/TOUCH/gt9xxx.c
)

# ---- HAL 驱动（只取本工程用得到的模块）----
set(HAL_SRC
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sram.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c
	# HAL 的 FMC 封装在 LL 层：HAL_SRAM_Init 会调 FMC_NORSRAM_Init /
	# FMC_NORSRAM_Timing_Init / FMC_NORSRAM_Extended_Timing_Init，缺了会 undefined reference
	${TP}/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_fmc.c
)

# ---- YMGUI 库本体（12 个可移植层，GLOB 与官方同口径）----
file(GLOB YMGUI_SRC
	${YMGUI_ROOT}/CONFIG/*.c
	${YMGUI_ROOT}/DEBUG/*.c
	${YMGUI_ROOT}/COMMON/GEOM/*.c
	${YMGUI_ROOT}/COMMON/MATH/*.c
	${YMGUI_ROOT}/OPOBJ/*.c
	${YMGUI_ROOT}/CORE/*.c
	${YMGUI_ROOT}/GUI/*.c
	${YMGUI_ROOT}/ANIM/*.c
	${YMGUI_ROOT}/WIDGET/*.c
	${YMGUI_ROOT}/STATE/*.c
	${YMGUI_ROOT}/PLUGIN/*.c
	${YMGUI_ROOT}/HAL/*.c
)
if(NOT YMGUI_ANIM)
	list(FILTER YMGUI_SRC EXCLUDE REGEX "/ANIM/")
endif()
list(LENGTH YMGUI_SRC YMGUI_SRC_N)
message(STATUS "YMGUI 源文件数: ${YMGUI_SRC_N}")

add_executable(ymgui-h743.elf ${APP_SRC} ${VENDOR_SRC} ${HAL_SRC} ${YMGUI_SRC} ${FONT_OBJ} ${IME_OBJ})

# YMGUI 头文件里可能用到 -Wno-... 之类，先只做基本告警抑制
target_compile_options(ymgui-h743.elf PRIVATE
	-Wall
	-Wno-unused-parameter
	-Wno-sign-compare
)

# ---- 链接：自定义链接脚本 + newlib-nano（无系统调用）+ libm ----
# phone_shell 的 phone_ui.c 用了 sqrt/cos/sin/ceil（圆角、图标、画廊绘制都要），
# 官方 CMake 里也是这么链的（ymgui_add_app 里 `target_link_libraries(... m)`）。
target_link_options(ymgui-h743.elf PRIVATE
	-T${ROOT}/linker/stm32h743vit6.ld
	--specs=nano.specs
	--specs=nosys.specs
	-nostartfiles
	-Wl,-Map=${ROOT}/build/ymgui-h743.map
	-Wl,--print-memory-usage
	# ---- 可复现构建：剥离调试节 ----
	# 为什么必须加这条：
	#   本机 Windows 版和 CI 的 Linux 版 Arm GNU Toolchain 是**同一个 release（14.3.Rel1）**，
	#   但两边预编译好的 libgcc / libc_nano 里，DWARF 记录的是各自那台编译服务器的路径：
	#     Windows: /data/jenkins/workspace/GNU-toolchain/arm-14-5/build-...
	#     Linux  : /data/jenkins/workspace/GNU-toolchain/arm-14/build-...
	#   两边差 22 字节，全部落在 .debug_line_str ⇒ 代码字节完全一致，ELF 哈希却不一致。
	#   这是**编译期改动覆盖不到、也无法用 -ffile-prefix-map 改写**的部分（库是预编译好的），
	#   所以只能在链接期把调试节全部丢掉。
	# 为什么用 --strip-debug 而不是 -s：
	#   tools/bench.py 等 SWD 诊断脚本要靠 `arm-none-eabi-nm` 从 .symtab 解析符号地址
	#   （g_loop_count、g_cfg_brightness 等），符号表必须留下，只删 .debug_*。
	-Wl,--strip-debug
)

target_link_libraries(ymgui-h743.elf PRIVATE m)

# ---- 产物：bin / hex ----
# ⚠ 不用 add_custom_command(TARGET ... POST_BUILD)：本机 CMake/Ninja 生成的自定义
#    命令会经 cmd.exe 执行，而当前 shell 环境下找不到 cmd.exe，构建末端会报
#    `'cmd.exe' 不是内部或外部命令`。改成独立 target，显式用 ${CMAKE_COMMAND} -E，
#    不依赖任何 shell。
add_custom_target(ymgui-bin
	COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:ymgui-h743.elf> ${ROOT}/build/ymgui-h743.bin
	COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:ymgui-h743.elf> ${ROOT}/build/ymgui-h743.hex
	COMMAND ${CMAKE_SIZE}              $<TARGET_FILE:ymgui-h743.elf>
	DEPENDS ymgui-h743.elf
	COMMENT "生成 bin/hex 并打印尺寸"
)
