#define LOG_TAG "AA.SensorService"
#include "aauto/service/SensorService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/sensorsource/SensorSourceService.pb.h"
#include "aap_protobuf/service/sensorsource/message/Sensor.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorType.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorResponse.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"
#include "aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h"
#include "aap_protobuf/shared/MessageStatus.pb.h"

namespace aauto {
namespace service {

SensorService::SensorService() {
    RegisterHandler(session::aap::msg::SENSOR_START_REQUEST,
                    [this](const auto& p){ HandleSensorStartRequest(p); });
}

void SensorService::OnChannelOpened(uint8_t) {
    SendDrivingStatus();
}

void SensorService::HandleSensorStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::sensorsource::message::SensorRequest req;
    if (req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[SensorService] SensorStartRequest - type:" << req.type()
                   << " minPeriod:" << req.min_update_period();
    }

    aap_protobuf::service::sensorsource::message::SensorResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSize());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::SENSOR_START_RESPONSE, out);
        AA_LOG_I() << "[SensorService] SensorStartResponse 송신 완료";
    }

    SendDrivingStatus();
}

void SensorService::SendDrivingStatus() {
    aap_protobuf::service::sensorsource::message::SensorBatch batch;
    auto* ds = batch.add_driving_status_data();
    ds->set_status(0); // DRIVING_STATUS_UNRESTRICTED

    std::vector<uint8_t> out(batch.ByteSize());
    if (batch.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::SENSOR_EVENT, out);
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

} // namespace service
} // namespace aauto
