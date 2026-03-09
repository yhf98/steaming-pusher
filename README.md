# WHIP WebRTC 推流工具

将本地视频文件、摄像头画面或网络流通过 WebRTC (WHIP) 推送到 SRS 服务器。

支持 x86_64 和 ARM (RK3588/RK3566 等瑞芯微) 平台。

## 编译

### 依赖安装 (Ubuntu/Debian x86_64)

```bash
# 基础工具
apt-get install -y build-essential cmake pkg-config

# FFmpeg 开发库
apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libavdevice-dev libswscale-dev

# libcurl
apt-get install -y libcurl4-openssl-dev

# libdatachannel (WebRTC)
# 如果系统没有，从源码编译安装:
# git clone https://github.com/niclaas/libdatachannel.git
# cd libdatachannel && git submodule update --init
# cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0 -DCMAKE_BUILD_TYPE=Release
# cmake --build build -j$(nproc) && cmake --install build
```

### 依赖安装 (ARM RK 系列)

```bash
# 基础工具 + FFmpeg (确保含 rkmpp 编码器)
apt-get install -y build-essential cmake pkg-config
apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libavdevice-dev libswscale-dev
apt-get install -y libcurl4-openssl-dev
```

### 编译

```bash
cd whip-push-demo
bash build.sh
```

编译产物: `build/whip_push`

如需手动编译:

```bash
cd whip-push-demo
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 用法

```
./build/whip_push [选项] <输入源> <推流地址>
```

**重要: 推流地址包含 `&` 符号，必须用引号包裹，否则 bash 会将 `&` 后面的内容当作后台命令。**

### 输入源类型

| 类型 | 格式示例 | 说明 |
|------|----------|------|
| 本地文件 | `/path/to/video.mp4` | 默认循环播放 |
| V4L2 摄像头 | `/dev/video0` | USB 摄像头 / CSI 摄像头 |
| RTSP 流 | `rtsp://ip/stream` | 网络摄像头 |
| HTTP 流 | `http://ip/live/xxx.flv` | HTTP-FLV 流 |

### 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--width N` | 1280 | 摄像头采集宽度 |
| `--height N` | 720 | 摄像头采集高度 |
| `--fps N` | 30 | 摄像头采集帧率 |
| `--bitrate N` | 2000000 | 编码码率 (bps)，仅摄像头非 H264 输出时有效 |
| `--loop` | 开启 | 文件播放完毕后循环 |
| `--no-loop` | - | 文件播放完毕后停止 |
| `-h, --help` | - | 显示帮助信息 |

## 运行示例

### 推送本地视频文件

```bash
./build/whip_push \
  /root/workspace/ms-fish-recg-pro/2026-01-06-08-12-21.mp4 \
  "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=222&secret=557ea19cf905454bad9dc988d0c6a5g1"
```

### 推送摄像头画面

```bash
# 默认 1280x720 @ 30fps
./build/whip_push \
  /dev/video0 \
  "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=cam1&secret=557ea19cf905454bad9dc988d0c6a5g1"

# 指定分辨率和帧率
./build/whip_push \
  --width 1920 --height 1080 --fps 25 \
  /dev/video0 \
  "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=cam1&secret=557ea19cf905454bad9dc988d0c6a5g1"
```

### 推送 RTSP 流

```bash
./build/whip_push \
  "rtsp://admin:password@192.168.1.100:554/stream1" \
  "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=ipcam&secret=557ea19cf905454bad9dc988d0c6a5g1"
```

### 使用 run.sh 快捷脚本

```bash
# 使用默认输入文件和推流地址
./run.sh

# 自定义输入和地址
./run.sh /dev/video0 "http://192.168.0.138:2022/rtc/v1/whip/?app=live&stream=111&secret=xxx"
```
