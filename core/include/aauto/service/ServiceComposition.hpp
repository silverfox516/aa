#pragma once

#include <optional>
#include <vector>

#include "aauto/service/AudioService.hpp"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/service/InputService.hpp"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/service/SensorService.hpp"
#include "aauto/service/VideoService.hpp"

namespace aauto {
namespace service {

// What services this head unit exposes to the phone, and with what options.
// Built by the app layer at engine creation time and passed to ServiceFactory.
// Each field's presence (or vector size) controls whether the corresponding
// service is created and advertised in ServiceDiscoveryResponse.
//
// A field left empty / std::nullopt means the platform does not support
// that service, and it will not appear in the discovery response.
struct ServiceComposition {
    std::vector<AudioServiceConfig>            audio_streams;  // empty = no audio
    std::optional<VideoServiceConfig>          video;
    std::optional<InputServiceConfig>          input;
    std::optional<SensorServiceConfig>         sensor;
    std::optional<MicrophoneServiceConfig>     microphone;
    std::optional<BluetoothServiceConfig>      bluetooth;
};

} // namespace service
} // namespace aauto
