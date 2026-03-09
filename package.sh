#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/build/whip_push"
PKG_DIR="$SCRIPT_DIR/dist/whip-push"

if [ ! -f "$BIN" ]; then
    echo "可执行文件不存在，先编译..."
    bash "$SCRIPT_DIR/build.sh"
fi

echo "=== 打包 whip_push ==="

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/lib"

# 复制可执行文件
cp "$BIN" "$PKG_DIR/"

# 收集非系统共享库（/usr/local/lib 下的）
ldd "$BIN" | grep '/usr/local/lib' | awk '{print $3}' | while read -r lib; do
    if [ -f "$lib" ]; then
        # 跟踪符号链接复制实际文件
        real=$(readlink -f "$lib")
        cp "$real" "$PKG_DIR/lib/"
        base=$(basename "$lib")
        realbase=$(basename "$real")
        if [ "$base" != "$realbase" ]; then
            ln -sf "$realbase" "$PKG_DIR/lib/$base"
        fi
        echo "  打包: $base"
    fi
done

# 生成运行脚本
cat > "$PKG_DIR/run.sh" << 'RUNEOF'
#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/whip_push"

INPUT="${1:-water_line.mp4}"
WHIP_URL="${2:-http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=test123&secret=557ea19cf905454bad9dc988d0c6a5g1}"

export LD_LIBRARY_PATH="$SCRIPT_DIR/lib:${LD_LIBRARY_PATH:-}"

echo "输入文件: $INPUT"
echo "推流地址: $WHIP_URL"
echo ""
echo "用法: ./run.sh [input.mp4] [whip_url]"
echo ""

exec "$BIN" "$INPUT" "$WHIP_URL"
RUNEOF
chmod +x "$PKG_DIR/run.sh"

# 打包为 tar.gz
cd "$SCRIPT_DIR/dist"
tar czf whip-push.tar.gz whip-push/
TARBALL="$SCRIPT_DIR/dist/whip-push.tar.gz"

echo ""
echo "=== 打包完成 ==="
echo "目录: $PKG_DIR/"
echo "压缩包: $TARBALL"
echo "大小: $(du -sh "$TARBALL" | cut -f1)"
echo ""
echo "部署方式:"
echo "  1. 复制 $TARBALL 到目标机器"
echo "  2. tar xzf whip-push.tar.gz"
echo "  3. cd whip-push && ./run.sh input.mp4 'http://host:port/rtc/v1/whip/?app=live&stream=xxx'"
echo ""
echo "目标机器需要安装: libcurl, ffmpeg (libavformat/libavcodec/libavutil), libssl"
