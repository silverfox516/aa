#pragma once

#include <memory>
#include <vector>

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace session {

class SessionBuilder {
   public:
    SessionBuilder() = default;

    SessionBuilder& SetTransport(std::shared_ptr<transport::ITransport> transport);
    SessionBuilder& SetCryptoManager(std::shared_ptr<crypto::CryptoManager> crypto);
    SessionBuilder& AddService(std::shared_ptr<service::IService> service);

    std::shared_ptr<Session> Build();

   private:
    std::shared_ptr<transport::ITransport> transport_;
    std::shared_ptr<crypto::CryptoManager> crypto_;
    std::vector<std::shared_ptr<service::IService>> services_;
};

}  // namespace session
}  // namespace aauto
