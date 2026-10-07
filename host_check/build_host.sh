#!/usr/bin/env bash
# ===========================================================================
# 无头宿主构建：把 YMGUI + phone_shell 编成宿主可执行文件（不依赖 SDL2）
#
# 本机有 /c/mingw64 的 gcc（宿主编译器），但没有 SDL2 开发包；
# SDL 依赖已被 host_lcd_stub.c 顶掉，所以能直接编。
#
# 与板级构建的差异（三处，都是为了在最小改动下跑起来）：
#   1. 内存分配器用 YMGUI_Mem.c.vendor_orig（malloc 版）。
#      板级的 YMGUI_Mem.c 已改成接 board_alloc（DTCM/AXI 双档堆），宿主上不存在。
#   2. YAOMI_IME=0 → phone_ime.c 不参与编译，phone_ime.h 里是 static inline 桩。
#      原因：chinese_ime/*.bin 拼音词典在本仓库中不存在（见阶段 5 记录）。
#   3. 用 host_lcd_stub.c 提供 SDL_LCD_* 五个符号，flush_cb 累积整帧并可导出。
# ===========================================================================
set -e

CC=${CC:-/c/mingw64/bin/gcc}
R=/e/ymgui-h743/third_party/YMGUI
Y=$R/YMGUI
P=$R/project_Demo/phone_shell
H=/e/ymgui-h743/host_check
B=$H/build
mkdir -p "$B"

INC="-I$Y/CONFIG -I$Y/DEBUG -I$Y/COMMON/GEOM -I$Y/COMMON/MATH -I$Y/OPOBJ -I$Y/CORE"
INC="$INC -I$Y/GUI -I$Y/ANIM -I$Y/WIDGET -I$Y/STATE -I$Y/PLUGIN -I$Y/HAL"
INC="$INC -I$R/SDL_LCD -I$P -I$H"

# 库源码 GLOB（与官方 ymgui_app.cmake 同口径），排除板级改写过的 YMGUI_Mem.c
LIB=$(ls $Y/CONFIG/*.c $Y/DEBUG/*.c $Y/COMMON/GEOM/*.c $Y/COMMON/MATH/*.c \
         $Y/OPOBJ/*.c $Y/CORE/*.c $Y/GUI/*.c $Y/ANIM/*.c $Y/WIDGET/*.c \
         $Y/STATE/*.c $Y/PLUGIN/*.c $Y/HAL/*.c | grep -v '/CONFIG/YMGUI_Mem\.c$')

APP="$P/phone_shell.c $P/phone_font.c $P/phone_locale.c $P/phone_launcher.c"
APP="$APP $P/phone_ui.c $P/phone_app.c $P/phone_shade.c $P/phone_desktop.c"
APP="$APP $P/phone_quick.c $P/phone_quick_defaults.c $P/phone_quick_builtin.c"
APP="$APP $(ls $P/apps/*.c | grep -v 'example\.c$' | tr '\n' ' ')"

FONT_BIN="$R/tools/gb2312_glyphs.bin"
[ -f "$FONT_BIN" ] || { echo "缺字库: $FONT_BIN"; exit 1; }

echo "库源文件 $(echo $LIB | wc -w) 个，应用源文件 $(echo $APP | wc -w) 个"

"$CC" -std=gnu99 -O1 -g -Wno-unused-parameter -Wno-sign-compare $INC \
    -DYMGUI_COLOR_DEPTH=16 -DYMGUI_ANIM=1 -DYAOMI_IME=0 \
    -DGB2312_BIN_PATH="\"$FONT_BIN\"" \
    -o "$B/phone_host.exe" \
    $APP $LIB "$H/ymgui_mem_host.c" "$H/host_lcd_stub.c" -lm

echo "构建成功: $B/phone_host.exe"
ls -la "$B/phone_host.exe"
