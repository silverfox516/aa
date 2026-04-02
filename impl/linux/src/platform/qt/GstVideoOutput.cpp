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

    AA_LOG_I() << "[GstVideoOutput] Initialized - " << width_ << "x" << height_;
    return true;
}

bool GstVideoOutput::BuildPipeline() {
    // Try VAAPI (HW) first; fall back to avdec_h264 (SW) on failure
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

    // Try HW decode pipeline
    pipeline_ = gst_parse_launch(kPipelineHw, &err);
    if (err) {
        AA_LOG_W() << "[GstVideoOutput] HW pipeline parse failed, falling back to SW: " << err->message;
        g_error_free(err);
        err = nullptr;
        if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
        pipeline_ = gst_parse_launch(kPipelineSw, &err);
    }

    if (err || !pipeline_) {
        AA_LOG_E() << "[GstVideoOutput] Pipeline creation failed"
                   << (err ? std::string(": ") + err->message : "");
        if (err) g_error_free(err);
        return false;
    }

    appsrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
    appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "sink"));
    if (!appsrc_ || !appsink_) {
        AA_LOG_E() << "[GstVideoOutput] Failed to get appsrc/appsink references";
        return false;
    }

    // Register appsink callback
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = &GstVideoOutput::OnNewSample;
    gst_app_sink_set_callbacks(appsink_, &cbs, this, nullptr);

    // Test whether the HW pipeline actually works by transitioning to READY
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        AA_LOG_W() << "[GstVideoOutput] HW pipeline READY failed, retrying with SW";
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(appsrc_);
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsrc_ = nullptr; appsink_ = nullptr; pipeline_ = nullptr;

        pipeline_ = gst_parse_launch(kPipelineSw, &err);
        if (err || !pipeline_) {
            AA_LOG_E() << "[GstVideoOutput] SW pipeline also failed";
            if (err) g_error_free(err);
            return false;
        }
        appsrc_  = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
        appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "sink"));
        gst_app_sink_set_callbacks(appsink_, &cbs, this, nullptr);
        gst_element_set_state(pipeline_, GST_STATE_READY);
        AA_LOG_I() << "[GstVideoOutput] Using SW pipeline (avdec_h264)";
    } else {
        AA_LOG_I() << "[GstVideoOutput] Using HW pipeline (vaapidecodebin)";
    }

    return true;
}

void GstVideoOutput::Open(int /*width*/, int /*height*/) {
    // Clean up any previous session before rebuilding (prevents bus_thread_ leak on reconnect)
    Stop();

    if (!BuildPipeline()) {
        AA_LOG_E() << "[GstVideoOutput] Pipeline rebuild failed";
        return;
    }

    running_.store(true);
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    bus_thread_ = std::thread(&GstVideoOutput::BusWatchLoop, this);

    QMetaObject::invokeMethod(widget_, "show", Qt::QueuedConnection);
    AA_LOG_I() << "[GstVideoOutput] Pipeline started";
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

    AA_LOG_I() << "[GstVideoOutput] Pipeline stopped";
}

void GstVideoOutput::PushVideoData(const std::vector<uint8_t>& data) {
    if (!appsrc_ || !running_.load() || data.empty()) return;

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
    gst_buffer_fill(buf, 0, data.data(), data.size());
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn ret = gst_app_src_push_buffer(appsrc_, buf); // buf ownership transferred
    if (ret != GST_FLOW_OK) {
        AA_LOG_W() << "[GstVideoOutput] push_buffer failed: " << ret;
    }
}

void GstVideoOutput::SetTouchCallback(TouchCallback cb) {
    touch_cb_ = std::move(cb);
    if (widget_) widget_->SetTouchCallback(touch_cb_);
}

// static
GstFlowReturn GstVideoOutput::OnNewSample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<GstVideoOutput*>(user_data);

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
            AA_LOG_E() << "[GstVideoOutput] Pipeline error: " << err->message;
            if (dbg) AA_LOG_E() << "[GstVideoOutput] Debug: " << dbg;
            g_error_free(err);
            g_free(dbg);
        } else if (mtype == GST_MESSAGE_WARNING) {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            AA_LOG_W() << "[GstVideoOutput] Pipeline warning: " << err->message;
            if (dbg) AA_LOG_W() << "[GstVideoOutput] Warning debug: " << dbg;
            g_error_free(err);
            g_free(dbg);
        } else if (mtype == GST_MESSAGE_EOS) {
            AA_LOG_I() << "[GstVideoOutput] Pipeline EOS";
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

} // namespace qt
} // namespace platform
} // namespace aauto
