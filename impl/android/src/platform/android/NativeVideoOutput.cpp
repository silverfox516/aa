#define LOG_TAG "AA.NativeVideoOutput"

#include "aauto/platform/android/NativeVideoOutput.hpp"

#include <string.h>

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {
namespace android {

NativeVideoOutput::NativeVideoOutput() = default;

NativeVideoOutput::~NativeVideoOutput() {
    Close();
    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

void NativeVideoOutput::SetSurface(ANativeWindow* window) {
    ANativeWindow* old = window_;

    if (old != window) {
        if (window) ANativeWindow_acquire(window);
        window_ = window;
        if (old) ANativeWindow_release(old);
    }

    AA_LOG_I() << "Surface set: " << window_ << (old != window ? " (changed)" : " (same)");

    if (codec_ && is_open_.load() && window_ && old != window) {
        media_status_t st = AMediaCodec_setOutputSurface(codec_, window_);
        AA_LOG_I() << "setOutputSurface -> " << st;
    }
}

void NativeVideoOutput::Open(int width, int height) {
    if (is_open_.load()) {
        AA_LOG_W() << "Already open, ignoring Open()";
        return;
    }
    if (!window_) {
        AA_LOG_E() << "No surface set — call SetSurface() before Open()";
        return;
    }

    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
        AA_LOG_E() << "Failed to create AVC decoder";
        return;
    }

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH,  width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);

    media_status_t status = AMediaCodec_configure(codec_, format, window_, nullptr, 0);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        AA_LOG_E() << "AMediaCodec_configure failed: " << status;
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return;
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        AA_LOG_E() << "AMediaCodec_start failed: " << status;
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return;
    }

    is_open_.store(true);
    next_pts_us_ = 0;
    frames_in_   = 0;
    frames_out_  = 0;
    AA_LOG_I() << "Opened " << width << "x" << height;
}

void NativeVideoOutput::Close() {
    if (!is_open_.exchange(false)) return;

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    AA_LOG_I() << "Closed";
}

void NativeVideoOutput::PushVideoData(const std::vector<uint8_t>& data, bool is_codec_config) {
    if (!is_open_.load() || !codec_) return;

    // Android Auto video wire format:
    //   MEDIA_CODEC_CONFIG: [SPS/PPS NAL data]           — no timestamp prefix
    //   MEDIA_DATA:         [8-byte timestamp][NAL data]  — skip 8 bytes
    const size_t kHeaderSize = is_codec_config ? 0 : 8;
    if (data.size() <= kHeaderSize) return;

    const uint8_t* nal      = data.data() + kHeaderSize;
    size_t         nal_size = data.size() - kHeaderSize;

    // Dequeue an input buffer with 1-second timeout.
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 1000000LL);
    if (index < 0) {
        AA_LOG_W() << "dequeueInputBuffer returned " << index << ", dropping frame";
        return;
    }

    size_t   buf_size = 0;
    uint8_t* buf      = AMediaCodec_getInputBuffer(codec_, (size_t)index, &buf_size);
    if (!buf) {
        AA_LOG_E() << "getInputBuffer returned null for index " << index;
        return;
    }

    size_t copy_size = nal_size < buf_size ? nal_size : buf_size;
    memcpy(buf, nal, copy_size);

    // Codec config gets pts=0; regular frames get monotonic pts.
    // Submit with flags=0 for all — hardware decoders may not handle CODEC_CONFIG flag.
    int64_t pts = is_codec_config ? 0 : next_pts_us_;
    if (!is_codec_config) next_pts_us_ += 33333;

    ++frames_in_;
    media_status_t st = AMediaCodec_queueInputBuffer(codec_, (size_t)index, 0, copy_size, pts, 0);
    if (frames_in_ <= 5 || frames_in_ % 100 == 0) {
        AA_LOG_I() << "QueueInput: frame " << frames_in_
                   << " size=" << copy_size << " pts=" << pts
                   << " cfg=" << is_codec_config << " st=" << st;
    }

    // Poll and release all available output buffers to the surface for rendering.
    AMediaCodecBufferInfo info;
    ssize_t out_index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    while (out_index >= 0) {
        ++frames_out_;
        if (frames_out_ <= 5 || frames_out_ % 100 == 0) {
            AA_LOG_I() << "OutputFrame " << frames_out_
                       << " pts=" << info.presentationTimeUs;
        }
        AMediaCodec_releaseOutputBuffer(codec_, (size_t)out_index, /*render=*/true);
        out_index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }
}

void NativeVideoOutput::SetTouchCallback(TouchCallback cb) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    touch_callback_ = std::move(cb);
}

}  // namespace android
}  // namespace platform
}  // namespace aauto
