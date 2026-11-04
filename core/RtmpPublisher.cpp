// ============================================================================
// RtmpPublisher.cpp
// ============================================================================

#include "core/RtmpPublisher.h"

namespace streamer {

void RtmpPublisher::OutputDeleter::operator()(AVFormatContext* ctx) const {
    if (!ctx) return;
    // 输出到文件/网络时，pb（AVIOContext）是我们用 avio_open2 打开的，要自己关
    if (ctx->oformat && !(ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&ctx->pb);
    avformat_free_context(ctx);  // 输出上下文用 avformat_free_context 释放（输入才用 close_input）
}

RtmpPublisher::~RtmpPublisher() { close(); }

int RtmpPublisher::interruptCallback(void* opaque) {
    return static_cast<RtmpPublisher*>(opaque)->stopRequested_ ? 1 : 0;
}

bool RtmpPublisher::open(const std::string& url, const AVCodecContext* encoder,
                         const AVCodecContext* audioEncoder) {
    close();
    stopRequested_ = false;
    url_ = url;

    // 1) 创建输出上下文。第三个参数指定封装格式 "flv"：RTMP 推流必须用 FLV
    AVFormatContext* raw = nullptr;
    int ret = avformat_alloc_output_context2(&raw, nullptr, "flv", url.c_str());
    if (ret < 0 || !raw) {
        lastError_ = "avformat_alloc_output_context2 失败: " + avErrorToString(ret);
        return false;
    }
    formatCtx_.reset(raw);
    formatCtx_->interrupt_callback.callback = &RtmpPublisher::interruptCallback;
    formatCtx_->interrupt_callback.opaque = this;

    // 2) 新建一路视频流，把编码器参数拷进去（codec_id、宽高、extradata 里的 SPS/PPS）
    stream_ = avformat_new_stream(formatCtx_.get(), nullptr);
    if (!stream_) {
        lastError_ = "avformat_new_stream 失败";
        close();
        return false;
    }
    ret = avcodec_parameters_from_context(stream_->codecpar, encoder);
    if (ret < 0) {
        lastError_ = "avcodec_parameters_from_context 失败: " + avErrorToString(ret);
        close();
        return false;
    }
    stream_->time_base = encoder->time_base;  // 只是建议值，write_header 时 FLV 会改成 1/1000

    // 2.5) 有音频就再加一路 AAC 流。FLV 里一个视频 + 一个音频是最常见的组合
    if (audioEncoder) {
        audioStream_ = avformat_new_stream(formatCtx_.get(), nullptr);
        if (!audioStream_ ||
            (ret = avcodec_parameters_from_context(audioStream_->codecpar, audioEncoder)) < 0) {
            lastError_ = "添加音频流失败: " + avErrorToString(ret);
            close();
            return false;
        }
        audioStream_->time_base = audioEncoder->time_base;
        // max_interleave_delta: 音视频交错写入时，最多为了“等另一路”缓存多久（微秒）。
        // 默认 10 秒，万一麦克风卡住，视频会被憋住 10 秒；直播改成 0.5 秒
        formatCtx_->max_interleave_delta = 500000;
    }

    // 3) 打开网络连接（或本地文件）。rtmp:// 地址会在这里完成 TCP 连接和 RTMP 握手
    if (!(formatCtx_->oformat->flags & AVFMT_NOFILE)) {
        // rw_timeout: 网络读写超过 5 秒没反应就报错，避免服务器失联时程序一直卡住（单位微秒）
        AVDictionary* ioOpts = nullptr;
        av_dict_set(&ioOpts, "rw_timeout", "5000000", 0);
        ret = avio_open2(&formatCtx_->pb, url.c_str(), AVIO_FLAG_WRITE,
                         &formatCtx_->interrupt_callback, &ioOpts);
        av_dict_free(&ioOpts);
        if (ret < 0) {
            lastError_ = "连接失败 [" + url + "]: " + avErrorToString(ret);
            close();
            return false;
        }
    }

    // 4) 写文件头：对 RTMP 来说就是把“这是 H.264、多大分辨率、SPS/PPS”发给服务器
    ret = avformat_write_header(formatCtx_.get(), nullptr);
    if (ret < 0) {
        lastError_ = "avformat_write_header 失败: " + avErrorToString(ret);
        close();
        return false;
    }
    headerWritten_ = true;
    bytesWritten_ = 0;
    av_dump_format(formatCtx_.get(), 0, url.c_str(), 1);  // 最后一个参数 1 表示这是输出
    lastError_.clear();
    return true;
}

bool RtmpPublisher::writePacket(AVPacket* packet, AVRational srcTimeBase, bool isAudio) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!headerWritten_) {
        lastError_ = "推流未打开";
        return false;
    }
    AVStream* st = isAudio ? audioStream_ : stream_;
    if (!st) {
        lastError_ = "没有音频流，却写了音频包";
        return false;
    }
    packet->stream_index = st->index;
    // av_packet_rescale_ts: 把 pts/dts/duration 从编码器的时间单位换算成输出流的时间单位
    av_packet_rescale_ts(packet, srcTimeBase, st->time_base);
    const int size = packet->size;

    // av_interleaved_write_frame: 写一个包。多路流（音频+视频）时它会按时间戳排好顺序再发。
    // 调用后 packet 的数据归 FFmpeg 管，packet 会被重置为空
    int ret = av_interleaved_write_frame(formatCtx_.get(), packet);
    if (ret < 0) {
        lastError_ = "发送数据失败（服务器断开了？）: " + avErrorToString(ret);
        return false;
    }
    bytesWritten_ += size;
    return true;
}

void RtmpPublisher::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (formatCtx_ && headerWritten_) {
        // 走到这里说明主循环已经退出，不再需要“中断”。先清掉标志，
        // 否则 av_write_trailer 会被中断回调打断，结尾写不完整（本地 flv 文件会没有时长信息）
        stopRequested_ = false;
        // av_write_trailer: 写结尾，并把缓冲里剩下的数据发出去
        av_write_trailer(formatCtx_.get());
    }
    headerWritten_ = false;
    stream_ = nullptr;
    audioStream_ = nullptr;
    formatCtx_.reset();
}

}  // namespace streamer
