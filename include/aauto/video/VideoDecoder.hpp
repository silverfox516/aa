#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace aauto {
namespace video {

// H.264 decoder: receives NAL units, emits decoded AVFrame* via callback.
// Platform-independent — no SDL, no window, no event loop.
class VideoDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;

    VideoDecoder();
    ~VideoDecoder();

    // Initialize FFmpeg H.264 decoder. Call from any thread before PushVideoData.
    bool Initialize();

    // Register callback invoked from the decode thread for each decoded AVFrame.
    // Ownership of AVFrame* is transferred to the callback — caller must av_frame_free it.
    void SetFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }

    // Push a raw H.264 NAL unit. Thread-safe.
    void PushVideoData(const std::vector<uint8_t>& data);

    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void DecodeLoop();

    // FFmpeg
    AVCodecContext* codec_ctx_ = nullptr;
    AVPacket*       packet_    = nullptr;
    AVFrame*        frame_     = nullptr;

    FrameCallback frame_cb_;

    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::vector<uint8_t>> frame_queue_;
    static constexpr size_t kMaxQueueSize = 4;

    std::thread       decode_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
};

}  // namespace video
}  // namespace aauto
