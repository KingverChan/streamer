#pragma once
// ============================================================================
// RtmpPublisher.h —— 把 H.264 压缩包封装成 FLV，通过 RTMP 推到服务器
//
// 为什么要“封装”：编码器吐出来的只是一个个 H.264 数据包，服务器还需要知道
// 这是什么编码、宽高多少、每个包的时间戳。RTMP 协议规定用 FLV 格式来装这些信息。
//
// 流程：avformat_alloc_output_context2("flv") → 加一路视频流 → avio_open 连服务器
//       → avformat_write_header（发送编码参数）→ 循环 av_interleaved_write_frame
//       → av_write_trailer
//
// url 也可以是本地文件名（例如 out.flv），方便没有服务器时测试。
// ============================================================================

#include "core/FFmpegUtils.h"

#include <atomic>
#include <mutex>
#include <string>

namespace streamer {

class RtmpPublisher {
public:
    RtmpPublisher() = default;
    ~RtmpPublisher();
    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    // encoder: 已经打开的编码器上下文，用来告诉服务器视频参数（宽高、SPS/PPS 等）
    // audioEncoder: 可选，AAC 编码器上下文；传了就多加一路音频流
    bool open(const std::string& url, const AVCodecContext* encoder,
              const AVCodecContext* audioEncoder = nullptr);

    // 写一个压缩包。srcTimeBase 是包里 pts/dts 的时间单位（编码器的 time_base）
    // isAudio=true 写到音频流。线程安全：视频线程和音频线程可以同时调用
    bool writePacket(AVPacket* packet, AVRational srcTimeBase, bool isAudio = false);

    // 写结尾并断开连接。可以重复调用
    void close();

    bool isOpen() const { return formatCtx_ != nullptr; }
    int64_t bytesWritten() const { return bytesWritten_; }
    std::string lastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

    // 让卡在网络读写上的调用尽快返回（Ctrl+C 用）
    void requestStop() { stopRequested_ = true; }

private:
    static int interruptCallback(void* opaque);

    // 输出上下文的 deleter：先关网络/文件，再释放上下文
    struct OutputDeleter {
        void operator()(AVFormatContext* ctx) const;
    };

    std::unique_ptr<AVFormatContext, OutputDeleter> formatCtx_;
    AVStream* stream_ = nullptr;   // 归 formatCtx_ 所有，不需要单独释放
    AVStream* audioStream_ = nullptr;
    bool headerWritten_ = false;
    std::atomic<int64_t> bytesWritten_{0};
    // av_interleaved_write_frame 不是线程安全的，音视频两个线程写同一个 formatCtx_ 要加锁
    mutable std::mutex mutex_;
    std::string url_;
    std::string lastError_;
    std::atomic<bool> stopRequested_{false};
};

}  // namespace streamer
