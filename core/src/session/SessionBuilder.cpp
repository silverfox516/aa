#include "aauto/session/SessionBuilder.hpp"

namespace aauto {
namespace session {

SessionBuilder& SessionBuilder::SetTransport(std::shared_ptr<transport::ITransport> transport) {
    transport_ = std::move(transport);
    return *this;
}

SessionBuilder& SessionBuilder::SetCryptoManager(std::shared_ptr<crypto::CryptoManager> crypto) {
    crypto_ = std::move(crypto);
    return *this;
}

SessionBuilder& SessionBuilder::AddService(std::shared_ptr<service::IService> service) {
    if (service) {
        services_.push_back(std::move(service));
    }
    return *this;
}

std::shared_ptr<Session> SessionBuilder::Build() {
    if (!transport_ || !crypto_) {
        return nullptr;  // 필수 의존성 누락
    }

    auto session = std::make_shared<Session>(transport_, crypto_);
    for (const auto& svc : services_) {
        session->RegisterService(svc);
    }

    return session;
}

}  // namespace session
}  // namespace aauto
