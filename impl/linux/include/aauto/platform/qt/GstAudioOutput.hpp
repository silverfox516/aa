#pragma once

#include "aauto/platform/IAudioOutput.hpp"

#include <atomic>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace aauto {
namespace platform {
namespace qt {

class GstAudioOutput : public IAudioOutput {
   public:
    GstAudioOutput() = default;
    ~GstAudioOutput() override;

    bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) override;
    void Start() override;
    void Close() override;
    void PushAudioData(const std::vector<uint8_t>& data) override;

   private:
    GstElement* pipeline_ = nullptr;
    GstAppSrc*  appsrc_   = nullptr;
    std::atomic<bool> running_{false};

    uint32_t sample_rate_ = 0;
    uint8_t  channels_    = 0;
    uint8_t  bits_        = 0;
};

} // namespace qt
} // namespace platform
} // namespace aauto
