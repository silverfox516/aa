#define LOG_TAG "AA.CORE.ServiceFactory"
#include "aauto/service/ServiceFactory.hpp"
#include "aauto/service/ControlService.hpp"
#include "aauto/service/AudioService.hpp"
#include "aauto/service/VideoService.hpp"
#include "aauto/service/InputService.hpp"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/service/SensorService.hpp"
#include "aauto/service/BluetoothService.hpp"

namespace aauto {
namespace service {

ServiceFactory::ServiceFactory(ServiceContext context)
    : ctx_(std::move(context)) {}

std::vector<std::shared_ptr<IService>> ServiceFactory::CreateAll() const {
    std::vector<std::shared_ptr<IService>> peers = {
        CreateAudioMedia(),
        CreateAudioGuidance(),
        CreateAudioSystem(),
        CreateVideo(),
        CreateInput(),
        CreateSensor(),
        CreateMicrophone(),
        CreateBluetooth(),
    };

    auto control = CreateControl(peers);

    std::vector<std::shared_ptr<IService>> all;
    all.reserve(peers.size() + 1);
    all.push_back(std::move(control));
    all.insert(all.end(), peers.begin(), peers.end());
    return all;
}

std::shared_ptr<IService> ServiceFactory::CreateControl(
        const std::vector<std::shared_ptr<IService>>& peers) const {
    return std::make_shared<ControlService>(ctx_.config, peers);
}

std::shared_ptr<IService> ServiceFactory::CreateAudioMedia() const {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA, 48000, 2, "Audio (Media)");
}

std::shared_ptr<IService> ServiceFactory::CreateAudioGuidance() const {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE, 16000, 1, "Audio (Guidance)");
}

std::shared_ptr<IService> ServiceFactory::CreateAudioSystem() const {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO, 16000, 1, "Audio (System)");
}

std::shared_ptr<IService> ServiceFactory::CreateVideo() const {
    return std::make_shared<VideoService>(ctx_.config);
}

std::shared_ptr<IService> ServiceFactory::CreateInput() const {
    return std::make_shared<InputService>(ctx_.config);
}

std::shared_ptr<IService> ServiceFactory::CreateSensor() const {
    return std::make_shared<SensorService>();
}

std::shared_ptr<IService> ServiceFactory::CreateMicrophone() const {
    return std::make_shared<MicrophoneService>();
}

std::shared_ptr<IService> ServiceFactory::CreateBluetooth() const {
    return std::make_shared<BluetoothService>(ctx_.config.bluetooth_address);
}

} // namespace service
} // namespace aauto
