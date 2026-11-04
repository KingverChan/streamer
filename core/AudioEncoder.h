#pragma once
// ============================================================================
// AudioEncoder.h —— 把麦克风的 PCM 编码成 AAC
//
// 为什么要这几步：
//   1) 重采样（swr_convert）：麦克风给的是 s16 交错格式（LRLRLR...），
//      AAC 编码器要的是 fltp 平面格式（LLLL... RRRR...），采样率/声道数也可能不同
//   2) FIFO 攒帧（AVAudioFifo）：麦克风一包可能是 480 个采样点（10ms）、2205 个……
//      而 AAC 每帧必须正好 1024 个采样点，所以先放进 FIFO，攒够 1024 再送编码器
//   3) 时间戳：用“第几个采样点”当 pts（time_base = 1/采样率），比用系统时钟平滑；
//      起点和视频对齐到同一个时钟（见 encode 的 startMs 参数）
// ============================================================================

#include "core/FFmpegUtils.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

#include <functional>
#include <string>

namespace streamer {

class AudioEncoder {
public:
    using PacketCallback = std::function<bool(AVPacket*)>;

    AudioEncoder() = default;
    ~AudioEncoder() { close(); }
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // decoder: 麦克风解码器的上下文（从里面拿输入的采样率、声道、格式）
    bool open(const AVCodecContext* decoder, int64_t bitRate = 128000);
    void close();

    // 编码一帧 PCM。startMs: 这帧第一个采样点对应的时间（毫秒，和视频同一个时钟）
    bool encode(const AVFrame* frame, int64_t startMs, const PacketCallback& onPacket);
    // 结束时把 FIFO 和编码器里剩下的数据都吐出来
    bool flush(const PacketCallback& onPacket);

    const AVCodecContext* context() const { return codecCtx_.get(); }
    AVRational timeBase() const { return codecCtx_ ? codecCtx_->time_base : AVRational{1, 48000}; }
    const std::string& lastError() const { return lastError_; }

private:
    bool sendFifoFrames(const PacketCallback& onPacket, bool flushing);
    bool drain(const PacketCallback& onPacket);

    struct SwrDeleter {
        void operator()(SwrContext* s) const { if (s) swr_free(&s); }
    };
    struct FifoDeleter {
        void operator()(AVAudioFifo* f) const { if (f) av_audio_fifo_free(f); }
    };

    CodecContextPtr codecCtx_;
    std::unique_ptr<SwrContext, SwrDeleter> swr_;
    std::unique_ptr<AVAudioFifo, FifoDeleter> fifo_;
    FramePtr encFrame_;
    PacketPtr packet_;
    int64_t nextPts_ = AV_NOPTS_VALUE;  // 下一帧送进编码器的 pts（单位：采样点）
    std::string lastError_;
};

}  // namespace streamer
