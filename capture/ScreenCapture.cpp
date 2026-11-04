// ============================================================================
// ScreenCapture.cpp —— 桌面采集的实现（gdigrab）
//
// 和 CameraCapture.cpp 的结构一一对应，区别只有三处：
//   1) 输入格式是 gdigrab，URL 是 "desktop" 或 "title=窗口名"
//   2) 选项是 framerate / video_size / offset_x / offset_y / draw_mouse
//   3) Windows 上先声明 DPI 感知，否则缩放 125%/150% 的屏幕只能抓到左上角一部分
// ============================================================================

#include "capture/ScreenCapture.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <string>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace streamer {

ScreenCapture::ScreenCapture(ScreenOptions options) : options_(std::move(options)) {
    if (options_.inputFormat.empty()) options_.inputFormat = defaultInputFormat();
    if (options_.target.empty()) options_.target = (options_.inputFormat == "x11grab") ? ":0.0" : "desktop";
}

ScreenCapture::~ScreenCapture() { close(); }

std::string ScreenCapture::defaultInputFormat() {
#ifdef _WIN32
    return "gdigrab";
#elif defined(__APPLE__)
    return "avfoundation";
#else
    return "x11grab";
#endif
}

void ScreenCapture::setError(const std::string& what, int errnum) {
    lastError_ = what;
    if (errnum < 0) lastError_ += ": " + avErrorToString(errnum);
}

int ScreenCapture::interruptCallback(void* opaque) {
    auto* self = static_cast<ScreenCapture*>(opaque);
    return self->stopRequested_ ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 打开桌面
// ---------------------------------------------------------------------------
bool ScreenCapture::open() {
    close();  // 允许重复 open：先把上一次的资源释放掉，避免泄漏
    stopRequested_ = false;
    decoderDrained_ = false;

#ifdef _WIN32
    // 笔记本常把显示缩放设为 150%。程序如果不声明“我能处理 DPI”，Windows 会对它撒谎，
    // 报告的屏幕尺寸是缩放后的（例如 2560x1600 报成 1707x1067），gdigrab 就只抓到一部分。
    // SetProcessDPIAware: 告诉系统按真实像素给我坐标。必须在第一次截屏前调用。
    SetProcessDPIAware();
#endif

    // gdigrab 的 "desktop" 是所有显示器拼起来的“虚拟桌面”（3 个屏就是一张超大图）。
    // 默认只要主显示器：Windows 规定主显示器左上角就是坐标原点 (0,0)，
    // 所以 offset 保持 0,0，video_size 设成主显示器的大小即可。
    std::string videoSize = options_.videoSize;
#ifdef _WIN32
    if (options_.inputFormat == "gdigrab" && options_.target == "desktop" &&
        videoSize.empty() && !options_.allMonitors) {
        // GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN): 主显示器的宽高（上面声明了 DPI 感知，所以是真实像素）
        const int w = GetSystemMetrics(SM_CXSCREEN);
        const int h = GetSystemMetrics(SM_CYSCREEN);
        if (w > 0 && h > 0) videoSize = std::to_string(w) + "x" + std::to_string(h);
    }
#endif

    avdevice_register_all();  // 注册 gdigrab / dshow 等设备，否则 av_find_input_format 找不到

    // 1) 找到输入格式（gdigrab / x11grab）
    const AVInputFormat* ifmt = av_find_input_format(options_.inputFormat.c_str());
    if (!ifmt) {
        setError("找不到输入格式 '" + options_.inputFormat + "'（FFmpeg 编译时没带这个设备？）", 0);
        return false;
    }

    // 2) 设置选项（等价于命令行 -framerate 30 -video_size 1280x720 -offset_x 0 ...）
    AVDictionary* rawOpts = nullptr;
    if (!options_.frameRate.empty()) av_dict_set(&rawOpts, "framerate", options_.frameRate.c_str(), 0);
    if (!videoSize.empty()) av_dict_set(&rawOpts, "video_size", videoSize.c_str(), 0);
    if (options_.inputFormat == "gdigrab") {
        // av_dict_set_int: 和 av_dict_set 一样，只是值是整数
        if (options_.offsetX) av_dict_set_int(&rawOpts, "offset_x", options_.offsetX, 0);
        if (options_.offsetY) av_dict_set_int(&rawOpts, "offset_y", options_.offsetY, 0);
    }
    if (options_.inputFormat == "gdigrab" || options_.inputFormat == "x11grab") {
        av_dict_set(&rawOpts, "draw_mouse", options_.drawMouse ? "1" : "0", 0);
    }

    // 3) 先分配上下文，装上中断回调（Ctrl+C 时让阻塞的读取尽快返回）
    AVFormatContext* rawCtx = avformat_alloc_context();
    if (!rawCtx) {
        av_dict_free(&rawOpts);
        setError("avformat_alloc_context 失败（内存不足）", 0);
        return false;
    }
    rawCtx->interrupt_callback.callback = &ScreenCapture::interruptCallback;
    rawCtx->interrupt_callback.opaque = this;

    // 4) avformat_open_input: 打开桌面。失败时 FFmpeg 会自己释放 rawCtx
    int ret = avformat_open_input(&rawCtx, options_.target.c_str(), ifmt, &rawOpts);
    std::unique_ptr<AVDictionary, DictionaryDeleter> optsGuard(rawOpts);
    if (ret < 0) {
        setError("打开桌面采集失败 [" + options_.inputFormat + " " + options_.target + "]", ret);
        return false;
    }
    formatCtx_.reset(rawCtx);

    const AVDictionaryEntry* unused = nullptr;
    while ((unused = av_dict_iterate(optsGuard.get(), unused))) {
        av_log(nullptr, AV_LOG_WARNING, "选项未被设备使用: %s=%s\n", unused->key, unused->value);
    }

    // 5) 探测流信息并打印（会显示 Stream #0:0: Video: bmp, bgra, 1920x1080, 30 fps）
    ret = avformat_find_stream_info(formatCtx_.get(), nullptr);
    if (ret < 0) {
        setError("avformat_find_stream_info 失败", ret);
        close();
        return false;
    }
    av_dump_format(formatCtx_.get(), 0, options_.target.c_str(), 0);

    // 6) 找视频流和对应的解码器（gdigrab 对应 bmp 解码器）
    const AVCodec* decoder = nullptr;
    ret = av_find_best_stream(formatCtx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        setError("没有找到视频流", ret);
        close();
        return false;
    }
    videoStreamIndex_ = ret;
    AVStream* stream = formatCtx_->streams[videoStreamIndex_];
    if (!decoder) {
        setError(std::string("找不到解码器: ") + avcodec_get_name(stream->codecpar->codec_id), 0);
        close();
        return false;
    }

    // 7) 创建并打开解码器。BMP 是未压缩的，“解码”只是去掉文件头、把倒着存的行翻过来，很便宜
    codecCtx_.reset(avcodec_alloc_context3(decoder));
    if (!codecCtx_) {
        setError("avcodec_alloc_context3 失败", 0);
        close();
        return false;
    }
    ret = avcodec_parameters_to_context(codecCtx_.get(), stream->codecpar);
    if (ret < 0) {
        setError("avcodec_parameters_to_context 失败", ret);
        close();
        return false;
    }
    codecCtx_->pkt_timebase = stream->time_base;
    ret = avcodec_open2(codecCtx_.get(), decoder, nullptr);
    if (ret < 0) {
        setError("avcodec_open2 失败", ret);
        close();
        return false;
    }

    packet_ = makePacket();
    if (!packet_) {
        setError("av_packet_alloc 失败", 0);
        close();
        return false;
    }

    info_.width = codecCtx_->width;
    info_.height = codecCtx_->height;
    info_.pixelFormat = codecCtx_->pix_fmt;
    info_.timeBase = stream->time_base;  // gdigrab 是 1/1000000（微秒），pts 来自系统时钟
    info_.frameRate = av_guess_frame_rate(formatCtx_.get(), stream, nullptr);

    lastError_.clear();
    return true;
}

void ScreenCapture::close() {
    packet_.reset();
    codecCtx_.reset();
    formatCtx_.reset();
    videoStreamIndex_ = -1;
    info_ = VideoStreamInfo{};
}

// ---------------------------------------------------------------------------
// 读取一帧：送包 / 取帧模型，和摄像头一样
// gdigrab 的 av_read_frame 会自己按帧率睡眠（30fps 就是每 33ms 截一次屏），
// 所以这里不需要额外控制节奏。
// ---------------------------------------------------------------------------
ReadResult ScreenCapture::readFrame(AVFrame* frame) {
    if (!isOpen()) {
        setError("桌面采集未打开", 0);
        return ReadResult::Error;
    }
    av_frame_unref(frame);

    while (true) {
        int ret = avcodec_receive_frame(codecCtx_.get(), frame);
        if (ret == 0) {
            info_.width = frame->width;
            info_.height = frame->height;
            info_.pixelFormat = static_cast<AVPixelFormat>(frame->format);
            return ReadResult::Ok;
        }
        if (ret == AVERROR_EOF) return ReadResult::Eof;
        if (ret != AVERROR(EAGAIN)) {
            setError("avcodec_receive_frame 失败", ret);
            return ReadResult::Error;
        }

        if (decoderDrained_) return ReadResult::Eof;
        if (stopRequested_) {
            lastError_ = "用户请求停止";
            return ReadResult::Eof;
        }

        ret = av_read_frame(formatCtx_.get(), packet_.get());
        if (ret == AVERROR(EAGAIN)) return ReadResult::Again;
        if (ret == AVERROR_EOF || ret == AVERROR_EXIT) {
            if (ret == AVERROR_EXIT) lastError_ = "用户请求停止";
            else setError("采集源已结束", ret);
            avcodec_send_packet(codecCtx_.get(), nullptr);  // flush 解码器
            decoderDrained_ = true;
            continue;
        }
        if (ret < 0) {
            // gdigrab 抓单个窗口时，窗口被关掉会走到这里
            setError("av_read_frame 失败（抓窗口时窗口可能被关闭了）", ret);
            return ReadResult::Error;
        }

        if (packet_->stream_index != videoStreamIndex_) {
            av_packet_unref(packet_.get());
            continue;
        }

        ret = avcodec_send_packet(codecCtx_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_INVALIDDATA) {
                av_log(nullptr, AV_LOG_WARNING, "跳过一个损坏的数据包\n");
                continue;
            }
            setError("avcodec_send_packet 失败", ret);
            return ReadResult::Error;
        }
    }
}

}  // namespace streamer
