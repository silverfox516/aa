#define LOG_TAG "GstVideoOutput"
#include "aauto/platform/qt/GstVideoOutput.hpp"
#include "aauto/platform/qt/GlVideoWidget.hpp"
#include "aauto/utils/Logger.hpp"

#include <gst/video/video.h>
#include <QMetaObject>
#include <cstring>

namespace aauto {
namespace platform {
namespace qt {

GstVideoOutput::GstVideoOutput(int width, int height)
    : width_(width), height_(height) {}

GstVideoOutput::~GstVideoOutput() {
    Stop();
}

bool GstVideoOutput::Initialize() {
    gst_init(nullptr, nullptr);

    widget_ = new GlVideoWidget(width_, height_);
    if (touch_cb_) widget_->SetTouchCallback(touch_cb_);

    if (!BuildPipeline()) return false;

    AA_LOG_I() << "[GstVideoOutput] 초기화 완료 - " << width_ << "x" << height_;
    return true;
}

bool GstVideoOutput::BuildPipeline() {
    // VAAPI(HW) 우선 시도, 실패 시 avdec_h264(SW) 사용
    static const char* kPipelineHw =
        "appsrc name=src format=time is-live=true "
            "caps=video/x-h264,stream-format=byte-stream,alignment=nal ! "
        "h264parse ! "
        "vaapidecodebin ! "
        "videoconvert ! "
        "video/x-raw,format=NV12 ! "
        "appsink name=sink sync=false max-buffers=2 drop=true";

    static const char* kPipelineSw =
        "appsrc name=src format=time is-live=true "
            "caps=video/x-h264,stream-format=byte-stream,alignment=nal ! "
        "h264parse ! "
        "avdec_h264 ! "
        "videoconvert ! "
        "video/x-raw,format=NV12 ! "
        "appsink name=sink sync=false max-buffers=2 drop=true";

    GError* err = nullptr;

    // HW decode 파이프라인 시도
    pipeline_ = gst_parse_launch(kPipelineHw, &err);
    if (err) {
        AA_LOG_W() << "[GstVideoOutput] HW 파이프라인 파싱 실패, SW로 전환: " << err->message;
        g_error_free(err);
        err = nullptr;
        if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
        pipeline_ = gst_parse_launch(kPipelineSw, &err);
    }

    if (err || !pipeline_) {
        AA_LOG_E() << "[GstVideoOutput] 파이프라인 생성 실패"
                   << (err ? std::string(": ") + err->message : "");
        if (err) g_error_free(err);
        return false;
    }

    appsrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
    appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "sink"));
    if (!appsrc_ || !appsink_) {
        AA_LOG_E() << "[GstVideoOutput] appsrc/appsink 참조 실패";
        return false;
    }

    // appsink 콜백 등록
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = &GstVideoOutput::OnNewSample;
    gst_app_sink_set_callbacks(appsink_, &cbs, this, nullptr);

    // HW 파이프라인이 실제로 동작하는지 READY 상태로 테스트
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        AA_LOG_W() << "[GstVideoOutput] HW 파이프라인 READY 실패, SW로 재시도";
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(appsrc_);
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsrc_ = nullptr; appsink_ = nullptr; pipeline_ = nullptr;

        pipeline_ = gst_parse_launch(kPipelineSw, &err);
        if (err || !pipeline_) {
            AA_LOG_E() << "[GstVideoOutput] SW 파이프라인도 실패";
            if (err) g_error_free(err);
            return false;
        }
        appsrc_  = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
        appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "sink"));
        gst_app_sink_set_callbacks(appsink_, &cbs, this, nullptr);
        gst_element_set_state(pipeline_, GST_STATE_READY);
        AA_LOG_I() << "[GstVideoOutput] SW(avdec_h264) 파이프라인 사용";
    } else {
        AA_LOG_I() << "[GstVideoOutput] HW(vaapidecodebin) 파이프라인 사용";
    }

    return true;
}

void GstVideoOutput::Open(int /*width*/, int /*height*/) {
    // 이전 세션 정리 (재연결 시 bus_thread_ 가 joinable 상태로 남아 terminate 방지)
    Stop();

    if (!BuildPipeline()) {
        AA_LOG_E() << "[GstVideoOutput] 파이프라인 재생성 실패";
        return;
    }

    running_.store(true);
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    bus_thread_ = std::thread(&GstVideoOutput::BusWatchLoop, this);

    QMetaObject::invokeMethod(widget_, "show", Qt::QueuedConnection);
    AA_LOG_I() << "[GstVideoOutput] 파이프라인 시작";
}

void GstVideoOutput::Close() {
    Stop();
    QMetaObject::invokeMethod(widget_, "hide", Qt::QueuedConnection);
}

void GstVideoOutput::Stop() {
    if (!running_.exchange(false)) return;

    if (appsrc_) gst_app_src_end_of_stream(appsrc_);
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_thread_.joinable()) bus_thread_.join();

    if (appsrc_)  { gst_object_unref(appsrc_);   appsrc_  = nullptr; }
    if (appsink_) { gst_object_unref(appsink_);  appsink_ = nullptr; }
    if (pipeline_){ gst_object_unref(pipeline_); pipeline_ = nullptr; }

    AA_LOG_I() << "[GstVideoOutput] 파이프라인 중지";
}

void GstVideoOutput::PushVideoData(const std::vector<uint8_t>& data) {
    if (!appsrc_ || !running_.load() || data.empty()) return;

    static std::atomic<int> push_count{0};
    int cnt = ++push_count;
    if (cnt <= 5 || cnt % 30 == 0) {
        AA_LOG_I() << "[GstVideoOutput] push_buffer #" << cnt
                   << " size=" << data.size()
                   << " b0=0x" << std::hex << (int)data[0] << std::dec;
    }

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
    gst_buffer_fill(buf, 0, data.data(), data.size());
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn ret = gst_app_src_push_buffer(appsrc_, buf); // buf ownership transferred
    if (ret != GST_FLOW_OK) {
        AA_LOG_W() << "[GstVideoOutput] push_buffer 실패: " << ret;
    }
}

void GstVideoOutput::SetTouchCallback(TouchCallback cb) {
    touch_cb_ = std::move(cb);
    if (widget_) widget_->SetTouchCallback(touch_cb_);
}

// static
GstFlowReturn GstVideoOutput::OnNewSample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<GstVideoOutput*>(user_data);

    static std::atomic<int> frame_count{0};
    int fc = ++frame_count;
    if (fc <= 5 || fc % 30 == 0) {
        AA_LOG_I() << "[GstVideoOutput] OnNewSample #" << fc;
    }

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstCaps*   caps   = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    const int w         = GST_VIDEO_INFO_WIDTH(&info);
    const int h         = GST_VIDEO_INFO_HEIGHT(&info);
    const int y_stride  = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
    const int uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 1);
    const int y_offset  = static_cast<int>(GST_VIDEO_INFO_PLANE_OFFSET(&info, 0));
    const int uv_offset = static_cast<int>(GST_VIDEO_INFO_PLANE_OFFSET(&info, 1));

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        self->widget_->SetNV12Frame(
            map.data + y_offset,  y_stride,
            map.data + uv_offset, uv_stride,
            w, h);
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void GstVideoOutput::BusWatchLoop() {
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    while (running_.load()) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
        if (!msg) continue;

        GstMessageType mtype = GST_MESSAGE_TYPE(msg);
        if (mtype == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            AA_LOG_E() << "[GstVideoOutput] 파이프라인 오류: " << err->message;
            if (dbg) AA_LOG_E() << "[GstVideoOutput] 디버그: " << dbg;
            g_error_free(err);
            g_free(dbg);
        } else if (mtype == GST_MESSAGE_WARNING) {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            AA_LOG_W() << "[GstVideoOutput] 파이프라인 경고: " << err->message;
            if (dbg) AA_LOG_W() << "[GstVideoOutput] 경고 디버그: " << dbg;
            g_error_free(err);
            g_free(dbg);
        } else if (mtype == GST_MESSAGE_EOS) {
            AA_LOG_I() << "[GstVideoOutput] 파이프라인 EOS";
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

} // namespace qt
} // namespace platform
} // namespace aauto
