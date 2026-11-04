#pragma once
// ============================================================================
// ScreenCapture.h —— 用 FFmpeg libavdevice 采集桌面
//
// Windows 上用 gdigrab（GDI 截屏），它在 FFmpeg 里和 dshow 一样，只是一种“输入设备”：
//   命令行等价于  ffmpeg -f gdigrab -framerate 30 -i desktop ...
// 流程和 CameraCapture 完全一样：
//   avformat_open_input("desktop") -> 找视频流 -> 打开解码器
//   -> 循环 av_read_frame 读包 -> avcodec_send_packet / receive_frame 解码
// gdigrab 每一包是一张 BMP 图（AV_CODEC_ID_BMP），用 bmp 解码器解成 BGRA/BGR24，
// 后面的 VideoEncoder 会用 sws_scale 转成 yuv420p 再编码。
//
// Linux 上用 x11grab（URL 是 ":0.0"），方便在没有 Windows 的地方测试。
// ============================================================================

#include "capture/ICapture.h"

#include <atomic>
#include <string>

namespace streamer {

struct ScreenOptions {
    // 采集目标：
    //   "desktop"          整个桌面（所有显示器拼起来的大画面）
    //   "title=窗口标题"    只抓某个窗口（gdigrab 支持）
    // 为空 = 自动：gdigrab 用 "desktop"，x11grab 用 ":0.0"
    std::string target;
    std::string inputFormat;      // 为空则自动：Windows=gdigrab，Linux=x11grab
    std::string frameRate = "30"; // gdigrab 默认 29.97（ntsc），这里改成整数 30
    std::string videoSize;        // 只抓一块区域时的大小，例如 "1280x720"，空 = 全屏
    int offsetX = 0;              // 区域左上角 x（配合 videoSize 用）
    int offsetY = 0;              // 区域左上角 y
    bool drawMouse = true;        // 画面里是否画出鼠标指针
    // false（默认）= 只抓主显示器；true = 抓所有显示器拼成的整个虚拟桌面。
    // 只在 target 是 "desktop" 且没指定 videoSize 时起作用
    bool allMonitors = false;
};

class ScreenCapture : public ICapture {
public:
    explicit ScreenCapture(ScreenOptions options);
    ~ScreenCapture() override;

    // 禁止拷贝（内部持有 FFmpeg 资源）
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool open() override;
    void close() override;
    ReadResult readFrame(AVFrame* frame) override;

    bool isOpen() const override { return formatCtx_ != nullptr; }
    VideoStreamInfo streamInfo() const override { return info_; }
    std::string lastError() const override { return lastError_; }
    void requestStop() override { stopRequested_ = true; }

    // 当前平台默认的桌面采集格式名
    static std::string defaultInputFormat();

private:
    static int interruptCallback(void* opaque);
    void setError(const std::string& what, int errnum);

    ScreenOptions options_;
    FormatInputPtr formatCtx_;
    CodecContextPtr codecCtx_;
    PacketPtr packet_;
    int videoStreamIndex_ = -1;
    bool decoderDrained_ = false;
    VideoStreamInfo info_;
    std::string lastError_;
    std::atomic<bool> stopRequested_{false};
};

}  // namespace streamer
