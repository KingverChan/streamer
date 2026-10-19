// ============================================================================
// FrameSaver.cpp
// ============================================================================

#include "core/FrameSaver.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace streamer {

namespace {

// 判断字符串结尾（忽略大小写只处理了常见写法）
bool endsWith(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != suffix[i]) return false;
    }
    return true;
}

// 按小端序写 2 / 4 字节整数（BMP 文件头要求小端）
void writeLE16(std::ofstream& out, uint16_t v) {
    char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    out.write(b, 2);
}
void writeLE32(std::ofstream& out, uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    out.write(b, 4);
}

}  // namespace

bool FrameSaver::save(const AVFrame* frame, const std::string& path) {
    if (endsWith(path, ".ppm")) return savePpm(frame, path);
    return saveBmp(frame, path);  // 默认 BMP，Windows 自带的照片查看器就能打开
}

bool FrameSaver::convert(const AVFrame* frame, AVPixelFormat dstFormat) {
    const auto srcFmt = static_cast<AVPixelFormat>(frame->format);
    if (frame->width <= 0 || frame->height <= 0 || srcFmt == AV_PIX_FMT_NONE) {
        lastError_ = "帧数据无效（宽高或像素格式为空）";
        return false;
    }

    // 参数变化时才重新创建 SwsContext
    if (!sws_ || srcW_ != frame->width || srcH_ != frame->height || srcFmt_ != srcFmt ||
        dstFmt_ != dstFormat) {
        // sws_getContext: 创建转换器（源宽高/格式 -> 目标宽高/格式，缩放算法）
        // 这里宽高不变，只做像素格式转换；SWS_BILINEAR 是常用的插值算法
        sws_.reset(sws_getContext(frame->width, frame->height, srcFmt,
                                  frame->width, frame->height, dstFormat,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws_) {
            lastError_ = "sws_getContext 失败，不支持的像素格式转换";
            return false;
        }
        srcW_ = frame->width;
        srcH_ = frame->height;
        srcFmt_ = srcFmt;
        dstFmt_ = dstFormat;

        // 准备目标帧：设置格式和宽高后，av_frame_get_buffer 按对齐要求分配像素内存
        rgbFrame_ = makeFrame();
        if (!rgbFrame_) {
            lastError_ = "av_frame_alloc 失败";
            return false;
        }
        rgbFrame_->format = dstFormat;
        rgbFrame_->width = frame->width;
        rgbFrame_->height = frame->height;
        int ret = av_frame_get_buffer(rgbFrame_.get(), 0);  // 0 = 自动选择对齐
        if (ret < 0) {
            lastError_ = "av_frame_get_buffer 失败: " + avErrorToString(ret);
            rgbFrame_.reset();
            sws_.reset();
            return false;
        }
    }

    // sws_scale: 执行转换。参数依次是：源各平面指针、源每行字节数、
    // 从第 0 行开始、处理全部高度、目标各平面指针、目标每行字节数
    sws_scale(sws_.get(), frame->data, frame->linesize, 0, frame->height,
              rgbFrame_->data, rgbFrame_->linesize);
    return true;
}

// PPM (P6) 格式：一行文本头 "P6 宽 高 255"，后面紧跟 RGB 字节，从上到下。最简单的图片格式
bool FrameSaver::savePpm(const AVFrame* frame, const std::string& path) {
    if (!convert(frame, AV_PIX_FMT_RGB24)) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        lastError_ = "无法创建文件: " + path;
        return false;
    }
    out << "P6\n" << rgbFrame_->width << " " << rgbFrame_->height << "\n255\n";
    const int rowBytes = rgbFrame_->width * 3;
    for (int y = 0; y < rgbFrame_->height; ++y) {
        // linesize 可能大于 width*3（有对齐填充），所以要按行写
        out.write(reinterpret_cast<const char*>(rgbFrame_->data[0] + y * rgbFrame_->linesize[0]), rowBytes);
    }
    return static_cast<bool>(out);
}

// BMP (24 位) 格式：14 字节文件头 + 40 字节信息头 + 像素数据
// 注意两点：像素是 BGR 顺序；行默认从下往上存；每行要补齐到 4 字节的倍数
bool FrameSaver::saveBmp(const AVFrame* frame, const std::string& path) {
    if (!convert(frame, AV_PIX_FMT_BGR24)) return false;

    const int w = rgbFrame_->width;
    const int h = rgbFrame_->height;
    const int rowBytes = w * 3;
    const int paddedRow = (rowBytes + 3) & ~3;  // 向上取整到 4 的倍数
    const uint32_t imageSize = static_cast<uint32_t>(paddedRow) * h;
    const uint32_t headerSize = 14 + 40;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        lastError_ = "无法创建文件: " + path;
        return false;
    }

    // BITMAPFILEHEADER
    out.write("BM", 2);
    writeLE32(out, headerSize + imageSize);  // 文件总大小
    writeLE16(out, 0);                       // 保留
    writeLE16(out, 0);                       // 保留
    writeLE32(out, headerSize);              // 像素数据的偏移

    // BITMAPINFOHEADER
    writeLE32(out, 40);                       // 信息头大小
    writeLE32(out, static_cast<uint32_t>(w));
    writeLE32(out, static_cast<uint32_t>(h));  // 正数 = 从下往上存
    writeLE16(out, 1);                        // 平面数，固定 1
    writeLE16(out, 24);                       // 每像素 24 位
    writeLE32(out, 0);                        // 不压缩 (BI_RGB)
    writeLE32(out, imageSize);
    writeLE32(out, 2835);                     // 水平分辨率 72 DPI
    writeLE32(out, 2835);                     // 垂直分辨率
    writeLE32(out, 0);                        // 调色板颜色数
    writeLE32(out, 0);                        // 重要颜色数

    std::vector<char> padding(paddedRow - rowBytes, 0);
    for (int y = h - 1; y >= 0; --y) {  // 从最后一行开始写
        out.write(reinterpret_cast<const char*>(rgbFrame_->data[0] + y * rgbFrame_->linesize[0]), rowBytes);
        if (!padding.empty()) out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }
    return static_cast<bool>(out);
}

}  // namespace streamer
