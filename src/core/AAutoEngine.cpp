#include "aauto/core/AAutoEngine.hpp"
#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/ServiceFactory.hpp"
#include "aauto/session/SessionBuilder.hpp"

namespace aauto {
namespace core {

AAutoEngine::AAutoEngine(DeviceManager& device_manager,
                         std::shared_ptr<platform::IPlatform> platform,
                         HeadunitConfig config)
    : device_manager_(device_manager)
    , platform_(std::move(platform))
    , config_(std::move(config)) {
    listener_handle_ = device_manager_.AddConnectionListener(
        [this](const transport::DeviceInfo& device, std::shared_ptr<transport::ITransport> transport) {
            OnDeviceConnected(device, transport);
        },
        [this](const std::string& device_id) {
            OnDeviceDisconnected(device_id);
        });
}

AAutoEngine::~AAutoEngine() {
    device_manager_.RemoveListener(listener_handle_);
    std::map<std::string, std::shared_ptr<session::Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions = std::move(active_sessions_);
    }
    for (auto& [id, session] : sessions) {
        session->Stop();
    }
}

bool AAutoEngine::Initialize() { return true; }

void AAutoEngine::OnDeviceConnected(const transport::DeviceInfo& device,
                                    std::shared_ptr<transport::ITransport> transport) {
    auto crypto = std::make_shared<crypto::CryptoManager>(nullptr);

    service::ServiceContext ctx{
        config_,
        platform_ ? platform_->GetVideoOutput() : nullptr,
        platform_ ? platform_->GetAudioOutput() : nullptr,
    };
    service::ServiceFactory factory(std::move(ctx));

    session::SessionBuilder builder;
    builder.SetTransport(transport).SetCryptoManager(crypto);
    for (auto& svc : factory.CreateAll()) {
        builder.AddService(svc);
    }

    auto session = builder.Build();
    if (!session) return;

    if (session->Start()) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        active_sessions_[device.id] = std::move(session);
    }
}

void AAutoEngine::OnDeviceDisconnected(const std::string& device_id) {
    std::shared_ptr<session::Session> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = active_sessions_.find(device_id);
        if (it == active_sessions_.end()) return;
        session = std::move(it->second);
        active_sessions_.erase(it);
    }
    session->Stop();
}

} // namespace core
} // namespace aauto
