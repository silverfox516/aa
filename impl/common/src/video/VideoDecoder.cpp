#define LOG_TAG "AA.VideoDecoder"
#include "aauto/video/VideoDecoder.hpp"
#include "aauto/utils/Logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace aauto {
namespace video {

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    Stop();
    if (decode_thread_.joinable()) decode_thread_.join();

    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_ctx_);
}

bool VideoDecoder::Initialize() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        AA_LOG_E() << "[VideoDecoder] H264 디코더를 찾을 수 없습니다";
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_ctx_->thread_count = 2;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        AA_LOG_E() << "[VideoDecoder] H264 디코더 열기 실패";
        return false;
    }

    packet_ = av_packet_alloc();
    frame_  = av_frame_alloc();
    if (!packet_ || !frame_) return false;

    initialized_.store(true);
    running_.store(true);
    AA_LOG_I() << "[VideoDecoder] 초기화 완료";

    decode_thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
    return true;
}

void VideoDecoder::PushVideoData(const std::vector<uint8_t>& data) {
    if (!initialized_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (frame_queue_.size() >= kMaxQueueSize) {
            frame_queue_.pop();
            AA_LOG_W() << "[VideoDecoder] 프레임 드랍 (queue full)";
        }
        frame_queue_.push(data);
    }
    queue_cv_.notify_one();
}

void VideoDecoder::Stop() {
    running_.store(false);
    initialized_.store(false);
    queue_cv_.notify_all();
}

void VideoDecoder::DecodeLoop() {
    AA_LOG_I() << "[VideoDecoder] 디코딩 스레드 시작";
    while (running_.load()) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !frame_queue_.empty() || !running_.load();
            });
            if (frame_queue_.empty()) break;
            data = std::move(frame_queue_.front());
            frame_queue_.pop();
        }

        if (data.size() < 12) continue;

        // 8바이트 타임스탬프 스킵 후 start code 탐색
        const uint8_t* nal_data = data.data() + 8;
        int nal_size = static_cast<int>(data.size()) - 8;

        bool has_sc = (nal_size >= 4 && nal_data[0] == 0 && nal_data[1] == 0 && nal_data[2] == 0 && nal_data[3] == 1)
                   || (nal_size >= 3 && nal_data[0] == 0 && nal_data[1] == 0 && nal_data[2] == 1);
        if (!has_sc) {
            int search_limit = std::min(static_cast<int>(data.size()), 64);
            for (int i = 0; i + 3 < search_limit; ++i) {
                if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
                    nal_data = data.data() + i;
                    nal_size = static_cast<int>(data.size()) - i;
                    has_sc = true;
                    break;
                }
            }
        }
        if (!has_sc || nal_size <= 0) continue;

        packet_->data = const_cast<uint8_t*>(nal_data);
        packet_->size = nal_size;

        int ret = avcodec_send_packet(codec_ctx_, packet_);
        if (ret < 0) {
            if (ret != AVERROR_INVALIDDATA) {
                char err[64]; av_strerror(ret, err, sizeof(err));
                AA_LOG_W() << "[VideoDecoder] send_packet 실패: " << err;
            }
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            AVFrame* out = av_frame_clone(frame_);
            if (!out) continue;

            if (frame_cb_) frame_cb_(out);
            // 콜백이 없으면 그냥 해제
            else           av_frame_free(&out);
        }
    }
    AA_LOG_I() << "[VideoDecoder] 디코딩 스레드 종료";
}

}  // namespace video
}  // namespace aauto
