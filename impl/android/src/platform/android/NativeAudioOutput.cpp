#define LOG_TAG "AA.NativeAudioOutput"

#include "aauto/platform/android/NativeAudioOutput.hpp"

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {
namespace android {

NativeAudioOutput::NativeAudioOutput() = default;

NativeAudioOutput::~NativeAudioOutput() {
    Close();
}

bool NativeAudioOutput::Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    if (is_open_.load()) {
        AA_LOG_W() << "Already open";
        return true;
    }

    // Create OpenSL ES engine
    SLresult result = slCreateEngine(&engine_obj_, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        AA_LOG_E() << "slCreateEngine failed: " << result;
        return false;
    }
    (*engine_obj_)->Realize(engine_obj_, SL_BOOLEAN_FALSE);
    (*engine_obj_)->GetInterface(engine_obj_, SL_IID_ENGINE, &engine_);

    // Create output mix
    (*engine_)->CreateOutputMix(engine_, &mix_obj_, 0, nullptr, nullptr);
    (*mix_obj_)->Realize(mix_obj_, SL_BOOLEAN_FALSE);

    // Configure audio source (PCM buffer queue)
    SLDataLocator_AndroidSimpleBufferQueue loc_bq{
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kNumBuffers
    };
    SLDataFormat_PCM fmt_pcm{
        SL_DATAFORMAT_PCM,
        static_cast<SLuint32>(channels),
        sample_rate * 1000,  // milliHz
        static_cast<SLuint32>(bits),
        static_cast<SLuint32>(bits),
        channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                      : SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audio_src{&loc_bq, &fmt_pcm};

    // Configure audio sink (output mix)
    SLDataLocator_OutputMix loc_outmix{SL_DATALOCATOR_OUTPUTMIX, mix_obj_};
    SLDataSink audio_sink{&loc_outmix, nullptr};

    const SLInterfaceID ids[]  = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean     reqs[] = {SL_BOOLEAN_TRUE};

    result = (*engine_)->CreateAudioPlayer(
        engine_, &player_obj_, &audio_src, &audio_sink, 1, ids, reqs);
    if (result != SL_RESULT_SUCCESS) {
        AA_LOG_E() << "CreateAudioPlayer failed: " << result;
        Close();
        return false;
    }

    (*player_obj_)->Realize(player_obj_, SL_BOOLEAN_FALSE);
    (*player_obj_)->GetInterface(player_obj_, SL_IID_PLAY, &player_);
    (*player_obj_)->GetInterface(player_obj_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &buffer_queue_);

    (*buffer_queue_)->RegisterCallback(buffer_queue_, BufferQueueCallback, this);

    is_open_.store(true);
    AA_LOG_I() << "Opened " << sample_rate << "Hz " << (int)channels
               << "ch " << (int)bits << "bit";
    return true;
}

void NativeAudioOutput::Start() {
    if (!is_open_.load() || !player_) return;
    (*player_)->SetPlayState(player_, SL_PLAYSTATE_PLAYING);
    AA_LOG_I() << "Playback started";
}

void NativeAudioOutput::Close() {
    if (!is_open_.exchange(false)) return;

    if (player_)     { (*player_)->SetPlayState(player_, SL_PLAYSTATE_STOPPED); }
    if (player_obj_) { (*player_obj_)->Destroy(player_obj_); player_obj_ = nullptr; }
    if (mix_obj_)    { (*mix_obj_)->Destroy(mix_obj_);       mix_obj_    = nullptr; }
    if (engine_obj_) { (*engine_obj_)->Destroy(engine_obj_); engine_obj_ = nullptr; }

    player_       = nullptr;
    engine_       = nullptr;
    buffer_queue_ = nullptr;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_.clear();
    enqueued_  = 0;
    write_idx_ = 0;

    AA_LOG_I() << "Closed";
}

void NativeAudioOutput::PushAudioData(const std::vector<uint8_t>& data) {
    if (!is_open_.load() || !buffer_queue_) return;

    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Prevent unbounded backlog: drop the oldest frame if too far behind.
    if (pending_.size() >= kMaxPending) {
        pending_.pop_front();
        AA_LOG_W() << "Audio backlog full, dropping oldest frame";
    }
    pending_.push_back(data);
    DrainPending();
}

// ─── Private ──────────────────────────────────────────────────────────────────

void NativeAudioOutput::DrainPending() {
    // Enqueue as many pending buffers as there are free OpenSL slots.
    // Caller must hold queue_mutex_.
    while (enqueued_ < static_cast<int>(kNumBuffers) && !pending_.empty()) {
        live_[write_idx_] = std::move(pending_.front());
        pending_.pop_front();

        SLresult r = (*buffer_queue_)->Enqueue(
            buffer_queue_,
            live_[write_idx_].data(),
            live_[write_idx_].size());

        if (r == SL_RESULT_SUCCESS) {
            write_idx_ = (write_idx_ + 1) % kNumBuffers;
            ++enqueued_;
        } else {
            // Enqueue failed — put the data back and stop trying.
            pending_.push_front(std::move(live_[write_idx_]));
            AA_LOG_E() << "OpenSL Enqueue failed: " << r;
            break;
        }
    }
}

void NativeAudioOutput::BufferQueueCallback(SLAndroidSimpleBufferQueueItf /*bq*/,
                                            void* context) {
    static_cast<NativeAudioOutput*>(context)->OnBufferConsumed();
}

void NativeAudioOutput::OnBufferConsumed() {
    if (!is_open_.load()) return;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    // One slot has been freed by OpenSL — record it and fill from pending.
    --enqueued_;
    DrainPending();
}

}  // namespace android
}  // namespace platform
}  // namespace aauto
