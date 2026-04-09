#include "aauto/core/AAutoEngine.hpp"

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/ServiceFactory.hpp"
#include "aauto/session/SessionBuilder.hpp"

namespace aauto {
namespace core {

AAutoEngine::AAutoEngine(HeadunitConfig identity, service::ServiceComposition composition)
    : identity_(std::move(identity))
    , composition_(std::move(composition)) {}

std::shared_ptr<session::Session> AAutoEngine::CreateSession(
        std::shared_ptr<transport::ITransport> transport,
        SessionCallbacks                       callbacks) {
    if (!transport) return nullptr;

    auto crypto = std::make_shared<crypto::CryptoManager>(
        std::make_shared<crypto::TlsCryptoStrategy>());

    // Hand the PhoneInfo callback to the service layer so the factory can
    // wire it into ControlService at construction time. Session itself
    // remains agnostic of service-specific notifications.
    service::ServiceContext ctx{
        identity_,
        composition_,
        std::move(callbacks.on_phone_info)
    };
    service::ServiceFactory factory(std::move(ctx));

    session::SessionBuilder builder;
    builder.SetTransport(std::move(transport)).SetCryptoManager(crypto);
    for (auto& svc : factory.CreateAll()) {
        builder.AddService(svc);
    }

    auto session = builder.Build();
    if (!session) return nullptr;

    session->SetClosedCallback(std::move(callbacks.on_closed));
    return session;
}

} // namespace core
} // namespace aauto
