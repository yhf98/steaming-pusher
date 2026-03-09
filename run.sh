#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/build/whip_push"

if [ ! -f "$BIN" ]; then
    echo "可执行文件不存在，先编译..."
    bash "$SCRIPT_DIR/build.sh"
fi

# 默认参数
INPUT="${1:-/root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4}"
WHIP_URL="${2:-http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=111&secret=557ea19cf905454bad9dc988d0c6a5g1}"

echo "输入: $INPUT"
echo "推流地址: $WHIP_URL"
echo ""

export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"

exec "$BIN" "$INPUT" "$WHIP_URL"
