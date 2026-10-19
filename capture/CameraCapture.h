#pragma once
// ============================================================================
// CameraCapture.h —— 用 FFmpeg libavdevice 采集摄像头
//
// Windows 上用 dshow（DirectShow），Linux 上可以用 v4l2 做测试。
// 流程：打开设备(avformat_open_input) -> 找视频流 -> 打开解码器
//       -> 循环 av_read_frame 读包 -> avcodec_send_packet / receive_frame 解码
// ============================================================================

#include "capture/ICapture.h"

#include <atomic>
#include <string>
#include <vector>

namespace streamer {

struct CameraOptions {
    std::string deviceName;   // dshow 设备名，例如 "Integrated Camera"
    std::string inputFormat;  // 为空则自动：Windows=dshow，Linux=v4l2
    std::string videoSize;    // 例如 "1280x720"，空 = 设备默认
    std::string frameRate;    // 例如 "30"，空 = 设备默认
    std::string pixelFormat;  // 例如 "yuyv422"，空 = 设备默认（和 vcodec 二选一）
    std::string vcodec;       // 例如 "mjpeg"，要求摄像头直接输出 MJPEG（高分辨率时常用）
    std::string rtbufsize = "100M";  // dshow 实时缓冲区，太小会出现 "real-time buffer full" 丢帧
};

// 一个设备的描述
struct CameraDeviceInfo {
    std::string name;         // 打开时用的名字
    std::string description;  // 给人看的描述
};

class CameraCapture : public ICapture {
public:
    explicit CameraCapture(CameraOptions options);
    ~CameraCapture() override;

    // 禁止拷贝（内部持有 FFmpeg 资源）
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    bool open() override;
    void close() override;
    ReadResult readFrame(AVFrame* frame) override;

    bool isOpen() const override { return formatCtx_ != nullptr; }
    VideoStreamInfo streamInfo() const override { return info_; }
    std::string lastError() const override { return lastError_; }
    void requestStop() override { stopRequested_ = true; }

    // 列出摄像头。成功返回 true；某些 FFmpeg 版本/设备不支持枚举，此时返回 false，
    // 可以再调用 printDevicesToLog() 让 FFmpeg 把设备列表打印到日志。
    static bool listDevices(const std::string& inputFormat,
                            std::vector<CameraDeviceInfo>& out,
                            std::string& error);

    // 等价于命令行 ffmpeg -f dshow -list_devices true -i dummy
    static void printDevicesToLog(const std::string& inputFormat);

    // 当前平台默认的输入设备格式名
    static std::string defaultInputFormat();

private:
    // FFmpeg 阻塞读取时会周期性调用这个回调，返回 1 表示“请中断”
    static int interruptCallback(void* opaque);

    void setError(const std::string& what, int errnum);

    CameraOptions options_;
    FormatInputPtr formatCtx_;
    CodecContextPtr codecCtx_;
    PacketPtr packet_;
    int videoStreamIndex_ = -1;
    bool decoderDrained_ = false;  // 已经向解码器发送了“结束”信号
    VideoStreamInfo info_;
    std::string lastError_;
    std::atomic<bool> stopRequested_{false};
};

}  // namespace streamer
