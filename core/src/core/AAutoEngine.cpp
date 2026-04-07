#include "aauto/core/AAutoEngine.hpp"

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/ServiceFactory.hpp"
#include "aauto/session/SessionBuilder.hpp"

namespace aauto {
namespace core {

AAutoEngine::AAutoEngine(HeadunitConfig config)
    : config_(std::move(config)) {}

std::shared_ptr<session::Session> AAutoEngine::CreateSession(
        std::shared_ptr<transport::ITransport> transport,
        session::SessionCallbacks              callbacks) {
    if (!transport) return nullptr;

    auto crypto = std::make_shared<crypto::CryptoManager>(
        std::make_shared<crypto::TlsCryptoStrategy>());

    service::ServiceContext ctx{config_};
    service::ServiceFactory factory(std::move(ctx));

    session::SessionBuilder builder;
    builder.SetTransport(std::move(transport)).SetCryptoManager(crypto);
    for (auto& svc : factory.CreateAll()) {
        builder.AddService(svc);
    }

    auto session = builder.Build();
    if (!session) return nullptr;

    session->SetCallbacks(std::move(callbacks));
    return session;
}

} // namespace core
} // namespace aauto
