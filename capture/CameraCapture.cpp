// ============================================================================
// CameraCapture.cpp —— 摄像头采集的实现
// ============================================================================

#include "capture/CameraCapture.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/pixdesc.h>
}

#include <utility>

namespace streamer {

CameraCapture::CameraCapture(CameraOptions options) : options_(std::move(options)) {
    if (options_.inputFormat.empty()) options_.inputFormat = defaultInputFormat();
}

CameraCapture::~CameraCapture() { close(); }

std::string CameraCapture::defaultInputFormat() {
#ifdef _WIN32
    return "dshow";
#elif defined(__APPLE__)
    return "avfoundation";
#else
    return "v4l2";
#endif
}

void CameraCapture::setError(const std::string& what, int errnum) {
    lastError_ = what;
    if (errnum < 0) lastError_ += ": " + avErrorToString(errnum);
}

int CameraCapture::interruptCallback(void* opaque) {
    auto* self = static_cast<CameraCapture*>(opaque);
    return self->stopRequested_ ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 列出设备
// ---------------------------------------------------------------------------
bool CameraCapture::listDevices(const std::string& inputFormat,
                                std::vector<CameraDeviceInfo>& out,
                                std::string& error) {
    // avdevice_register_all: 注册 dshow / gdigrab / v4l2 等所有“设备”类型的输入格式。
    // 不调用的话 av_find_input_format("dshow") 会找不到。重复调用是安全的。
    avdevice_register_all();

    // av_find_input_format: 按名字找输入格式（demuxer），设备也是一种“输入格式”
    const AVInputFormat* ifmt = av_find_input_format(inputFormat.c_str());
    if (!ifmt) {
        error = "找不到输入格式 '" + inputFormat + "'，FFmpeg 编译时可能没开启这个设备";
        return false;
    }

    // avdevice_list_input_sources: 让设备模块枚举可用的输入源。
    // dshow 会把视频设备和音频设备（麦克风）都列出来，下面只保留视频设备。
    AVDeviceInfoList* list = nullptr;
    int ret = avdevice_list_input_sources(ifmt, nullptr, nullptr, &list);
    if (ret < 0) {
        error = "avdevice_list_input_sources 失败: " + avErrorToString(ret);
        avdevice_free_list_devices(&list);
        return false;
    }

    for (int i = 0; i < list->nb_devices; ++i) {
        const AVDeviceInfo* dev = list->devices[i];

        // media_types 说明这个设备能提供什么类型的数据（视频/音频）
        bool isVideo = (dev->nb_media_types == 0);  // 没写类型的就当作可能是视频
        for (int t = 0; t < dev->nb_media_types; ++t) {
            if (dev->media_types[t] == AVMEDIA_TYPE_VIDEO) isVideo = true;
        }
        if (!isVideo) continue;

        CameraDeviceInfo info;
        // dshow: device_description 是“友好名”（如 Integrated Camera），打开时用它最直观；
        //        device_name 是一长串唯一名（@device_pnp_...），也能用来打开。
        // v4l2:  device_name 是 /dev/video0 这种路径。
        if (inputFormat == "dshow" && dev->device_description) {
            info.name = dev->device_description;
            info.description = dev->device_name ? dev->device_name : "";
        } else {
            info.name = dev->device_name ? dev->device_name : "";
            info.description = dev->device_description ? dev->device_description : "";
        }
        out.push_back(std::move(info));
    }

    // 释放枚举结果
    avdevice_free_list_devices(&list);
    return true;
}

void CameraCapture::printDevicesToLog(const std::string& inputFormat) {
    avdevice_register_all();
    const AVInputFormat* ifmt = av_find_input_format(inputFormat.c_str());
    if (!ifmt) return;

    // 相当于 ffmpeg -f dshow -list_devices true -i dummy
    // 打开会“失败”（这是正常的），但 dshow 会通过 av_log 把设备列表打印到控制台。
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "list_devices", "true", 0);
    AVFormatContext* ctx = nullptr;
    const char* url = (inputFormat == "dshow") ? "video=dummy" : "dummy";
    if (avformat_open_input(&ctx, url, ifmt, &opts) == 0) avformat_close_input(&ctx);
    av_dict_free(&opts);
}

// ---------------------------------------------------------------------------
// 打开摄像头
// ---------------------------------------------------------------------------
bool CameraCapture::open() {
    close();
    stopRequested_ = false;
    decoderDrained_ = false;

    avdevice_register_all();

    // 1) 找到输入格式（dshow / v4l2）
    const AVInputFormat* ifmt = av_find_input_format(options_.inputFormat.c_str());
    if (!ifmt) {
        setError("找不到输入格式 '" + options_.inputFormat + "'", 0);
        return false;
    }

    // 2) 组装设备 URL。dshow 的格式是 "video=设备名"
    std::string url = options_.deviceName;
    if (options_.inputFormat == "dshow" && url.rfind("video=", 0) != 0) {
        url = "video=" + url;
    }

    // 3) 设置打开选项（等价于命令行里 -i 前面的 -video_size 1280x720 -framerate 30 ...）
    //    av_dict_set: 往字典里放 key=value；字典为空指针时会自动分配
    AVDictionary* rawOpts = nullptr;
    if (!options_.videoSize.empty()) av_dict_set(&rawOpts, "video_size", options_.videoSize.c_str(), 0);
    if (!options_.frameRate.empty()) av_dict_set(&rawOpts, "framerate", options_.frameRate.c_str(), 0);
    if (!options_.pixelFormat.empty()) {
        // dshow 用 pixel_format，v4l2 用 input_format，两个都设上，不认识的会被忽略
        av_dict_set(&rawOpts, "pixel_format", options_.pixelFormat.c_str(), 0);
        if (options_.inputFormat == "v4l2") av_dict_set(&rawOpts, "input_format", options_.pixelFormat.c_str(), 0);
    }
    if (!options_.vcodec.empty()) {
        if (options_.inputFormat == "dshow") av_dict_set(&rawOpts, "vcodec", options_.vcodec.c_str(), 0);
        else av_dict_set(&rawOpts, "input_format", options_.vcodec.c_str(), 0);
    }
    if (options_.inputFormat == "dshow" && !options_.rtbufsize.empty()) {
        av_dict_set(&rawOpts, "rtbufsize", options_.rtbufsize.c_str(), 0);
    }

    // 4) 先分配 AVFormatContext，这样可以在打开前装上中断回调（用于 Ctrl+C 退出）
    //    avformat_alloc_context: 分配一个空的容器上下文
    AVFormatContext* rawCtx = avformat_alloc_context();
    if (!rawCtx) {
        av_dict_free(&rawOpts);
        setError("avformat_alloc_context 失败（内存不足）", 0);
        return false;
    }
    rawCtx->interrupt_callback.callback = &CameraCapture::interruptCallback;
    rawCtx->interrupt_callback.opaque = this;

    // 5) avformat_open_input: 真正打开设备。
    //    - 失败时 FFmpeg 会自己释放 rawCtx 并置空，所以不需要我们 free
    //    - 成功后 rawOpts 里只剩“没被用到的选项”，可以拿来提示拼写错误
    int ret = avformat_open_input(&rawCtx, url.c_str(), ifmt, &rawOpts);
    std::unique_ptr<AVDictionary, DictionaryDeleter> optsGuard(rawOpts);  // 不管成功失败都释放字典
    if (ret < 0) {
        setError("打开摄像头失败 [" + url + "]", ret);
        return false;
    }
    formatCtx_.reset(rawCtx);

    const AVDictionaryEntry* unused = nullptr;
    // av_dict_iterate: 遍历字典（FFmpeg 6.1+ 提供，替代旧的 av_dict_get 遍历写法）
    while ((unused = av_dict_iterate(optsGuard.get(), unused))) {
        av_log(nullptr, AV_LOG_WARNING, "选项未被设备使用: %s=%s\n", unused->key, unused->value);
    }

    // 6) avformat_find_stream_info: 读几包数据，探测流的详细参数（分辨率、帧率、编码）
    ret = avformat_find_stream_info(formatCtx_.get(), nullptr);
    if (ret < 0) {
        setError("avformat_find_stream_info 失败", ret);
        close();
        return false;
    }

    // av_dump_format: 把探测到的流信息打印出来（和 ffmpeg 命令行显示的 Input #0 一样）
    av_dump_format(formatCtx_.get(), 0, url.c_str(), 0);

    // 7) av_find_best_stream: 找“最合适”的视频流，同时返回对应的解码器
    const AVCodec* decoder = nullptr;
    ret = av_find_best_stream(formatCtx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        setError("设备里没有找到视频流", ret);
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

    // 8) 创建并配置解码器。
    //    摄像头输出 YUYV 等原始格式时用 rawvideo 解码器（几乎是直接拷贝）；
    //    输出 MJPEG 时用 mjpeg 解码器解成 YUVJ422P 之类的格式。
    codecCtx_.reset(avcodec_alloc_context3(decoder));  // 分配解码器上下文
    if (!codecCtx_) {
        setError("avcodec_alloc_context3 失败", 0);
        close();
        return false;
    }
    // avcodec_parameters_to_context: 把流参数（宽高、格式、extradata）拷贝给解码器
    ret = avcodec_parameters_to_context(codecCtx_.get(), stream->codecpar);
    if (ret < 0) {
        setError("avcodec_parameters_to_context 失败", ret);
        close();
        return false;
    }
    codecCtx_->pkt_timebase = stream->time_base;  // 告诉解码器 pts 的单位，输出帧的 pts 才正确

    // avcodec_open2: 初始化解码器
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

    // 记录流信息。像素格式以解码后的帧为准，readFrame 里会再更新一次
    info_.width = codecCtx_->width;
    info_.height = codecCtx_->height;
    info_.pixelFormat = codecCtx_->pix_fmt;
    info_.timeBase = stream->time_base;
    // av_guess_frame_rate: 综合 r_frame_rate / avg_frame_rate 等字段猜测帧率
    info_.frameRate = av_guess_frame_rate(formatCtx_.get(), stream, nullptr);

    lastError_.clear();
    return true;
}

void CameraCapture::close() {
    // 顺序：先释放解码器和 packet，再关设备。unique_ptr::reset 会调用对应 deleter
    packet_.reset();
    codecCtx_.reset();
    formatCtx_.reset();
    videoStreamIndex_ = -1;
    info_ = VideoStreamInfo{};
}

// ---------------------------------------------------------------------------
// 读取一帧
// 解码 API 是“送包 / 取帧”模型：
//   avcodec_send_packet  —— 把压缩包送进解码器
//   avcodec_receive_frame —— 从解码器取出解好的帧
//   取帧返回 EAGAIN 表示“需要再送包”，返回 EOF 表示解码器已经冲刷完
// ---------------------------------------------------------------------------
ReadResult CameraCapture::readFrame(AVFrame* frame) {
    if (!isOpen()) {
        setError("摄像头未打开", 0);
        return ReadResult::Error;
    }
    av_frame_unref(frame);  // 清掉上一帧的数据引用，避免内存泄漏

    while (true) {
        // 先尝试从解码器拿已经解好的帧
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

        // 解码器需要更多数据：从设备读一个包
        // av_read_frame: 读取下一个压缩包（名字叫 frame，实际返回的是 packet）
        ret = av_read_frame(formatCtx_.get(), packet_.get());
        if (ret == AVERROR(EAGAIN)) return ReadResult::Again;
        if (ret == AVERROR_EOF || ret == AVERROR_EXIT || ret == AVERROR(EIO)) {
            // 记下原因，否则上层只看到“结束”却不知道为什么。
            // dshow 返回 EIO 通常是摄像头被别的程序占用（OBS、Teams、相机 App）或被拔出
            if (ret == AVERROR(EIO))
                setError("摄像头读取出错（可能被 OBS / Teams / 相机等其他程序占用，或已断开）", ret);
            else if (ret == AVERROR_EXIT)
                lastError_ = "用户请求停止";
            else
                setError("采集源已结束", ret);
            // 设备结束或被中断：送一个空包让解码器把缓存的帧吐出来（flush）
            avcodec_send_packet(codecCtx_.get(), nullptr);
            decoderDrained_ = true;
            continue;
        }
        if (ret < 0) {
            setError("av_read_frame 失败", ret);
            return ReadResult::Error;
        }

        // 只处理视频流的包（dshow 同时开了音频时会有音频包）
        if (packet_->stream_index != videoStreamIndex_) {
            av_packet_unref(packet_.get());
            continue;
        }

        ret = avcodec_send_packet(codecCtx_.get(), packet_.get());
        av_packet_unref(packet_.get());  // 解码器内部会自己保留需要的引用，这里可以立刻释放
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            // 某一帧 MJPEG 损坏时可能报 INVALIDDATA，跳过这包继续
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
