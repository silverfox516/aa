#define LOG_TAG "Sdl2VideoOutput"
#include "aauto/platform/sdl2/Sdl2VideoOutput.hpp"
#include "aauto/video/VideoDecoder.hpp"
#include "aauto/utils/Logger.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>

namespace aauto {
namespace platform {
namespace sdl2 {

Sdl2VideoOutput::Sdl2VideoOutput(Config config)
    : config_(std::move(config))
    , win_width_(config_.width)
    , win_height_(config_.height) {}

Sdl2VideoOutput::~Sdl2VideoOutput() {
    Stop();

    // 남은 AVFrame 해제
    std::lock_guard<std::mutex> lock(render_mutex_);
    while (!render_queue_.empty()) {
        AVFrame* f = render_queue_.front();
        render_queue_.pop();
        av_frame_free(&f);
    }

    DoCloseWindow();
    SDL_Quit();
}

bool Sdl2VideoOutput::Initialize() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        AA_LOG_E() << "[Sdl2VideoOutput] SDL 초기화 실패: " << SDL_GetError();
        return false;
    }

    decoder_ = std::make_shared<video::VideoDecoder>();
    decoder_->SetFrameCallback([this](AVFrame* frame) { OnFrame(frame); });
    if (!decoder_->Initialize()) {
        AA_LOG_E() << "[Sdl2VideoOutput] VideoDecoder 초기화 실패";
        return false;
    }

    AA_LOG_I() << "[Sdl2VideoOutput] 초기화 완료 - "
               << config_.width << "x" << config_.height;
    return true;
}

void Sdl2VideoOutput::Open(int /*width*/, int /*height*/) {
    std::lock_guard<std::mutex> lock(window_cmd_mutex_);
    window_cmd_queue_.push(WindowCmd::OPEN);
}

void Sdl2VideoOutput::Close() {
    std::lock_guard<std::mutex> lock(window_cmd_mutex_);
    window_cmd_queue_.push(WindowCmd::CLOSE);
}

void Sdl2VideoOutput::PushVideoData(const std::vector<uint8_t>& data) {
    if (decoder_) decoder_->PushVideoData(data);
}

void Sdl2VideoOutput::SetTouchCallback(TouchCallback cb) {
    touch_cb_ = std::move(cb);
}

void Sdl2VideoOutput::Run() {
    running_.store(true);
    AA_LOG_I() << "[Sdl2VideoOutput] 렌더링 루프 시작";

    SDL_Event event;
    while (running_.load()) {
        // 1. window 제어 커맨드 처리 (SDL window 조작은 메인 스레드에서만)
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
                int ax = (event.button.x * config_.width)  / std::max(win_width_,  1);
                int ay = (event.button.y * config_.height) / std::max(win_height_, 1);
                touch_cb_(TouchEvent{ax, ay, 0, event.type == SDL_MOUSEBUTTONDOWN ? 0 : 1});
                break;
            }
            case SDL_MOUSEMOTION: {
                if (!touch_cb_ || event.motion.state == 0) break;
                SDL_GetWindowSize(window_, &win_width_, &win_height_);
                int ax = (event.motion.x * config_.width)  / std::max(win_width_,  1);
                int ay = (event.motion.y * config_.height) / std::max(win_height_, 1);
                touch_cb_(TouchEvent{ax, ay, 0, 2});
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
    AA_LOG_I() << "[Sdl2VideoOutput] 렌더링 루프 종료";
}

void Sdl2VideoOutput::Stop() {
    running_.store(false);
    if (decoder_) decoder_->Stop();
    render_cv_.notify_all();
}

void Sdl2VideoOutput::OnFrame(AVFrame* frame) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (render_queue_.size() >= kMaxRenderQueueSize) {
        av_frame_free(&render_queue_.front());
        render_queue_.pop();
    }
    render_queue_.push(frame);
    render_cv_.notify_one();
}

void Sdl2VideoOutput::DoOpenWindow() {
    if (window_) return;

    window_ = SDL_CreateWindow(config_.title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        config_.width, config_.height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        AA_LOG_E() << "[Sdl2VideoOutput] SDL Window 생성 실패: " << SDL_GetError();
        return;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        AA_LOG_E() << "[Sdl2VideoOutput] SDL Renderer 생성 실패: " << SDL_GetError();
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    AA_LOG_I() << "[Sdl2VideoOutput] 창 열림";
}

void Sdl2VideoOutput::DoCloseWindow() {
    if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; tex_width_ = 0; tex_height_ = 0; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    AA_LOG_I() << "[Sdl2VideoOutput] 창 닫힘";
}

void Sdl2VideoOutput::EnsureTextureSize(int width, int height) {
    if (texture_ && tex_width_ == width && tex_height_ == height) return;
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }

    texture_ = SDL_CreateTexture(renderer_,
        SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
        AA_LOG_E() << "[Sdl2VideoOutput] SDL_CreateTexture 실패: " << SDL_GetError();
        return;
    }
    tex_width_  = width;
    tex_height_ = height;
    AA_LOG_I() << "[Sdl2VideoOutput] 텍스처 생성: " << width << "x" << height;
}

} // namespace sdl2
} // namespace platform
} // namespace aauto
