# Streaming Pusher

一个跨平台的推流工具，支持 RTMP, RTSP, SRT, RTP 以及 WebRTC (WHIP) 协议。

## 功能

- 支持 Windows, macOS, Linux。
- 输入源：文件、摄像头、网络流。
- 输出协议：RTMP, RTSP, SRT, RTP, WHIP。
- 自动检测并优先使用 GPU 硬件编码 (NVENC)。

## 构建

### 依赖项

- FFmpeg 4.4+ (libavformat, libavcodec, libavutil, libavdevice, libswscale)
- libdatachannel (用于 WebRTC WHIP)
- libcurl (用于 WHIP HTTP 握手)
- CMake 3.10+

### 编译步骤

```bash
mkdir build && cd build
cmake .. -DENABLE_WHIP=ON
cmake --build .
```

## 使用方法

```bash
./streaming-pusher <输入源> <输出URL>
```

示例：
- 推流至 RTMP: `./streaming-pusher video.mp4 rtmp://server/app/stream`
- 推流至 WHIP: `./streaming-pusher /dev/video0 https://whip-endpoint.com/whip`
- 推流 RTSP: `./streaming-pusher rtsp://source rtsp://dest`
