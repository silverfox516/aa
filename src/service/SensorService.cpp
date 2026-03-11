#define LOG_TAG "SensorService"
#include "aauto/service/SensorService.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/sensorsource/SensorSourceService.pb.h"
#include "aap_protobuf/service/sensorsource/message/Sensor.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorType.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorResponse.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"
#include "aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h"
#include "aap_protobuf/service/sensorsource/message/NightModeData.pb.h"
#include "aap_protobuf/service/sensorsource/SensorMessageId.pb.h"
#include "aap_protobuf/shared/MessageStatus.pb.h"

namespace aauto {
namespace service {

// Sensor message type constants
static constexpr uint16_t MSG_SENSOR_START_REQ  = 0x8001; // SENSOR_START_REQUEST
static constexpr uint16_t MSG_SENSOR_START_RESP = 0x8002; // SENSOR_START_RESPONSE
static constexpr uint16_t MSG_SENSOR_EVENT      = 0x8003; // SENSOR_EVENT (SensorBatch)

void SensorService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == 0x07) { // ChannelOpenRequest
        HandleChannelOpenRequest(msg_type, payload, send_cb_, GetChannel());
        SendDrivingStatus(); // 채널 열리면 즉시 UNRESTRICTED 상태 전송
        return;
    }

    switch (msg_type) {
        case MSG_SENSOR_START_REQ:
            HandleSensorStartRequest(payload);
            break;
        default:
            AA_LOG_W() << "[SensorService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void SensorService::HandleSensorStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::sensorsource::message::SensorRequest req;
    if (req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[SensorService] SensorStartRequest - type:" << req.type()
                   << " minPeriod:" << req.min_update_period();
    }

    // SensorStartResponse: STATUS_SUCCESS
    aap_protobuf::service::sensorsource::message::SensorResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSizeLong());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_SENSOR_START_RESP, out);
        AA_LOG_I() << "[SensorService] SensorStartResponse 송신 완료";
    }

    // Driving Status 이벤트 즉시 전송 (UNRESTRICTED)
    SendDrivingStatus();
}

void SensorService::SendDrivingStatus() {
    aap_protobuf::service::sensorsource::message::SensorBatch batch;
    auto* ds = batch.add_driving_status_data();
    ds->set_status(0); // 0 = DRIVING_STATUS_UNRESTRICTED

    std::vector<uint8_t> out(batch.ByteSizeLong());
    if (batch.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_SENSOR_EVENT, out);
        AA_LOG_I() << "[SensorService] DrivingStatus(UNRESTRICTED) 이벤트 송신 완료";
    }
}

void SensorService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sensor = service_proto->mutable_sensor_source_service();
    
    auto* s1 = sensor->add_sensors();
    s1->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA);
    
    auto* s2 = sensor->add_sensors();
    s2->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_NIGHT_MODE);

    auto* s3 = sensor->add_sensors();
    s3->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_LOCATION);
}

void SensorService::OnChannelOpened(uint8_t channel) {
    // DrivingStatus는 HandleMessage에서 ChannelOpen 직후 전송
}

}  // namespace service
}  // namespace aauto
