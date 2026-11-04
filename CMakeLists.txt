cmake_minimum_required(VERSION 3.20)
project(streamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # /utf-8: 源码里有中文注释和中文字符串，按 UTF-8 解析
    add_compile_options(/utf-8 /W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ============================================================================
# 查找 FFmpeg，三种方式按顺序尝试：
#   1. -DFFMPEG_ROOT=C:/ffmpeg  （手动下载的 shared 包，里面有 include/ lib/ bin/）
#   2. vcpkg 工具链（find_package(FFMPEG)）
#   3. pkg-config（Linux / MSYS2）
# 最终统一成一个目标 FFmpeg::FFmpeg
# ============================================================================
set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg 安装目录（包含 include/ lib/ bin/）")

# 没有手动指定时，自动在常见位置找（Visual Studio “打开文件夹”时不会传 -D 参数）：
#   streamer/third_party、streamer/thirdParty、C:/ffmpeg，以及它们下面多套了几层的情况
#   （例如解压出来是 C:/ffmpeg/ffmpeg-9.0.2-full_build-shared/include）
if(NOT FFMPEG_ROOT AND WIN32)
    foreach(base "${CMAKE_CURRENT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/thirdParty" "C:/ffmpeg")
        # 往下找最多 3 层，兼容 7z 解压时多套了几层同名文件夹的情况
        file(GLOB _candidates LIST_DIRECTORIES true "${base}" "${base}/*" "${base}/*/*" "${base}/*/*/*")
        foreach(dir ${_candidates})
            if(EXISTS "${dir}/include/libavformat/avformat.h" AND EXISTS "${dir}/lib/avformat.lib")
                set(FFMPEG_ROOT "${dir}" CACHE PATH "FFmpeg 安装目录（包含 include/ lib/ bin/）" FORCE)
                break()
            endif()
        endforeach()
        if(FFMPEG_ROOT)
            break()
        endif()
    endforeach()
endif()
set(FFMPEG_COMPONENTS avdevice avformat avcodec swscale avutil)

add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)

if(FFMPEG_ROOT)
    message(STATUS "使用 FFMPEG_ROOT = ${FFMPEG_ROOT}")
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
              PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    set(_ffmpeg_libs "")
    foreach(comp ${FFMPEG_COMPONENTS})
        find_library(FFMPEG_${comp}_LIB NAMES ${comp}
                     PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        list(APPEND _ffmpeg_libs ${FFMPEG_${comp}_LIB})
    endforeach()
    target_include_directories(FFmpeg::FFmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(FFmpeg::FFmpeg INTERFACE ${_ffmpeg_libs})
    set(FFMPEG_DLL_DIR "${FFMPEG_ROOT}/bin")
else()
    find_package(FFMPEG QUIET)  # vcpkg 提供的 FindFFMPEG.cmake
    if(FFMPEG_FOUND)
        message(STATUS "使用 vcpkg 的 FFmpeg")
        target_include_directories(FFmpeg::FFmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(FFmpeg::FFmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(FFmpeg::FFmpeg INTERFACE ${FFMPEG_LIBRARIES})
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG_PC IMPORTED_TARGET
                libavdevice libavformat libavcodec libswscale libavutil)
        endif()
        if(FFMPEG_PC_FOUND)
            message(STATUS "使用 pkg-config 的 FFmpeg ${FFMPEG_PC_libavformat_VERSION}")
            target_link_libraries(FFmpeg::FFmpeg INTERFACE PkgConfig::FFMPEG_PC)
        else()
            message(FATAL_ERROR
                "找不到 FFmpeg。请把 ffmpeg-xxx-full_build-shared.7z 解压到 "
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg 或 C:/ffmpeg（里面要有 include、lib、bin），"
                "或用 -DFFMPEG_ROOT=<FFmpeg目录> 指定，"
                "或者用 vcpkg 工具链（-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake）")
        endif()
    endif()
endif()

# ============================================================================
# core: 公共工具（RAII 包装、错误处理、图片保存、H.264 编码、RTMP 推流）
# ============================================================================
add_library(streamer_core STATIC
    core/FFmpegUtils.h
    core/FrameSaver.h
    core/FrameSaver.cpp
    core/VideoEncoder.h
    core/VideoEncoder.cpp
    core/RtmpPublisher.h
    core/RtmpPublisher.cpp
    core/FrameQueue.h
)
target_include_directories(streamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(streamer_core PUBLIC FFmpeg::FFmpeg)

# ============================================================================
# capture: 采集源（摄像头 CameraCapture + 桌面 ScreenCapture）
# ============================================================================
add_library(streamer_capture STATIC
    capture/ICapture.h
    capture/CameraCapture.h
    capture/CameraCapture.cpp
    capture/ScreenCapture.h
    capture/ScreenCapture.cpp
)
target_link_libraries(streamer_capture PUBLIC streamer_core)
if(WIN32)
    # ScreenCapture 调用了 SetProcessDPIAware（在 user32.dll 里）
    target_link_libraries(streamer_capture PUBLIC user32)
endif()

# ============================================================================
# app: 命令行工具
# ============================================================================
add_executable(camera_capture app/main.cpp)
target_link_libraries(camera_capture PRIVATE streamer_capture)

# 摄像头 → H.264 编码 → RTMP 推流
add_executable(camera_push app/push_main.cpp)
find_package(Threads REQUIRED)
target_link_libraries(camera_push PRIVATE streamer_capture Threads::Threads)

# Windows 上把 FFmpeg 的 DLL 复制到 exe 旁边，双击/命令行运行时才能找到
if(WIN32 AND FFMPEG_DLL_DIR AND EXISTS "${FFMPEG_DLL_DIR}")
    file(GLOB FFMPEG_DLLS "${FFMPEG_DLL_DIR}/*.dll")
    if(FFMPEG_DLLS)
        foreach(exe camera_capture camera_push)
            add_custom_command(TARGET ${exe} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:${exe}>
                COMMENT "复制 FFmpeg DLL 到输出目录")
        endforeach()
    endif()
endif()
