// ============================================================================
// AudioEncoder.cpp —— PCM → 重采样 → FIFO → AAC
// ============================================================================

#include "core/AudioEncoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace streamer {

bool AudioEncoder::open(const AVCodecContext* dec, int64_t bitRate) {
    close();
    // FFmpeg 自带的 AAC 编码器（名字就叫 "aac"），不需要额外的库
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        lastError_ = "找不到 AAC 编码器";
        return false;
    }
    codecCtx_.reset(avcodec_alloc_context3(codec));
    if (!codecCtx_) {
        lastError_ = "avcodec_alloc_context3 失败";
        return false;
    }
    AVCodecContext* c = codecCtx_.get();

    // 采样率：44100 / 48000 直接沿用，其他的统一转成 48000
    c->sample_rate = (dec->sample_rate == 44100 || dec->sample_rate == 48000) ? dec->sample_rate : 48000;
    // 声道：单声道麦克风就编单声道，其他一律立体声
    const int outChannels = dec->ch_layout.nb_channels == 1 ? 1 : 2;
    av_channel_layout_default(&c->ch_layout, outChannels);
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;          // FFmpeg aac 编码器只支持 fltp
    c->bit_rate = bitRate;
    c->time_base = AVRational{1, c->sample_rate};  // pts 单位 = 1 个采样点
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;       // FLV 要求 AudioSpecificConfig 放在 extradata

    int ret = avcodec_open2(c, codec, nullptr);
    if (ret < 0) {
        lastError_ = "打开 AAC 编码器失败: " + avErrorToString(ret);
        codecCtx_.reset();
        return false;
    }

    // 重采样器：输入（麦克风格式）→ 输出（编码器格式）
    AVChannelLayout inLayout{};
    if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || dec->ch_layout.nb_channels == 0)
        av_channel_layout_default(&inLayout, dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels : 2);
    else
        av_channel_layout_copy(&inLayout, &dec->ch_layout);
    SwrContext* swr = nullptr;
    // swr_alloc_set_opts2: 一次性设好输出/输入的声道布局、采样格式、采样率
    ret = swr_alloc_set_opts2(&swr, &c->ch_layout, c->sample_fmt, c->sample_rate,
                              &inLayout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (ret < 0 || !swr || (ret = swr_init(swr)) < 0) {
        swr_free(&swr);
        lastError_ = "初始化重采样失败: " + avErrorToString(ret);
        close();
        return false;
    }
    swr_.reset(swr);

    // FIFO 先给 1 秒的容量，不够会自动扩容
    fifo_.reset(av_audio_fifo_alloc(c->sample_fmt, c->ch_layout.nb_channels, c->sample_rate));
    encFrame_ = makeFrame();
    packet_ = makePacket();
    if (!fifo_ || !encFrame_ || !packet_) {
        lastError_ = "分配 FIFO/帧/包失败";
        close();
        return false;
    }
    nextPts_ = AV_NOPTS_VALUE;
    lastError_.clear();
    return true;
}

void AudioEncoder::close() {
    packet_.reset();
    encFrame_.reset();
    fifo_.reset();
    swr_.reset();
    codecCtx_.reset();
    nextPts_ = AV_NOPTS_VALUE;
}

bool AudioEncoder::encode(const AVFrame* in, int64_t startMs, const PacketCallback& onPacket) {
    if (!codecCtx_) {
        lastError_ = "音频编码器未打开";
        return false;
    }
    AVCodecContext* c = codecCtx_.get();

    // 1) 对齐时钟：第一帧时，用它的时间（毫秒）换算出起始 pts（采样点）。
    //    之后按采样点累加，声音不会因为系统调度抖动而断断续续。
    //    如果和系统时钟差太多（>200ms，例如麦克风丢过数据），就重新对齐，避免音画越走越偏。
    const int64_t startPts = av_rescale_q(startMs, AVRational{1, 1000}, c->time_base);
    const int64_t expected = (nextPts_ == AV_NOPTS_VALUE) ? AV_NOPTS_VALUE
                                                          : nextPts_ + av_audio_fifo_size(fifo_.get());
    if (expected == AV_NOPTS_VALUE || std::llabs(startPts - expected) > c->sample_rate / 5) {
        if (expected != AV_NOPTS_VALUE)
            av_log(nullptr, AV_LOG_WARNING, "音频时间戳偏差 %lld ms，重新对齐\n",
                   static_cast<long long>((startPts - expected) * 1000 / c->sample_rate));
        nextPts_ = startPts - av_audio_fifo_size(fifo_.get());
    }

    // 2) 重采样到编码器格式。swr_get_out_samples: 这次最多会输出多少个采样点
    const int maxOut = swr_get_out_samples(swr_.get(), in->nb_samples);
    if (maxOut < 0) {
        lastError_ = "swr_get_out_samples 失败";
        return false;
    }
    uint8_t** outData = nullptr;
    int ret = av_samples_alloc_array_and_samples(&outData, nullptr, c->ch_layout.nb_channels, maxOut,
                                                 c->sample_fmt, 0);
    if (ret < 0) {
        lastError_ = "分配重采样缓冲失败: " + avErrorToString(ret);
        return false;
    }
    const int got = swr_convert(swr_.get(), outData, maxOut,
                                const_cast<const uint8_t**>(in->extended_data), in->nb_samples);
    // 3) 放进 FIFO
    if (got > 0) ret = av_audio_fifo_write(fifo_.get(), reinterpret_cast<void**>(outData), got);
    av_freep(&outData[0]);
    av_freep(&outData);
    if (got < 0 || ret < 0) {
        lastError_ = "重采样/写 FIFO 失败";
        return false;
    }
    // 4) 攒够 1024 就编码
    return sendFifoFrames(onPacket, false);
}

bool AudioEncoder::sendFifoFrames(const PacketCallback& onPacket, bool flushing) {
    AVCodecContext* c = codecCtx_.get();
    const int frameSize = c->frame_size > 0 ? c->frame_size : 1024;
    while (av_audio_fifo_size(fifo_.get()) >= frameSize ||
           (flushing && av_audio_fifo_size(fifo_.get()) > 0)) {
        const int n = std::min(frameSize, av_audio_fifo_size(fifo_.get()));
        // 每次都准备一块新的缓冲：编码器可能还引用着上一帧的数据
        av_frame_unref(encFrame_.get());
        encFrame_->nb_samples = n;
        encFrame_->format = c->sample_fmt;
        encFrame_->sample_rate = c->sample_rate;
        av_channel_layout_copy(&encFrame_->ch_layout, &c->ch_layout);
        int ret = av_frame_get_buffer(encFrame_.get(), 0);
        if (ret < 0) {
            lastError_ = "av_frame_get_buffer 失败: " + avErrorToString(ret);
            return false;
        }
        if (av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(encFrame_->data), n) < n) {
            lastError_ = "读 FIFO 失败";
            return false;
        }
        encFrame_->pts = nextPts_;
        nextPts_ += n;

        ret = avcodec_send_frame(c, encFrame_.get());
        if (ret < 0) {
            lastError_ = "avcodec_send_frame(音频) 失败: " + avErrorToString(ret);
            return false;
        }
        if (!drain(onPacket)) return false;
    }
    return true;
}

bool AudioEncoder::drain(const PacketCallback& onPacket) {
    while (true) {
        int ret = avcodec_receive_packet(codecCtx_.get(), packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            lastError_ = "avcodec_receive_packet(音频) 失败: " + avErrorToString(ret);
            return false;
        }
        const bool ok = onPacket(packet_.get());
        av_packet_unref(packet_.get());
        if (!ok) return false;
    }
}

bool AudioEncoder::flush(const PacketCallback& onPacket) {
    if (!codecCtx_ || nextPts_ == AV_NOPTS_VALUE) return true;
    if (!sendFifoFrames(onPacket, true)) return false;
    avcodec_send_frame(codecCtx_.get(), nullptr);  // 空帧 = 通知编码器结束
    return drain(onPacket);
}

}  // namespace streamer
