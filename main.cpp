#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef ENABLE_WHIP
#include <curl/curl.h>
#include <rtc/rtc.hpp>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

// ==================== 全局状态 ====================

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

// ==================== 工具函数 ====================

static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

#ifdef ENABLE_WHIP
static void str_replace_all(std::string &s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}
#endif

// ==================== 输出协议检测 ====================

enum class OutputType { RTMP, SRT, RTP, RTSP, WHIP };

static const char *output_type_name(OutputType t) {
    switch (t) {
        case OutputType::RTMP: return "RTMP";
        case OutputType::SRT:  return "SRT";
        case OutputType::RTP:  return "RTP";
        case OutputType::RTSP: return "RTSP";
        case OutputType::WHIP: return "WHIP/WebRTC";
    }
    return "未知";
}

static OutputType detect_output_type(const std::string &url) {
    if (starts_with(url, "rtmp://") || starts_with(url, "rtmps://")) return OutputType::RTMP;
    if (starts_with(url, "srt://"))  return OutputType::SRT;
    if (starts_with(url, "rtp://"))  return OutputType::RTP;
    if (starts_with(url, "rtsp://")) return OutputType::RTSP;
    // http(s):// 默认视为 WHIP
    return OutputType::WHIP;
}

// ==================== 输入源检测 ====================

enum class InputType { File, Camera, Stream, RawVideo };

static InputType detect_input_type(const std::string &input) {
#ifdef _WIN32
    if (starts_with(input, "video=")) return InputType::Camera;
#else
    if (starts_with(input, "/dev/video")) return InputType::Camera;
#endif
    if (starts_with(input, "rtsp://") || starts_with(input, "rtmp://") ||
        starts_with(input, "http://") || starts_with(input, "https://"))
        return InputType::Stream;
    return InputType::File;
}

// ==================== 输入源打开 ====================

struct InputSource {
    AVFormatContext *fmt_ctx = nullptr;
    int video_idx = -1;
    int audio_idx = -1;  // 音频流索引 (-1 表示无音频)
    bool is_camera = false;
    bool is_annex_b = false;
};

static bool open_input(const std::string &input, InputType type, InputSource &src,
                       int cam_width = 1280, int cam_height = 720, int cam_fps = 30) {
    avdevice_register_all();

    AVDictionary *opts = nullptr;
    AVInputFormat *ifmt = nullptr;

    if (type == InputType::Camera) {
        src.is_camera = true;
#ifdef _WIN32
        ifmt = av_find_input_format("dshow");
#elif defined(__APPLE__)
        ifmt = av_find_input_format("avfoundation");
#else
        ifmt = av_find_input_format("v4l2");
#endif
        if (!ifmt) {
            fprintf(stderr, "[输入] 未找到合适的摄像头输入格式\n");
            return false;
        }
        char size_buf[32], fps_buf[16];
        snprintf(size_buf, sizeof(size_buf), "%dx%d", cam_width, cam_height);
        snprintf(fps_buf, sizeof(fps_buf), "%d", cam_fps);
        av_dict_set(&opts, "video_size", size_buf, 0);
        av_dict_set(&opts, "framerate", fps_buf, 0);
#ifndef _WIN32
        av_dict_set(&opts, "input_format", "h264", 0);
#endif
    } else if (type == InputType::Stream) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
    }

    int ret = avformat_open_input(&src.fmt_ctx, input.c_str(), ifmt, &opts);
    av_dict_free(&opts);

    if (ret < 0 && type == InputType::Camera) {
        fprintf(stderr, "[输入] 摄像头尝试打开失败，重试默认配置\n");
        opts = nullptr;
        ret = avformat_open_input(&src.fmt_ctx, input.c_str(), ifmt, &opts);
    }

    if (ret < 0) {
        char err[256]; av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "[输入] 无法打开 %s: %s\n", input.c_str(), err);
        return false;
    }

    if (avformat_find_stream_info(src.fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "[输入] 无法获取流信息\n");
        avformat_close_input(&src.fmt_ctx);
        return false;
    }

    for (unsigned i = 0; i < src.fmt_ctx->nb_streams; i++) {
        if (src.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && src.video_idx < 0) {
            src.video_idx = (int)i;
        } else if (src.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && src.audio_idx < 0) {
            src.audio_idx = (int)i;
        }
    }
    if (src.video_idx < 0) {
        fprintf(stderr, "[输入] 未找到视频流\n");
        avformat_close_input(&src.fmt_ctx);
        return false;
    }

    AVCodecParameters *cp = src.fmt_ctx->streams[src.video_idx]->codecpar;
    if (cp->codec_id == AV_CODEC_ID_H264) {
        src.is_annex_b = src.is_camera;
        printf("[输入] H264 %dx%d (需要%s BSF)\n", cp->width, cp->height,
               src.is_annex_b ? "不" : "");
    } else {
        printf("[输入] %s %dx%d (需要编码为 H264)\n",
               avcodec_get_name(cp->codec_id), cp->width, cp->height);
    }

    if (src.audio_idx >= 0) {
        AVCodecParameters *ap = src.fmt_ctx->streams[src.audio_idx]->codecpar;
        printf("[输入] 音频: %s, %d Hz, %d ch\n",
               avcodec_get_name(ap->codec_id), ap->sample_rate, ap->channels);
    } else {
        printf("[输入] 无音频流 (将生成静音 AAC)\n");
    }

    return true;
}

// ==================== H264 编码器 ====================

struct H264Encoder {
    AVCodecContext *enc_ctx = nullptr;
    SwsContext *sws_ctx = nullptr;
    AVFrame *yuv_frame = nullptr;
    int64_t frame_pts = 0;
    bool initialized = false;

    bool init(int width, int height, int fps, AVPixelFormat src_fmt, int bitrate = 2000000) {
        // 按优先级尝试编码器：NVENC (GPU) → libx264 (CPU，最通用)
        const char *encoder_names[] = {"h264_nvenc", "libx264", nullptr};
        const AVCodec *codec = nullptr;

        for (int i = 0; encoder_names[i]; i++) {
            const AVCodec *c = avcodec_find_encoder_by_name(encoder_names[i]);
            if (!c) continue;
            // 尝试分配上下文并打开，失败则跳到下一个
            AVCodecContext *test_ctx = avcodec_alloc_context3(c);
            test_ctx->width     = width;
            test_ctx->height    = height;
            test_ctx->time_base = {1, fps};
            test_ctx->framerate = {fps, 1};
            test_ctx->gop_size  = fps * 2;
            test_ctx->max_b_frames = 0;
            test_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
            test_ctx->bit_rate  = bitrate;
            test_ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (strcmp(encoder_names[i], "h264_nvenc") == 0) {
                av_opt_set(test_ctx->priv_data, "preset", "p4", 0);
                av_opt_set(test_ctx->priv_data, "tune",   "ll", 0);
                av_opt_set(test_ctx->priv_data, "rc",     "cbr", 0);
            } else {
                av_opt_set(test_ctx->priv_data, "preset",  "ultrafast", 0);
                av_opt_set(test_ctx->priv_data, "tune",    "zerolatency", 0);
                av_opt_set(test_ctx->priv_data, "profile", "baseline", 0);
            }
            if (avcodec_open2(test_ctx, c, nullptr) == 0) {
                printf("[编码器] 使用 %s\n", encoder_names[i]);
                enc_ctx = test_ctx;
                codec   = c;
                break;
            }
            avcodec_free_context(&test_ctx);
        }

        if (!enc_ctx) {
            fprintf(stderr, "[编码器] 未找到可用的 H264 编码器\n");
            return false;
        }
        enc_ctx->time_base = {1, fps};
        enc_ctx->framerate = {fps, 1};
        // enc_ctx 已在上面的探测循环中初始化并打开，无需重复设置

        sws_ctx = sws_getContext(width, height, src_fmt,
                                 width, height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            fprintf(stderr, "[编码器] 无法创建格式转换\n");
            avcodec_free_context(&enc_ctx);
            return false;
        }

        yuv_frame = av_frame_alloc();
        yuv_frame->format = AV_PIX_FMT_YUV420P;
        yuv_frame->width = width;
        yuv_frame->height = height;
        av_frame_get_buffer(yuv_frame, 32);

        initialized = true;
        printf("[编码器] %dx%d @ %d fps, gop=%d, bitrate=%lld\n",
               width, height, fps, enc_ctx->gop_size, (long long)enc_ctx->bit_rate);
        return true;
    }

    bool encode(AVFrame *raw_frame, AVPacket *out_pkt) {
        if (!initialized) return false;
        sws_scale(sws_ctx, raw_frame->data, raw_frame->linesize, 0,
                  enc_ctx->height, yuv_frame->data, yuv_frame->linesize);
        yuv_frame->pts = frame_pts++;
        int ret = avcodec_send_frame(enc_ctx, yuv_frame);
        if (ret < 0) return false;
        ret = avcodec_receive_packet(enc_ctx, out_pkt);
        return ret == 0;
    }

    void request_keyframe() {
        if (yuv_frame) yuv_frame->pict_type = AV_PICTURE_TYPE_I;
    }

    void cleanup() {
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        if (yuv_frame) { av_frame_free(&yuv_frame); }
        if (enc_ctx) { avcodec_free_context(&enc_ctx); }
        initialized = false;
    }
};

// ==================== FFmpeg 输出 (RTMP/SRT/RTP/RTSP) ====================

struct FFmpegOutput {
    AVFormatContext *fmt_ctx = nullptr;
    int video_idx = -1;
    int audio_idx = -1;
    bool opened = false;
    bool has_source_audio = false;

    // 静音 AAC 编码器 (当源无音频时使用)
    AVCodecContext *silent_enc = nullptr;
    AVFrame *silent_frame = nullptr;
    int64_t silent_pts = 0;
    double last_audio_sec = 0.0;

    bool open(const std::string &url, OutputType type, AVCodecParameters *in_codecpar,
              AVRational in_tb, int width, int height,
              AVCodecParameters *in_audio_codecpar = nullptr, AVRational audio_tb = {0,1}) {
        const char *format_name = nullptr;
        switch (type) {
            case OutputType::RTMP: format_name = "flv"; break;
            case OutputType::SRT:  format_name = "mpegts"; break;
            case OutputType::RTP:  format_name = "rtp"; break;
            case OutputType::RTSP: format_name = "rtsp"; break;
            default: return false;
        }

        int ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, format_name, url.c_str());
        if (ret < 0 || !fmt_ctx) {
            char err[256]; av_strerror(ret, err, sizeof(err));
            fprintf(stderr, "[输出] 无法创建输出上下文 (%s): %s\n", format_name, err);
            return false;
        }

        // 视频流
        AVStream *vstream = avformat_new_stream(fmt_ctx, nullptr);
        if (!vstream) {
            fprintf(stderr, "[输出] 无法创建视频输出流\n");
            avformat_free_context(fmt_ctx); fmt_ctx = nullptr;
            return false;
        }
        avcodec_parameters_copy(vstream->codecpar, in_codecpar);
        vstream->codecpar->codec_tag = 0;
        if (vstream->codecpar->width == 0) vstream->codecpar->width = width;
        if (vstream->codecpar->height == 0) vstream->codecpar->height = height;
        vstream->time_base = in_tb;
        video_idx = vstream->index;

        // 音频流
        has_source_audio = (in_audio_codecpar != nullptr);
        if (has_source_audio) {
            // 转发源音频
            AVStream *astream = avformat_new_stream(fmt_ctx, nullptr);
            if (astream) {
                avcodec_parameters_copy(astream->codecpar, in_audio_codecpar);
                astream->codecpar->codec_tag = 0;
                astream->time_base = audio_tb;
                audio_idx = astream->index;
                printf("[输出] 音频: 转发源音频流\n");
            }
        } else if (type == OutputType::RTMP || type == OutputType::SRT) {
            // RTMP/mpegts 需要音频流，生成静音 AAC
            if (init_silent_aac()) {
                AVStream *astream = avformat_new_stream(fmt_ctx, nullptr);
                if (astream) {
                    avcodec_parameters_from_context(astream->codecpar, silent_enc);
                    astream->time_base = silent_enc->time_base;
                    audio_idx = astream->index;
                    printf("[输出] 音频: 生成静音 AAC\n");
                }
            }
        }

        // 协议选项
        AVDictionary *opts = nullptr;
        if (type == OutputType::RTSP) {
            av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        } else if (type == OutputType::SRT) {
            av_dict_set(&opts, "pkt_size", "1316", 0);
        }

        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&fmt_ctx->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
            if (ret < 0) {
                char err[256]; av_strerror(ret, err, sizeof(err));
                fprintf(stderr, "[输出] 无法打开 %s: %s\n", url.c_str(), err);
                av_dict_free(&opts);
                avformat_free_context(fmt_ctx); fmt_ctx = nullptr;
                return false;
            }
        }

        ret = avformat_write_header(fmt_ctx, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            char err[256]; av_strerror(ret, err, sizeof(err));
            fprintf(stderr, "[输出] 写 header 失败: %s\n", err);
            if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
            avformat_free_context(fmt_ctx); fmt_ctx = nullptr;
            return false;
        }

        if (type == OutputType::RTP) {
            char sdp[2048];
            av_sdp_create(&fmt_ctx, 1, sdp, sizeof(sdp));
            printf("\n========== RTP SDP ==========\n%s=================================\n\n", sdp);
            printf("播放端使用: ffplay -protocol_whitelist file,rtp,udp -i sdp_file.sdp\n\n");
        }

        opened = true;
        printf("[输出] %s 已就绪\n", output_type_name(type));
        return true;
    }

    bool write_video(AVPacket *pkt, AVRational in_tb) {
        if (!opened || !fmt_ctx) return false;
        AVPacket *out_pkt = av_packet_clone(pkt);
        if (!out_pkt) return false;
        av_packet_rescale_ts(out_pkt, in_tb, fmt_ctx->streams[video_idx]->time_base);
        out_pkt->stream_index = video_idx;
        int ret = av_interleaved_write_frame(fmt_ctx, out_pkt);
        av_packet_free(&out_pkt);
        return ret >= 0;
    }

    bool write_audio(AVPacket *pkt, AVRational in_tb) {
        if (!opened || !fmt_ctx || audio_idx < 0) return false;
        AVPacket *out_pkt = av_packet_clone(pkt);
        if (!out_pkt) return false;
        av_packet_rescale_ts(out_pkt, in_tb, fmt_ctx->streams[audio_idx]->time_base);
        out_pkt->stream_index = audio_idx;
        int ret = av_interleaved_write_frame(fmt_ctx, out_pkt);
        av_packet_free(&out_pkt);
        return ret >= 0;
    }

    // 生成静音音频帧追赶到指定视频时间
    void generate_silence_up_to(double video_sec) {
        if (!opened || !fmt_ctx || audio_idx < 0 || has_source_audio || !silent_enc)
            return;
        // 每帧 1024 samples @ 44100 Hz ≈ 23.2ms
        double frame_dur = 1024.0 / 44100.0;
        while (last_audio_sec < video_sec && g_running) {
            AVPacket *apkt = av_packet_alloc();
            silent_frame->pts = silent_pts++;
            int ret = avcodec_send_frame(silent_enc, silent_frame);
            if (ret < 0) { av_packet_free(&apkt); break; }
            ret = avcodec_receive_packet(silent_enc, apkt);
            if (ret == 0) {
                apkt->stream_index = audio_idx;
                av_packet_rescale_ts(apkt, silent_enc->time_base, fmt_ctx->streams[audio_idx]->time_base);
                av_interleaved_write_frame(fmt_ctx, apkt);
                last_audio_sec += frame_dur;
            }
            av_packet_free(&apkt);
            if (ret < 0) break;
        }
    }

    // 旧的 write 方法保留兼容 (映射到 write_video)
    bool write(AVPacket *pkt, AVRational in_tb) {
        return write_video(pkt, in_tb);
    }

    void close() {
        if (fmt_ctx && opened) {
            av_write_trailer(fmt_ctx);
        }
        if (fmt_ctx) {
            if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb)
                avio_closep(&fmt_ctx->pb);
            avformat_free_context(fmt_ctx);
            fmt_ctx = nullptr;
        }
        if (silent_enc) avcodec_free_context(&silent_enc);
        if (silent_frame) av_frame_free(&silent_frame);
        opened = false;
    }

private:
    bool init_silent_aac() {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) {
            fprintf(stderr, "[音频] 未找到 AAC 编码器\n");
            return false;
        }
        silent_enc = avcodec_alloc_context3(codec);
        silent_enc->codec_type = AVMEDIA_TYPE_AUDIO;
        silent_enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        silent_enc->sample_rate = 44100;
        silent_enc->channel_layout = AV_CH_LAYOUT_STEREO;
        silent_enc->channels = 2;
        silent_enc->bit_rate = 64000;
        silent_enc->time_base = {1, 44100};
        silent_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (avcodec_open2(silent_enc, codec, nullptr) < 0) {
            fprintf(stderr, "[音频] 打开 AAC 编码器失败\n");
            avcodec_free_context(&silent_enc);
            return false;
        }

        silent_frame = av_frame_alloc();
        silent_frame->format = silent_enc->sample_fmt;
        silent_frame->channel_layout = silent_enc->channel_layout;
        silent_frame->sample_rate = silent_enc->sample_rate;
        silent_frame->nb_samples = silent_enc->frame_size;
        av_frame_get_buffer(silent_frame, 0);
        // 填充静音 (浮点零)
        av_samples_set_silence(silent_frame->data, 0, silent_frame->nb_samples,
                               silent_enc->channels, silent_enc->sample_fmt);
        return true;
    }
};

// ==================== SPS/PPS 提取 ====================

static std::vector<uint8_t> extract_sps_pps_from_extradata(const uint8_t *extra, int extra_size) {
    std::vector<uint8_t> result;
    if (!extra || extra_size < 8) return result;

    // AVCC 格式
    if (extra[0] == 1) {
        int num_sps = extra[5] & 0x1f;
        int offset = 6;
        for (int i = 0; i < num_sps && offset + 2 <= extra_size; i++) {
            int len = (extra[offset] << 8) | extra[offset + 1];
            offset += 2;
            if (offset + len > extra_size) break;
            result.insert(result.end(), {0, 0, 0, 1});
            result.insert(result.end(), extra + offset, extra + offset + len);
            offset += len;
        }
        if (offset < extra_size) {
            int num_pps = extra[offset++];
            for (int i = 0; i < num_pps && offset + 2 <= extra_size; i++) {
                int len = (extra[offset] << 8) | extra[offset + 1];
                offset += 2;
                if (offset + len > extra_size) break;
                result.insert(result.end(), {0, 0, 0, 1});
                result.insert(result.end(), extra + offset, extra + offset + len);
                offset += len;
            }
        }
    }
    // Annex-B 格式
    else if (extra[0] == 0 && extra[1] == 0) {
        result.assign(extra, extra + extra_size);
    }

    return result;
}

// ==================== NAL 解析与帧组装 (WHIP 路径使用) ====================

#ifdef ENABLE_WHIP

struct NalInfo { int offset; int sc_len; int nal_type; };

static std::vector<NalInfo> find_nals(const uint8_t *data, int size) {
    std::vector<NalInfo> nals;
    for (int j = 0; j + 3 < size; j++) {
        if (data[j] == 0 && data[j+1] == 0) {
            if (data[j+2] == 0 && j + 4 < size && data[j+3] == 1) {
                nals.push_back({j, 4, data[j+4] & 0x1f});
                j += 4;
            } else if (data[j+2] == 1) {
                nals.push_back({j, 3, data[j+3] & 0x1f});
                j += 3;
            }
        }
    }
    return nals;
}

static rtc::binary build_frame_data(const uint8_t *pkt_data, int pkt_size, bool keyFrame,
                                     const std::vector<uint8_t> &sps_pps) {
    rtc::binary frame_data;
    auto nals = find_nals(pkt_data, pkt_size);

    if (keyFrame && !sps_pps.empty()) {
        auto *sp = reinterpret_cast<const std::byte *>(sps_pps.data());
        frame_data.insert(frame_data.end(), sp, sp + sps_pps.size());
    }

    for (size_t ni = 0; ni < nals.size(); ni++) {
        int nt = nals[ni].nal_type;
        if (nt < 1 || nt > 5) continue;
        int start = nals[ni].offset;
        int end = (ni + 1 < nals.size()) ? nals[ni + 1].offset : pkt_size;
        auto *p = reinterpret_cast<const std::byte *>(pkt_data + start);
        frame_data.insert(frame_data.end(), p, p + (end - start));
    }

    return frame_data;
}

// ==================== HTTP (CURL) — WHIP 专用 ====================

struct CurlResponse {
    std::string body;
    long http_code = 0;
};

static size_t curl_write_cb(void *data, size_t size, size_t nmemb, void *userp) {
    auto *resp = static_cast<CurlResponse *>(userp);
    resp->body.append(static_cast<char *>(data), size * nmemb);
    return size * nmemb;
}

static bool http_post(const std::string &url, const std::string &content_type,
                      const std::string &body, CurlResponse &resp,
                      std::string *location_out = nullptr) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct LocationCapture { std::string value; };
    LocationCapture loc_cap;
    if (location_out) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
            +[](char *buf, size_t size, size_t n, void *userp) -> size_t {
                auto *lc = static_cast<LocationCapture *>(userp);
                std::string line(buf, size * n);
                if (line.size() > 10 && (line[0] == 'L' || line[0] == 'l') &&
                    line.substr(0, 9) == "Location:" || line.substr(0, 9) == "location:") {
                    lc->value = line.substr(10);
                    while (!lc->value.empty() && (lc->value.back() == '\r' || lc->value.back() == '\n' || lc->value.back() == ' '))
                        lc->value.pop_back();
                    while (!lc->value.empty() && lc->value.front() == ' ')
                        lc->value.erase(0, 1);
                }
                return size * n;
            });
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &loc_cap);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[HTTP] POST failed: %s\n", curl_easy_strerror(res));
        return false;
    }
    if (location_out) *location_out = loc_cap.value;
    return true;
}

static bool http_delete(const std::string &url) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return false;
    return (http_code >= 200 && http_code < 300);
}

// ==================== JSON 工具 — WHIP 专用 ====================

static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 64);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c;
        }
    }
    return out;
}

static bool json_get_string(const std::string &json, const std::string &key, std::string &value) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    auto quote = json.find('"', colon + 1);
    if (quote == std::string::npos) return false;
    value.clear();
    for (size_t i = quote + 1; i < json.size(); i++) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char next = json[i + 1];
            if (next == 'n') value += '\n';
            else if (next == 'r') value += '\r';
            else if (next == 't') value += '\t';
            else if (next == '"') value += '"';
            else if (next == '\\') value += '\\';
            else value += next;
            i++;
        } else if (json[i] == '"') {
            break;
        } else {
            value += json[i];
        }
    }
    return !value.empty();
}

static bool json_get_int(const std::string &json, const std::string &key, int &value) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    value = atoi(json.c_str() + colon + 1);
    return true;
}

// ==================== URL 解析 — WHIP 专用 ====================

struct UrlParts {
    std::string scheme, host, host_only, app, stream, secret;
};

static UrlParts parse_url(const std::string &url) {
    UrlParts u;
    auto se = url.find("://");
    if (se == std::string::npos) return u;
    u.scheme = url.substr(0, se);
    auto hs = se + 3;
    auto ps = url.find('/', hs);
    if (ps == std::string::npos) { u.host = url.substr(hs); u.host_only = u.host; return u; }
    u.host = url.substr(hs, ps - hs);
    auto colon = u.host.find(':');
    u.host_only = (colon != std::string::npos) ? u.host.substr(0, colon) : u.host;
    auto q = url.find('?', ps);
    if (q != std::string::npos) {
        std::istringstream qs(url.substr(q + 1));
        std::string param;
        while (std::getline(qs, param, '&')) {
            auto eq = param.find('=');
            if (eq == std::string::npos) continue;
            std::string k = param.substr(0, eq), v = param.substr(eq + 1);
            if (k == "app") u.app = v;
            else if (k == "stream") u.stream = v;
            else if (k == "secret") u.secret = v;
        }
    }
    return u;
}

// ==================== SDP 处理 — WHIP 专用 ====================

static std::string prepare_offer(const std::string &sdp) {
    std::string result;
    std::istringstream iss(sdp);
    std::string line;
    int mid_counter = 0;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts_with(line, "m=audio")) mid_counter++;
        else if (starts_with(line, "m=video")) mid_counter++;
    }

    iss.clear();
    iss.str(sdp);
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts_with(line, "a=group:LS")) continue;
        if (starts_with(line, "a=ice-options:")) continue;
        if (starts_with(line, "a=end-of-candidates")) continue;
        if (starts_with(line, "m=audio "))
            line = "m=audio 9 UDP/TLS/RTP/SAVPF 111";
        else if (starts_with(line, "m=video "))
            line = "m=video 9 UDP/TLS/RTP/SAVPF 96";
        else if (starts_with(line, "c=IN IP4"))
            line = "c=IN IP4 0.0.0.0";
        else if (line == "a=setup:actpass")
            line = "a=setup:active";
        result += line + "\r\n";
    }

    str_replace_all(result, "BUNDLE audio video", "BUNDLE 0 1");
    str_replace_all(result, "BUNDLE audio", "BUNDLE 0");
    str_replace_all(result, "BUNDLE video", "BUNDLE 0");
    str_replace_all(result, "a=mid:audio", "a=mid:0");
    str_replace_all(result, "a=mid:video", "a=mid:1");
    return result;
}

static std::string prepare_answer(const std::string &sdp) {
    std::string s = sdp;
    str_replace_all(s, "BUNDLE 0 1", "BUNDLE audio video");
    str_replace_all(s, "BUNDLE 0", "BUNDLE audio");
    str_replace_all(s, "BUNDLE 1", "BUNDLE video");

    std::string result;
    std::istringstream iss(s);
    std::string line;
    bool in_audio = false, in_video = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts_with(line, "m=audio")) { in_audio = true; in_video = false; }
        else if (starts_with(line, "m=video")) { in_video = true; in_audio = false; }
        if (line == "a=mid:0" && in_audio) line = "a=mid:audio";
        else if (line == "a=mid:1" && in_video) line = "a=mid:video";
        else if (line == "a=mid:0" && !in_audio && !in_video) line = "a=mid:audio";
        if (!line.empty()) result += line + "\r\n";
    }

    std::vector<std::string> lines;
    std::istringstream iss2(result);
    while (std::getline(iss2, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }

    int m_idx = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (starts_with(lines[i], "m=")) { m_idx = (int)i; break; }
    }
    if (m_idx < 0) return result;

    bool has_ufrag = false, has_pwd = false, has_fp = false;
    for (int i = 0; i < m_idx; i++) {
        if (starts_with(lines[i], "a=ice-ufrag:")) has_ufrag = true;
        if (starts_with(lines[i], "a=ice-pwd:"))   has_pwd = true;
        if (starts_with(lines[i], "a=fingerprint:")) has_fp = true;
    }
    if (has_ufrag && has_pwd && has_fp) return result;

    std::string ufrag_line, pwd_line, fp_line;
    for (size_t i = (size_t)m_idx; i < lines.size(); i++) {
        if (!has_ufrag && ufrag_line.empty() && starts_with(lines[i], "a=ice-ufrag:"))
            ufrag_line = lines[i];
        if (!has_pwd && pwd_line.empty() && starts_with(lines[i], "a=ice-pwd:"))
            pwd_line = lines[i];
        if (!has_fp && fp_line.empty() && starts_with(lines[i], "a=fingerprint:"))
            fp_line = lines[i];
    }

    result.clear();
    for (int i = 0; i < m_idx; i++) result += lines[i] + "\r\n";
    if (!ufrag_line.empty()) result += ufrag_line + "\r\n";
    if (!pwd_line.empty())   result += pwd_line + "\r\n";
    if (!fp_line.empty())    result += fp_line + "\r\n";
    for (size_t i = (size_t)m_idx; i < lines.size(); i++) result += lines[i] + "\r\n";
    return result;
}

// ==================== WHIP 信令 ====================

static std::string g_whip_resource_url;

static void kickoff_old_stream(const UrlParts &u) {
    if (u.host.empty() || u.app.empty() || u.stream.empty()) return;
    std::string api_base = u.scheme + "://" + u.host;
    CurlResponse resp;
    CURL *curl = curl_easy_init();
    if (!curl) return;
    std::string clients_url = api_base + "/api/v1/clients/";
    curl_easy_setopt(curl, CURLOPT_URL, clients_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || resp.http_code != 200) return;

    std::string target_url = "/" + u.app + "/" + u.stream;
    size_t search_pos = 0;
    while (true) {
        auto id_pos = resp.body.find("\"id\":", search_pos);
        if (id_pos == std::string::npos) break;
        auto id_start = resp.body.find_first_of("0123456789", id_pos + 4);
        if (id_start == std::string::npos) break;
        auto id_end = resp.body.find_first_not_of("0123456789", id_start);
        std::string client_id = resp.body.substr(id_start, id_end - id_start);
        auto url_pos = resp.body.find("\"url\":", id_pos);
        auto next_id = resp.body.find("\"id\":", id_pos + 4);
        if (url_pos != std::string::npos && (next_id == std::string::npos || url_pos < next_id)) {
            auto quote1 = resp.body.find('"', url_pos + 5);
            auto quote2 = resp.body.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string client_url = resp.body.substr(quote1 + 1, quote2 - quote1 - 1);
                if (client_url.find(target_url) != std::string::npos) {
                    std::string kick_url = api_base + "/api/v1/clients/" + client_id;
                    printf("[清理] 踢掉旧 session: client=%s url=%s\n", client_id.c_str(), client_url.c_str());
                    http_delete(kick_url);
                }
            }
        }
        search_pos = (id_end != std::string::npos) ? id_end : resp.body.size();
    }
}

static bool signaling_whip(const std::string &url, const std::string &sdp_offer,
                           std::string &sdp_answer) {
    CurlResponse resp;
    std::string location;
    if (!http_post(url, "application/sdp", sdp_offer, resp, &location)) return false;
    if (resp.http_code != 200 && resp.http_code != 201) {
        fprintf(stderr, "[WHIP] HTTP %ld: %s\n", resp.http_code, resp.body.c_str());
        return false;
    }
    sdp_answer = resp.body;
    if (!location.empty()) {
        if (starts_with(location, "/")) {
            auto se = url.find("://");
            auto ps = url.find('/', se + 3);
            g_whip_resource_url = url.substr(0, ps) + location;
        } else {
            g_whip_resource_url = location;
        }
        printf("[WHIP] 资源 URL: %s\n", g_whip_resource_url.c_str());
    }
    return sdp_answer.find("m=") != std::string::npos;
}

static bool signaling_srs(const UrlParts &u, const std::string &sdp_offer,
                          std::string &sdp_answer) {
    if (u.app.empty() || u.stream.empty()) return false;
    std::string api_url = u.scheme + "://" + u.host + "/rtc/v1/publish/";
    if (!u.secret.empty()) api_url += "?secret=" + u.secret;
    std::string stream_url = "webrtc://" + u.host_only + "/" + u.app + "/" + u.stream;
    std::string body = "{\"api\":\"" + api_url + "\",\"clientip\":\"\","
        "\"streamurl\":\"" + stream_url + "\","
        "\"sdp\":\"" + json_escape(sdp_offer) + "\"}";
    CurlResponse resp;
    if (!http_post(api_url, "application/json", body, resp)) return false;
    if (resp.http_code != 200) return false;
    int code = -1;
    json_get_int(resp.body, "code", code);
    if (code != 0) return false;
    return json_get_string(resp.body, "sdp", sdp_answer) &&
           sdp_answer.find("m=") != std::string::npos;
}

#endif // ENABLE_WHIP

// ==================== 主程序 ====================

static void print_usage(const char *prog) {
    fprintf(stderr,
        "用法: %s [选项] <input> <output_url>\n\n"
        "input (输入源):\n"
        "  /path/to/video.mp4       本地视频文件 (默认循环播放)\n"
        "  /dev/video0              V4L2 摄像头\n"
        "  -              stdin rawvideo (需配合 --rawvideo --width N --height N)\n\n"
        "output_url (推流地址):\n"
        "  rtmp://server/app/stream           RTMP 推流\n"
        "  srt://server:port?streamid=...     SRT 推流\n"
        "  rtp://host:port                    RTP 推流 (自动输出 SDP)\n"
        "  rtsp://server:port/path            RTSP 推流\n"
#ifdef ENABLE_WHIP
        "  http://srs/rtc/v1/whip/?...        WHIP/WebRTC 推流\n"
#endif
        "\n"
        "选项:\n"
        "  --width  N     摄像头宽度 (默认 1280)\n"
        "  --height N     摄像头高度 (默认 720)\n"
        "  --fps    N     摄像头帧率 (默认 30)\n"
        "  --bitrate N    编码码率 bps (默认 2000000)\n"
        "  --loop         文件推流循环播放 (默认开启)\n"
        "  --no-loop      文件推流不循环\n"
        "  --rawvideo     从 stdin 读取 BGR24 原始帧 (input 必须为 \"-\")\n\n"
        "示例:\n"
        "  %s video.mp4 rtmp://server/live/test\n"
        "  %s video.mp4 \"srt://server:10080?streamid=live/test\"\n"
        "  %s /dev/video0 rtp://192.168.1.100:5004\n"
#ifdef ENABLE_WHIP
        "  %s video.mp4 \"http://srs:2022/rtc/v1/whip/?app=live&stream=test&secret=xxx\"\n"
#endif
        ,
        prog, prog, prog, prog
#ifdef ENABLE_WHIP
        , prog
#endif
    );
}

int main(int argc, char *argv[]) {
    // ========== 参数解析 ==========
    std::string input_str, url_str;
    int cam_width = 1280, cam_height = 720, cam_fps = 30;
    int bitrate = 2000000;
    bool loop_file = true;
    bool rawvideo_mode = false;  // --rawvideo: 从 stdin 读取 BGR24 原始帧

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--width" && i + 1 < args.size())  { cam_width = std::stoi(args[++i]); }
        else if (args[i] == "--height" && i + 1 < args.size()) { cam_height = std::stoi(args[++i]); }
        else if (args[i] == "--fps" && i + 1 < args.size())    { cam_fps = std::stoi(args[++i]); }
        else if (args[i] == "--bitrate" && i + 1 < args.size()){ bitrate = std::stoi(args[++i]); }
        else if (args[i] == "--loop")    { loop_file = true; }
        else if (args[i] == "--no-loop") { loop_file = false; }
        else if (args[i] == "--rawvideo") { rawvideo_mode = true; }
        else if (args[i] == "-h" || args[i] == "--help") { print_usage(argv[0]); return 0; }
        else if (input_str.empty()) input_str = args[i];
        else if (url_str.empty()) url_str = args[i];
    }

    if (input_str.empty() || url_str.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    InputType input_type = detect_input_type(input_str);
    OutputType output_type = detect_output_type(url_str);

    // rawvideo 模式：input 必须为 "-"（stdin），强制覆盖类型
    if (rawvideo_mode) {
        if (input_str != "-") {
            fprintf(stderr, "[错误] --rawvideo 模式下 input 必须为 \"-\"\n");
            return 1;
        }
        input_type = InputType::RawVideo;
    }

#ifndef ENABLE_WHIP
    if (output_type == OutputType::WHIP) {
        fprintf(stderr, "[错误] WHIP/WebRTC 未启用。请使用 cmake -DENABLE_WHIP=ON 重新编译。\n");
        fprintf(stderr, "支持的协议: rtmp://, srt://, rtp://, rtsp://\n");
        return 1;
    }
#endif

    const char *input_type_name[] = {"文件", "摄像头", "网络流", "RawVideo/stdin"};
    printf("=== stream_push 多协议推流 ===\n");
    printf("输入: %s (%s)\n", input_str.c_str(), input_type_name[(int)input_type]);
    printf("输出: %s (%s)\n", url_str.c_str(), output_type_name(output_type));

    // ========== rawvideo/stdin 模式（完整独立分支，末尾 return）==========
    if (input_type == InputType::RawVideo) {
        fprintf(stderr, "[RawVideo] stdin BGR24 %dx%d @ %d fps, bitrate=%d\n",
                cam_width, cam_height, cam_fps, bitrate);

        // 1. 初始化 H264 编码器（BGR24 → YUV420P → H264）
        H264Encoder encoder;
        if (!encoder.init(cam_width, cam_height, cam_fps, AV_PIX_FMT_BGR24, bitrate))
            return 1;

        // 2. 提取 SPS/PPS
        std::vector<uint8_t> sps_pps;
        if (encoder.enc_ctx->extradata && encoder.enc_ctx->extradata_size > 0)
            sps_pps = extract_sps_pps_from_extradata(
                encoder.enc_ctx->extradata, encoder.enc_ctx->extradata_size);
        if (!sps_pps.empty())
            printf("SPS/PPS: %zu bytes\n", sps_pps.size());

        // 3. 打开输出（FFmpeg / WHIP 均支持）
        FFmpegOutput ffmpeg_out;
        bool use_ffmpeg_output_rv = (output_type != OutputType::WHIP);

        if (use_ffmpeg_output_rv) {
            AVCodecParameters *enc_codecpar = avcodec_parameters_alloc();
            avcodec_parameters_from_context(enc_codecpar, encoder.enc_ctx);
            AVRational enc_tb = encoder.enc_ctx->time_base;
            if (!ffmpeg_out.open(url_str, output_type, enc_codecpar, enc_tb,
                                 cam_width, cam_height, nullptr, {0, 1})) {
                avcodec_parameters_free(&enc_codecpar);
                encoder.cleanup();
                return 1;
            }
            avcodec_parameters_free(&enc_codecpar);
            printf("连接成功，开始推流...\n");
        }

#ifdef ENABLE_WHIP
        // 4. WHIP 信令（与原有逻辑完全对称）
        std::shared_ptr<rtc::PeerConnection> rv_pc;
        std::shared_ptr<rtc::Track> rv_videoTrack, rv_audioTrack;
        std::atomic<bool> rv_pc_connected{false};
        std::atomic<bool> rv_pli_requested{false};
        const std::byte rv_opusSilence[] = {
            std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE}};
        double rv_last_audio = 0.0;

        if (output_type == OutputType::WHIP) {
            UrlParts url_parts = parse_url(url_str);
            rtc::SetThreadPoolSize(2);

            rtc::Configuration rtc_config;
            rtc_config.disableAutoNegotiation = true;
            rv_pc = std::make_shared<rtc::PeerConnection>(rtc_config);

            std::mutex mtx;
            std::condition_variable cv;
            bool gathering_done = false;

            rv_pc->onStateChange([&](rtc::PeerConnection::State state) {
                const char *names[] = {"New","Connecting","Connected",
                                       "Disconnected","Failed","Closed"};
                printf("[WebRTC] 状态: %s\n", names[(int)state]);
                if (state == rtc::PeerConnection::State::Connected)
                    rv_pc_connected = true;
                else if (state >= rtc::PeerConnection::State::Disconnected) {
                    rv_pc_connected = false;
                    if (state >= rtc::PeerConnection::State::Failed)
                        g_running = false;
                }
            });
            rv_pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    std::lock_guard<std::mutex> lk(mtx);
                    gathering_done = true;
                    cv.notify_one();
                }
            });

            const std::string cname = "stream-push";

            rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendOnly);
            audioMedia.addOpusCodec(111);
            audioMedia.addSSRC(2, cname, "stream", "audio");
            rv_audioTrack = rv_pc->addTrack(audioMedia);
            auto audioRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(2, cname, 111, 48000);
            audioRtpCfg->startTimestamp = 0;
            audioRtpCfg->sequenceNumber = 0;
            auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpCfg);
            auto audioSr = std::make_shared<rtc::RtcpSrReporter>(audioRtpCfg);
            audioPacketizer->addToChain(audioSr);
            rv_audioTrack->setMediaHandler(audioPacketizer);

            rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendOnly);
            videoMedia.addH264Codec(96,
                "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c01f");
            videoMedia.addSSRC(1, cname, "stream", "video");
            rv_videoTrack = rv_pc->addTrack(videoMedia);
            auto videoRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(1, cname, 96, 90000);
            videoRtpCfg->startTimestamp = 0;
            videoRtpCfg->sequenceNumber = 0;
            auto videoPacketizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, videoRtpCfg);
            auto videoSr = std::make_shared<rtc::RtcpSrReporter>(videoRtpCfg);
            videoPacketizer->addToChain(videoSr);
            auto nackResp = std::make_shared<rtc::RtcpNackResponder>();
            videoSr->addToChain(nackResp);
            auto pliHandler = std::make_shared<rtc::PliHandler>(
                [&rv_pli_requested]() { rv_pli_requested = true; });
            nackResp->addToChain(pliHandler);
            rv_videoTrack->setMediaHandler(videoPacketizer);

            rv_pc->setLocalDescription(rtc::Description::Type::Offer);
            {
                std::unique_lock<std::mutex> lk(mtx);
                if (!cv.wait_for(lk, std::chrono::seconds(10),
                                 [&] { return gathering_done; })) {
                    fprintf(stderr, "[WebRTC] ICE gathering 超时\n");
                    encoder.cleanup();
                    return 1;
                }
            }

            auto local_desc = rv_pc->localDescription();
            if (!local_desc) {
                fprintf(stderr, "[WebRTC] 无法获取本地 SDP\n");
                encoder.cleanup();
                return 1;
            }

            std::string sdp_offer = prepare_offer(local_desc->generateSdp());
            std::string sdp_answer_raw;
            bool signaling_ok = signaling_whip(url_str.c_str(), sdp_offer, sdp_answer_raw);
            if (!signaling_ok)
                signaling_ok = signaling_srs(url_parts, sdp_offer, sdp_answer_raw);

            if (!signaling_ok) {
                printf("[信令] 尝试踢掉旧 session...\n");
                kickoff_old_stream(url_parts);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                for (int attempt = 1; attempt <= 6 && g_running && !signaling_ok; attempt++) {
                    printf("[信令] 重试 (%d/6)...\n", attempt);
                    signaling_ok = signaling_whip(url_str.c_str(), sdp_offer, sdp_answer_raw);
                    if (!signaling_ok)
                        signaling_ok = signaling_srs(url_parts, sdp_offer, sdp_answer_raw);
                    if (!signaling_ok)
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
            if (!signaling_ok) {
                fprintf(stderr, "[信令] 失败\n");
                encoder.cleanup();
                return 1;
            }

            std::string sdp_answer = prepare_answer(sdp_answer_raw);
            try {
                rv_pc->setRemoteDescription(
                    rtc::Description(sdp_answer, rtc::Description::Type::Answer));
            } catch (const std::exception &e) {
                fprintf(stderr, "[WebRTC] setRemoteDescription 失败: %s\n", e.what());
                encoder.cleanup();
                return 1;
            }

            for (int i = 0; i < 100 && g_running && !rv_pc_connected; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!rv_pc_connected) {
                fprintf(stderr, "[WebRTC] 连接超时\n");
                encoder.cleanup();
                return 1;
            }
            printf("连接成功，开始推流...\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
#endif // ENABLE_WHIP

        // 5. 推流循环
        size_t frame_bytes = (size_t)cam_width * cam_height * 3;  // BGR24
        std::vector<uint8_t> bgr_buf(frame_bytes);

        AVFrame *raw_frame = av_frame_alloc();
        raw_frame->format = AV_PIX_FMT_BGR24;
        raw_frame->width  = cam_width;
        raw_frame->height = cam_height;
        av_frame_get_buffer(raw_frame, 1);

        int rv_frame_count = 0;
        int64_t frame_interval_us = 1000000LL / cam_fps;
        int64_t next_frame_time   = av_gettime();

        auto is_connected_rv = [&]() -> bool {
            if (use_ffmpeg_output_rv) return ffmpeg_out.opened;
#ifdef ENABLE_WHIP
            if (output_type == OutputType::WHIP) return rv_pc_connected.load();
#endif
            return false;
        };

        while (g_running && is_connected_rv()) {
#ifdef ENABLE_WHIP
            // PLI：请求关键帧
            if (output_type == OutputType::WHIP && rv_pli_requested.exchange(false))
                encoder.request_keyframe();
#endif
            // 从 stdin 读取一完整 BGR24 帧
            size_t total_read = 0;
            while (total_read < frame_bytes && g_running) {
                size_t n = fread(bgr_buf.data() + total_read, 1,
                                 frame_bytes - total_read, stdin);
                if (n == 0) {
                    if (feof(stdin))
                        fprintf(stderr, "\n[RawVideo] stdin EOF\n");
                    else
                        fprintf(stderr, "\n[RawVideo] stdin 读取错误\n");
                    g_running = false;
                    break;
                }
                total_read += n;
            }
            if (!g_running) break;

            // 填入 AVFrame（处理 linesize 内存对齐）
            for (int row = 0; row < cam_height; row++) {
                memcpy(raw_frame->data[0] + row * raw_frame->linesize[0],
                       bgr_buf.data() + row * cam_width * 3,
                       cam_width * 3);
            }

            // 编码
            AVPacket *enc_pkt = av_packet_alloc();
            if (encoder.encode(raw_frame, enc_pkt)) {
                double dts_sec = enc_pkt->pts * av_q2d(encoder.enc_ctx->time_base);
                bool key = (enc_pkt->flags & AV_PKT_FLAG_KEY) != 0;

                if (use_ffmpeg_output_rv) {
                    ffmpeg_out.write_video(enc_pkt, encoder.enc_ctx->time_base);
                    ffmpeg_out.generate_silence_up_to(dts_sec);
                }
#ifdef ENABLE_WHIP
                else if (output_type == OutputType::WHIP && rv_videoTrack) {
                    auto frame_data = build_frame_data(
                        enc_pkt->data, enc_pkt->size, key, sps_pps);
                    try {
                        rtc::FrameInfo fi{std::chrono::duration<double>{dts_sec}};
                        fi.isKeyFrame = key;
                        rv_videoTrack->sendFrame(std::move(frame_data), fi);
                        while (rv_last_audio < dts_sec) {
                            rtc::binary ad(rv_opusSilence,
                                           rv_opusSilence + sizeof(rv_opusSilence));
                            rv_audioTrack->sendFrame(
                                std::move(ad),
                                rtc::FrameInfo{std::chrono::duration<double>{rv_last_audio}});
                            rv_last_audio += 0.020;
                        }
                    } catch (const std::exception &e) {
                        fprintf(stderr, "\n[发送] %s\n", e.what());
                        g_running = false;
                    }
                }
#endif
                rv_frame_count++;
            }
            av_packet_free(&enc_pkt);
            encoder.yuv_frame->pict_type = AV_PICTURE_TYPE_NONE;

            if (rv_frame_count > 0 && rv_frame_count % cam_fps == 0) {
                printf("\r推流中: %d 帧 (%.1f 秒)",
                       rv_frame_count, (double)rv_frame_count / cam_fps);
                fflush(stdout);
            }

            // 帧率限速
            next_frame_time += frame_interval_us;
            int64_t now = av_gettime();
            if (next_frame_time > now) {
                int64_t wait = next_frame_time - now;
                while (wait > 0 && g_running) {
                    int64_t chunk = std::min(wait, (int64_t)50000);
                    av_usleep((unsigned)chunk);
                    wait -= chunk;
                }
            } else {
                next_frame_time = av_gettime();  // 落后时重置，避免追帧
            }
        }

        printf("\n推流结束，共发送 %d 帧\n", rv_frame_count);

        // 清理
        av_frame_free(&raw_frame);
        encoder.cleanup();
        if (use_ffmpeg_output_rv) ffmpeg_out.close();
#ifdef ENABLE_WHIP
        if (output_type == OutputType::WHIP) {
            if (!g_whip_resource_url.empty()) http_delete(g_whip_resource_url);
            if (rv_videoTrack) rv_videoTrack->close();
            if (rv_audioTrack) rv_audioTrack->close();
            if (rv_pc) rv_pc->close();
        }
#endif
        printf("资源已释放。\n");
        return 0;
    }
    // ========== rawvideo 模式结束 ==========

    // ========== 1. 打开输入源 ==========
    InputSource src;
    if (!open_input(input_str, input_type, src, cam_width, cam_height, cam_fps))
        return 1;

    AVStream *in_stream = src.fmt_ctx->streams[src.video_idx];
    AVCodecParameters *codecpar = in_stream->codecpar;
    double fps = av_q2d(in_stream->avg_frame_rate);
    if (fps <= 0) fps = (input_type == InputType::Camera) ? cam_fps : 25.0;

    bool need_encode = (codecpar->codec_id != AV_CODEC_ID_H264);
    bool need_bsf = (!need_encode && !src.is_annex_b);

    printf("视频: %dx%d @ %.2f fps, codec=%s\n",
           codecpar->width, codecpar->height, fps, avcodec_get_name(codecpar->codec_id));

    // ========== 2. 编码器 (非 H264 输入时需要) ==========
    H264Encoder encoder;
    AVCodecContext *dec_ctx = nullptr;
    AVFrame *dec_frame = nullptr;

    if (need_encode) {
        const AVCodec *dec = avcodec_find_decoder(codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "[解码] 找不到解码器: %s\n", avcodec_get_name(codecpar->codec_id));
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
        dec_ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(dec_ctx, codecpar);
        if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
            fprintf(stderr, "[解码] 打开解码器失败\n");
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
        dec_frame = av_frame_alloc();

        if (!encoder.init(codecpar->width, codecpar->height, (int)fps,
                          (AVPixelFormat)codecpar->format, bitrate)) {
            avcodec_free_context(&dec_ctx);
            av_frame_free(&dec_frame);
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
    }

    // ========== 3. BSF (MP4: AVCC -> Annex-B) ==========
    AVBSFContext *bsf_ctx = nullptr;
    if (need_bsf) {
        const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (!bsf) {
            fprintf(stderr, "[BSF] 找不到 h264_mp4toannexb\n");
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
        av_bsf_alloc(bsf, &bsf_ctx);
        avcodec_parameters_copy(bsf_ctx->par_in, codecpar);
        bsf_ctx->time_base_in = in_stream->time_base;
        if (av_bsf_init(bsf_ctx) < 0) {
            fprintf(stderr, "[BSF] 初始化失败\n");
            av_bsf_free(&bsf_ctx);
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
    }

    // ========== 4. 提取 SPS/PPS ==========
    std::vector<uint8_t> sps_pps;
    if (!need_encode && codecpar->extradata && codecpar->extradata_size > 0) {
        sps_pps = extract_sps_pps_from_extradata(codecpar->extradata, codecpar->extradata_size);
    } else if (need_encode && encoder.enc_ctx->extradata) {
        sps_pps = extract_sps_pps_from_extradata(encoder.enc_ctx->extradata,
                                                   encoder.enc_ctx->extradata_size);
    }
    if (!sps_pps.empty())
        printf("SPS/PPS: %zu bytes\n", sps_pps.size());

    // ========== 5. 打开输出 ==========

    // --- FFmpeg 输出路径 (RTMP/SRT/RTP/RTSP) ---
    FFmpegOutput ffmpeg_out;
    bool use_ffmpeg_output = (output_type != OutputType::WHIP);

    if (use_ffmpeg_output) {
        // 对于 FFmpeg 输出，使用编码后的 codecpar
        AVCodecParameters *out_codecpar = codecpar;
        AVCodecParameters *enc_codecpar = nullptr;
        if (need_encode && encoder.enc_ctx) {
            enc_codecpar = avcodec_parameters_alloc();
            avcodec_parameters_from_context(enc_codecpar, encoder.enc_ctx);
            out_codecpar = enc_codecpar;
        }

        // 音频参数 (如果源有音频流)
        AVCodecParameters *audio_codecpar = nullptr;
        AVRational audio_tb = {0, 1};
        if (src.audio_idx >= 0) {
            audio_codecpar = src.fmt_ctx->streams[src.audio_idx]->codecpar;
            audio_tb = src.fmt_ctx->streams[src.audio_idx]->time_base;
        }

        if (!ffmpeg_out.open(url_str, output_type, out_codecpar, in_stream->time_base,
                             codecpar->width, codecpar->height,
                             audio_codecpar, audio_tb)) {
            if (enc_codecpar) avcodec_parameters_free(&enc_codecpar);
            encoder.cleanup();
            if (dec_ctx) avcodec_free_context(&dec_ctx);
            if (dec_frame) av_frame_free(&dec_frame);
            if (bsf_ctx) av_bsf_free(&bsf_ctx);
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
        if (enc_codecpar) avcodec_parameters_free(&enc_codecpar);
        printf("连接成功，开始推流...\n");
    }

    // --- WHIP/WebRTC 输出路径 ---
#ifdef ENABLE_WHIP
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack, audioTrack;
    std::atomic<bool> pc_connected{false};
    std::atomic<bool> pli_requested{false};
    const std::byte opusSilence[] = { std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE} };

    if (output_type == OutputType::WHIP) {
        UrlParts url_parts = parse_url(url_str);
        rtc::SetThreadPoolSize(2);

        rtc::Configuration rtc_config;
        rtc_config.disableAutoNegotiation = true;
        pc = std::make_shared<rtc::PeerConnection>(rtc_config);

        std::mutex mtx;
        std::condition_variable cv;
        bool gathering_done = false;

        pc->onStateChange([&](rtc::PeerConnection::State state) {
            const char *names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            printf("[WebRTC] 状态: %s\n", names[(int)state]);
            if (state == rtc::PeerConnection::State::Connected) pc_connected = true;
            else if (state >= rtc::PeerConnection::State::Disconnected) {
                pc_connected = false;
                if (state >= rtc::PeerConnection::State::Failed) g_running = false;
            }
        });
        pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                std::lock_guard<std::mutex> lk(mtx);
                gathering_done = true;
                cv.notify_one();
            }
        });

        const std::string cname = "stream-push";

        rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendOnly);
        audioMedia.addOpusCodec(111);
        audioMedia.addSSRC(2, cname, "stream", "audio");
        audioTrack = pc->addTrack(audioMedia);

        auto audioRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(2, cname, 111, 48000);
        audioRtpCfg->startTimestamp = 0;
        audioRtpCfg->sequenceNumber = 0;
        auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpCfg);
        auto audioSr = std::make_shared<rtc::RtcpSrReporter>(audioRtpCfg);
        audioPacketizer->addToChain(audioSr);
        audioTrack->setMediaHandler(audioPacketizer);

        rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendOnly);
        videoMedia.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c01f");
        videoMedia.addSSRC(1, cname, "stream", "video");
        videoTrack = pc->addTrack(videoMedia);

        auto videoRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(1, cname, 96, 90000);
        videoRtpCfg->startTimestamp = 0;
        videoRtpCfg->sequenceNumber = 0;
        auto videoPacketizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, videoRtpCfg);
        auto videoSr = std::make_shared<rtc::RtcpSrReporter>(videoRtpCfg);
        videoPacketizer->addToChain(videoSr);
        auto nackResp = std::make_shared<rtc::RtcpNackResponder>();
        videoSr->addToChain(nackResp);
        auto pliHandler = std::make_shared<rtc::PliHandler>([&pli_requested]() {
            pli_requested = true;
        });
        nackResp->addToChain(pliHandler);
        videoTrack->setMediaHandler(videoPacketizer);

        // 信令
        pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lk(mtx);
            if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return gathering_done; })) {
                fprintf(stderr, "[WebRTC] ICE gathering 超时\n");
                return 1;
            }
        }

        auto local_desc = pc->localDescription();
        if (!local_desc) { fprintf(stderr, "[WebRTC] 无法获取本地 SDP\n"); return 1; }

        std::string sdp_offer = prepare_offer(local_desc->generateSdp());
        std::string sdp_answer_raw;
        bool signaling_ok = false;

        signaling_ok = signaling_whip(url_str.c_str(), sdp_offer, sdp_answer_raw);
        if (!signaling_ok)
            signaling_ok = signaling_srs(url_parts, sdp_offer, sdp_answer_raw);

        if (!signaling_ok) {
            printf("[信令] 尝试踢掉旧 session...\n");
            kickoff_old_stream(url_parts);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            for (int attempt = 1; attempt <= 6 && g_running && !signaling_ok; attempt++) {
                printf("[信令] 重试 (%d/6)...\n", attempt);
                signaling_ok = signaling_whip(url_str.c_str(), sdp_offer, sdp_answer_raw);
                if (!signaling_ok)
                    signaling_ok = signaling_srs(url_parts, sdp_offer, sdp_answer_raw);
                if (!signaling_ok)
                    std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        if (!signaling_ok) { fprintf(stderr, "[信令] 失败\n"); return 1; }

        std::string sdp_answer = prepare_answer(sdp_answer_raw);
        try {
            pc->setRemoteDescription(rtc::Description(sdp_answer, rtc::Description::Type::Answer));
        } catch (const std::exception &e) {
            fprintf(stderr, "[WebRTC] setRemoteDescription 失败: %s\n", e.what());
            return 1;
        }

        for (int i = 0; i < 100 && g_running && !pc_connected; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!pc_connected) { fprintf(stderr, "[WebRTC] 连接超时\n"); return 1; }
        printf("连接成功，开始推流...\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif

    // ========== 6. 推流循环 ==========
    AVPacket *pkt = av_packet_alloc();
    int frame_count = 0;
    int64_t wall_start = av_gettime();
    double dts_offset = 0.0, last_dts = 0.0;
    bool force_keyframe = false;

#ifdef ENABLE_WHIP
    double last_audio = 0.0;
#endif

    auto is_connected = [&]() -> bool {
        if (use_ffmpeg_output) return ffmpeg_out.opened;
#ifdef ENABLE_WHIP
        if (output_type == OutputType::WHIP) return pc_connected.load();
#endif
        return false;
    };

    while (g_running && is_connected()) {
#ifdef ENABLE_WHIP
        // PLI 处理 (仅 WHIP)
        if (output_type == OutputType::WHIP && pli_requested.exchange(false)) {
            if (need_encode) {
                encoder.request_keyframe();
            } else if (!src.is_camera) {
                dts_offset = last_dts + 1.0 / fps;
                force_keyframe = true;
                av_seek_frame(src.fmt_ctx, src.video_idx, 0, AVSEEK_FLAG_BACKWARD);
                if (bsf_ctx) av_bsf_flush(bsf_ctx);
                wall_start = av_gettime();
            }
        }
#endif

        int ret = av_read_frame(src.fmt_ctx, pkt);
        if (ret < 0) {
            if (src.is_camera || input_type == InputType::Stream) {
                fprintf(stderr, "\n[输入] 断开\n");
                break;
            }
            if (!loop_file) break;
            dts_offset = last_dts + 1.0 / fps;
            av_seek_frame(src.fmt_ctx, src.video_idx, 0, AVSEEK_FLAG_BACKWARD);
            if (bsf_ctx) av_bsf_flush(bsf_ctx);
            wall_start = av_gettime();
            continue;
        }
        if (pkt->stream_index != src.video_idx) {
            // 转发源音频包到 FFmpeg 输出
            if (use_ffmpeg_output && pkt->stream_index == src.audio_idx && ffmpeg_out.has_source_audio) {
                ffmpeg_out.write_audio(pkt, src.fmt_ctx->streams[src.audio_idx]->time_base);
            }
            av_packet_unref(pkt);
            continue;
        }

        // 节奏控制 (文件和网络流需要，摄像头自带节奏)
        if (!src.is_camera && !force_keyframe) {
            int64_t ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (ts != AV_NOPTS_VALUE) {
                int64_t ts_us = av_rescale_q(ts, in_stream->time_base, AVRational{1, AV_TIME_BASE});
                int64_t wait = ts_us - (av_gettime() - wall_start);
                while (wait > 0 && g_running) {
                    int64_t chunk = std::min(wait, (int64_t)50000);
                    av_usleep((unsigned)chunk);
                    wait -= chunk;
                }
            }
        }

        // ---- 路径A: 需要编码 (非 H264 输入) ----
        if (need_encode) {
            ret = avcodec_send_packet(dec_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;

            while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
                AVPacket *enc_pkt = av_packet_alloc();
                if (encoder.encode(dec_frame, enc_pkt)) {
                    double dts_sec = enc_pkt->pts * av_q2d(encoder.enc_ctx->time_base) + dts_offset;
                    last_dts = dts_sec;
                    bool key = (enc_pkt->flags & AV_PKT_FLAG_KEY) != 0;

                    if (use_ffmpeg_output) {
                        // FFmpeg 输出
                        ffmpeg_out.write_video(enc_pkt, encoder.enc_ctx->time_base);
                        ffmpeg_out.generate_silence_up_to(dts_sec);
                    }
#ifdef ENABLE_WHIP
                    else if (output_type == OutputType::WHIP && videoTrack) {
                        auto frame_data = build_frame_data(enc_pkt->data, enc_pkt->size, key, sps_pps);
                        try {
                            rtc::FrameInfo fi{std::chrono::duration<double>{dts_sec}};
                            fi.isKeyFrame = key;
                            videoTrack->sendFrame(std::move(frame_data), fi);
                            while (last_audio < dts_sec) {
                                rtc::binary ad(opusSilence, opusSilence + sizeof(opusSilence));
                                audioTrack->sendFrame(std::move(ad),
                                    rtc::FrameInfo{std::chrono::duration<double>{last_audio}});
                                last_audio += 0.020;
                            }
                        } catch (const std::exception &e) {
                            fprintf(stderr, "\n[发送] %s\n", e.what());
                            g_running = false;
                        }
                    }
#endif
                    frame_count++;
                }
                av_packet_free(&enc_pkt);
                encoder.yuv_frame->pict_type = AV_PICTURE_TYPE_NONE;
            }
        }
        // ---- 路径B: 直推 H264 ----
        else {
            auto process_h264_packet = [&](uint8_t *data, int size, int64_t dts_val,
                                           int64_t pts_val, int flags, AVRational tb) {
                double dts_sec;
                if (dts_val != AV_NOPTS_VALUE)
                    dts_sec = dts_val * av_q2d(tb) + dts_offset;
                else
                    dts_sec = last_dts + 1.0 / fps;
                last_dts = dts_sec;
                bool key = (flags & AV_PKT_FLAG_KEY) != 0;

                if (force_keyframe && !key) return;
                if (force_keyframe && key) force_keyframe = false;

                if (use_ffmpeg_output) {
                    // FFmpeg 输出: 构建临时 pkt 写入
                    AVPacket *out_pkt = av_packet_alloc();
                    out_pkt->data = data;
                    out_pkt->size = size;
                    out_pkt->dts = dts_val;
                    out_pkt->pts = pts_val;
                    out_pkt->flags = flags;
                    ffmpeg_out.write_video(out_pkt, tb);
                    out_pkt->data = nullptr; out_pkt->size = 0;
                    av_packet_free(&out_pkt);
                    ffmpeg_out.generate_silence_up_to(dts_sec);
                }
#ifdef ENABLE_WHIP
                else if (output_type == OutputType::WHIP && videoTrack) {
                    auto frame_data = build_frame_data(data, size, key, sps_pps);
                    try {
                        rtc::FrameInfo fi{std::chrono::duration<double>{dts_sec}};
                        fi.isKeyFrame = key;
                        videoTrack->sendFrame(std::move(frame_data), fi);
                        while (last_audio < dts_sec) {
                            rtc::binary ad(opusSilence, opusSilence + sizeof(opusSilence));
                            audioTrack->sendFrame(std::move(ad),
                                rtc::FrameInfo{std::chrono::duration<double>{last_audio}});
                            last_audio += 0.020;
                        }
                    } catch (const std::exception &e) {
                        fprintf(stderr, "\n[发送] %s\n", e.what());
                        g_running = false;
                    }
                }
#endif
                frame_count++;
            };

            if (need_bsf) {
                ret = av_bsf_send_packet(bsf_ctx, pkt);
                if (ret < 0) { av_packet_unref(pkt); continue; }
                while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                    if (!g_running || !is_connected()) { av_packet_unref(pkt); break; }
                    process_h264_packet(pkt->data, pkt->size, pkt->dts, pkt->pts,
                                        pkt->flags, bsf_ctx->time_base_out);
                    av_packet_unref(pkt);
                }
            } else {
                process_h264_packet(pkt->data, pkt->size, pkt->dts, pkt->pts,
                                    pkt->flags, in_stream->time_base);
                av_packet_unref(pkt);
            }
        }

        if (frame_count > 0 && frame_count % std::max(1, (int)fps) == 0) {
            printf("\r推流中: %d 帧 (%.1f 秒)", frame_count, (double)frame_count / fps);
            fflush(stdout);
        }
    }

    printf("\n推流结束，共发送 %d 帧\n", frame_count);

    // ========== 7. 清理 ==========
    if (use_ffmpeg_output) {
        ffmpeg_out.close();
    }

#ifdef ENABLE_WHIP
    if (output_type == OutputType::WHIP) {
        if (!g_whip_resource_url.empty()) {
            printf("[WHIP] DELETE %s\n", g_whip_resource_url.c_str());
            http_delete(g_whip_resource_url);
        }
        if (videoTrack) videoTrack->close();
        if (audioTrack) audioTrack->close();
        if (pc) pc->close();
    }
#endif

    av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (dec_frame) av_frame_free(&dec_frame);
    encoder.cleanup();
    avformat_close_input(&src.fmt_ctx);

    printf("资源已释放。\n");
    return 0;
}
