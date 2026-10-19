// ============================================================================
// VideoEncoder.cpp
// ============================================================================

#include "core/VideoEncoder.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <vector>

namespace streamer {

namespace {

// 选编码器支持的像素格式：优先 yuv420p（兼容性最好，浏览器/播放器都支持），其次 nv12
AVPixelFormat choosePixelFormat(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    // avcodec_get_supported_config: 查询编码器支持哪些像素格式（FFmpeg 7.1 起替代旧的 codec->pix_fmts）
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 ||
        !configs || count == 0) {
        return AV_PIX_FMT_YUV420P;  // 没有声明就当作支持 yuv420p
    }
    const auto* fmts = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < count; ++i)
        if (fmts[i] == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    for (int i = 0; i < count; ++i)
        if (fmts[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    return fmts[0];
}

}  // namespace

bool VideoEncoder::open(const EncoderOptions& options) {
    close();
    if (options.width <= 0 || options.height <= 0) {
        lastError_ = "编码器宽高无效";
        return false;
    }

    // 候选编码器列表。libx264 是软件编码，画质好、兼容性好；
    // 后面几个是显卡/系统的硬件编码器，CPU 占用低，作为备选
    std::vector<std::string> candidates;
    if (!options.codecName.empty()) {
        candidates.push_back(options.codecName);
    } else {
        candidates = {"libx264", "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
    }

    std::string errors;
    for (const auto& name : candidates) {
        // avcodec_find_encoder_by_name: 按名字找编码器，FFmpeg 编译时没带它就返回 nullptr
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            errors += "\n  " + name + ": 当前 FFmpeg 没有这个编码器";
            continue;
        }
        if (tryOpen(codec, options)) {
            codecName_ = name;
            lastError_.clear();
            return true;
        }
        errors += "\n  " + name + ": " + lastError_;
    }
    lastError_ = "没有可用的 H.264 编码器:" + errors;
    return false;
}

bool VideoEncoder::tryOpen(const AVCodec* codec, const EncoderOptions& options) {
    // avcodec_alloc_context3: 为这个编码器分配上下文
    codecCtx_.reset(avcodec_alloc_context3(codec));
    if (!codecCtx_) {
        lastError_ = "avcodec_alloc_context3 失败";
        return false;
    }
    AVCodecContext* c = codecCtx_.get();
    c->width = options.width;
    c->height = options.height;
    c->pix_fmt = choosePixelFormat(codec);
    // 时间基设成 1/1000：pts 的单位就是毫秒。直播用真实时间做 pts，
    // 摄像头的实际帧率会有抖动（29.7、30.2……），用毫秒比用“第几帧”更准
    c->time_base = AVRational{1, 1000};
    c->framerate = options.frameRate;
    c->bit_rate = options.bitRate;
    // GOP：两个关键帧之间的帧数
    const double fps = av_q2d(options.frameRate) > 0 ? av_q2d(options.frameRate) : 30.0;
    c->gop_size = static_cast<int>(fps * options.gopSeconds);
    // 不用 B 帧：B 帧要参考后面的帧，会增加延迟，直播一般关掉
    c->max_b_frames = 0;
    if (options.globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 编码器私有选项
    AVDictionary* rawOpts = nullptr;
    const std::string name = codec->name;
    if (name == "libx264") {
        av_dict_set(&rawOpts, "preset", "veryfast", 0);    // 速度和画质的折中，越快 CPU 越省、画质越差
        av_dict_set(&rawOpts, "tune", "zerolatency", 0);   // 低延迟模式：不缓存帧，编完立刻输出
    } else if (name == "h264_nvenc") {
        av_dict_set(&rawOpts, "preset", "p4", 0);
        av_dict_set(&rawOpts, "tune", "ll", 0);            // low latency
    }

    // avcodec_open2: 用上面的参数初始化编码器
    int ret = avcodec_open2(c, codec, &rawOpts);
    av_dict_free(&rawOpts);
    if (ret < 0) {
        lastError_ = "avcodec_open2 失败: " + avErrorToString(ret);
        codecCtx_.reset();
        return false;
    }

    // 准备送进编码器的帧缓冲
    encFrame_ = makeFrame();
    packet_ = makePacket();
    if (!encFrame_ || !packet_) {
        lastError_ = "分配帧/包失败";
        codecCtx_.reset();
        return false;
    }
    encFrame_->format = c->pix_fmt;
    encFrame_->width = c->width;
    encFrame_->height = c->height;
    ret = av_frame_get_buffer(encFrame_.get(), 0);
    if (ret < 0) {
        lastError_ = "av_frame_get_buffer 失败: " + avErrorToString(ret);
        codecCtx_.reset();
        return false;
    }
    sws_.reset();
    srcW_ = srcH_ = 0;
    srcFmt_ = AV_PIX_FMT_NONE;
    return true;
}

void VideoEncoder::close() {
    sws_.reset();
    encFrame_.reset();
    packet_.reset();
    codecCtx_.reset();
    codecName_.clear();
}

bool VideoEncoder::convert(const AVFrame* src) {
    const auto srcFmt = static_cast<AVPixelFormat>(src->format);
    if (!sws_ || src->width != srcW_ || src->height != srcH_ || srcFmt != srcFmt_) {
        // 输入尺寸/格式 → 编码器尺寸/格式。尺寸不同时顺便缩放
        sws_.reset(sws_getContext(src->width, src->height, srcFmt,
                                  codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws_) {
            lastError_ = std::string("不支持的像素格式转换: ") +
                         (av_get_pix_fmt_name(srcFmt) ? av_get_pix_fmt_name(srcFmt) : "unknown");
            return false;
        }
        srcW_ = src->width;
        srcH_ = src->height;
        srcFmt_ = srcFmt;
    }

    // 关键：编码器可能还引用着上一帧的缓冲区（内部排队），直接覆盖会把它要编的数据改掉。
    // av_frame_make_writable: 如果缓冲区还被别人引用，就重新分配一块新的
    int ret = av_frame_make_writable(encFrame_.get());
    if (ret < 0) {
        lastError_ = "av_frame_make_writable 失败: " + avErrorToString(ret);
        return false;
    }
    sws_scale(sws_.get(), src->data, src->linesize, 0, src->height,
              encFrame_->data, encFrame_->linesize);
    return true;
}

bool VideoEncoder::encode(const AVFrame* frame, int64_t ptsMs, const PacketCallback& onPacket) {
    if (!isOpen()) {
        lastError_ = "编码器未打开";
        return false;
    }
    if (!convert(frame)) return false;
    encFrame_->pts = ptsMs;  // 时间基是 1/1000，所以直接填毫秒

    // avcodec_send_frame: 把一帧送进编码器。编码器可能攒几帧才输出，所以送一帧不一定马上有包
    int ret = avcodec_send_frame(codecCtx_.get(), encFrame_.get());
    if (ret < 0) {
        lastError_ = "avcodec_send_frame 失败: " + avErrorToString(ret);
        return false;
    }
    return drain(onPacket);
}

bool VideoEncoder::flush(const PacketCallback& onPacket) {
    if (!isOpen()) return true;
    // 送一个空帧（nullptr）表示“没有更多输入了”，编码器会把剩下的帧全部编完
    int ret = avcodec_send_frame(codecCtx_.get(), nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        lastError_ = "flush 失败: " + avErrorToString(ret);
        return false;
    }
    return drain(onPacket);
}

bool VideoEncoder::drain(const PacketCallback& onPacket) {
    while (true) {
        // avcodec_receive_packet: 取一个编好的压缩包
        //   EAGAIN = 需要再送帧才能出包；EOF = flush 完了
        int ret = avcodec_receive_packet(codecCtx_.get(), packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            lastError_ = "avcodec_receive_packet 失败: " + avErrorToString(ret);
            return false;
        }
        bool ok = onPacket(packet_.get());
        av_packet_unref(packet_.get());  // 回调用完就释放，否则每个包都会泄漏
        if (!ok) return false;
    }
}

}  // namespace streamer
