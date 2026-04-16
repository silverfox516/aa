#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

    // Stable per-session id ("s1", "s2", ...) assigned at construction time.
    // Used as the log correlation tag before the phone identifies itself.
    uint32_t           GetSessionIndex() const { return session_index_; }
    std::shared_ptr<const std::string> GetSessionTag() const;

    // Replace the session tag with "s<N>:<phone_name>" once SD_REQ arrives.
    // Worker threads pick up the change on their next loop iteration.
    void               SetPhoneTag(const std::string& phone_name);

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

    // Session identifier. session_tag_ptr_ is swapped atomically (free
    // std::atomic_load/store on shared_ptr — std::atomic<shared_ptr<T>>
    // requires C++20; this codebase is C++17).
    uint32_t                                  session_index_ = 0;
    std::shared_ptr<const std::string>        session_tag_ptr_;

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
