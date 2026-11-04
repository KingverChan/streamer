# streamer —— 摄像头采集（第 1 步）

用 C++17 + FFmpeg API（已在 7.1 和 9.0.2 上编译测试）（libavdevice 的 dshow）采集摄像头，打印每一帧的信息，并可把前 N 帧保存成 BMP/PPM 图片。
这是「摄像头 + 桌面采集 → 编码 → RTMP 推流」项目的第一步，现在只做采集。

## 目录结构

```
streamer/
├── CMakeLists.txt
├── core/                    公共工具
│   ├── FFmpegUtils.h        unique_ptr + 自定义 deleter 管理 FFmpeg 资源；av_strerror 错误转字符串
│   ├── FrameSaver.h/.cpp    sws_scale 转 RGB，写 BMP / PPM
│   ├── VideoEncoder.h/.cpp  转 yuv420p，编码成 H.264（libx264 / 硬件编码器）
│   ├── RtmpPublisher.h/.cpp FLV 封装 + RTMP 推流（也能写本地 .flv 文件）
├── capture/                 采集源
│   ├── ICapture.h           统一接口：open / close / readFrame
│   ├── CameraCapture.h/.cpp 摄像头实现（dshow），列设备、打开、读包、解码
└── app/
    ├── main.cpp             camera_capture：采集、打印帧信息、存图
    └── push_main.cpp        camera_push：采集 → 编码 → 推流
```

后面要加的模块：
- `capture/ScreenCapture`：桌面采集（gdigrab 或 DXGI），同样实现 `ICapture`
- `core/Encoder`：把 `AVFrame` 编码成 H.264
- `core/RtmpPublisher`：封装成 FLV，推到 RTMP 服务器

## 1. 安装 FFmpeg 开发库（二选一）

### 方式 A：下载预编译的 shared 包（推荐，最简单）

1. 打开 https://www.gyan.dev/ffmpeg/builds/ ，在“发布版本”里下载 **`ffmpeg-release-full-shared.7z`**（目前是 9.0.2），用 7-Zip 解压。
   也可以用 https://github.com/BtbN/FFmpeg-Builds/releases 里名字带 `win64-gpl-shared` 的包。
   一定要选 **shared**，只有它带 `include/` 和 `lib/`（开发用的头文件和导入库）。
2. 解压到 **`streamer\third_party\ffmpeg`** 或 **`C:\ffmpeg`**，CMake 会自动找到（多套一层文件夹也没关系，不用再传 `-DFFMPEG_ROOT`，用 Visual Studio 直接“打开文件夹”也行）。确认里面有：
   ```
   C:\ffmpeg\bin\avformat-63.dll ...   （数字是版本号，9.0 是 63，7.1 是 61）
   C:\ffmpeg\include\libavformat\avformat.h
   C:\ffmpeg\lib\avformat.lib
   ```

### 方式 B：vcpkg

```powershell
vcpkg install ffmpeg:x64-windows
```
默认特性已包含 avcodec / avdevice / avformat / swscale。

## 2. 编译（Windows + Visual Studio 2022）

在 “x64 Native Tools Command Prompt for VS 2022” 或 PowerShell 里：

```powershell
cd C:\Users\Jimmy\Documents\GitHub\streamer

# 方式 A：指定 FFMPEG_ROOT
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_ROOT=C:/ffmpeg

# 方式 B：用 vcpkg 工具链（路径换成你自己的 vcpkg 位置）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

方式 A 编译完会自动把 `C:\ffmpeg\bin\*.dll` 复制到 `build\Release\`，所以 exe 可以直接运行。
也可以用 Visual Studio 直接“打开文件夹”，或用 CLion 打开 `CMakeLists.txt`（在 CMake 选项里加 `-DFFMPEG_ROOT=C:/ffmpeg`）。

## 3. 运行

```powershell
cd build\Release

# 不带参数：列出摄像头
.\camera_capture.exe

# 打开摄像头（名字用上一步列出来的），一直读到 Ctrl+C
.\camera_capture.exe -d "Integrated Camera"

# 指定分辨率和帧率，保存前 5 帧为 BMP（在 frames\ 目录）
.\camera_capture.exe -d "Integrated Camera" -s 1280x720 -r 30 --save 5

# 很多 USB 摄像头 1080p 只在 MJPEG 下才有 30fps
.\camera_capture.exe -d "USB Camera" -s 1920x1080 -r 30 --vcodec mjpeg --save 3

# 读 100 帧后自动退出
.\camera_capture.exe -d "Integrated Camera" -n 100
```

输出示例：
```
帧 #12     1280x720  yuyv422    pts=123456789 (12.346s)  实际帧率=29.97  -> 已保存 frames\frame_0012.bmp
```

全部参数：`camera_capture.exe --help`

### 没有摄像头时怎么测试

用 FFmpeg 自带的测试画面源 lavfi（也属于 libavdevice），走的是同一套代码：
```powershell
.\camera_capture.exe -f lavfi -d "testsrc=size=640x480:rate=30" -n 60 --save 2
```

## 4. 推流到服务器（camera_push）

数据流：`CameraCapture` 采集原始帧 → `VideoEncoder` 转 yuv420p 并编码成 H.264 → `RtmpPublisher` 封装成 FLV，通过 RTMP 发给服务器。

接收端用你自己的 [live-streaming-server](../live-streaming-server)（Docker 里的 SRS）：RTMP 端口 1935，播放端口 8080。

```powershell
# 推到你的服务器（流名 test）
.\camera_push.exe -d "Integrated Camera" -s 1280x720 -o rtmp://jimmy-chan.duckdns.org/live/test

# 没有服务器时，先录成本地文件验证编码是否正常（用 VLC 或 ffplay 打开 test.flv）
.\camera_push.exe -d "Integrated Camera" -s 1280x720 -o test.flv -n 300
```

播放验证（任选一个）：
- 浏览器打开 `http://jimmy-chan.duckdns.org:8080/app/live.html?s=test`
- VLC → 媒体 → 打开网络串流 → `http://jimmy-chan.duckdns.org:8080/live/test.flv`
- 命令行 `ffplay http://jimmy-chan.duckdns.org:8080/live/test.flv`

常用参数：`-b 3000` 码率 3000 kbps；`--out-size 854x480` 推流时缩小分辨率；`--encoder h264_mf` 指定编码器（默认先用 libx264，没有就依次试 NVIDIA / Intel / AMD 硬件编码器和 Windows 自带的 h264_mf）。

| 现象 | 原因 / 解决 |
|---|---|
| `连接失败 ... Connection refused` | 服务器没启动（`docker compose up -d`），或路由器没转发 1935 端口 |
| `连接失败 ... timed out` | 域名解析不对或防火墙拦了 1935，先在服务器本机用 `./scripts/push_test.sh` 测 |
| 能连上但播放器黑屏 | 播放器要等第一个关键帧（默认每 2 秒一个），等几秒；或检查播放地址的流名是否一致 |
| 延迟比较大（3~5 秒） | 正常：HTTP-FLV 有缓冲，HLS 延迟更大（10 秒以上） |

## 5. 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `Could not set video options` / `I/O error` | 分辨率、帧率、像素格式不是摄像头支持的组合。运行 `ffmpeg -f dshow -list_options true -i video="设备名"` 查看支持的组合 |
| `Could not run graph` / 打不开 | 摄像头被其他程序占用（Teams、微信、相机 App），关掉再试 |
| `real-time buffer full` 丢帧 | 处理太慢或缓冲区太小，加 `--rtbufsize 200M` |
| `deprecated pixel format used` | 用 MJPEG 时 FFmpeg 的提示（yuvj422p），无害，可以忽略 |
| 运行时提示找不到 `avformat-63.dll` | DLL 没在 exe 旁边，把 `C:\ffmpeg\bin` 里的 dll 复制过去，或加进 PATH |
| 列设备失败 | 程序会自动改用 `list_devices` 方式把列表打到日志里；也可以运行 `ffmpeg -f dshow -list_devices true -i dummy` |

## 6. 代码主线（建议按这个顺序读）

1. `app/main.cpp` → `main()`：解析参数 → 创建 `CameraCapture` → `open()` → 循环 `readFrame()`
2. `capture/CameraCapture.cpp` → `open()`（详细拆解见第 7 节）：
   `avdevice_register_all` → `av_find_input_format("dshow")` → `avformat_open_input("video=名字")`
   → `avformat_find_stream_info` → `av_find_best_stream` → `avcodec_alloc_context3` / `avcodec_parameters_to_context` / `avcodec_open2`
3. `CameraCapture::readFrame()`：`av_read_frame` 读包 → `avcodec_send_packet` → `avcodec_receive_frame` 取帧
4. `core/FrameSaver.cpp`：`sws_getContext` + `sws_scale` 把 YUV 转成 RGB/BGR，再写 BMP / PPM 文件头和像素
5. `core/FFmpegUtils.h`：所有 FFmpeg 对象的 RAII 包装
6. `app/push_main.cpp` → `main()`：打开摄像头 → 打开编码器 → 连接服务器 → 循环“读帧 → 算 pts → encode → writePacket”
7. `core/VideoEncoder.cpp`：`avcodec_find_encoder_by_name` → `avcodec_open2`（preset/tune 低延迟）→ `sws_scale` → `avcodec_send_frame` / `avcodec_receive_packet`
8. `core/RtmpPublisher.cpp`：`avformat_alloc_output_context2("flv")` → `avformat_new_stream` → `avio_open2` → `avformat_write_header` → `av_interleaved_write_frame` → `av_write_trailer`

## 7. `CameraCapture::open()` 详解

`open()` 要回答三个问题：**设备在哪 → 里面是什么 → 怎么变成帧**。对应代码里的三段：

### 7.1 找到并打开设备（设备在哪）

| 步骤 | 函数 | 作用 |
|---|---|---|
| ① | `avdevice_register_all()` | 注册 dshow / v4l2 / lavfi 等设备类输入格式，不调用的话下一步找不到 |
| ② | `av_find_input_format("dshow")` | 按名字找输入格式（demuxer），摄像头在 FFmpeg 眼里也是一种“输入格式” |
| ③ | 拼 URL | dshow 的设备地址是 `video=设备名`，比如 `video=Integrated Camera` |
| ④ | `av_dict_set(&opts, ...)` | 把 `video_size` / `framerate` / `pixel_format` / `vcodec` / `rtbufsize` 放进选项字典，等价于命令行 `-i` 前面的参数 |
| ⑤ | `avformat_alloc_context()` | **先**分配一个空的 `AVFormatContext`，目的是在打开前装上中断回调（见 7.4） |
| ⑥ | `avformat_open_input(&ctx, url, ifmt, &opts)` | 真正打开设备。失败时 FFmpeg 会自己释放 ctx；成功后字典里剩下的是“没被用到的选项”，程序会打印警告提示拼写错误 |

### 7.2 探流（里面是什么）

| 步骤 | 函数 | 作用 |
|---|---|---|
| ⑦ | `avformat_find_stream_info()` | 先读几包数据，探测流的详细参数：分辨率、帧率、编码格式（rawvideo 还是 mjpeg） |
| ⑧ | `av_dump_format()` | 把探测结果打印出来，和 ffmpeg 命令行里的 `Input #0` 一样 |
| ⑨ | `av_find_best_stream(VIDEO)` | 找出“最合适”的视频流下标，顺便返回它的解码器 `AVCodec*` |

### 7.3 准备解码器（怎么变成帧）

| 步骤 | 函数 | 作用 |
|---|---|---|
| ⑩ | `avcodec_alloc_context3(decoder)` | 分配解码器上下文 |
| ⑪ | `avcodec_parameters_to_context()` | 把流参数（宽高、像素格式、extradata）拷给解码器 |
| ⑫ | `codecCtx->pkt_timebase = stream->time_base` | 告诉解码器 pts 的单位，解出来的帧 pts 才对 |
| ⑬ | `avcodec_open2()` | 初始化解码器。YUYV 等原始格式走 rawvideo（几乎直接拷贝），MJPEG 走 mjpeg 解码器 |
| ⑭ | `makePacket()` + 记录 `info_` | 分配之后 `readFrame()` 反复使用的 `AVPacket`；记下宽高、像素格式、time_base，并用 `av_guess_frame_rate` 估计帧率 |

任何一步失败都会 `setError(...)` 记下原因、`close()` 释放已经打开的资源，然后返回 `false`。

### 7.4 中断回调（interrupt_callback）是干嘛的

**问题**：`avformat_open_input`、`av_read_frame`、网络写包这些 FFmpeg 函数都是**阻塞调用**：设备卡住、网络断了的时候，函数不返回，程序就一直等在里面。这时候按 Ctrl+C，主循环根本没机会去检查“要不要退出”。

**解决**：给 `AVFormatContext` 装一个回调函数。FFmpeg 在阻塞等待的过程中会**定期调用它问一句“还要继续等吗？”**：
- 返回 `0`：继续等
- 返回非 `0`：立刻放弃这次阻塞调用，函数返回错误码 `AVERROR_EXIT`

本项目里的实现（`capture/CameraCapture.cpp`）：

```cpp
int CameraCapture::interruptCallback(void* opaque) {
    auto* self = static_cast<CameraCapture*>(opaque);   // opaque 就是装回调时传进去的 this
    return self->stopRequested_ ? 1 : 0;                // 用户要求停止就返回 1
}

// open() 里，avformat_open_input 之前：
rawCtx->interrupt_callback.callback = &CameraCapture::interruptCallback;
rawCtx->interrupt_callback.opaque = this;
```

整条 Ctrl+C 退出链路：

```
Ctrl+C → consoleCtrlHandler / signalHandler（app/main.cpp）
       → requestStop() → capture->requestStop()       // 只是把 atomic<bool> stopRequested_ 设成 true
       → FFmpeg 阻塞中定期调用 interruptCallback → 返回 1
       → av_read_frame 返回 AVERROR_EXIT
       → readFrame() 记下“用户请求停止”，送空包把解码器里剩下的帧冲出来，最后返回 Eof
       → main 循环结束，析构时释放所有资源
```

几个细节：
- **为什么要先 `avformat_alloc_context` 再 open**：如果直接 `avformat_open_input(&nullCtx, ...)`，上下文是 FFmpeg 在函数内部才创建的，我们没机会在“打开”这一步之前装回调，打开设备卡住时就没法中断。
- **为什么回调是 `static` 函数 + `opaque`**：FFmpeg 是 C 库，只认普通函数指针，不能直接传成员函数，所以用 `opaque` 把 `this` 带进去，在回调里再转回 `CameraCapture*`。
- **为什么 `stopRequested_` 是 `std::atomic<bool>`**：Ctrl+C 处理函数在另一个线程（Windows）或信号上下文里写它，FFmpeg 在采集线程里读它，用 atomic 避免数据竞争。
- **`readFrame()` 自己也会检查 `stopRequested_`**：每次要读新包之前看一眼，就算 FFmpeg 没有走到回调，正常出帧时也能及时退出。回调负责的是“卡在 FFmpeg 里面出不来”的那种情况。
- `RtmpPublisher` 用的是同一套做法（再加上 `rw_timeout` 5 秒超时），所以推流时服务器失联也能 Ctrl+C 退出。
