#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
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

// Item queued for async send. Carries the pre-packed plaintext (type prefix
// + payload) plus the AAP header fields needed to rebuild the wire packet
// after encryption.
struct SendItem {
    uint8_t              channel;
    uint8_t              flags;
    std::vector<uint8_t> plain;   // [type(2) | payload]
};

class Session {
   public:
    Session(std::shared_ptr<transport::ITransport> transport,
            std::shared_ptr<crypto::CryptoManager> crypto);
    ~Session();

    void RegisterService(std::shared_ptr<service::IService> service);

    void SetClosedCallback(std::function<void()> on_closed);

    bool Start();
    void Stop();

    SessionState GetState() const { return state_.load(); }

    std::shared_ptr<service::IService> GetService(service::ServiceType type) const;
    std::vector<std::shared_ptr<service::IService>>
    GetServicesByType(service::ServiceType type) const;

   private:
    void ReceiveLoop();
    void ProcessLoop();
    void SendLoop();

    // Queue a message for async send. Returns immediately (non-blocking).
    bool SendEncrypted(uint8_t channel, uint16_t msg_type,
                       const std::vector<uint8_t>& payload);

   private:
    std::shared_ptr<transport::ITransport> transport_;
    std::shared_ptr<crypto::CryptoManager> crypto_;
    ServiceRegistry                        registry_;
    std::function<void()>                  closed_cb_;

    std::atomic<SessionState> state_{SessionState::DISCONNECTED};
    std::once_flag            stop_once_;

    std::thread receive_thread_;
    std::thread process_thread_;
    std::thread send_thread_;

    // Receive queue (ReceiveLoop → ProcessLoop)
    std::mutex                            queue_mutex_;
    std::condition_variable               queue_cv_;
    std::vector<std::vector<uint8_t>>     message_queue_;

    // Send queue (any thread → SendLoop)
    static constexpr size_t               kMaxSendQueue = 256;
    std::mutex                            send_queue_mutex_;
    std::condition_variable               send_queue_cv_;
    std::deque<SendItem>                  send_queue_;
};

} // namespace session
} // namespace aauto
