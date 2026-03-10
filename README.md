# stream_push — 轻量级多协议推流工具

C++ 单文件实现的多协议推流工具，基于 FFmpeg。支持将视频文件、摄像头、网络流推送到各种直播服务器。

## 支持的协议

| 输出协议 | URL 格式 | 依赖 |
|---------|----------|------|
| **RTMP** | `rtmp://192.168.0.138:2022/app/stream` | FFmpeg |
| **SRT** | `srt://192.168.0.138:2022:port?streamid=...` | FFmpeg (with libsrt) |
| **RTP** | `rtp://host:port` | FFmpeg |
| **RTSP** | `rtsp://192.168.0.138:2022:port/path` | FFmpeg |
| **WHIP/WebRTC** | `http://srs/rtc/v1/whip/?...` | libdatachannel + libcurl (可选编译) |

## 输入源

| 类型 | 格式示例 | 说明 |
|------|----------|------|
| 本地文件 | `/path/to//root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4` | 默认循环播放 |
| V4L2 摄像头 | `/dev/video0` | USB / CSI 摄像头 |
| RTSP 流 | `rtsp://ip/stream` | 网络摄像头 |
| HTTP 流 | `http://ip/live/xxx.flv` | HTTP-FLV 流 |

## 编译

### 轻量模式（默认，仅 FFmpeg 依赖）

```bash
bash build.sh
```

依赖安装:
```bash
apt-get install -y build-essential cmake pkg-config
apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libavdevice-dev libswscale-dev
```

### 完整模式（含 WHIP/WebRTC）

```bash
bash build.sh --whip
```

额外依赖:
```bash
apt-get install -y libcurl4-openssl-dev
# libdatachannel 从源码编译:
# git clone https://github.com/niclaas/libdatachannel.git
# cd libdatachannel && git submodule update --init
# cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0 -DCMAKE_BUILD_TYPE=Release
# cmake --build build -j$(nproc) && cmake --install build
```

编译产物: `build/stream_push`

## 使用

```
./build/stream_push [选项] <input> <output_url>
```

### 示例

```bash
# RTMP 推流
./build/stream_push /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 rtmp://192.168.0.138:1935/live/111?secret=557ea19cf905454bad9dc988d0c6a5g1 

ffmpeg -re -stream_loop -1 \
-i /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 \
-c copy \
-f rtsp \
rtsp://192.168.0.138:7554/live/test




# SRT 推流
./build/stream_push /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 "srt://192.168.0.138:2022:10080?streamid=live/test&secret=557ea19cf905454bad9dc988d0c6a5g1"

# RTP 推流 (自动输出 SDP)
./build/stream_push /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 rtp://192.168.1.100:5004

# RTSP 推流
./build/stream_push /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 rtsp://192.168.0.138:2022:8554/live/test

# 摄像头 → RTMP
./build/stream_push /dev/video0 rtmp://192.168.0.138:2022/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1 

# 摄像头指定分辨率
./build/stream_push --width 1920 --height 1080 --fps 25 /dev/video0 rtmp://192.168.0.138:2022/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1 

# RTSP 转 RTMP
./build/stream_push "rtsp://admin:password@192.168.1.100:554/stream1" rtmp://192.168.0.138:2022/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1 

# WHIP/WebRTC (需 --whip 编译)
./build/stream_push /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=test&secret=557ea19cf905454bad9dc988d0c6a5g1"
```

### 选项

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--width N` | 摄像头宽度 | 1280 |
| `--height N` | 摄像头高度 | 720 |
| `--fps N` | 摄像头帧率 | 30 |
| `--bitrate N` | 编码码率 (bps) | 2000000 |
| `--loop` | 文件循环播放 | 开启 |
| `--no-loop` | 不循环 | - |

## 打包部署

```bash
bash package.sh
# 生成 dist/stream-push.tar.gz

# 部署:
tar xzf stream-push.tar.gz
cd stream-push && ./run.sh input.mp4 rtmp://192.168.0.138:2022/live/test
```

## 硬件编码支持

在 RK3588/RK3566 等 ARM 平台，若 FFmpeg 编译时包含 `h264_rkmpp`，会自动使用硬件编码器。

优先级: `h264_rkmpp` > `h264_v4l2m2m` > `libx264`




curl http://192.168.0.138:7080/index/api/getMediaList


ffmpeg \
-rtsp_transport tcp \
-i rtsp://192.168.0.138:7554/live/test \
-c copy \
-f flv \
rtmp://192.168.0.138:1935/live/333