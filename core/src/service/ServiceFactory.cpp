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
    std::vector<std::shared_ptr<IService>> peers;

    for (const auto& cfg : ctx_.composition.audio_streams) {
        peers.push_back(std::make_shared<AudioService>(cfg));
    }
    if (ctx_.composition.video) {
        peers.push_back(std::make_shared<VideoService>(*ctx_.composition.video));
    }
    if (ctx_.composition.input) {
        peers.push_back(std::make_shared<InputService>(*ctx_.composition.input));
    }
    if (ctx_.composition.sensor) {
        peers.push_back(std::make_shared<SensorService>(*ctx_.composition.sensor));
    }
    if (ctx_.composition.microphone) {
        peers.push_back(std::make_shared<MicrophoneService>(*ctx_.composition.microphone));
    }
    if (ctx_.composition.bluetooth) {
        peers.push_back(std::make_shared<BluetoothService>(*ctx_.composition.bluetooth));
    }

    auto control = CreateControl(peers);

    std::vector<std::shared_ptr<IService>> all;
    all.reserve(peers.size() + 1);
    all.push_back(std::move(control));
    all.insert(all.end(), peers.begin(), peers.end());
    return all;
}

std::shared_ptr<IService> ServiceFactory::CreateControl(
        const std::vector<std::shared_ptr<IService>>& peers) const {
    auto control = std::make_shared<ControlService>(ctx_.identity, peers);
    if (ctx_.phone_info_cb) {
        control->SetPhoneInfoCallback(ctx_.phone_info_cb);
    }
    return control;
}

} // namespace service
} // namespace aauto
