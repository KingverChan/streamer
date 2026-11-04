// ============================================================================
// AudioCapture.cpp —— 麦克风采集的实现（dshow）
// 结构和 CameraCapture.cpp 一一对应，只是找的是音频流
// ============================================================================

#include "capture/AudioCapture.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <utility>

namespace streamer {

AudioCapture::AudioCapture(AudioOptions options) : options_(std::move(options)) {
    if (options_.inputFormat.empty()) options_.inputFormat = defaultInputFormat();
}

AudioCapture::~AudioCapture() { close(); }

std::string AudioCapture::defaultInputFormat() {
#ifdef _WIN32
    return "dshow";
#elif defined(__APPLE__)
    return "avfoundation";
#else
    return "pulse";
#endif
}

void AudioCapture::setError(const std::string& what, int errnum) {
    lastError_ = what;
    if (errnum < 0) lastError_ += ": " + avErrorToString(errnum);
}

int AudioCapture::interruptCallback(void* opaque) {
    return static_cast<AudioCapture*>(opaque)->stopRequested_ ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 列出麦克风：和 CameraCapture::listDevices 一样，只是保留 AVMEDIA_TYPE_AUDIO 的设备
// ---------------------------------------------------------------------------
bool AudioCapture::listDevices(const std::string& inputFormat, std::vector<std::string>& out,
                               std::string& error) {
    avdevice_register_all();
    const AVInputFormat* ifmt = av_find_input_format(inputFormat.c_str());
    if (!ifmt) {
        error = "找不到输入格式 '" + inputFormat + "'";
        return false;
    }
    AVDeviceInfoList* list = nullptr;
    int ret = avdevice_list_input_sources(ifmt, nullptr, nullptr, &list);
    if (ret < 0) {
        error = "avdevice_list_input_sources 失败: " + avErrorToString(ret);
        avdevice_free_list_devices(&list);
        return false;
    }
    for (int i = 0; i < list->nb_devices; ++i) {
        const AVDeviceInfo* dev = list->devices[i];
        bool isAudio = false;
        for (int t = 0; t < dev->nb_media_types; ++t)
            if (dev->media_types[t] == AVMEDIA_TYPE_AUDIO) isAudio = true;
        if (!isAudio) continue;
        // dshow 的 device_description 是友好名（打开时用它）
        const char* name = dev->device_description ? dev->device_description : dev->device_name;
        if (name) out.emplace_back(name);
    }
    avdevice_free_list_devices(&list);
    return true;
}

// ---------------------------------------------------------------------------
// 打开麦克风
// ---------------------------------------------------------------------------
bool AudioCapture::open() {
    close();
    stopRequested_ = false;
    decoderDrained_ = false;

    avdevice_register_all();
    const AVInputFormat* ifmt = av_find_input_format(options_.inputFormat.c_str());
    if (!ifmt) {
        setError("找不到输入格式 '" + options_.inputFormat + "'", 0);
        return false;
    }

    // dshow 的 URL 是 "audio=设备名"
    std::string url = options_.deviceName;
    if (options_.inputFormat == "dshow" && url.rfind("audio=", 0) != 0) url = "audio=" + url;

    AVDictionary* rawOpts = nullptr;
    if (options_.inputFormat == "dshow") {
        // audio_buffer_size（毫秒）: dshow 每攒够这么多声音才交一包。默认约 500ms，
        // 意味着声音天生比画面晚半秒，所以改小
        if (options_.bufferMs > 0) av_dict_set_int(&rawOpts, "audio_buffer_size", options_.bufferMs, 0);
    }
    if (!options_.sampleRate.empty()) av_dict_set(&rawOpts, "sample_rate", options_.sampleRate.c_str(), 0);
    if (!options_.channels.empty()) av_dict_set(&rawOpts, "channels", options_.channels.c_str(), 0);

    AVFormatContext* rawCtx = avformat_alloc_context();
    if (!rawCtx) {
        av_dict_free(&rawOpts);
        setError("avformat_alloc_context 失败", 0);
        return false;
    }
    rawCtx->interrupt_callback.callback = &AudioCapture::interruptCallback;
    rawCtx->interrupt_callback.opaque = this;

    int ret = avformat_open_input(&rawCtx, url.c_str(), ifmt, &rawOpts);
    std::unique_ptr<AVDictionary, DictionaryDeleter> optsGuard(rawOpts);
    if (ret < 0) {
        setError("打开麦克风失败 [" + url + "]（名字要和 --list-audio 打印的完全一致）", ret);
        return false;
    }
    formatCtx_.reset(rawCtx);

    const AVDictionaryEntry* unused = nullptr;
    while ((unused = av_dict_iterate(optsGuard.get(), unused)))
        av_log(nullptr, AV_LOG_WARNING, "选项未被设备使用: %s=%s\n", unused->key, unused->value);

    ret = avformat_find_stream_info(formatCtx_.get(), nullptr);
    if (ret < 0) {
        setError("avformat_find_stream_info 失败", ret);
        close();
        return false;
    }
    av_dump_format(formatCtx_.get(), 0, url.c_str(), 0);

    // 找音频流（dshow 麦克风输出 pcm_s16le，对应的“解码器”只是把字节整理成 AVFrame）
    const AVCodec* decoder = nullptr;
    ret = av_find_best_stream(formatCtx_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (ret < 0 || !decoder) {
        setError("设备里没有找到音频流", ret);
        close();
        return false;
    }
    audioStreamIndex_ = ret;
    AVStream* stream = formatCtx_->streams[audioStreamIndex_];

    codecCtx_.reset(avcodec_alloc_context3(decoder));
    if (!codecCtx_) {
        setError("avcodec_alloc_context3 失败", 0);
        close();
        return false;
    }
    ret = avcodec_parameters_to_context(codecCtx_.get(), stream->codecpar);
    if (ret >= 0) {
        codecCtx_->pkt_timebase = stream->time_base;
        ret = avcodec_open2(codecCtx_.get(), decoder, nullptr);
    }
    if (ret < 0) {
        setError("打开音频解码器失败", ret);
        close();
        return false;
    }
    packet_ = makePacket();
    if (!packet_) {
        setError("av_packet_alloc 失败", 0);
        close();
        return false;
    }
    lastError_.clear();
    return true;
}

void AudioCapture::close() {
    packet_.reset();
    codecCtx_.reset();
    formatCtx_.reset();
    audioStreamIndex_ = -1;
}

// ---------------------------------------------------------------------------
// 读一帧 PCM：送包 / 取帧，和视频完全一样
// ---------------------------------------------------------------------------
ReadResult AudioCapture::readFrame(AVFrame* frame) {
    if (!isOpen()) {
        setError("麦克风未打开", 0);
        return ReadResult::Error;
    }
    av_frame_unref(frame);
    while (true) {
        int ret = avcodec_receive_frame(codecCtx_.get(), frame);
        if (ret == 0) return ReadResult::Ok;
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
            else setError("音频源已结束", ret);
            avcodec_send_packet(codecCtx_.get(), nullptr);
            decoderDrained_ = true;
            continue;
        }
        if (ret < 0) {
            setError("读取麦克风失败（被占用或拔出？）", ret);
            return ReadResult::Error;
        }
        if (packet_->stream_index != audioStreamIndex_) {
            av_packet_unref(packet_.get());
            continue;
        }
        ret = avcodec_send_packet(codecCtx_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            setError("avcodec_send_packet 失败", ret);
            return ReadResult::Error;
        }
    }
}

}  // namespace streamer
