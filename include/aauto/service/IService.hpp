#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/Service.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"

#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"

namespace aauto {
namespace service {

// 서비스 타입 식별 (물리 채널 ID와 일치)
enum class ServiceType { CONTROL = 0, AUDIO = 1, VIDEO = 2, INPUT = 3, SENSOR = 4, MIC = 5, BLUETOOTH = 255 };

// 개별 서비스의 공통 인터페이스
class IService {
   public:
    virtual ~IService() = default;

    // 메시지 수신 시 처리
    virtual void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) = 0;

    // 메시지 송신 콜백 설정 (Session이 주입)
    using SendCallback = std::function<bool(uint8_t channel, uint16_t msg_type, const std::vector<uint8_t>&)>;
    virtual void SetSendCallback(SendCallback cb) = 0;

    // 서비스 스펙 (Protobuf) 정의 기입
    virtual void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) = 0;

    // 메시지 송신 전 포맷팅 (페이로드 받아 패킷 구성)
    virtual std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) = 0;

    virtual ServiceType GetType() const = 0;
    virtual std::string GetName() const = 0;

    virtual uint8_t GetChannel() const { return channel_; }
    virtual void SetChannel(uint8_t channel) { channel_ = channel; }

    // 채널이 성공적으로 오픈되었을 때 호출 (셋업 메시지 전송 등)
    virtual void OnChannelOpened(uint8_t channel) {}

   protected:
    uint8_t channel_ = 0; // Default to 0, will be set by SetChannel

    // 공통 ChannelOpenRequest 처리 헬퍼
    void HandleChannelOpenRequest(uint16_t msg_type, const std::vector<uint8_t>& payload, SendCallback send_cb, uint8_t channel) {
        if (msg_type == 0x07) { // CHANNEL_OPEN_REQUEST
            aap_protobuf::service::control::message::ChannelOpenRequest req;
            if (req.ParseFromArray(payload.data(), payload.size())) {
                // 1. 채널 여는 동작을 먼저 수행 (셋업 메시지 전송 등 내역 준비)
                OnChannelOpened(channel);

                // 2. 그 결과를 응답으로 보냄
                aap_protobuf::service::control::message::ChannelOpenResponse resp;
                resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

                std::vector<uint8_t> out_payload(resp.ByteSizeLong());
                if (resp.SerializeToArray(out_payload.data(), out_payload.size())) {
                    if (send_cb) send_cb(channel, 0x08, out_payload); // CHANNELOPENRESPONSE (0x08)
                    AA_LOG_I() << "  [" << GetName() << "] ChannelOpenResponse 송신 완료 (Ch:" << utils::ProtocolUtil::GetChannelName(channel) << ")";
                }
            }
        }
    }
};

// 팩토리 (구현체 로드를 위해 개별 헤더 인클루드 필요)
class ServiceFactory {
   public:
    static std::shared_ptr<IService> CreateService(ServiceType type);
    
    // 오디오 서비스 세분화 생성자
    static std::shared_ptr<IService> CreateAudioMediaService();
    static std::shared_ptr<IService> CreateAudioSystemService();
    static std::shared_ptr<IService> CreateAudioGuidanceService();

    // VideoRenderer 주입 (main.cpp에서 호출)
    static void SetVideoRenderer(std::shared_ptr<void> renderer);
    static std::shared_ptr<void> s_video_renderer;
};

}  // namespace service
}  // namespace aauto
