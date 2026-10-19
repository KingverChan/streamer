#pragma once
// ============================================================================
// FrameSaver.h —— 把一帧图像保存成 BMP / PPM，方便用看图软件验证画面
//
// 摄像头出来的通常是 YUYV422 / NV12 / YUVJ422P 等 YUV 格式，看图软件不认识，
// 所以先用 libswscale 转成 RGB，再按 BMP / PPM 的文件格式写出去。
// ============================================================================

#include "core/FFmpegUtils.h"

#include <string>

namespace streamer {

class FrameSaver {
public:
    // 根据扩展名自动选择格式：.bmp 或 .ppm。成功返回 true，失败原因见 lastError()
    bool save(const AVFrame* frame, const std::string& path);

    bool saveBmp(const AVFrame* frame, const std::string& path);
    bool savePpm(const AVFrame* frame, const std::string& path);

    const std::string& lastError() const { return lastError_; }

private:
    // 把 frame 转成指定的打包 RGB 格式（RGB24 或 BGR24），结果放在 rgbFrame_
    bool convert(const AVFrame* frame, AVPixelFormat dstFormat);

    SwsContextPtr sws_;
    FramePtr rgbFrame_;
    // 记录 sws_ 当前对应的参数，参数变了才重建（分辨率/格式不变时复用，省开销）
    int srcW_ = 0, srcH_ = 0;
    AVPixelFormat srcFmt_ = AV_PIX_FMT_NONE;
    AVPixelFormat dstFmt_ = AV_PIX_FMT_NONE;
    std::string lastError_;
};

}  // namespace streamer
