#pragma once
// ============================================================================
// ICapture.h —— 采集源的统一接口
//
// 现在只有 CameraCapture（摄像头）。以后要加：
//   - ScreenCapture（gdigrab / DXGI 桌面采集）
// 它们都实现这个接口，后面的 Encoder / RtmpPublisher 只依赖 ICapture，
// 不关心画面到底来自摄像头还是桌面。
// ============================================================================

#include "core/FFmpegUtils.h"

#include <string>

namespace streamer {

// readFrame 的返回状态
enum class ReadResult {
    Ok,     // 成功拿到一帧，frame 已填好
    Again,  // 这次没拿到完整帧（例如解码器需要更多数据），再调一次即可
    Eof,    // 采集源结束（设备拔出 / 文件读完）
    Error,  // 出错，具体原因看 lastError()
};

// 采集到的视频流的基本信息
struct VideoStreamInfo {
    int width = 0;
    int height = 0;
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;  // 解码后的像素格式
    AVRational timeBase{0, 1};                    // pts 的时间单位（秒 = pts * timeBase）
    AVRational frameRate{0, 1};                   // 设备报告的帧率
};

class ICapture {
public:
    virtual ~ICapture() = default;

    // 打开采集源。失败返回 false，原因见 lastError()
    virtual bool open() = 0;

    // 关闭并释放所有资源。可以重复调用
    virtual void close() = 0;

    // 读取一帧“已解码的原始图像”到 frame 中。
    // 调用方负责分配 frame（makeFrame()），本函数会先 av_frame_unref 再写入。
    virtual ReadResult readFrame(AVFrame* frame) = 0;

    virtual bool isOpen() const = 0;
    virtual VideoStreamInfo streamInfo() const = 0;
    virtual std::string lastError() const = 0;

    // 让阻塞中的 readFrame 尽快返回（用于 Ctrl+C 退出），可以从其他线程调用
    virtual void requestStop() = 0;
};

}  // namespace streamer
