#define LOG_TAG "Session"
#include "aauto/session/Session.hpp"
#include <iomanip>

#include "aauto/utils/Logger.hpp"
#include "aauto/service/ControlService.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"

namespace aauto {
namespace session {

Session::Session(std::shared_ptr<transport::ITransport> transport, std::shared_ptr<crypto::CryptoManager> crypto)
    : transport_(std::move(transport)), crypto_(std::move(crypto)), state_(SessionState::DISCONNECTED) {}

Session::~Session() { Stop(); }

void Session::RegisterService(std::shared_ptr<service::IService> service) {
    if (!service) return;

    {
        std::unique_lock<std::shared_mutex> lock(services_mutex_);
        uint8_t channel = 0;
        if (service->GetType() == service::ServiceType::CONTROL) {
            channel = 0;
        } else if (service->GetType() == service::ServiceType::BLUETOOTH) {
            channel = 255;
        } else {
            // 이미 할당된 채널들 중 0과 255를 제외하고 비어있는 가장 작은 값 찾기
            // 여기서는 단순 등록 순서대로 하되, Bluetooth 같은 특수 채널과 겹치지 않게 관리
            channel = 1;
            while (services_.count(channel)) {
                channel++;
            }
        }
        service->SetChannel(channel);
        services_[channel] = service;
    }

    // 서비스가 단독으로 데이터를 보낼 수 있도록 콜백 주입
    service->SetSendCallback([this](uint8_t channel, uint16_t msg_type, const std::vector<uint8_t>& payload) -> bool {
        // 암호화 전 패킷 패킹
        auto packet = aap::Pack(channel, msg_type, payload);
        
        // 데이터 암호화 (헤더 4바이트 제외한 부분)
        std::vector<uint8_t> plain_part(packet.begin() + aap::HEADER_SIZE, packet.end());
        
        // 서비스 디스커버리 응답(0x06)은 전체 데이터를 로깅 (사용자 요청)
        size_t dump_len = (msg_type == 0x06) ? 0 : 16;
        AA_LOG_D() << "[Session] >> 송신 (Plain) " << utils::ProtocolUtil::DumpHex(plain_part, dump_len);
        
        std::vector<uint8_t> encrypted_payload = crypto_->EncryptData(plain_part);

        // 암호화된 데이터로 새로운 패킷 조립 (encrypted bit은 Pack()에서 이미 설정됨)
        uint16_t total_len = static_cast<uint16_t>(encrypted_payload.size());
        packet.resize(aap::HEADER_SIZE + total_len);
        packet[2] = (total_len >> 8) & 0xFF;
        packet[3] = total_len & 0xFF;
        std::copy(encrypted_payload.begin(), encrypted_payload.end(), packet.begin() + aap::HEADER_SIZE);

        return transport_->Send(packet);
    });

    if (service->GetType() == service::ServiceType::CONTROL) {
        auto control_service = std::static_pointer_cast<service::ControlService>(service);
        control_service->SetServiceProvider([this]() {
            std::vector<std::shared_ptr<service::IService>> list;
            std::shared_lock<std::shared_mutex> lock(services_mutex_);
            // 서비스 리스트를 채널 순서대로 정렬하여 반환 (Discovery Response 생성용)
            for (uint8_t i = 0; i < services_.size(); ++i) {
                if (services_.count(i)) {
                    list.push_back(services_[i]);
                }
            }
            return list;
        });
    }

    AA_LOG_I() << "서비스 등록됨: " << service->GetName();
}

bool Session::Start() {
    if (!transport_ || !crypto_) return false;

    if (!transport_->Connect({})) {
        AA_LOG_E() << "Transport 연결 실패";
        return false;
    }

    // 상태 전환: DISCONNECTED -> HANDSHAKE
    SessionState expected = SessionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, SessionState::HANDSHAKE)) {
        return false;
    }

    AA_LOG_I() << "핸드셰이크를 시작합니다...";

    // 1단계: 버전 교환
    if (!DoVersionExchange()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    // 2단계: SSL 핸드셰이크
    if (!DoSslHandshake()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    // 3단계: 인증 완료 알림
    if (!SendSslAuthComplete()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    state_.store(SessionState::CONNECTED);
    AA_LOG_I() << "세션이 정상적으로 연결되었습니다 (CONNECTED).";

    // 수신 및 처리 루프 시작
    receive_thread_ = std::thread(&Session::ReceiveLoop, this);
    process_thread_ = std::thread(&Session::ProcessLoop, this);
    heartbeat_thread_ = std::thread(&Session::HeartbeatLoop, this);

    return true;
}

bool Session::DoVersionExchange() {
    std::vector<uint8_t> version_payload = {0, 1, 0, 1};  // AAP v1.1
    auto version_packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_VERSION_REQ, version_payload, 0x03);

    if (!transport_->Send(version_packet)) {
        AA_LOG_E() << "버전 요청 송신 실패";
        return false;
    }

    std::vector<uint8_t> buffer;
    while (true) {
        auto resp = transport_->Receive();
        if (resp.empty()) {
            AA_LOG_E() << "버전 응답 수신 실패 (Timeout/Empty)";
            return false;
        }
        buffer.insert(buffer.end(), resp.begin(), resp.end());

        while (buffer.size() >= aap::HEADER_SIZE) {
            uint16_t payload_len = (buffer[2] << 8) | buffer[3];
            size_t total_packet_len = aap::HEADER_SIZE + payload_len;
            if (buffer.size() < total_packet_len) break;

            std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + total_packet_len);
            buffer.erase(buffer.begin(), buffer.begin() + total_packet_len);

            uint16_t msg_type = (packet[4] << 8) | packet[5];
            if (msg_type == aap::TYPE_VERSION_RESP) {
                uint16_t major = (packet[6] << 8) | packet[7];
                uint16_t minor = (packet[8] << 8) | packet[9];
                AA_LOG_I() << "버전 응답 수신 완료 (Version: " << major << "." << minor << ")";
                
                // 남은 데이터가 있다면 다음 단계를 위해 보존할 필요가 있으나,
                // 버전 응답 후 바로 SSL 핸드셰이크가 오므로 DoSslHandshake에서 이어서 처리해야 함.
                // 여기서는 간단히 message_queue_에 넣음 (나중에 ProcessLoop가 처리)
                if (!buffer.empty()) {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    message_queue_.push_back(std::move(buffer));
                    buffer.clear();
                }
                return true;
            } else {
                // 버전 응답이 아닌 다른 패킷이 먼저 왔다면 큐에 보관
                std::lock_guard<std::mutex> lock(queue_mutex_);
                message_queue_.push_back(std::move(packet));
            }
        }
    }
}

bool Session::DoSslHandshake() {
    // 장치 안정화를 위한 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    crypto_->SetStrategy(std::make_shared<crypto::TlsCryptoStrategy>());

    std::vector<uint8_t> buffer;
    int handshake_count = 0;
    while (!crypto_->IsHandshakeComplete() && handshake_count++ < 20) {
        AA_LOG_I() << "SSL 핸드셰이크 시도 (" << handshake_count << "/20)...";

        auto out_data = crypto_->GetHandshakeData();
        if (!out_data.empty()) {
            auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_HANDSHAKE, out_data, 0x03);
            if (!transport_->Send(packet)) {
                AA_LOG_E() << "SSL 핸드셰이크 데이터 송신 실패";
                break;
            }
        }

        if (crypto_->IsHandshakeComplete()) break;

        auto in_resp = transport_->Receive();
        if (in_resp.empty()) continue;
        buffer.insert(buffer.end(), in_resp.begin(), in_resp.end());

        while (buffer.size() >= aap::HEADER_SIZE) {
            uint16_t payload_len = (buffer[2] << 8) | buffer[3];
            size_t total_packet_len = aap::HEADER_SIZE + payload_len;
            if (buffer.size() < total_packet_len) break;

            std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + total_packet_len);
            buffer.erase(buffer.begin(), buffer.begin() + total_packet_len);

            uint8_t channel = packet[0];
            uint16_t msg_type = (packet[4] << 8) | packet[5];

            if (channel == aap::CH_CONTROL && msg_type == aap::TYPE_SSL_HANDSHAKE) {
                std::vector<uint8_t> ssl_payload(packet.begin() + 6, packet.end());
                crypto_->PutHandshakeData(ssl_payload);
            } else {
                // 핸드셰이크가 아닌 데이터는 큐에 보존
                std::lock_guard<std::mutex> lock(queue_mutex_);
                message_queue_.push_back(std::move(packet));
            }
        }
    }

    if (!crypto_->IsHandshakeComplete()) {
        AA_LOG_E() << "SSL 핸드셰이크 실패";
        return false;
    }
    AA_LOG_I() << "SSL 핸드셰이크 완료!";

    // 남아있는 자투리 데이터가 있다면 큐에 넘김
    if (!buffer.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        message_queue_.push_back(std::move(buffer));
    }

    return true;
}

bool Session::SendSslAuthComplete() {
    aap_protobuf::service::control::message::AuthResponse auth;
    auth.set_status(0); // OK
    
    std::vector<uint8_t> out_payload(auth.ByteSizeLong());
    if (auth.SerializeToArray(out_payload.data(), out_payload.size())) {
        auto auth_packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_AUTH_COMPLETE, out_payload, 0x03);
        return transport_->Send(auth_packet);
    }
    return false;
}


void Session::Stop() {
    SessionState expected = state_.load();
    while (expected != SessionState::DISCONNECTED) {
        if (state_.compare_exchange_weak(expected, SessionState::DISCONNECTED)) {
            AA_LOG_I() << "세션이 종료되었습니다 (DISCONNECTED).";

            // 스레드 및 통신 리소스 정리
            if (transport_) {
                transport_->Disconnect();
            }

            // 큐 대기 중인 처리 스레드를 깨움
            queue_cv_.notify_all();

            // 스레드 종료 대기
            if (receive_thread_.joinable()) receive_thread_.join();
            if (process_thread_.joinable()) process_thread_.join();
            if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

            break;
        }
    }
}

// 채널이 특정 데이터를 전송하고 싶을 때 호출하는 API
bool Session::SendData(std::shared_ptr<service::IService> service, const std::vector<uint8_t>& payload) {
    if (state_.load() != SessionState::CONNECTED) {
        AA_LOG_E() << "오류: 세션이 연결된 상태가 아닙니다.";
        return false;
    }

    // AAP 헤더 구성 및 암호화
    // Android Auto는 헤더(4바이트)는 평문으로, 그 뒤의 데이터(메시지 타입 + 실제 페이로드)를 암호화합니다.
    uint8_t ch = service->GetChannel(); 

    // 서비스에서 준비한 메시지 (보통 [Type(2)] + [Payload])
    std::vector<uint8_t> message_to_encrypt = service->PrepareMessage(payload);
    
    // 데이터 암호화
    std::vector<uint8_t> encrypted_data = crypto_->EncryptData(message_to_encrypt);

    // 전체 패킷 구성
    uint16_t total_len = static_cast<uint16_t>(encrypted_data.size());
    std::vector<uint8_t> packet(aap::HEADER_SIZE + total_len);

    packet[0] = ch;
    packet[1] = 0x08 | 0x01 | 0x02; // ENCRYPTED | FIRST | LAST
    packet[2] = (total_len >> 8) & 0xFF;
    packet[3] = total_len & 0xFF;

    std::copy(encrypted_data.begin(), encrypted_data.end(), packet.begin() + aap::HEADER_SIZE);

    if (transport_->Send(packet)) {
        return true;
    }

    return false;
}

void Session::ReceiveLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        // 1. 트랜스포트에서 데이터 수신
        std::vector<uint8_t> data = transport_->Receive();
        if (data.empty()) continue;

        // 2. 메시지를 큐에 삽입 (생산자)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push_back(std::move(data));
        }
        queue_cv_.notify_one();
    }
}

void Session::ProcessLoop() {
    std::vector<uint8_t> receive_buffer;
    size_t read_offset = 0;

    while (state_.load() != SessionState::DISCONNECTED) {
        std::vector<uint8_t> incoming_data;

        // 1. 큐에서 메시지 꺼내기
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock,
                           [this]() { return !message_queue_.empty() || state_.load() == SessionState::DISCONNECTED; });

            if (state_.load() == SessionState::DISCONNECTED && message_queue_.empty()) {
                break;
            }

            incoming_data = std::move(message_queue_.front());
            message_queue_.erase(message_queue_.begin());
        }

        // 버퍼에 누적
        /*
        size_t l = incoming_data.size() > 16 ? 16 : incoming_data.size();
        AA_LOG_D() << "insert " << utils::ProtocolUtil::DumpHex(incoming_data, l) << " to " << receive_buffer.size();
        */
        AA_LOG_D() << "receve_buffer size : " << receive_buffer.size() << ", read_offset : " << read_offset;
        receive_buffer.insert(receive_buffer.end(), incoming_data.begin(), incoming_data.end());


        // 버퍼에서 완전한 패킷 추출 및 처리
        while (receive_buffer.size() - read_offset >= aap::HEADER_SIZE) {
            uint16_t payload_len = (receive_buffer[read_offset + 2] << 8) | receive_buffer[read_offset + 3];
            size_t aap_packet_len = aap::HEADER_SIZE + payload_len;

            if (receive_buffer.size() - read_offset < aap_packet_len) {
                break;
            }

            // 패킷 메타데이터 추출
            const uint8_t* packet_start = receive_buffer.data() + read_offset;
            uint8_t channel = packet_start[0];
            uint8_t flags = packet_start[1];
            bool is_first = (flags & 0x01);
            bool is_last  = (flags & 0x02);
            bool is_encrypted = (flags & 0x08);

            // flags=0x09 (first, not last): 헤더 직후 4바이트는 total_size 필드 — skip
            // flags=0x0b (first+last, 단일 패킷): total_size 없음
            bool is_multi_first = is_first && !is_last;
            size_t payload_skip = is_multi_first ? 4 : 0;
            const uint8_t* payload_start = packet_start + aap::HEADER_SIZE + payload_skip;
            size_t payload_data_len = aap_packet_len - aap::HEADER_SIZE;

            if (is_multi_first) {
                uint32_t total_size = (packet_start[aap::HEADER_SIZE + 0] << 24) |
                                      (packet_start[aap::HEADER_SIZE + 1] << 16) |
                                      (packet_start[aap::HEADER_SIZE + 2] <<  8) |
                                      (packet_start[aap::HEADER_SIZE + 3]);
                AA_LOG_D() << "[Session] First fragment total_size: " << total_size;
                read_offset += payload_skip;
            }

            std::vector<uint8_t> encrypted_payload(payload_start, payload_start + payload_data_len);
            read_offset += aap_packet_len;

            AA_LOG_D() << "[Session] << 수신 (Ch:" << (int)channel
                       << ", Flags:0x" << std::hex << (int)flags << std::dec
                       << ", First:" << is_first << ", Last:" << is_last
                       << ", Len:" << encrypted_payload.size() << ") "
                       << utils::ProtocolUtil::DumpHex(encrypted_payload, 16);

            // 2. 조각 ciphertext를 누적, last 조각 도착 시 한 번에 decrypt
            //    OpenSSL BIO는 여러 BIO_write에 걸친 partial TLS 레코드를 내부 버퍼링하므로
            //    조각들을 순서대로 넣고 last에서 SSL_read하면 됨
            auto& frag_buffer = fragment_buffers_[channel];
            if (is_first) {
                frag_buffer.clear();
            }
            frag_buffer.insert(frag_buffer.end(), encrypted_payload.begin(), encrypted_payload.end());

            if (!is_last) {
                continue; // 다음 조각 대기
            }

            // last 조각 — 누적된 ciphertext 전체를 decrypt (WANT_READ 없이 완전한 TLS 레코드)
            std::vector<uint8_t> full_message;
            if (is_encrypted) {
                full_message = crypto_->DecryptData(frag_buffer);
            } else {
                full_message = std::move(frag_buffer);
            }
            frag_buffer.clear();

            if (full_message.empty()) {
                AA_LOG_E() << "[Session] 복호화 실패 (Ch:" << (int)channel << ")";
                continue;
            }

            // 4. 메시지 처리
            if (full_message.size() >= aap::TYPE_SIZE) {
                uint16_t msg_type = (full_message[0] << 8) | full_message[1];

                AA_LOG_I() << "[ProcessLoop] 수신 [" << utils::ProtocolUtil::GetChannelName(channel)
                           << "] Type: " << utils::ProtocolUtil::GetMessageTypeName(msg_type)
                           << " (0x" << std::hex << msg_type << std::dec << "), Len: " << (full_message.size() - aap::TYPE_SIZE);

                AA_LOG_D() << "[Session] << 수신 (Decrypted) " << utils::ProtocolUtil::DumpHex(full_message, 16);

                auto service = FindService(channel);
                if (service) {
                    std::vector<uint8_t> final_payload(full_message.begin() + aap::TYPE_SIZE, full_message.end());
                    service->HandleMessage(msg_type, final_payload);
                } else {
                    AA_LOG_W() << "[Session] 수신된 패킷을 처리할 서비스를 찾을 수 없음 (채널: " << (int)channel << ")";
                }
            }
        }

        // 루프 종료 후 한 번에 수신 버퍼 정리 (성능 및 안정성)
        if (read_offset > 512 * 1024) { 
            receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + read_offset);
            read_offset = 0;
        }
    }
}

std::shared_ptr<service::IService> Session::FindService(uint8_t channel) {
    std::shared_lock<std::shared_mutex> lock(services_mutex_);
    auto it = services_.find(channel);
    if (it != services_.end()) {
        return it->second;
    }
    return nullptr;
}

void Session::HeartbeatLoop() {
    AA_LOG_I() << "Heartbeat(Ping) Loop 시작";
    while (state_.load() == SessionState::CONNECTED) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (state_.load() != SessionState::CONNECTED) break;

        // 핑 메시지 구성 (Timestamp 포함)
        aap_protobuf::service::control::message::PingRequest ping;
        ping.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count());

        std::vector<uint8_t> payload(ping.ByteSizeLong());
        if (ping.SerializeToArray(payload.data(), payload.size())) {
            auto packet = aap::Pack(aap::CH_CONTROL, 0x0B, payload); // PINGREQUEST (0x0B)
            
            // 핑은 보통 암호화하지 않거나 평문으로 보낼 수도 있으나, 
            // 여기서는 표준 send_cb와 유사하게 암호화하여 전송 (보통 제어 채널은 암호화됨)
            std::vector<uint8_t> plain_part(packet.begin() + aap::HEADER_SIZE, packet.end());
            std::vector<uint8_t> encrypted_payload = crypto_->EncryptData(plain_part);

            uint16_t total_len = static_cast<uint16_t>(encrypted_payload.size());
            packet.resize(aap::HEADER_SIZE + total_len);
            packet[1] |= 0x08; // ENCRYPTED
            packet[2] = (total_len >> 8) & 0xFF;
            packet[3] = total_len & 0xFF;
            std::copy(encrypted_payload.begin(), encrypted_payload.end(), packet.begin() + aap::HEADER_SIZE);

            if (!transport_->Send(packet)) {
                AA_LOG_E() << "Heartbeat Ping 송신 실패";
            }
        }
    }
    AA_LOG_I() << "Heartbeat(Ping) Loop 종료";
}

}  // namespace session
}  // namespace aauto
