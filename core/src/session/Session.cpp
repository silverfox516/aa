#define LOG_TAG "Session"
#include "aauto/session/Session.hpp"
#include "aauto/session/AapHandshaker.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/session/MessageFramer.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"

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

    AA_LOG_I() << "서비스 등록됨: " << service->GetName()
               << " (Ch:" << (int)service->GetChannel() << ")";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Session::Start() {
    if (!transport_ || !crypto_) return false;

    if (!transport_->Connect({})) {
        AA_LOG_E() << "Transport 연결 실패";
        return false;
    }

    SessionState expected = SessionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, SessionState::HANDSHAKE)) {
        return false;
    }

    AA_LOG_I() << "핸드셰이크를 시작합니다...";

    AapHandshaker handshaker(*transport_, *crypto_);
    if (!handshaker.Run()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    state_.store(SessionState::CONNECTED);
    AA_LOG_I() << "세션 연결 완료 (CONNECTED).";

    // Seed leftover bytes from handshake into the receive queue
    auto leftover = handshaker.TakeLeftoverBytes();
    if (!leftover.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        message_queue_.push_back(std::move(leftover));
    }

    receive_thread_  = std::thread(&Session::ReceiveLoop,  this);
    process_thread_  = std::thread(&Session::ProcessLoop,  this);
    heartbeat_thread_ = std::thread(&Session::HeartbeatLoop, this);

    return true;
}

void Session::Stop() {
    std::call_once(stop_once_, [this] {
        state_.store(SessionState::DISCONNECTED);
        AA_LOG_I() << "세션 종료 (DISCONNECTED).";

        if (transport_) transport_->Disconnect();

        for (auto& svc : registry_.All()) svc->OnSessionStopped();

        queue_cv_.notify_all();

        auto self_id = std::this_thread::get_id();
        if (receive_thread_.joinable()  && receive_thread_.get_id()  != self_id) receive_thread_.join();
        if (process_thread_.joinable()  && process_thread_.get_id()  != self_id) process_thread_.join();
        if (heartbeat_thread_.joinable() && heartbeat_thread_.get_id() != self_id) heartbeat_thread_.join();
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
    MessageFramer framer([this](AapMessage msg) {
        // 핸드셰이크 완료 이후 모든 메시지는 암호화됨.
        // FLAG_ENCRYPTED(0x08)는 ch=0 control 패킷에만 명시적으로 세팅되고,
        // 미디어 채널 멀티프래그먼트(0x09/0x0a)에는 비트가 없지만 실제로는 암호화되어 있어
        // 항상 decrypt를 시도한다.
        std::vector<uint8_t> full_message = crypto_->DecryptData(msg.payload);
        if (full_message.empty()) {
            AA_LOG_E() << "[ProcessLoop] 복호화 실패 (Ch:" << (int)msg.channel
                       << " encrypted_flag=" << msg.encrypted << "), 메시지 버림";
            return;
        }

        if (full_message.size() < aap::TYPE_SIZE) {
            AA_LOG_E() << "[ProcessLoop] 복호화 실패 또는 메시지 너무 짧음 (Ch:"
                       << (int)msg.channel << ")";
            return;
        }

        uint16_t msg_type = (full_message[0] << 8) | full_message[1];

        // Audio(ch 1-3), Video(ch 4), Input(ch 5) 채널 및 Ping 메시지는 로그 생략
        bool is_noisy_ch = (msg.channel >= 1 && msg.channel <= 5);
        bool is_ping = (msg_type == aap::msg::PING_REQUEST || msg_type == aap::msg::PING_RESPONSE);
        if (!is_noisy_ch && !is_ping) {
            AA_LOG_I() << "[ProcessLoop] 수신 ["
                       << utils::ProtocolUtil::GetChannelName(msg.channel) << "] "
                       << utils::ProtocolUtil::GetMessageTypeName(msg_type)
                       << " (0x" << std::hex << msg_type << std::dec
                       << ") Len:" << (full_message.size() - aap::TYPE_SIZE);
            AA_LOG_D() << "[Session] << " << utils::ProtocolUtil::DumpHex(full_message, 16);
        }

        auto service = registry_.Find(msg.channel);
        if (service) {
            std::vector<uint8_t> payload(full_message.begin() + aap::TYPE_SIZE, full_message.end());
            service->HandleMessage(msg_type, payload);
        } else {
            AA_LOG_W() << "[ProcessLoop] 서비스 없음 (Ch:" << (int)msg.channel << ")";
        }
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

void Session::HeartbeatLoop() {
    while (state_.load() == SessionState::CONNECTED) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (state_.load() != SessionState::CONNECTED) break;

        aap_protobuf::service::control::message::PingRequest ping;
        ping.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        std::vector<uint8_t> payload(ping.ByteSizeLong());
        if (ping.SerializeToArray(payload.data(), payload.size())) {
            if (!SendEncrypted(aap::CH_CONTROL, aap::msg::PING_REQUEST, payload)) {
                AA_LOG_E() << "Ping 송신 실패 — 연결 종료";
            }
        }
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
