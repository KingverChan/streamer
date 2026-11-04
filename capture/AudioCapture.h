#pragma once
// ============================================================================
// AudioCapture.h —— 用 FFmpeg libavdevice 采集麦克风
//
// Windows 上用 dshow，设备 URL 是 "audio=麦克风名"（命令行等价于
//   ffmpeg -f dshow -audio_buffer_size 50 -i audio="麦克风 (Realtek Audio)"）。
// 流程和 CameraCapture 一样：打开设备 → 找音频流 → 打开解码器（pcm_s16le，几乎是直接拷贝）
//   → 循环 av_read_frame / avcodec_send_packet / avcodec_receive_frame
//
// 隐私：只有命令行传了 --audio 才会创建这个对象，默认不碰麦克风。
// ============================================================================

#include "core/FFmpegUtils.h"
#include "capture/ICapture.h"  // 复用 ReadResult

#include <atomic>
#include <string>
#include <vector>

namespace streamer {

struct AudioOptions {
    std::string deviceName;          // dshow 麦克风名，例如 "麦克风阵列 (Realtek(R) Audio)"
    std::string inputFormat;         // 为空则自动：Windows=dshow，Linux=pulse
    // dshow 默认每 500ms 才交一次数据，延迟太大。改成 50ms 一包，延迟小、CPU 也够用
    int bufferMs = 50;
    std::string sampleRate;          // 例如 "48000"，空 = 设备默认
    std::string channels;            // 例如 "2"，空 = 设备默认
};

class AudioCapture {
public:
    explicit AudioCapture(AudioOptions options);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool open();
    void close();

    // 读一帧解码后的 PCM（frame->nb_samples 个采样点）。调用方负责分配 frame
    ReadResult readFrame(AVFrame* frame);

    bool isOpen() const { return formatCtx_ != nullptr; }
    // 解码器上下文：里面有采样率 sample_rate、声道 ch_layout、采样格式 sample_fmt
    const AVCodecContext* decoderContext() const { return codecCtx_.get(); }
    std::string lastError() const { return lastError_; }
    void requestStop() { stopRequested_ = true; }

    // 列出 dshow 里的音频设备（麦克风）名字
    static bool listDevices(const std::string& inputFormat, std::vector<std::string>& out, std::string& error);
    static std::string defaultInputFormat();

private:
    static int interruptCallback(void* opaque);
    void setError(const std::string& what, int errnum);

    AudioOptions options_;
    FormatInputPtr formatCtx_;
    CodecContextPtr codecCtx_;
    PacketPtr packet_;
    int audioStreamIndex_ = -1;
    bool decoderDrained_ = false;
    std::string lastError_;
    std::atomic<bool> stopRequested_{false};
};

}  // namespace streamer
