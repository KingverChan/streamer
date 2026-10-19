#pragma once
// ============================================================================
// FrameQueue.h —— 线程安全的有界帧队列（满了丢最旧的帧）
//
// 为什么需要它：如果"读摄像头 → 编码 → 发网络"全在一个线程里串行做，
// 只要编码或网络偶尔慢一下，摄像头的帧就来不及取，dshow 会把帧堆在它的
// 实时缓冲区（rtbufsize）里。缓冲区里堆了多少帧，画面就落后多少，延迟越积越大，
// 堆满了还会报 "real-time buffer too full ... frame dropped"。
//
// 解决办法（生产者-消费者模型）：
//   采集线程：只负责尽快把帧从摄像头取出来，放进队列
//   编码线程：从队列取帧去编码、发送
//   队列容量很小（例如 3 帧），编码跟不上时直接丢掉最旧的帧，
//   这样延迟最多只有几帧，不会越积越多。直播宁可丢帧，也不能越来越延迟。
// ============================================================================

#include "core/FFmpegUtils.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace streamer {

class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : capacity_(capacity > 0 ? capacity : 1) {}

    // 放入一帧：接管 frame 的数据（av_frame_move_ref），调用后 frame 变成空帧可以复用。
    // 队列满时丢掉最旧的一帧，返回 false 表示发生了丢帧
    bool push(AVFrame* frame) {
        FramePtr copy = makeFrame();
        if (!copy) return false;
        // av_frame_move_ref: 把 frame 里的数据引用"搬"到 copy，不拷贝像素，几乎零开销
        av_frame_move_ref(copy.get(), frame);

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= capacity_) {
                queue_.pop_front();  // unique_ptr 析构时自动 av_frame_free
                ++droppedCount_;
                dropped = true;
            }
            queue_.push_back(std::move(copy));
        }
        cond_.notify_one();  // 叫醒等待中的编码线程
        return !dropped;
    }

    // 取一帧，最多等 timeout。超时或已关闭且为空时返回空指针
    FramePtr pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        // wait_for + 谓词：防止"虚假唤醒"，只有队列非空或已关闭才继续
        cond_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return nullptr;
        FramePtr frame = std::move(queue_.front());
        queue_.pop_front();
        return frame;
    }

    // 生产者结束时调用，唤醒消费者让它退出
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cond_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    long long droppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return droppedCount_;
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<FramePtr> queue_;
    bool closed_ = false;
    long long droppedCount_ = 0;
};

}  // namespace streamer
