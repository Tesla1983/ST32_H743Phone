#!/usr/bin/env bash
# 命令通道一键验收（PC6 → 对端 GPIO16 那根线接好之后跑这个）
# ==============================================================================
# 为什么必须**先复位**：STM32 的开机命令状态机只在上电后跑一轮
# （IDLE→LINK→PING→WEA→WDATA→DONE），跑完就停在 DONE。
# 不复位的话，脚本里那些"增量"判据看到的只是静止状态，会误判。
#
# 判据（全经 SWD 读固件计数，不需要屏幕也不需要串口助手）：
#   P0  g_cmd_tx > 0 且 g_cmd_rx > 0     双向通
#   P1  g_cmd_weather_ok > 0             开机主动拉到天气
#   P2  g_cmd_bd_pkts 增加、CRC 坏不增加，且负载与 PC 抓的同一 URL 逐字节一致
# ==============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

PROBE="--probe 0416:5021:0123456789AB"
CHIP="STM32H743VITx"

echo ">>> 复位 STM32 ..."
probe-rs reset $PROBE --chip $CHIP || exit 1

echo ">>> 等 8 s，让开机命令流程跑完（会自动发 PING 和 $?WEA）"
sleep 8

python tools/cmd_link_check.py --ping --weather --wget
