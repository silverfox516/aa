#define LOG_TAG "AA.IMPL.MediaCodecVideoSink"

#include "aauto/output/MediaCodecVideoSink.hpp"

#include <cstring>

#include <media/NdkMediaFormat.h>

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace output {
namespace android {

namespace {
constexpr int64_t kFrameIntervalUs       = 33333;   // 30 fps nominal
constexpr int64_t kCodecConfigTimeoutUs  = 200000;  // 200 ms — wait for input slot post-start
}

MediaCodecVideoSink::MediaCodecVideoSink(ANativeWindow* surface)
    : window_(surface) {
    if (window_) ANativeWindow_acquire(window_);
    AA_LOG_I() << "MediaCodecVideoSink ctor surface=" << window_;
}

MediaCodecVideoSink::~MediaCodecVideoSink() {
    TeardownCodec();
    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    AA_LOG_I() << "MediaCodecVideoSink dtor";
}

void MediaCodecVideoSink::OnVideoConfig(const service::VideoCodecConfig& config) {
    AA_LOG_I() << "OnVideoConfig " << config.width << "x" << config.height
               << " fps=" << config.fps
               << " codec_data=" << config.codec_data.size() << "B";

    if (!EnsureCodec(config.width, config.height)) return;

    if (!config.codec_data.empty()) {
        // Push as a normal input buffer with pts=0; the AVC decoder treats
        // an SPS/PPS-only access unit as configuration. Using flags=0 (not
        // BUFFER_FLAG_CODEC_CONFIG) avoids quirks on some hardware decoders.
        // Use a non-trivial timeout: AMediaCodec_start may take a few ms
        // to make input buffers available, and dropping codec config is
        // catastrophic — the decoder cannot decode any frame without SPS/PPS.
        QueueRawInput(config.codec_data.data(), config.codec_data.size(), 0,
                      kCodecConfigTimeoutUs);
        DrainOutput();
    }
}

void MediaCodecVideoSink::OnVideoFrame(const service::VideoFrame& frame) {
    if (!is_open_ || !codec_) return;
    if (frame.size == 0 || !frame.data) return;

    // Non-blocking: drop the frame rather than stall the network thread.
    QueueRawInput(frame.data, frame.size, next_pts_us_, /*timeout_us=*/0);
    next_pts_us_ += kFrameIntervalUs;

    ++frames_in_;
    if (frames_in_ <= 5 || frames_in_ % 100 == 0) {
        AA_LOG_I() << "QueueInput frame=" << frames_in_ << " size=" << frame.size;
    }

    DrainOutput();
}

bool MediaCodecVideoSink::EnsureCodec(int width, int height) {
    if (is_open_ && codec_) return true;
    if (!window_) {
        AA_LOG_E() << "EnsureCodec: no surface";
        return false;
    }

    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
        AA_LOG_E() << "Failed to create AVC decoder";
        return false;
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
        return false;
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        AA_LOG_E() << "AMediaCodec_start failed: " << status;
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    is_open_     = true;
    next_pts_us_ = 0;
    frames_in_   = 0;
    frames_out_  = 0;
    AA_LOG_I() << "Codec opened " << width << "x" << height;
    return true;
}

void MediaCodecVideoSink::TeardownCodec() {
    if (!is_open_) return;
    is_open_ = false;
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    AA_LOG_I() << "Codec closed";
}

void MediaCodecVideoSink::QueueRawInput(const uint8_t* data, size_t size,
                                        int64_t pts_us, int64_t timeout_us) {
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, timeout_us);
    if (index < 0) {
        AA_LOG_W() << "dequeueInputBuffer returned " << index
                   << " (timeout_us=" << timeout_us << "), dropping";
        return;
    }
    size_t   buf_size = 0;
    uint8_t* buf      = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &buf_size);
    if (!buf) {
        AA_LOG_E() << "getInputBuffer null";
        return;
    }
    size_t copy_size = size < buf_size ? size : buf_size;
    std::memcpy(buf, data, copy_size);
    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, copy_size, pts_us, 0);
}

void MediaCodecVideoSink::DrainOutput() {
    AMediaCodecBufferInfo info;
    ssize_t out_index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    while (out_index >= 0) {
        ++frames_out_;
        if (frames_out_ <= 5 || frames_out_ % 100 == 0) {
            AA_LOG_I() << "OutputFrame " << frames_out_ << " pts=" << info.presentationTimeUs;
        }
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out_index), /*render=*/true);
        out_index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }
}

} // namespace android
} // namespace output
} // namespace aauto
