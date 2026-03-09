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

#include <curl/curl.h>
#include <rtc/rtc.hpp>

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

// ==================== 全局状态 ====================

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

// ==================== 工具函数 ====================

static void str_replace_all(std::string &s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ==================== HTTP (CURL) ====================

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

    // 抓取 Location header (WHIP 返回资源 URL)
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

// ==================== JSON 工具 ====================

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

// ==================== URL 解析 ====================

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

// ==================== SDP 处理 ====================

static std::string prepare_offer(const std::string &sdp) {
    std::string result;
    std::istringstream iss(sdp);
    std::string line;
    int audio_mid = -1, video_mid = -1;
    int mid_counter = 0;

    // 第一遍: 确定 mid 映射
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts_with(line, "m=audio")) audio_mid = mid_counter++;
        else if (starts_with(line, "m=video")) video_mid = mid_counter++;
    }

    // 第二遍: 重写
    iss.clear();
    iss.str(sdp);
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // 跳过 SRS 不认的属性
        if (starts_with(line, "a=group:LS")) continue;
        if (starts_with(line, "a=ice-options:")) continue;
        if (starts_with(line, "a=end-of-candidates")) continue;

        // m=audio: 保留 Opus (PT 111)
        if (starts_with(line, "m=audio "))
            line = "m=audio 9 UDP/TLS/RTP/SAVPF 111";
        // m=video: 保留 H264 (PT 96)
        else if (starts_with(line, "m=video "))
            line = "m=video 9 UDP/TLS/RTP/SAVPF 96";
        // c= 行
        else if (starts_with(line, "c=IN IP4"))
            line = "c=IN IP4 0.0.0.0";
        // setup
        else if (line == "a=setup:actpass")
            line = "a=setup:active";

        result += line + "\r\n";
    }

    // mid: libdatachannel 用 "audio"/"video"，SRS 要数字
    str_replace_all(result, "BUNDLE audio video", "BUNDLE 0 1");
    str_replace_all(result, "BUNDLE audio", "BUNDLE 0");
    str_replace_all(result, "BUNDLE video", "BUNDLE 0");
    str_replace_all(result, "a=mid:audio", "a=mid:0");
    str_replace_all(result, "a=mid:video", "a=mid:1");
    return result;
}

static std::string prepare_answer(const std::string &sdp) {
    std::string s = sdp;
    // SRS 返回数字 mid，libdatachannel 需要 "audio"/"video"
    str_replace_all(s, "BUNDLE 0 1", "BUNDLE audio video");
    str_replace_all(s, "BUNDLE 0", "BUNDLE audio");
    str_replace_all(s, "BUNDLE 1", "BUNDLE video");

    // 逐行替换 mid（需要区分 audio/video section）
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
        else if (line == "a=mid:0" && !in_audio && !in_video) line = "a=mid:audio";  // fallback

        if (!line.empty()) result += line + "\r\n";
    }

    // 提升 ice/fingerprint 到 session 级别
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

// ==================== 信令 ====================

// WHIP 资源 URL (用于 DELETE 释放 session)
static std::string g_whip_resource_url;

/// 通过 SRS API 踢掉指定 stream 的旧 session
static void kickoff_old_stream(const UrlParts &u) {
    if (u.host.empty() || u.app.empty() || u.stream.empty()) return;

    // SRS API: GET /api/v1/clients/ 列出所有客户端
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

    // 在 JSON 中找到匹配 stream 的 client id 并踢掉
    std::string target_url = "/" + u.app + "/" + u.stream;
    size_t search_pos = 0;
    while (true) {
        // 找 "id": 数字
        auto id_pos = resp.body.find("\"id\":", search_pos);
        if (id_pos == std::string::npos) break;
        auto id_start = resp.body.find_first_of("0123456789", id_pos + 4);
        if (id_start == std::string::npos) break;
        auto id_end = resp.body.find_first_not_of("0123456789", id_start);
        std::string client_id = resp.body.substr(id_start, id_end - id_start);

        // 找这个 client 块中的 url 字段
        auto url_pos = resp.body.find("\"url\":", id_pos);
        auto next_id = resp.body.find("\"id\":", id_pos + 4);
        if (url_pos != std::string::npos && (next_id == std::string::npos || url_pos < next_id)) {
            auto quote1 = resp.body.find('"', url_pos + 5);
            auto quote2 = resp.body.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string client_url = resp.body.substr(quote1 + 1, quote2 - quote1 - 1);
                if (client_url.find(target_url) != std::string::npos) {
                    // DELETE /api/v1/clients/{id}
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
    // 保存 WHIP 资源 URL 用于退出时 DELETE
    if (!location.empty()) {
        if (starts_with(location, "/")) {
            // 相对路径，拼接 base URL
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

// ==================== 输入源检测 ====================

enum class InputType { File, Camera, Stream };

static InputType detect_input_type(const std::string &input) {
    if (starts_with(input, "/dev/video")) return InputType::Camera;
    if (starts_with(input, "rtsp://") || starts_with(input, "rtmp://") ||
        starts_with(input, "http://") || starts_with(input, "https://"))
        return InputType::Stream;
    return InputType::File;
}

// ==================== 输入源打开 ====================

struct InputSource {
    AVFormatContext *fmt_ctx = nullptr;
    int video_idx = -1;
    bool is_camera = false;
    bool is_annex_b = false;  // 摄像头 H264 已经是 Annex-B 格式
};

static bool open_input(const std::string &input, InputType type, InputSource &src,
                       int cam_width = 1280, int cam_height = 720, int cam_fps = 30) {
    avdevice_register_all();

    AVDictionary *opts = nullptr;
    AVInputFormat *ifmt = nullptr;

    if (type == InputType::Camera) {
        src.is_camera = true;
        ifmt = (AVInputFormat *)av_find_input_format("v4l2");
        if (!ifmt) {
            fprintf(stderr, "[输入] 未找到 v4l2 输入格式，确认 FFmpeg 编译时启用了 --enable-libv4l2\n");
            return false;
        }
        char size_buf[32], fps_buf[16];
        snprintf(size_buf, sizeof(size_buf), "%dx%d", cam_width, cam_height);
        snprintf(fps_buf, sizeof(fps_buf), "%d", cam_fps);
        av_dict_set(&opts, "video_size", size_buf, 0);
        av_dict_set(&opts, "framerate", fps_buf, 0);
        // 优先请求 H264 (RK 系列 USB 摄像头通常支持)
        av_dict_set(&opts, "input_format", "h264", 0);
    } else if (type == InputType::Stream) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
    }

    int ret = avformat_open_input(&src.fmt_ctx, input.c_str(), ifmt, &opts);
    av_dict_free(&opts);

    // 摄像头: 如果 H264 打开失败，回退到 YUYV/MJPEG
    if (ret < 0 && type == InputType::Camera) {
        fprintf(stderr, "[输入] 摄像头不支持 H264 输出，回退到原始格式\n");
        opts = nullptr;
        char size_buf[32], fps_buf[16];
        snprintf(size_buf, sizeof(size_buf), "%dx%d", cam_width, cam_height);
        snprintf(fps_buf, sizeof(fps_buf), "%d", cam_fps);
        av_dict_set(&opts, "video_size", size_buf, 0);
        av_dict_set(&opts, "framerate", fps_buf, 0);
        ret = avformat_open_input(&src.fmt_ctx, input.c_str(), ifmt, &opts);
        av_dict_free(&opts);
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
        if (src.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            src.video_idx = (int)i;
            break;
        }
    }
    if (src.video_idx < 0) {
        fprintf(stderr, "[输入] 未找到视频流\n");
        avformat_close_input(&src.fmt_ctx);
        return false;
    }

    AVCodecParameters *cp = src.fmt_ctx->streams[src.video_idx]->codecpar;
    if (cp->codec_id == AV_CODEC_ID_H264) {
        // 摄像头直出 H264 或 MP4 文件
        src.is_annex_b = src.is_camera;  // V4L2 H264 输出是 Annex-B
        printf("[输入] H264 %dx%d (需要%s BSF)\n", cp->width, cp->height,
               src.is_annex_b ? "不" : "");
    } else {
        printf("[输入] %s %dx%d (需要编码为 H264)\n",
               avcodec_get_name(cp->codec_id), cp->width, cp->height);
    }

    return true;
}

// ==================== H264 编码器 (摄像头 raw -> H264) ====================

struct H264Encoder {
    AVCodecContext *enc_ctx = nullptr;
    SwsContext *sws_ctx = nullptr;
    AVFrame *yuv_frame = nullptr;
    int64_t frame_pts = 0;
    bool initialized = false;

    bool init(int width, int height, int fps, AVPixelFormat src_fmt) {
        // 尝试编码器优先级: h264_rkmpp > h264_v4l2m2m > libx264
        const char *encoder_names[] = {"h264_rkmpp", "h264_v4l2m2m", "libx264", nullptr};
        const AVCodec *codec = nullptr;

        for (int i = 0; encoder_names[i]; i++) {
            codec = avcodec_find_encoder_by_name(encoder_names[i]);
            if (codec) {
                printf("[编码器] 使用 %s\n", encoder_names[i]);
                break;
            }
        }
        if (!codec) {
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (codec) printf("[编码器] 使用默认 H264 编码器: %s\n", codec->name);
        }
        if (!codec) {
            fprintf(stderr, "[编码器] 未找到 H264 编码器\n");
            return false;
        }

        enc_ctx = avcodec_alloc_context3(codec);
        enc_ctx->width = width;
        enc_ctx->height = height;
        enc_ctx->time_base = {1, fps};
        enc_ctx->framerate = {fps, 1};
        enc_ctx->gop_size = fps * 2;  // 关键帧间隔 2 秒
        enc_ctx->max_b_frames = 0;
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        enc_ctx->bit_rate = 2000000;  // 2 Mbps
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 提取 SPS/PPS

        // 低延迟配置
        av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(enc_ctx->priv_data, "profile", "baseline", 0);

        if (avcodec_open2(enc_ctx, codec, nullptr) < 0) {
            fprintf(stderr, "[编码器] 打开编码器失败\n");
            avcodec_free_context(&enc_ctx);
            return false;
        }

        // 格式转换 (src_fmt -> YUV420P)
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

    // 编码一帧，返回 H264 Annex-B 数据
    bool encode(AVFrame *raw_frame, AVPacket *out_pkt) {
        if (!initialized) return false;

        // 格式转换
        sws_scale(sws_ctx, raw_frame->data, raw_frame->linesize, 0,
                  enc_ctx->height, yuv_frame->data, yuv_frame->linesize);
        yuv_frame->pts = frame_pts++;

        int ret = avcodec_send_frame(enc_ctx, yuv_frame);
        if (ret < 0) return false;

        ret = avcodec_receive_packet(enc_ctx, out_pkt);
        return ret == 0;
    }

    // 请求关键帧
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
    // Annex-B 格式 (编码器 global_header)
    else if (extra[0] == 0 && extra[1] == 0) {
        result.assign(extra, extra + extra_size);
    }

    return result;
}

// ==================== NAL 解析与帧组装 ====================

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

    // 关键帧: 插入 SPS/PPS
    if (keyFrame && !sps_pps.empty()) {
        auto *sp = reinterpret_cast<const std::byte *>(sps_pps.data());
        frame_data.insert(frame_data.end(), sp, sp + sps_pps.size());
    }

    // 仅拷贝 VCL NAL (type 1-5)
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

// ==================== 主程序 ====================

static void print_usage(const char *prog) {
    fprintf(stderr,
        "用法: %s [选项] <input> <whip_url>\n\n"
        "input:\n"
        "  /path/to/video.mp4     本地视频文件 (循环播放)\n"
        "  /dev/video0            V4L2 摄像头\n"
        "  rtsp://...             RTSP 流\n\n"
        "选项:\n"
        "  --width  N     摄像头宽度 (默认 1280)\n"
        "  --height N     摄像头高度 (默认 720)\n"
        "  --fps    N     摄像头帧率 (默认 30)\n"
        "  --bitrate N    编码码率 bps (默认 2000000)\n"
        "  --loop         文件推流循环播放 (默认开启)\n"
        "  --no-loop      文件推流不循环\n\n"
        "示例:\n"
        "  %s /dev/video0 http://srs:2022/rtc/v1/whip/?app=live&stream=cam1&secret=xxx\n"
        "  %s video.mp4 http://srs:2022/rtc/v1/whip/?app=live&stream=test&secret=xxx\n",
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    // 参数解析
    std::string input_str, url_str;
    int cam_width = 1280, cam_height = 720, cam_fps = 30;
    int bitrate = 2000000;
    bool loop_file = true;

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--width" && i + 1 < args.size())  { cam_width = std::stoi(args[++i]); }
        else if (args[i] == "--height" && i + 1 < args.size()) { cam_height = std::stoi(args[++i]); }
        else if (args[i] == "--fps" && i + 1 < args.size())    { cam_fps = std::stoi(args[++i]); }
        else if (args[i] == "--bitrate" && i + 1 < args.size()){ bitrate = std::stoi(args[++i]); }
        else if (args[i] == "--loop")    { loop_file = true; }
        else if (args[i] == "--no-loop") { loop_file = false; }
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

    // 限制 libdatachannel 线程池: 默认 = CPU 核数，低端板子上浪费内存
    rtc::SetThreadPoolSize(2);

    InputType input_type = detect_input_type(input_str);
    const char *type_name[] = {"文件", "摄像头", "网络流"};
    printf("=== WebRTC WHIP Push ===\n");
    printf("输入: %s (%s)\n", input_str.c_str(), type_name[(int)input_type]);
    printf("地址: %s\n", url_str.c_str());

    UrlParts url_parts = parse_url(url_str);

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

    // ========== 2. 编码器 (仅摄像头非 H264 输出时需要) ==========
    H264Encoder encoder;
    AVCodecContext *dec_ctx = nullptr;
    AVFrame *dec_frame = nullptr;

    if (need_encode) {
        // 需要解码器 (解码 MJPEG/YUYV 等)
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
                          (AVPixelFormat)codecpar->format)) {
            avcodec_free_context(&dec_ctx);
            av_frame_free(&dec_frame);
            avformat_close_input(&src.fmt_ctx);
            return 1;
        }
        encoder.enc_ctx->bit_rate = bitrate;
    }

    // ========== 3. BSF (MP4 文件: AVCC -> Annex-B) ==========
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

    // ========== 5. WebRTC PeerConnection ==========
    rtc::Configuration rtc_config;
    rtc_config.disableAutoNegotiation = true;
    auto pc = std::make_shared<rtc::PeerConnection>(rtc_config);

    std::mutex mtx;
    std::condition_variable cv;
    bool gathering_done = false;
    std::atomic<bool> pc_connected{false};

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

    // ========== 6. 音频轨道 (静音 Opus) ==========
    const std::string cname = "whip-push";

    rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendOnly);
    audioMedia.addOpusCodec(111);
    audioMedia.addSSRC(2, cname, "stream", "audio");
    auto audioTrack = pc->addTrack(audioMedia);

    auto audioRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(2, cname, 111, 48000);
    audioRtpCfg->startTimestamp = 0;
    audioRtpCfg->sequenceNumber = 0;
    auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpCfg);
    auto audioSr = std::make_shared<rtc::RtcpSrReporter>(audioRtpCfg);
    audioPacketizer->addToChain(audioSr);
    audioTrack->setMediaHandler(audioPacketizer);

    const std::byte opusSilence[] = { std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE} };

    // ========== 7. 视频轨道 ==========
    rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendOnly);
    videoMedia.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c01f");
    videoMedia.addSSRC(1, cname, "stream", "video");
    auto videoTrack = pc->addTrack(videoMedia);

    auto videoRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(1, cname, 96, 90000);
    videoRtpCfg->startTimestamp = 0;
    videoRtpCfg->sequenceNumber = 0;
    auto videoPacketizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, videoRtpCfg);
    auto videoSr = std::make_shared<rtc::RtcpSrReporter>(videoRtpCfg);
    videoPacketizer->addToChain(videoSr);
    auto nackResp = std::make_shared<rtc::RtcpNackResponder>();
    videoSr->addToChain(nackResp);

    std::atomic<bool> pli_requested{false};
    auto pliHandler = std::make_shared<rtc::PliHandler>([&pli_requested]() {
        pli_requested = true;
    });
    nackResp->addToChain(pliHandler);
    videoTrack->setMediaHandler(videoPacketizer);

    // ========== 8. 信令 ==========
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

    // 第一次尝试
    signaling_ok = signaling_whip(url_str.c_str(), sdp_offer, sdp_answer_raw);
    if (!signaling_ok)
        signaling_ok = signaling_srs(url_parts, sdp_offer, sdp_answer_raw);

    // 失败时: 先踢掉旧 session，再重试
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

    // ========== 9. 推流循环 ==========
    AVPacket *pkt = av_packet_alloc();
    int frame_count = 0;
    int64_t wall_start = av_gettime();
    double dts_offset = 0.0, last_dts = 0.0, last_audio = 0.0;
    bool force_keyframe = false;

    while (g_running && pc_connected) {
        // PLI 处理
        if (pli_requested.exchange(false)) {
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

        int ret = av_read_frame(src.fmt_ctx, pkt);
        if (ret < 0) {
            if (src.is_camera || input_type == InputType::Stream) {
                // 摄像头/流断开
                fprintf(stderr, "\n[输入] 断开\n");
                break;
            }
            if (!loop_file) break;
            // 文件循环
            dts_offset = last_dts + 1.0 / fps;
            av_seek_frame(src.fmt_ctx, src.video_idx, 0, AVSEEK_FLAG_BACKWARD);
            if (bsf_ctx) av_bsf_flush(bsf_ctx);
            wall_start = av_gettime();
            continue;
        }
        if (pkt->stream_index != src.video_idx) { av_packet_unref(pkt); continue; }

        // 节奏控制 (文件需要, 摄像头 V4L2 自带节奏)
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

        // 路径A: 需要编码 (摄像头 non-H264)
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
                    frame_count++;
                }
                av_packet_free(&enc_pkt);
                // 重置 pict_type
                encoder.yuv_frame->pict_type = AV_PICTURE_TYPE_NONE;
            }
        }
        // 路径B: 直推 H264
        else {
            // 需要 BSF 时通过 BSF
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
                frame_count++;
            };

            if (need_bsf) {
                ret = av_bsf_send_packet(bsf_ctx, pkt);
                if (ret < 0) { av_packet_unref(pkt); continue; }
                while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                    if (!g_running || !pc_connected) { av_packet_unref(pkt); break; }
                    process_h264_packet(pkt->data, pkt->size, pkt->dts, pkt->pts,
                                        pkt->flags, bsf_ctx->time_base_out);
                    av_packet_unref(pkt);
                }
            } else {
                // Annex-B 直推 (摄像头 H264 输出)
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

    // ========== 10. 清理 ==========
    // WHIP DELETE: 通知 SRS 立即释放 session
    if (!g_whip_resource_url.empty()) {
        printf("[WHIP] DELETE %s\n", g_whip_resource_url.c_str());
        http_delete(g_whip_resource_url);
    }

    av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (dec_frame) av_frame_free(&dec_frame);
    encoder.cleanup();
    avformat_close_input(&src.fmt_ctx);
    videoTrack->close();
    audioTrack->close();
    pc->close();

    printf("资源已释放。\n");
    return 0;
}
