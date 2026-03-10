#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# 解析参数
ENABLE_WHIP=OFF
for arg in "$@"; do
    case "$arg" in
        --whip) ENABLE_WHIP=ON ;;
        --clean) rm -rf "$BUILD_DIR"; echo "已清理构建目录" ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --whip    启用 WHIP/WebRTC 支持 (需要 libdatachannel + libcurl)"
            echo "  --clean   清理构建目录后重新编译"
            echo ""
            echo "默认: 轻量模式 (仅 FFmpeg，支持 RTMP/SRT/RTP/RTSP)"
            exit 0
            ;;
    esac
done

if [ "$ENABLE_WHIP" = "ON" ]; then
    echo "=== 编译 stream_push (含 WHIP/WebRTC) ==="
else
    echo "=== 编译 stream_push (轻量模式) ==="
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_WHIP=$ENABLE_WHIP
make -j$(nproc)

echo ""
echo "=== 编译完成 ==="
echo "可执行文件: $BUILD_DIR/stream_push"

if [ "$ENABLE_WHIP" = "ON" ]; then
    echo "模式: 完整版 (RTMP/SRT/RTP/RTSP/WHIP)"
else
    echo "模式: 轻量版 (RTMP/SRT/RTP/RTSP)"
fi
