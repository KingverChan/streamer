#pragma once
// ============================================================================
// VideoEncoder.h —— 把原始图像帧编码成 H.264
//
// 采集到的帧是 YUYV422 / NV12 等“原始像素”，1280x720 一帧就要 1.8MB，
// 30fps 就是 55MB/s，网络根本传不动。所以推流前必须压缩（编码）成 H.264，
// 一般 2~4 Mbps 就够了，压缩了一百多倍。
//
// 流程：sws_scale 转成编码器要的像素格式（通常 yuv420p）
//       → avcodec_send_frame 送进编码器 → avcodec_receive_packet 取出压缩包
// ============================================================================

#include "core/FFmpegUtils.h"

#include <functional>
#include <string>

namespace streamer {

struct EncoderOptions {
    int width = 0;                 // 输出宽高（和输入不同时会自动缩放）
    int height = 0;
    AVRational frameRate{30, 1};   // 名义帧率，用来算 GOP 和码率控制
    int64_t bitRate = 2'000'000;   // 目标码率，单位 bit/s
    int gopSeconds = 2;            // 每隔几秒一个关键帧（I 帧）。观众中途进来要等到下一个关键帧才能看到画面
    std::string codecName;         // 为空则自动选择：libx264 → 硬件编码器 → h264_mf
    bool globalHeader = true;      // FLV/MP4 要求 SPS/PPS 放在 extradata 里，而不是每个关键帧前面
};

class VideoEncoder {
public:
    // 每编出一个压缩包就回调一次。回调返回 false 表示出错，编码会停止
    using PacketCallback = std::function<bool(AVPacket*)>;

    bool open(const EncoderOptions& options);
    void close();

    // 编码一帧。frame 可以是任意像素格式和尺寸；ptsMs 是这帧的时间戳（毫秒），必须递增
    bool encode(const AVFrame* frame, int64_t ptsMs, const PacketCallback& onPacket);

    // 结束时调用：让编码器把缓存在内部的帧全部吐出来
    bool flush(const PacketCallback& onPacket);

    bool isOpen() const { return codecCtx_ != nullptr; }
    const AVCodecContext* context() const { return codecCtx_.get(); }
    AVRational timeBase() const { return codecCtx_ ? codecCtx_->time_base : AVRational{1, 1000}; }
    const std::string& codecName() const { return codecName_; }
    const std::string& lastError() const { return lastError_; }

private:
    bool tryOpen(const AVCodec* codec, const EncoderOptions& options);
    bool convert(const AVFrame* src);
    bool drain(const PacketCallback& onPacket);  // 把编码器里现成的包都取出来

    CodecContextPtr codecCtx_;
    SwsContextPtr sws_;
    FramePtr encFrame_;   // 转换后、送进编码器的帧
    PacketPtr packet_;
    int srcW_ = 0, srcH_ = 0;
    AVPixelFormat srcFmt_ = AV_PIX_FMT_NONE;
    std::string codecName_;
    std::string lastError_;
};

}  // namespace streamer
