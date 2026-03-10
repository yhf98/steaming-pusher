#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/build/stream_push"

if [ ! -f "$BIN" ]; then
    echo "可执行文件不存在，先编译..."
    bash "$SCRIPT_DIR/build.sh"
fi

# 默认参数
INPUT="${1:-/root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4}"
OUTPUT_URL="${2:-rtmp://192.168.0.138/live/test}"

echo "输入: $INPUT"
echo "推流地址: $OUTPUT_URL"
echo ""
echo "支持的输出协议: rtmp://, srt://, rtp://, rtsp://"
echo ""

export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"

exec "$BIN" "$INPUT" "$OUTPUT_URL"
