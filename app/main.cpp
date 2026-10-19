// ============================================================================
// main.cpp —— 摄像头采集小工具的命令行入口
//
// 用法见 printUsage()。不带参数运行时会列出摄像头设备。
// ============================================================================

#include "capture/CameraCapture.h"
#include "core/FrameSaver.h"

extern "C" {
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace streamer;

namespace {

// ---------------------------------------------------------------------------
// Ctrl+C 处理：只设置标志位，主循环看到后正常退出，unique_ptr 会自动释放资源
// ---------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
ICapture* g_capture = nullptr;  // 让信号处理函数能通知采集对象中断阻塞

void requestStop() {
    g_stop = true;
    if (g_capture) g_capture->requestStop();  // 内部只是写一个 atomic<bool>
}

#ifdef _WIN32
// Windows 控制台的 Ctrl+C 回调运行在单独的线程里
BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        requestStop();
        return TRUE;  // TRUE = 我们处理了，不要直接杀进程
    }
    return FALSE;
}
#else
void signalHandler(int) { requestStop(); }
#endif

struct AppOptions {
    CameraOptions camera;
    bool listOnly = false;
    int saveCount = 0;            // 保存前 N 帧
    std::string outDir = "frames";
    std::string imageExt = "bmp";  // bmp 或 ppm
    long long maxFrames = 0;       // 读多少帧后自动退出，0 = 一直读到 Ctrl+C
    bool verbose = false;
};

void printUsage(const char* prog) {
    std::cout <<
        "用法:\n"
        "  " << prog << "                       列出摄像头设备\n"
        "  " << prog << " --list                列出摄像头设备\n"
        "  " << prog << " -d \"设备名\" [选项]     打开摄像头并读取帧\n"
        "\n"
        "选项:\n"
        "  -d, --device NAME     摄像头名字（用 --list 查看），Linux 上是 /dev/video0\n"
        "  -s, --size WxH        分辨率，例如 1280x720\n"
        "  -r, --fps N           帧率，例如 30\n"
        "      --pix-fmt FMT     摄像头原始像素格式，例如 yuyv422 / nv12\n"
        "      --vcodec CODEC    让摄像头输出压缩格式，例如 mjpeg（高分辨率高帧率时常用）\n"
        "      --save N          把前 N 帧保存成图片（默认 0 不保存）\n"
        "      --out DIR         图片保存目录（默认 frames）\n"
        "      --ext bmp|ppm     图片格式（默认 bmp）\n"
        "  -n, --frames N        读 N 帧后自动退出（默认一直读，按 Ctrl+C 退出）\n"
        "  -f, --format NAME     输入设备类型，默认 Windows=dshow，Linux=v4l2\n"
        "      --rtbufsize SIZE  dshow 实时缓冲区大小（默认 100M）\n"
        "  -v, --verbose         打印 FFmpeg 调试日志\n"
        "  -h, --help            显示帮助\n"
        "\n"
        "示例:\n"
        "  " << prog << " -d \"Integrated Camera\" -s 1280x720 -r 30 --save 5\n";
}

// 简单的命令行解析。返回 false 表示参数有误或只需显示帮助
bool parseArgs(int argc, char** argv, AppOptions& opt, int& exitCode) {
    exitCode = 0;
    if (argc <= 1) {
        opt.listOnly = true;
        return true;
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // 取下一个参数作为值；缺失时抛异常，由下面的 catch 统一报错
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("参数 " + a + " 需要一个值");
            return argv[++i];
        };
        // 把字符串转成整数，不是合法数字时抛异常
        auto toInt = [&](const std::string& v) -> long long {
            size_t pos = 0;
            long long n = -1;
            try {
                n = std::stoll(v, &pos);
            } catch (const std::invalid_argument&) {
                pos = 0;  // stoll 遇到 "abc" 这种完全不是数字的会抛异常，统一当作不合法
            }
            if (pos == 0 || pos != v.size() || n < 0) throw std::invalid_argument("参数 " + a + " 的值不合法: " + v);
            return n;
        };
        try {
            if (a == "-h" || a == "--help") { printUsage(argv[0]); return false; }
            else if (a == "-l" || a == "--list") opt.listOnly = true;
            else if (a == "-d" || a == "--device") opt.camera.deviceName = next();
            else if (a == "-s" || a == "--size") opt.camera.videoSize = next();
            else if (a == "-r" || a == "--fps") opt.camera.frameRate = next();
            else if (a == "--pix-fmt") opt.camera.pixelFormat = next();
            else if (a == "--vcodec") opt.camera.vcodec = next();
            else if (a == "-f" || a == "--format") opt.camera.inputFormat = next();
            else if (a == "--rtbufsize") opt.camera.rtbufsize = next();
            else if (a == "--out") opt.outDir = next();
            else if (a == "--ext") opt.imageExt = next();
            else if (a == "--save") opt.saveCount = static_cast<int>(toInt(next()));
            else if (a == "-n" || a == "--frames") opt.maxFrames = toInt(next());
            else if (a == "-v" || a == "--verbose") opt.verbose = true;
            else throw std::invalid_argument("未知参数: " + a);
        } catch (const std::invalid_argument& e) {
            std::cerr << e.what() << "\n\n";
            printUsage(argv[0]);
            exitCode = 1;
            return false;
        } catch (const std::out_of_range&) {
            std::cerr << "参数 " << a << " 的数值太大\n";
            exitCode = 1;
            return false;
        }
    }
    if (!opt.listOnly && opt.camera.deviceName.empty()) opt.listOnly = true;  // 没给设备名就列设备
    if (opt.imageExt != "bmp" && opt.imageExt != "ppm") {
        std::cerr << "--ext 只支持 bmp 或 ppm\n";
        exitCode = 1;
        return false;
    }
    return true;
}

int listCameras(const std::string& inputFormat) {
    std::string fmt = inputFormat.empty() ? CameraCapture::defaultInputFormat() : inputFormat;
    std::vector<CameraDeviceInfo> devices;
    std::string error;
    if (CameraCapture::listDevices(fmt, devices, error)) {
        if (devices.empty()) {
            std::cout << "没有找到视频设备。\n";
        } else {
            std::cout << "找到 " << devices.size() << " 个视频设备 (" << fmt << "):\n";
            for (size_t i = 0; i < devices.size(); ++i) {
                std::cout << "  [" << i << "] \"" << devices[i].name << "\"\n";
                if (!devices[i].description.empty())
                    std::cout << "      " << devices[i].description << "\n";
            }
            std::cout << "\n用法示例: -d \"" << devices[0].name << "\"\n";
        }
        return 0;
    }

    // 有些情况下枚举接口不可用，退回到让 FFmpeg 自己打印设备列表
    std::cout << "无法通过 API 枚举设备（" << error << "）\n"
              << "改用 list_devices 选项，设备列表会出现在下面的 FFmpeg 日志里：\n\n";
    CameraCapture::printDevicesToLog(fmt);
    std::cout << "\n也可以在命令行运行: ffmpeg -f " << fmt << " -list_devices true -i dummy\n";
    return 0;
}

std::string pixFmtName(AVPixelFormat fmt) {
    // av_get_pix_fmt_name: 像素格式枚举 -> 名字，例如 AV_PIX_FMT_YUYV422 -> "yuyv422"
    const char* name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // 让控制台正确显示 UTF-8 中文（源码用 /utf-8 编译）
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    AppOptions opt;
    int exitCode = 0;
    if (!parseArgs(argc, argv, opt, exitCode)) return exitCode;

    // av_log_set_level: 控制 FFmpeg 内部日志的详细程度
    av_log_set_level(opt.verbose ? AV_LOG_DEBUG : AV_LOG_INFO);

    if (opt.listOnly) return listCameras(opt.camera.inputFormat);

    // 用接口指针持有具体实现：以后换成 ScreenCapture 只需改这一行
    std::unique_ptr<ICapture> capture = std::make_unique<CameraCapture>(opt.camera);
    g_capture = capture.get();

    std::cout << "正在打开摄像头: " << opt.camera.deviceName << " ...\n";
    if (!capture->open()) {
        std::cerr << "错误: " << capture->lastError() << "\n"
                  << "提示: 先不带参数运行查看设备名；分辨率/帧率必须是摄像头支持的组合，\n"
                  << "      可用 ffmpeg -f dshow -list_options true -i video=\"设备名\" 查看。\n";
        g_capture = nullptr;
        return 1;
    }

    VideoStreamInfo info = capture->streamInfo();
    std::cout << "打开成功: " << info.width << "x" << info.height
              << " 设备帧率=" << av_q2d(info.frameRate)
              << " time_base=" << info.timeBase.num << "/" << info.timeBase.den << "\n"
              << "按 Ctrl+C 退出\n\n";

    if (opt.saveCount > 0) {
        std::error_code ec;
        std::filesystem::create_directories(opt.outDir, ec);
        if (ec) {
            std::cerr << "无法创建目录 " << opt.outDir << ": " << ec.message() << "\n";
            opt.saveCount = 0;
        }
    }

    FramePtr frame = makeFrame();
    if (!frame) {
        std::cerr << "av_frame_alloc 失败\n";
        return 1;
    }
    FrameSaver saver;

    using Clock = std::chrono::steady_clock;
    std::deque<Clock::time_point> recent;  // 最近 1 秒内收到帧的时间，用来算实际帧率
    long long frameIndex = 0;
    int saved = 0;
    int result = 0;

    while (!g_stop) {
        ReadResult r = capture->readFrame(frame.get());
        if (r == ReadResult::Again) continue;
        if (r == ReadResult::Eof) {
            if (!g_stop) std::cout << "采集结束: " << capture->lastError() << "\n";
            break;
        }
        if (r == ReadResult::Error) {
            std::cerr << "读取失败: " << capture->lastError() << "\n";
            result = 1;
            break;
        }

        // 计算实际帧率：统计最近 1 秒内收到的帧数
        auto now = Clock::now();
        recent.push_back(now);
        while (!recent.empty() && now - recent.front() > std::chrono::seconds(1)) recent.pop_front();
        double fps = 0.0;
        if (recent.size() >= 2) {
            double span = std::chrono::duration<double>(recent.back() - recent.front()).count();
            if (span > 0) fps = (recent.size() - 1) / span;
        }

        // pts 是以 time_base 为单位的时间戳，乘以 time_base 换算成秒
        int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        char line[256];
        if (pts != AV_NOPTS_VALUE) {
            std::snprintf(line, sizeof(line), "帧 #%-6lld %dx%d  %-10s pts=%lld (%.3fs)  实际帧率=%.2f",
                          frameIndex, frame->width, frame->height,
                          pixFmtName(static_cast<AVPixelFormat>(frame->format)).c_str(),
                          static_cast<long long>(pts), pts * av_q2d(info.timeBase), fps);
        } else {
            std::snprintf(line, sizeof(line), "帧 #%-6lld %dx%d  %-10s pts=N/A  实际帧率=%.2f",
                          frameIndex, frame->width, frame->height,
                          pixFmtName(static_cast<AVPixelFormat>(frame->format)).c_str(), fps);
        }
        std::cout << line;

        if (saved < opt.saveCount) {
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%04lld.%s", frameIndex, opt.imageExt.c_str());
            std::filesystem::path path = std::filesystem::path(opt.outDir) / name;
            if (saver.save(frame.get(), path.string())) {
                std::cout << "  -> 已保存 " << path.string();
                ++saved;
            } else {
                std::cout << "  -> 保存失败: " << saver.lastError();
                opt.saveCount = saved;  // 出错就不再尝试
            }
        }
        std::cout << "\n";

        ++frameIndex;
        if (opt.maxFrames > 0 && frameIndex >= opt.maxFrames) break;
    }

    if (g_stop) std::cout << "\n收到 Ctrl+C，正在退出...\n";
    capture->close();  // 显式关闭（不调用的话析构时也会关）
    g_capture = nullptr;
    std::cout << "共读取 " << frameIndex << " 帧，保存 " << saved << " 张图片。\n";
    return result;
}
