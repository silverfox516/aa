#define LOG_TAG "AA.IMPL.OpenSLAudioSink"

#include "aauto/output/OpenSLAudioSink.hpp"

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace output {
namespace android {

OpenSLAudioSink::OpenSLAudioSink() = default;

OpenSLAudioSink::~OpenSLAudioSink() {
    ClosePipeline();
}

void OpenSLAudioSink::OnAudioFormat(const service::AudioFormat& format) {
    AA_LOG_I() << "OnAudioFormat " << format.sample_rate << "Hz/"
               << int(format.channel_count) << "ch/"
               << int(format.bits_per_sample) << "bit";

    // Re-formatting an open pipeline is currently a no-op: AAP audio formats
    // are stable per channel for the life of the session. If a real change
    // is observed, tear down and re-open.
    if (is_open_.load()) {
        AA_LOG_W() << "OnAudioFormat called while open — ignoring";
        return;
    }
    OpenPipeline(format.sample_rate, format.channel_count, format.bits_per_sample);
    if (is_open_.load() && player_) {
        (*player_)->SetPlayState(player_, SL_PLAYSTATE_PLAYING);
    }
}

void OpenSLAudioSink::OnAudioData(const uint8_t* data, size_t size, uint64_t /*pts_us*/) {
    if (!is_open_.load() || !buffer_queue_ || size == 0) return;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (pending_.size() >= kMaxPending) {
        pending_.pop_front();
        AA_LOG_W() << "Audio backlog full, dropping oldest frame";
    }
    pending_.emplace_back(data, data + size);
    DrainPending();
}

bool OpenSLAudioSink::OpenPipeline(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    SLresult result = slCreateEngine(&engine_obj_, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        AA_LOG_E() << "slCreateEngine failed: " << result;
        return false;
    }
    (*engine_obj_)->Realize(engine_obj_, SL_BOOLEAN_FALSE);
    (*engine_obj_)->GetInterface(engine_obj_, SL_IID_ENGINE, &engine_);

    (*engine_)->CreateOutputMix(engine_, &mix_obj_, 0, nullptr, nullptr);
    (*mix_obj_)->Realize(mix_obj_, SL_BOOLEAN_FALSE);

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

    SLDataLocator_OutputMix loc_outmix{SL_DATALOCATOR_OUTPUTMIX, mix_obj_};
    SLDataSink              audio_sink{&loc_outmix, nullptr};

    const SLInterfaceID ids[]  = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean     reqs[] = {SL_BOOLEAN_TRUE};

    result = (*engine_)->CreateAudioPlayer(
        engine_, &player_obj_, &audio_src, &audio_sink, 1, ids, reqs);
    if (result != SL_RESULT_SUCCESS) {
        AA_LOG_E() << "CreateAudioPlayer failed: " << result;
        ClosePipeline();
        return false;
    }
    (*player_obj_)->Realize(player_obj_, SL_BOOLEAN_FALSE);
    (*player_obj_)->GetInterface(player_obj_, SL_IID_PLAY, &player_);
    (*player_obj_)->GetInterface(player_obj_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &buffer_queue_);

    (*buffer_queue_)->RegisterCallback(buffer_queue_, BufferQueueCallback, this);

    is_open_.store(true);
    AA_LOG_I() << "Pipeline opened " << sample_rate << "Hz/" << int(channels) << "ch";
    return true;
}

void OpenSLAudioSink::ClosePipeline() {
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

    AA_LOG_I() << "Pipeline closed";
}

void OpenSLAudioSink::DrainPending() {
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
            pending_.push_front(std::move(live_[write_idx_]));
            AA_LOG_E() << "OpenSL Enqueue failed: " << r;
            break;
        }
    }
}

void OpenSLAudioSink::BufferQueueCallback(SLAndroidSimpleBufferQueueItf /*bq*/, void* context) {
    static_cast<OpenSLAudioSink*>(context)->OnBufferConsumed();
}

void OpenSLAudioSink::OnBufferConsumed() {
    if (!is_open_.load()) return;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    --enqueued_;
    DrainPending();
}

} // namespace android
} // namespace output
} // namespace aauto
