#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace aauto {
namespace video {

class VideoRenderer {
public:
    using TouchCallback = std::function<void(int x, int y, int pointer_id, int action)>;

    VideoRenderer();
    ~VideoRenderer();

    // SDL/FFmpeg 초기화 (메인 스레드에서 호출). window는 생성하지 않음.
    bool Initialize(int width = 1280, int height = 720, const char* title = "Android Auto");

    // 비디오 스트리밍 시작 시 창 열기 (메인 스레드 큐에 요청 전달)
    void OpenWindow();
    // 비디오 스트리밍 종료 시 창 닫기
    void CloseWindow();

    void SetTouchCallback(TouchCallback cb) { touch_cb_ = std::move(cb); }
    void PushVideoData(const std::vector<uint8_t>& data);
    void Run();   // 메인 스레드 SDL 이벤트 루프
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void DecodeLoop();
    void EnsureTextureSize(int width, int height);

    // FFmpeg
    AVCodecContext* codec_ctx_ = nullptr;
    AVPacket*       packet_    = nullptr;
    AVFrame*        frame_     = nullptr;  // 디코딩용 (DecodeLoop 전용)

    // SDL2
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int tex_width_  = 0;
    int tex_height_ = 0;

    void DoOpenWindow();
    void DoCloseWindow();

    TouchCallback touch_cb_;
    int win_width_  = 1280;
    int win_height_ = 720;
    int aa_width_   = 1280;
    int aa_height_  = 720;
    std::string window_title_ = "Android Auto";

    // 메인 스레드에서 처리할 window 제어 요청
    enum class WindowCmd { OPEN, CLOSE };
    std::mutex              window_cmd_mutex_;
    std::queue<WindowCmd>   window_cmd_queue_;

    // NAL 입력 큐 (수신 스레드 → 디코딩 스레드)
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::vector<uint8_t>> frame_queue_;
    static constexpr size_t kMaxQueueSize = 4;

    // 디코딩 출력 큐 (디코딩 스레드 → 메인 스레드), AVFrame* 전달 (복사 없음)
    std::mutex              render_mutex_;
    std::condition_variable render_cv_;
    std::queue<AVFrame*>    render_queue_;
    static constexpr size_t kMaxRenderQueueSize = 2;

    std::thread         decode_thread_;
    std::atomic<bool>   running_{false};
    std::atomic<bool>   initialized_{false};
};

}  // namespace video
}  // namespace aauto
