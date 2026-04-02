#pragma once

#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <string>

#include "aauto/platform/IVideoOutput.hpp"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct AVFrame;

namespace aauto {
namespace video { class VideoDecoder; }

namespace platform {
namespace sdl2 {

struct Sdl2VideoOutputConfig {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "Android Auto";
};

// SDL2 implementation of IVideoOutput.
// Owns a VideoDecoder and drives the SDL event loop on the main thread via Run().
class Sdl2VideoOutput : public IVideoOutput {
public:
    using Config = Sdl2VideoOutputConfig;

    explicit Sdl2VideoOutput(Config config = {});
    ~Sdl2VideoOutput();

    // Initialize SDL2 + decoder. Call before Run().
    bool Initialize();

    // IVideoOutput
    void Open(int width, int height) override;
    void Close() override;
    void PushVideoData(const std::vector<uint8_t>& data, bool is_codec_config) override;
    void SetTouchCallback(TouchCallback cb) override;

    // SDL event loop — blocks until quit. Call from the main thread.
    void Run();
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void DoOpenWindow();
    void DoCloseWindow();
    void EnsureTextureSize(int width, int height);
    void OnFrame(AVFrame* frame);  // called from decode thread

    Config config_;

    // SDL2
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int tex_width_  = 0;
    int tex_height_ = 0;
    int win_width_  = 1280;
    int win_height_ = 720;

    // Window open/close requests from non-main threads
    enum class WindowCmd { OPEN, CLOSE };
    std::mutex            window_cmd_mutex_;
    std::queue<WindowCmd> window_cmd_queue_;

    // Decoded frames from decode thread → main thread
    std::mutex              render_mutex_;
    std::condition_variable render_cv_;
    std::queue<AVFrame*>    render_queue_;
    static constexpr size_t kMaxRenderQueueSize = 2;

    TouchCallback touch_cb_;

    std::shared_ptr<video::VideoDecoder> decoder_;
    std::atomic<bool> running_{false};
};

} // namespace sdl2
} // namespace platform
} // namespace aauto
