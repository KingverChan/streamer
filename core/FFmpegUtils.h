#pragma once
// ============================================================================
// FFmpegUtils.h —— FFmpeg 公共工具
//
// 1. FFmpeg 是 C 库，所有对象都要手动 alloc / free。这里用 std::unique_ptr
//    + 自定义 deleter 把它们包成 RAII 对象：离开作用域自动释放，不会漏。
// 2. FFmpeg 的错误码是负数 int，用 av_strerror 转成人能看懂的字符串。
// ============================================================================

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace streamer {

// ---------------------------------------------------------------------------
// 错误处理
// ---------------------------------------------------------------------------

// 把 FFmpeg 错误码（负数）转成可读字符串，例如 -5 -> "I/O error"
inline std::string avErrorToString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    // av_strerror: 根据错误码填充描述文字；找不到描述时返回 <0，但 buf 里仍有通用信息
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf) + " (code " + std::to_string(errnum) + ")";
}

// ---------------------------------------------------------------------------
// RAII 包装：每个 deleter 调用对应的 FFmpeg 释放函数
// ---------------------------------------------------------------------------

// AVFormatContext：输入容器/设备的上下文（相当于“打开的文件/摄像头”）
// 注意：通过 avformat_open_input 打开的必须用 avformat_close_input 关闭，
// 它会顺带释放 context 本身并把指针置空。
struct FormatInputDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);
    }
};
using FormatInputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;

// AVCodecContext：解码器上下文（摄像头可能输出 MJPEG 等压缩数据，需要解码成原始像素）
struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        // FFmpeg 7 中 avcodec_close 已废弃，直接 avcodec_free_context 即可
        if (ctx) avcodec_free_context(&ctx);
    }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

// AVPacket：压缩数据包（从设备读出来的原始一包数据）
struct PacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);  // 内部会先 unref 再释放
    }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// AVFrame：解码后的一帧图像（原始像素 + 宽高 + 格式 + pts）
struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);  // 内部会先 unref 再释放
    }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// SwsContext：像素格式转换/缩放上下文（例如 YUYV422 -> RGB24）
struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const {
        if (ctx) sws_freeContext(ctx);
    }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

// AVDictionary：FFmpeg 的 key=value 选项表（打开设备时传 video_size、framerate 等）
struct DictionaryDeleter {
    void operator()(AVDictionary* dict) const {
        if (dict) av_dict_free(&dict);
    }
};

// 工厂函数：分配失败时返回空指针，调用方要检查
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }
inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }

}  // namespace streamer
