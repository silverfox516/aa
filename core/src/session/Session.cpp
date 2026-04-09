#define LOG_TAG "AA.CORE.Session"
#include "aauto/session/Session.hpp"
#include "aauto/service/ControlService.hpp"
#include "aauto/session/AapHandshaker.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/session/MessageFramer.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"

#include <unordered_map>

namespace aauto {
namespace session {

Session::Session(std::shared_ptr<transport::ITransport> transport,
                 std::shared_ptr<crypto::CryptoManager> crypto)
    : transport_(std::move(transport)), crypto_(std::move(crypto)) {}

Session::~Session() { Stop(); }

// ---------------------------------------------------------------------------
// Service registration
// ---------------------------------------------------------------------------

void Session::RegisterService(std::shared_ptr<service::IService> service) {
    if (!service) return;

    registry_.Register(service);

    service->SetSendCallback([this](uint8_t ch, uint16_t type, const std::vector<uint8_t>& pl) {
        return SendEncrypted(ch, type, pl);
    });

    AA_LOG_I() << "Service registered: " << service->GetName()
               << " (Ch:" << (int)service->GetChannel() << ")";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::shared_ptr<service::IService> Session::GetService(service::ServiceType type) const {
    for (auto& svc : registry_.All()) {
        if (svc->GetType() == type) return svc;
    }
    return nullptr;
}

std::vector<std::shared_ptr<service::IService>>
Session::GetServicesByType(service::ServiceType type) const {
    std::vector<std::shared_ptr<service::IService>> out;
    for (auto& svc : registry_.All()) {
        if (svc->GetType() == type) out.push_back(svc);
    }
    return out;
}

void Session::SetClosedCallback(std::function<void()> on_closed) {
    closed_cb_ = std::move(on_closed);
}

bool Session::Start() {
    if (!transport_ || !crypto_) return false;

    if (!transport_->Connect({})) {
        AA_LOG_E() << "Transport connection failed";
        return false;
    }

    SessionState expected = SessionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, SessionState::HANDSHAKE)) {
        return false;
    }

    AA_LOG_I() << "Starting handshake...";

    AapHandshaker handshaker(*transport_, *crypto_);
    if (!handshaker.Run()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    state_.store(SessionState::CONNECTED);
    AA_LOG_I() << "Session connected (CONNECTED).";

    // Wire ControlService -> Session::Stop so PING timeout / BYEBYE
    // can tear down the session from the protocol layer.
    if (auto svc = GetService(service::ServiceType::CONTROL)) {
        if (auto cs = std::dynamic_pointer_cast<service::ControlService>(svc)) {
            cs->SetSessionCloseCallback([this]() {
                Stop();
            });
        }
    }

    // Seed leftover bytes from handshake into the receive queue
    auto leftover = handshaker.TakeLeftoverBytes();
    if (!leftover.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        message_queue_.push_back(std::move(leftover));
    }

    receive_thread_ = std::thread(&Session::ReceiveLoop, this);
    process_thread_ = std::thread(&Session::ProcessLoop, this);

    return true;
}

void Session::Stop() {
    std::call_once(stop_once_, [this] {
        state_.store(SessionState::DISCONNECTED);
        AA_LOG_I() << "Session stopped (DISCONNECTED).";

        if (transport_) transport_->Disconnect();

        for (auto& svc : registry_.All()) svc->OnSessionStopped();

        queue_cv_.notify_all();

        auto self_id = std::this_thread::get_id();
        if (receive_thread_.joinable() && receive_thread_.get_id() != self_id) receive_thread_.join();
        if (process_thread_.joinable() && process_thread_.get_id() != self_id) process_thread_.join();

        if (closed_cb_) closed_cb_();
    });
}

// ---------------------------------------------------------------------------
// Worker threads
// ---------------------------------------------------------------------------

void Session::ReceiveLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        auto data = transport_->Receive();
        if (data.empty()) continue;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push_back(std::move(data));
        }
        queue_cv_.notify_one();
    }
}

void Session::ProcessLoop() {
    // Per-channel plaintext payload accumulator. Each fragment is decrypted
    // immediately (matching aasdk MessageInStream model) and appended here;
    // the LAST/BULK fragment triggers dispatch.
    std::unordered_map<uint8_t, std::vector<uint8_t>> channel_payloads;

    MessageFramer framer([this, &channel_payloads](AapFragment frag) {
        auto& payload = channel_payloads[frag.channel];

        // FIRST resets the channel buffer. (Stray MIDDLE/LAST without a
        // preceding FIRST is unexpected; we tolerate it by appending and
        // letting the dispatcher decide.)
        if (frag.is_first) {
            payload.clear();
        }

        if (frag.encrypted) {
            if (!crypto_->DecryptData(frag.ciphertext, payload)) {
                AA_LOG_E() << "[ProcessLoop] Decrypt failed (Ch:" << (int)frag.channel
                           << " encrypted_flag=1), dropping message";
                payload.clear();
                return;
            }
        } else {
            payload.insert(payload.end(), frag.ciphertext.begin(), frag.ciphertext.end());
        }

        if (!frag.is_last) return;  // wait for more fragments

        if (payload.size() < aap::TYPE_SIZE) {
            AA_LOG_E() << "[ProcessLoop] message too short (Ch:" << (int)frag.channel << ")";
            payload.clear();
            return;
        }

        uint16_t msg_type = (payload[0] << 8) | payload[1];

        // Suppress logging for high-frequency channels: Audio(1-3), Video(4), Input(5), Ping
        bool is_noisy_ch = (frag.channel >= 1 && frag.channel <= 5);
        bool is_ping = (msg_type == aap::msg::PING_REQUEST || msg_type == aap::msg::PING_RESPONSE);
        if (!is_noisy_ch && !is_ping) {
            AA_LOG_I() << "[ProcessLoop] recv ["
                       << utils::ProtocolUtil::GetChannelName(frag.channel) << "] "
                       << utils::ProtocolUtil::GetMessageTypeName(msg_type)
                       << " (0x" << std::hex << msg_type << std::dec
                       << ") Len:" << (payload.size() - aap::TYPE_SIZE);
            AA_LOG_D() << "[Session] << " << utils::ProtocolUtil::DumpHex(payload, 16);
        }

        auto service = registry_.Find(frag.channel);
        if (service) {
            std::vector<uint8_t> service_payload(payload.begin() + aap::TYPE_SIZE, payload.end());
            service->HandleMessage(msg_type, service_payload);
        } else {
            AA_LOG_W() << "[ProcessLoop] no service for Ch:" << (int)frag.channel;
        }

        payload.clear();
    });

    while (state_.load() != SessionState::DISCONNECTED) {
        std::vector<std::vector<uint8_t>> chunks;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !message_queue_.empty() || state_.load() == SessionState::DISCONNECTED;
            });
            if (state_.load() == SessionState::DISCONNECTED && message_queue_.empty()) break;
            chunks = std::move(message_queue_);
        }

        for (auto& chunk : chunks) framer.Feed(chunk);
    }
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

bool Session::SendEncrypted(uint8_t channel, uint16_t msg_type,
                             const std::vector<uint8_t>& payload) {
    if (state_.load() == SessionState::DISCONNECTED) return false;

    auto packet = aap::Pack(channel, msg_type, payload);
    std::vector<uint8_t> plain(packet.begin() + aap::HEADER_SIZE, packet.end());

    size_t dump_len = (msg_type == aap::msg::SERVICE_DISCOVERY_RESP) ? 0 : 16;
    AA_LOG_D() << "[Session] >> " << utils::ProtocolUtil::DumpHex(plain, dump_len);

    auto encrypted = crypto_->EncryptData(plain);
    uint16_t enc_len = static_cast<uint16_t>(encrypted.size());
    packet.resize(aap::HEADER_SIZE + enc_len);
    packet[2] = (enc_len >> 8) & 0xFF;
    packet[3] =  enc_len       & 0xFF;
    std::copy(encrypted.begin(), encrypted.end(), packet.begin() + aap::HEADER_SIZE);

    return transport_->Send(packet);
}

}  // namespace session
}  // namespace aauto
