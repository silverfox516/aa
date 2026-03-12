#define LOG_TAG "VideoRenderer"
#include "aauto/video/VideoRenderer.hpp"
#include "aauto/utils/Logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <SDL2/SDL.h>

namespace aauto {
namespace video {

VideoRenderer::VideoRenderer() = default;

VideoRenderer::~VideoRenderer() {
    Stop();
    if (decode_thread_.joinable()) decode_thread_.join();

    // render_queue에 남은 AVFrame 해제
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        while (!render_queue_.empty()) {
            AVFrame* f = render_queue_.front();
            render_queue_.pop();
            av_frame_free(&f);
        }
    }

    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_ctx_);
    DoCloseWindow();
    SDL_Quit();
}

bool VideoRenderer::Initialize(int width, int height, const char* title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        AA_LOG_E() << "[VideoRenderer] SDL 초기화 실패: " << SDL_GetError();
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        AA_LOG_E() << "[VideoRenderer] H264 디코더를 찾을 수 없습니다";
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_ctx_->thread_count = 2;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        AA_LOG_E() << "[VideoRenderer] H264 디코더 열기 실패";
        return false;
    }

    packet_ = av_packet_alloc();
    frame_  = av_frame_alloc();
    if (!packet_ || !frame_) return false;

    aa_width_      = width;
    aa_height_     = height;
    win_width_     = width;
    win_height_    = height;
    window_title_  = title ? title : "Android Auto";

    initialized_.store(true);
    AA_LOG_I() << "[VideoRenderer] 초기화 완료 - " << width << "x" << height;

    decode_thread_ = std::thread(&VideoRenderer::DecodeLoop, this);
    return true;
}

void VideoRenderer::OpenWindow() {
    std::lock_guard<std::mutex> lock(window_cmd_mutex_);
    window_cmd_queue_.push(WindowCmd::OPEN);
}

void VideoRenderer::CloseWindow() {
    std::lock_guard<std::mutex> lock(window_cmd_mutex_);
    window_cmd_queue_.push(WindowCmd::CLOSE);
}

void VideoRenderer::DoOpenWindow() {
    if (window_) return; // 이미 열려 있음

    window_ = SDL_CreateWindow(window_title_.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        aa_width_, aa_height_,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        AA_LOG_E() << "[VideoRenderer] SDL Window 생성 실패: " << SDL_GetError();
        return;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        AA_LOG_E() << "[VideoRenderer] SDL Renderer 생성 실패: " << SDL_GetError();
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    AA_LOG_I() << "[VideoRenderer] 창 열림";
}

void VideoRenderer::DoCloseWindow() {
    if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; tex_width_ = 0; tex_height_ = 0; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    AA_LOG_I() << "[VideoRenderer] 창 닫힘";
}

void VideoRenderer::PushVideoData(const std::vector<uint8_t>& data) {
    if (!initialized_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (frame_queue_.size() >= kMaxQueueSize) {
            // 큐가 꽉 찼으면 가장 오래된 프레임 1개만 버리고 최신으로 교체
            frame_queue_.pop();
            AA_LOG_W() << "[VideoRenderer] 프레임 드랍 (1 frame, queue full)";
        }
        frame_queue_.push(data);
    }
    queue_cv_.notify_one();
}

void VideoRenderer::Run() {
    if (!initialized_.load()) return;
    running_.store(true);
    AA_LOG_I() << "[VideoRenderer] 렌더링 루프 시작";

    SDL_Event event;
    while (running_.load()) {
        // 1. window 제어 커맨드 처리 (메인 스레드에서만 SDL window 조작)
        {
            std::lock_guard<std::mutex> lock(window_cmd_mutex_);
            while (!window_cmd_queue_.empty()) {
                WindowCmd cmd = window_cmd_queue_.front();
                window_cmd_queue_.pop();
                if (cmd == WindowCmd::OPEN)  DoOpenWindow();
                else                         DoCloseWindow();
            }
        }

        // 2. window가 없으면 이벤트만 flush하고 대기
        if (!window_) {
            SDL_PumpEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // 3. SDL 이벤트 처리
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running_.store(false);
                return;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running_.store(false);
                    return;
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    win_width_  = event.window.data1;
                    win_height_ = event.window.data2;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                if (!touch_cb_) break;
                SDL_GetWindowSize(window_, &win_width_, &win_height_);
                int ax = (event.button.x * aa_width_)  / std::max(win_width_,  1);
                int ay = (event.button.y * aa_height_) / std::max(win_height_, 1);
                touch_cb_(ax, ay, 0, event.type == SDL_MOUSEBUTTONDOWN ? 0 : 1);
                break;
            }
            case SDL_MOUSEMOTION: {
                if (!touch_cb_ || event.motion.state == 0) break;
                SDL_GetWindowSize(window_, &win_width_, &win_height_);
                int ax = (event.motion.x * aa_width_)  / std::max(win_width_,  1);
                int ay = (event.motion.y * aa_height_) / std::max(win_height_, 1);
                touch_cb_(ax, ay, 0, 2);
                break;
            }
            default: break;
            }
        }

        // 4. 디코딩된 AVFrame 렌더링 — 최신 프레임만
        AVFrame* frame = nullptr;
        {
            std::unique_lock<std::mutex> lock(render_mutex_);
            if (render_queue_.empty()) {
                render_cv_.wait_for(lock, std::chrono::milliseconds(8));
                if (render_queue_.empty()) continue;
            }
            while (render_queue_.size() > 1) {
                av_frame_free(&render_queue_.front());
                render_queue_.pop();
            }
            frame = render_queue_.front();
            render_queue_.pop();
        }

        EnsureTextureSize(frame->width, frame->height);
        if (texture_) {
            SDL_UpdateYUVTexture(texture_, nullptr,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);
            SDL_RenderClear(renderer_);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
            SDL_RenderPresent(renderer_);
        }
        av_frame_free(&frame);
    }
    AA_LOG_I() << "[VideoRenderer] 렌더링 루프 종료";
}

void VideoRenderer::Stop() {
    running_.store(false);
    initialized_.store(false);
    queue_cv_.notify_all();
    render_cv_.notify_all();
}

void VideoRenderer::DecodeLoop() {
    AA_LOG_I() << "[VideoRenderer] 디코딩 스레드 시작";
    while (initialized_.load()) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !frame_queue_.empty() || !initialized_.load();
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
        // 앞부분 일부(64바이트)에서만 start code 찾기 (성능 최적화)
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
                AA_LOG_W() << "[VideoRenderer] send_packet 실패: " << err;
            }
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // av_frame_clone으로 참조 카운트 올려서 복사 없이 전달
            AVFrame* out = av_frame_clone(frame_);
            if (!out) continue;

            {
                std::lock_guard<std::mutex> lock(render_mutex_);
                if (render_queue_.size() >= kMaxRenderQueueSize) {
                    AVFrame* old = render_queue_.front();
                    render_queue_.pop();
                    av_frame_free(&old);
                }
                render_queue_.push(out);
            }
            render_cv_.notify_one();
        }
    }
    AA_LOG_I() << "[VideoRenderer] 디코딩 스레드 종료";
}

void VideoRenderer::EnsureTextureSize(int width, int height) {
    if (texture_ && tex_width_ == width && tex_height_ == height) return;
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }

    texture_ = SDL_CreateTexture(renderer_,
        SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
        AA_LOG_E() << "[VideoRenderer] SDL_CreateTexture 실패: " << SDL_GetError();
        return;
    }
    tex_width_  = width;
    tex_height_ = height;
    AA_LOG_I() << "[VideoRenderer] 텍스처 생성: " << width << "x" << height;
}

}  // namespace video
}  // namespace aauto
