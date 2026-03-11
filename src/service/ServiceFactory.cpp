#define LOG_TAG "ServiceFactory"
#include "aauto/service/IService.hpp"
#include "aauto/service/ControlService.hpp"
#include "aauto/service/AudioService.hpp"
#include "aauto/service/VideoService.hpp"
#include "aauto/service/InputService.hpp"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/service/SensorService.hpp"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/video/VideoRenderer.hpp"

namespace aauto {
namespace service {

// Static VideoRenderer 저장소 (main.cpp에서 주입)
std::shared_ptr<void> ServiceFactory::s_video_renderer;

void ServiceFactory::SetVideoRenderer(std::shared_ptr<void> renderer) {
    s_video_renderer = std::move(renderer);
}

std::shared_ptr<IService> ServiceFactory::CreateService(ServiceType type) {
    switch (type) {
        case ServiceType::CONTROL:
            return std::make_shared<ControlService>();
        case ServiceType::AUDIO:
            return CreateAudioMediaService();
        case ServiceType::VIDEO: {
            auto svc = std::make_shared<VideoService>();
            // VideoRenderer 주입
            if (s_video_renderer) {
                svc->SetRenderer(std::static_pointer_cast<aauto::video::VideoRenderer>(s_video_renderer));
            }
            return svc;
        }
        case ServiceType::INPUT: {
            auto svc = std::make_shared<InputService>();
            // VideoRenderer와 연결 - 터치 이벤트를 폰으로 전달
            if (s_video_renderer) {
                svc->AttachToRenderer(std::static_pointer_cast<aauto::video::VideoRenderer>(s_video_renderer));
            }
            return svc;
        }
        case ServiceType::MIC:
            return std::make_shared<MicrophoneService>();
        case ServiceType::SENSOR:
            return std::make_shared<SensorService>();
        case ServiceType::BLUETOOTH:
            return std::make_shared<BluetoothService>();
        default:
            return nullptr;
    }
}

std::shared_ptr<IService> ServiceFactory::CreateAudioMediaService() {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA, 48000, 2,
        "Audio (Media)");
}

std::shared_ptr<IService> ServiceFactory::CreateAudioSystemService() {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO, 16000, 1,
        "Audio (System)");
}

std::shared_ptr<IService> ServiceFactory::CreateAudioGuidanceService() {
    return std::make_shared<AudioService>(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE, 16000, 1,
        "Audio (Guidance)");
}

}  // namespace service
}  // namespace aauto
