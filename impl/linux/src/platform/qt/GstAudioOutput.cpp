#define LOG_TAG "GstAudioOutput"
#include "aauto/platform/qt/GstAudioOutput.hpp"
#include "aauto/utils/Logger.hpp"

#include <gst/app/gstappsrc.h>

namespace aauto {
namespace platform {
namespace qt {

GstAudioOutput::~GstAudioOutput() {
    Close();
}

static GstElement* TryPipeline(const std::string& desc) {
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.c_str(), &err);
    if (err || !pipeline) {
        if (err) g_error_free(err);
        if (pipeline) gst_object_unref(pipeline);
        return nullptr;
    }
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        GstBus* bus = gst_element_get_bus(pipeline);
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, 200 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR));
        if (msg) {
            GError* gerr = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            g_printerr("[GstAudioOutput] pipeline error: %s\n", gerr ? gerr->message : "?");
            if (gerr) g_error_free(gerr);
            g_free(dbg);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return nullptr;
    }
    return pipeline;
}

bool GstAudioOutput::Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    Close();

    sample_rate_ = sample_rate;
    channels_    = channels;
    bits_        = bits;

    const std::string candidates[] = {
        "appsrc name=src format=bytes "
        "! queue max-size-buffers=10 min-threshold-buffers=3 "
        "! audioconvert ! audioresample "
        "! autoaudiosink sync=false"

        "appsrc name=src format=bytes is-live=true "
        "! audioconvert ! audioresample "
        "! pulsesink sync=false",

        "appsrc name=src format=bytes is-live=true "
        "! audioconvert ! audioresample "
        "! alsasink device=default sync=false",

        "appsrc name=src format=bytes is-live=true "
        "! audioconvert ! audioresample "
        "! alsasink device=plughw:0,0 sync=false",
    };
    const char* names[] = { "autoaudiosink", "pulsesink", "alsasink(default)", "alsasink(plughw:0,0)" };

    for (int i = 0; i < 4; ++i) {
        pipeline_ = TryPipeline(candidates[i]);
        if (pipeline_) { AA_LOG_I() << "[GstAudioOutput] sink: " << names[i]; break; }
        AA_LOG_W() << "[GstAudioOutput] " << names[i] << " failed, trying next";
    }
    if (!pipeline_) { AA_LOG_E() << "[GstAudioOutput] all sinks failed"; return false; }

    appsrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
    if (!appsrc_) {
        AA_LOG_E() << "[GstAudioOutput] appsrc not found";
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        return false;
    }

    const char* fmt = (bits == 16) ? "S16LE" : (bits == 8 ? "S8" : "S16LE");
    GstCaps* caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, fmt,
        "rate",     G_TYPE_INT,    (int)sample_rate,
        "channels", G_TYPE_INT,    (int)channels,
        "layout",   G_TYPE_STRING, "interleaved",
        nullptr);
    gst_app_src_set_caps(appsrc_, caps);
    gst_caps_unref(caps);

    AA_LOG_I() << "[GstAudioOutput] ready: "
               << sample_rate << "Hz / " << (int)channels << "ch / " << (int)bits << "bit";
    return true;
}

void GstAudioOutput::Start() {
    if (!pipeline_ || running_.load()) return;
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    running_.store(true);
    AA_LOG_I() << "[GstAudioOutput] playing";
}

void GstAudioOutput::Close() {
    if (!running_.exchange(false)) return;
    if (appsrc_)   gst_app_src_end_of_stream(appsrc_);
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (appsrc_)   { gst_object_unref(appsrc_);   appsrc_   = nullptr; }
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
    AA_LOG_I() << "[GstAudioOutput] closed";
}

void GstAudioOutput::PushAudioData(const std::vector<uint8_t>& data) {
    if (!running_.load() || !appsrc_ || data.empty()) return;

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
    gst_buffer_fill(buf, 0, data.data(), data.size());

    GstFlowReturn ret = gst_app_src_push_buffer(appsrc_, buf);
    if (ret != GST_FLOW_OK) {
        AA_LOG_W() << "[GstAudioOutput] push_buffer failed: " << ret;
    }
}

} // namespace qt
} // namespace platform
} // namespace aauto
