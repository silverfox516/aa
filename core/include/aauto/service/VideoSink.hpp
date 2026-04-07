#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aauto {
namespace service {

// Codec configuration delivered to a VideoSink before any frames.
//
// Sources:
//   - width/height/fps come from the HU's advertised VideoConfiguration in
//     ServiceDiscoveryResponse and are known at VideoService construction time.
//   - codec_data is the raw codec-specific data blob the phone sends in the
//     AAP MEDIA_CODEC_CONFIG message (for H.264 BP this is csd-0, typically
//     SPS+PPS concatenated as Annex-B NAL units). The service forwards it
//     opaquely; the sink decides how to feed it to its decoder.
//
// Replayed automatically when a sink is attached after the configuration
// (specifically codec_data) has been cached.
struct VideoCodecConfig {
    int                  width  = 0;
    int                  height = 0;
    int                  fps    = 0;
    std::vector<uint8_t> codec_data;
};

// One encoded video access unit.
struct VideoFrame {
    const uint8_t* data        = nullptr;
    size_t         size        = 0;
    uint64_t       pts_us      = 0;
    bool           is_keyframe = false;
};

// Sink interface implemented by the consumer (impl/app) to receive
// encoded video data from a VideoService. The service does not own
// any platform output object — it only forwards data to the attached
// sink. A null sink means the session is inactive (frames are dropped).
class IVideoSink {
   public:
    virtual ~IVideoSink() = default;

    // Called once after attach if the service has a cached config,
    // and again whenever the phone re-negotiates the codec config.
    virtual void OnVideoConfig(const VideoCodecConfig& config) = 0;

    // Called for every received frame while this sink is attached.
    virtual void OnVideoFrame(const VideoFrame& frame) = 0;
};

} // namespace service
} // namespace aauto
