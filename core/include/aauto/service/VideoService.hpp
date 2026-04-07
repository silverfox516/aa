#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"
#include "aauto/service/VideoSink.hpp"

namespace aauto {
namespace service {

// VideoService is a pure data source. It owns no platform output object.
// The consumer (impl/app) attaches an IVideoSink to receive codec config
// and frames. Attach/detach is the activation signal:
//   - SetSink(non-null) → SendVideoFocusGain (phone starts streaming)
//                       → cached config replayed if already received
//   - SetSink(nullptr)  → SendVideoFocusLoss (phone stops streaming)
//
// While no sink is attached, frames received from the phone are dropped.
class VideoService : public ServiceBase {
   public:
    explicit VideoService(core::HeadunitConfig config);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;
    ServiceType GetType() const override { return ServiceType::VIDEO; }
    std::string GetName() const override { return "VideoService"; }

    // Attach or detach a sink. Thread-safe. Pass nullptr to detach.
    void SetSink(std::shared_ptr<IVideoSink> sink);

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);
    void HandleStartRequest(const std::vector<uint8_t>& payload);
    void HandleCodecConfig(const std::vector<uint8_t>& payload);
    void HandleMediaData(const std::vector<uint8_t>& payload);
    void SendMediaAck();
    void SendVideoFocusGain();
    void SendVideoFocusLoss();

    core::HeadunitConfig config_;
    int32_t              session_id_        = 0;
    uint64_t             video_frame_count_ = 0;

    // sink_, cached_config_, have_codec_data_ are all guarded by sink_mutex_.
    std::mutex                  sink_mutex_;
    std::shared_ptr<IVideoSink> sink_;
    VideoCodecConfig            cached_config_;
    bool                        have_codec_data_ = false;
};

} // namespace service
} // namespace aauto
