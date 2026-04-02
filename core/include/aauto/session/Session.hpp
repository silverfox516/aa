#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/session/ServiceRegistry.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace session {

enum class SessionState { DISCONNECTED, HANDSHAKE, CONNECTED };

// Coordinates the lifecycle of one Android Auto session:
//   - Runs the AAP handshake (via AapHandshaker)
//   - Owns three worker threads: receive, process, heartbeat
//   - Routes decrypted messages to services (via MessageFramer + ServiceRegistry)
//
// Single responsibility: lifecycle + thread management.
// Protocol details live in AapHandshaker and MessageFramer.
class Session {
   public:
    Session(std::shared_ptr<transport::ITransport> transport,
            std::shared_ptr<crypto::CryptoManager> crypto);
    ~Session();

    void RegisterService(std::shared_ptr<service::IService> service);
    bool Start();
    void Stop();

    SessionState GetState() const { return state_.load(); }

   private:
    void ReceiveLoop();
    void ProcessLoop();

    // Encrypt [type(2) | payload] and send as an AAP packet.
    bool SendEncrypted(uint8_t channel, uint16_t msg_type,
                       const std::vector<uint8_t>& payload);

   private:
    std::shared_ptr<transport::ITransport> transport_;
    std::shared_ptr<crypto::CryptoManager> crypto_;
    ServiceRegistry                        registry_;

    std::atomic<SessionState> state_{SessionState::DISCONNECTED};
    std::once_flag            stop_once_;

    std::thread receive_thread_;
    std::thread process_thread_;

    std::mutex                            queue_mutex_;
    std::condition_variable               queue_cv_;
    std::vector<std::vector<uint8_t>>     message_queue_;
};

} // namespace session
} // namespace aauto
