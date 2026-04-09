#define LOG_TAG "AA.CORE.SensorService"
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
#include "aap_protobuf/service/sensorsource/message/LocationData.pb.h"
#include "aap_protobuf/shared/MessageStatus.pb.h"

#include <chrono>
#include <climits>

namespace aauto {
namespace service {

SensorService::SensorService(SensorServiceConfig config)
    : config_(std::move(config)) {
    RegisterHandler(session::aap::msg::SENSOR_START_REQUEST,
                    [this](const auto& p){ HandleSensorStartRequest(p); });
}

void SensorService::OnChannelOpened(uint8_t) {
    if (config_.driving_status) SendDrivingStatus();
}

void SensorService::HandleSensorStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::sensorsource::message::SensorRequest req;
    aap_protobuf::service::sensorsource::message::SensorType type =
        aap_protobuf::service::sensorsource::message::SENSOR_LOCATION;
    if (req.ParseFromArray(payload.data(), payload.size())) {
        type = req.type();
        AA_LOG_I() << "[SensorService] SensorStartRequest - type:" << type
                   << " minPeriod:" << req.min_update_period();
    }

    aap_protobuf::service::sensorsource::message::SensorResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSize());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::SENSOR_START_RESPONSE, out);
        AA_LOG_I() << "[SensorService] SensorStartResponse sent (type=" << type << ")";
    }

    // Send a first sample of the requested sensor type immediately so the
    // phone sees that stream as active. Subsequent samples come from the
    // platform-driven push paths (LocationManager listener for LOCATION,
    // OnChannelOpened for DRIVING_STATUS_DATA).
    switch (type) {
        case aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA:
            if (config_.driving_status) SendDrivingStatus();
            break;
        case aap_protobuf::service::sensorsource::message::SENSOR_LOCATION:
            // Location stream is driven by SendLocationFix(...) from the
            // app layer (LocationListener / LocationSimulator). The first
            // tick will arrive within ~1 s of the simulator starting; no
            // immediate echo here.
            break;
        default:
            // Other sensor types are advertised by FillServiceDefinition
            // only if the app explicitly enabled them. Their data path
            // (if any) is added per type as platforms wire real sources.
            break;
    }
}

void SensorService::SendLocationFix(int32_t lat_e7, int32_t lon_e7,
                                      int32_t alt_e2, uint32_t accuracy_e3,
                                      int32_t speed_e3, int32_t bearing_e6,
                                      uint64_t timestamp_us) {
    if (!config_.location) return;

    if (timestamp_us == 0) {
        timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    aap_protobuf::service::sensorsource::message::SensorBatch batch;
    auto* loc = batch.add_location_data();
    // The timestamp field is marked deprecated in the .proto, but the
    // Android Auto location consumer still relies on it: a fix that comes
    // through with timestamp == 0 (the field's default when not set) is
    // treated as stale and used only as an initial seed before being
    // dropped on subsequent fixes. Always populate it with monotonic
    // microseconds since epoch.
    loc->set_timestamp(timestamp_us);
    loc->set_latitude_e7(lat_e7);
    loc->set_longitude_e7(lon_e7);
    if (accuracy_e3 > 0)        loc->set_accuracy_e3(accuracy_e3);
    if (alt_e2     != INT_MIN)  loc->set_altitude_e2(alt_e2);
    if (speed_e3   != INT_MIN)  loc->set_speed_e3(speed_e3);
    if (bearing_e6 != INT_MIN)  loc->set_bearing_e6(bearing_e6);

    // Co-send driving_status_data so the phone correlates the moving
    // location with a driving (non-park) state. UNRESTRICTED (0) means
    // stationary "park"; sending stationary alongside a 100 km/h location
    // is contradictory and causes the navigation viewport to discard the
    // location stream as a spike. Use NO_KEYBOARD_INPUT (2) which is the
    // mildest "moving" bit and does not gate video / voice paths.
    if (config_.driving_status) {
        auto* ds = batch.add_driving_status_data();
        ds->set_status(2);  // DRIVE_STATUS_NO_KEYBOARD_INPUT
    }

    std::vector<uint8_t> out(batch.ByteSize());
    if (batch.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::SENSOR_EVENT, out);
    }
}

void SensorService::SendDrivingStatus() {
    aap_protobuf::service::sensorsource::message::SensorBatch batch;
    auto* ds = batch.add_driving_status_data();
    ds->set_status(0); // DRIVING_STATUS_UNRESTRICTED

    std::vector<uint8_t> out(batch.ByteSize());
    if (batch.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::SENSOR_EVENT, out);
        AA_LOG_I() << "[SensorService] DrivingStatus(UNRESTRICTED) sent";
    }
}

void SensorService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sensor = service_proto->mutable_sensor_source_service();

    if (config_.driving_status) {
        auto* s = sensor->add_sensors();
        s->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA);
    }
    if (config_.night_mode) {
        auto* s = sensor->add_sensors();
        s->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_NIGHT_MODE);
    }
    if (config_.location) {
        auto* s = sensor->add_sensors();
        s->set_sensor_type(aap_protobuf::service::sensorsource::message::SENSOR_LOCATION);
    }
}

} // namespace service
} // namespace aauto
