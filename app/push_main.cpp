// ============================================================================
// push_main.cpp —— 摄像头采集 → H.264 编码 → RTMP 推流
//
// 线程模型：采集线程读摄像头 → FrameQueue（最多 3 帧，满了丢最旧的）→ 主线程编码并推流
//
// 数据流（每一帧都走一遍）：
//   CameraCapture::readFrame   原始帧（yuyv422 / nv12 / mjpeg 解码后）
//        ↓
//   VideoEncoder::encode       转 yuv420p → H.264 压缩包
//        ↓
//   RtmpPublisher::writePacket FLV 封装 → RTMP 发给服务器
// ============================================================================

#include "capture/CameraCapture.h"
#include "core/FrameQueue.h"
#include "core/RtmpPublisher.h"
#include "core/VideoEncoder.h"

extern "C" {
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace streamer;

namespace {

// ---------------------------------------------------------------------------
// Ctrl+C：设置标志位，并通知采集和推流对象中断阻塞调用
// ---------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
ICapture* g_capture = nullptr;
RtmpPublisher* g_publisher = nullptr;

void requestStop() {
    g_stop = true;
    if (g_capture) g_capture->requestStop();
    if (g_publisher) g_publisher->requestStop();
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        requestStop();
        return TRUE;
    }
    return FALSE;
}
#else
void signalHandler(int) { requestStop(); }
#endif

struct PushOptions {
    CameraOptions camera;
    std::string url = "rtmp://localhost/live/test";
    std::string encoder;          // 空 = 自动选择
    int bitrateKbps = 2000;
    int gopSeconds = 1;           // 关键帧间隔（秒）。越小，观众起播越快、延迟越低，但同码率下画质稍差
    std::string outSize;          // 输出分辨率，空 = 和采集一样
    long long maxFrames = 0;
    bool verbose = false;
};

void printUsage(const char* prog) {
    std::cout <<
        "用法:\n"
        "  " << prog << " -d \"设备名\" -o rtmp://服务器/live/流名 [选项]\n"
        "\n"
        "选项:\n"
        "  -d, --device NAME     摄像头名字（用 camera_capture --list 查看）\n"
        "  -o, --output URL      推流地址，默认 rtmp://localhost/live/test\n"
        "                        也可以写本地文件名，例如 out.flv，用来离线测试\n"
        "  -s, --size WxH        采集分辨率，例如 1280x720\n"
        "  -r, --fps N           采集帧率，例如 30\n"
        "      --vcodec CODEC    让摄像头输出 mjpeg 等压缩格式\n"
        "      --pix-fmt FMT     摄像头原始像素格式\n"
        "      --out-size WxH    推流分辨率（和采集不同时自动缩放）\n"
        "  -b, --bitrate KBPS    码率，单位 kbps，默认 2000\n"
        "  -g, --gop SECONDS     关键帧间隔（秒），默认 1。越小延迟越低\n"
        "      --encoder NAME    指定编码器：libx264 / h264_nvenc / h264_qsv / h264_amf / h264_mf\n"
        "  -n, --frames N        推 N 帧后退出（默认一直推，按 Ctrl+C 停止）\n"
        "  -f, --format NAME     输入设备类型，默认 Windows=dshow\n"
        "  -v, --verbose         打印 FFmpeg 调试日志\n"
        "  -h, --help            显示帮助\n"
        "\n"
        "示例:\n"
        "  " << prog << " -d \"Integrated Camera\" -s 1280x720 -o rtmp://localhost/live/test\n";
}

bool parseSize(const std::string& s, int& w, int& h) {
    return std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

bool parseArgs(int argc, char** argv, PushOptions& opt, int& exitCode) {
    exitCode = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("参数 " + a + " 需要一个值");
            return argv[++i];
        };
        auto toInt = [&](const std::string& v) -> long long {
            size_t pos = 0;
            long long n = -1;
            try {
                n = std::stoll(v, &pos);
            } catch (const std::exception&) {
                pos = 0;
            }
            if (pos == 0 || pos != v.size() || n < 0) throw std::invalid_argument("参数 " + a + " 的值不合法: " + v);
            return n;
        };
        try {
            if (a == "-h" || a == "--help") { printUsage(argv[0]); return false; }
            else if (a == "-d" || a == "--device") opt.camera.deviceName = next();
            else if (a == "-o" || a == "--output") opt.url = next();
            else if (a == "-s" || a == "--size") opt.camera.videoSize = next();
            else if (a == "-r" || a == "--fps") opt.camera.frameRate = next();
            else if (a == "--vcodec") opt.camera.vcodec = next();
            else if (a == "--pix-fmt") opt.camera.pixelFormat = next();
            else if (a == "--out-size") opt.outSize = next();
            else if (a == "-b" || a == "--bitrate") opt.bitrateKbps = static_cast<int>(toInt(next()));
            else if (a == "--encoder") opt.encoder = next();
            else if (a == "-g" || a == "--gop") opt.gopSeconds = static_cast<int>(toInt(next()));
            else if (a == "-n" || a == "--frames") opt.maxFrames = toInt(next());
            else if (a == "-f" || a == "--format") opt.camera.inputFormat = next();
            else if (a == "-v" || a == "--verbose") opt.verbose = true;
            else throw std::invalid_argument("未知参数: " + a);
        } catch (const std::invalid_argument& e) {
            std::cerr << e.what() << "\n\n";
            printUsage(argv[0]);
            exitCode = 1;
            return false;
        }
    }
    if (opt.camera.deviceName.empty()) {
        std::cerr << "请用 -d 指定摄像头名字（先运行 camera_capture --list 查看）\n\n";
        printUsage(argv[0]);
        exitCode = 1;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    PushOptions opt;
    int exitCode = 0;
    if (!parseArgs(argc, argv, opt, exitCode)) return exitCode;
    av_log_set_level(opt.verbose ? AV_LOG_DEBUG : AV_LOG_INFO);

    // avformat_network_init: 初始化网络模块（Windows 上会初始化 Winsock），推 RTMP 前调用一次
    avformat_network_init();

    // ---------------- 1. 打开摄像头 ----------------
    std::unique_ptr<ICapture> capture = std::make_unique<CameraCapture>(opt.camera);
    g_capture = capture.get();
    std::cout << "正在打开摄像头: " << opt.camera.deviceName << " ...\n";
    if (!capture->open()) {
        std::cerr << "错误: " << capture->lastError() << "\n";
        return 1;
    }

    // 先读一帧：MJPEG 等格式要解出第一帧才知道真正的像素格式和尺寸
    FramePtr frame = makeFrame();
    if (!frame) return 1;
    ReadResult r;
    do {
        r = capture->readFrame(frame.get());
    } while (r == ReadResult::Again && !g_stop);
    if (r != ReadResult::Ok) {
        std::cerr << "读取第一帧失败: " << capture->lastError() << "\n";
        return 1;
    }
    VideoStreamInfo info = capture->streamInfo();
    std::cout << "采集: " << info.width << "x" << info.height << " "
              << (av_get_pix_fmt_name(info.pixelFormat) ? av_get_pix_fmt_name(info.pixelFormat) : "?")
              << " 设备帧率=" << av_q2d(info.frameRate) << "\n";

    // ---------------- 2. 马上启动采集线程 ----------------
    // 采集线程只管尽快从摄像头取帧放进队列，不会被编码或网络拖慢，
    // 所以 dshow 的缓冲区不会堆积；编码跟不上时，队列丢掉最旧的帧，延迟始终只有几帧。
    // 为什么在连接服务器之前就启动：连接可能要好几秒（服务器慢、地址不通要等超时），
    // 这期间如果没人读摄像头，dshow 缓冲区就会被塞满，报 "real-time buffer too full"。
    FrameQueue queue(3);
    std::atomic<bool> captureFailed{false};

    std::thread captureThread([&] {
        queue.push(frame.get());  // 先把前面读好的第一帧放进去
        FramePtr f = makeFrame();
        while (!g_stop && f) {
            ReadResult rr = capture->readFrame(f.get());
            if (rr == ReadResult::Again) continue;
            if (rr == ReadResult::Eof) break;  // 设备结束，或者主线程调用了 requestStop
            if (rr == ReadResult::Error) {
                captureFailed = true;
                break;
            }
            queue.push(f.get());  // 帧的数据被"搬"进队列，f 变空，下一轮可以复用
        }
        queue.close();  // 告诉编码线程：不会再有新帧了
    });

    // 出错提前退出时，先停掉采集线程再返回（std::thread 没 join 就析构会直接崩溃）
    auto stopCapture = [&] {
        capture->requestStop();
        if (captureThread.joinable()) captureThread.join();
        g_capture = nullptr;
        g_publisher = nullptr;
    };

    // ---------------- 3. 打开编码器 ----------------
    EncoderOptions encOpt;
    encOpt.width = info.width;
    encOpt.height = info.height;
    if (!opt.outSize.empty() && !parseSize(opt.outSize, encOpt.width, encOpt.height)) {
        std::cerr << "--out-size 格式应为 宽x高，例如 1280x720\n";
        stopCapture();
        return 1;
    }
    // H.264 的 yuv420p 要求宽高是偶数
    encOpt.width &= ~1;
    encOpt.height &= ~1;
    if (info.frameRate.num > 0 && info.frameRate.den > 0) encOpt.frameRate = info.frameRate;
    encOpt.bitRate = static_cast<int64_t>(opt.bitrateKbps) * 1000;
    encOpt.codecName = opt.encoder;
    encOpt.gopSeconds = opt.gopSeconds > 0 ? opt.gopSeconds : 1;

    VideoEncoder encoder;
    if (!encoder.open(encOpt)) {
        std::cerr << "错误: " << encoder.lastError() << "\n";
        stopCapture();
        return 1;
    }
    std::cout << "编码: " << encoder.codecName() << " " << encOpt.width << "x" << encOpt.height
              << " " << opt.bitrateKbps << " kbps\n";

    // ---------------- 4. 连接服务器（最多等 5 秒，见 RtmpPublisher 的 rw_timeout） ----------------
    RtmpPublisher publisher;
    g_publisher = &publisher;
    std::cout << "正在连接: " << opt.url << " ...\n";
    if (!publisher.open(opt.url, encoder.context())) {
        std::cerr << "错误: " << publisher.lastError() << "\n"
                  << "提示: 确认服务器已启动（docker compose up -d），地址形如 rtmp://IP/live/流名\n"
                  << "      在家里请用局域网地址（例如 rtmp://192.168.1.129:1935/live/test）；\n"
                  << "      用域名/公网地址的话，路由器要把外网 1935 端口转发到服务器\n";
        stopCapture();
        return 1;
    }
    std::cout << "推流中，按 Ctrl+C 停止\n\n";

    // 编码器每吐出一个包，就交给 publisher 发出去
    auto sendPacket = [&](AVPacket* pkt) { return publisher.writePacket(pkt, encoder.timeBase()); };

    // ---------------- 5. 编码推流（当前线程） ----------------
    using Clock = std::chrono::steady_clock;
    const auto startTime = Clock::now();
    auto lastReport = startTime;
    int64_t firstPts = AV_NOPTS_VALUE;
    int64_t lastPtsMs = -1;
    long long frameCount = 0, framesSinceReport = 0;
    long long droppedAtLastReport = 0;
    int64_t bytesAtLastReport = 0;
    int result = 0;

    while (true) {
        FramePtr f = queue.pop(std::chrono::milliseconds(200));
        if (!f) {
            if (queue.closed() || g_stop) break;  // 采集结束了，或者用户按了 Ctrl+C
            continue;                             // 只是暂时没帧，继续等
        }

        // 计算这一帧的时间戳（毫秒，从 0 开始）：
        // 优先用设备给的 pts（更准），没有就用系统时钟
        int64_t ptsMs;
        if (f->pts != AV_NOPTS_VALUE) {
            if (firstPts == AV_NOPTS_VALUE) firstPts = f->pts;
            // av_rescale_q: 把 pts 从采集的时间单位换算成毫秒，内部处理了溢出和取整
            ptsMs = av_rescale_q(f->pts - firstPts, info.timeBase, AVRational{1, 1000});
        } else {
            ptsMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime).count();
        }
        // pts 必须严格递增，否则编码器/服务器会报错
        if (ptsMs <= lastPtsMs) ptsMs = lastPtsMs + 1;
        lastPtsMs = ptsMs;

        if (!encoder.encode(f.get(), ptsMs, sendPacket)) {
            std::cerr << "\n错误: " << (publisher.lastError().empty() ? encoder.lastError() : publisher.lastError()) << "\n";
            result = 1;
            break;
        }
        ++frameCount;
        ++framesSinceReport;

        // 每秒打印一次统计
        auto now = Clock::now();
        double sec = std::chrono::duration<double>(now - lastReport).count();
        if (sec >= 1.0) {
            int64_t bytes = publisher.bytesWritten();
            long long dropped = queue.droppedCount();
            std::printf("已推 %lld 帧  时长 %.1fs  帧率 %.1f  码率 %.0f kbps  丢帧 %lld\n",
                        frameCount, ptsMs / 1000.0, framesSinceReport / sec,
                        (bytes - bytesAtLastReport) * 8 / 1000.0 / sec, dropped - droppedAtLastReport);
            std::fflush(stdout);
            lastReport = now;
            framesSinceReport = 0;
            bytesAtLastReport = bytes;
            droppedAtLastReport = dropped;
        }
        if (opt.maxFrames > 0 && frameCount >= opt.maxFrames) break;
    }

    // ---------------- 6. 收尾 ----------------
    if (g_stop) std::cout << "\n收到 Ctrl+C，正在停止...\n";
    capture->requestStop();  // 让采集线程的 readFrame 尽快返回
    captureThread.join();    // 等采集线程退出后，才能安全地关闭摄像头
    if (captureFailed) {
        std::cerr << "采集失败: " << capture->lastError() << "\n";
        result = 1;
    }
    if (result == 0) encoder.flush(sendPacket);  // 把编码器里剩下的帧发完
    publisher.close();
    encoder.close();
    capture->close();
    g_publisher = nullptr;
    g_capture = nullptr;
    avformat_network_deinit();
    std::cout << "共推送 " << frameCount << " 帧，编码跟不上丢弃 " << queue.droppedCount() << " 帧，"
              << publisher.bytesWritten() / 1024 << " KB\n";
    return result;
}
