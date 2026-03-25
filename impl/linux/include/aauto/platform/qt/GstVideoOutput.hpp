#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace platform {
namespace qt {

class GlVideoWidget;

// GStreamer-based IVideoOutput.
// Pipeline: appsrc → h264parse → [vaapidecodebin | avdec_h264]
//           → videoconvert → NV12 → appsink
// Decoded NV12 frames are pushed to GlVideoWidget for OpenGL rendering.
class GstVideoOutput : public IVideoOutput {
   public:
    explicit GstVideoOutput(int width = 1280, int height = 720);
    ~GstVideoOutput();

    // Build GStreamer pipeline and create GlVideoWidget.
    // Must be called from the GUI thread (after QApplication exists).
    bool Initialize();

    // IVideoOutput
    void Open(int width, int height) override;
    void Close() override;
    void PushVideoData(const std::vector<uint8_t>& data) override;
    void SetTouchCallback(TouchCallback cb) override;

    void Stop();

    GlVideoWidget* GetWidget() const { return widget_; }

   private:
    bool BuildPipeline();

    // appsink callback — runs on GStreamer internal thread
    static GstFlowReturn OnNewSample(GstAppSink* sink, gpointer user_data);

    // Bus watch thread for error/EOS messages
    void BusWatchLoop();

    int width_;
    int height_;

    GstElement*    pipeline_ = nullptr;
    GstAppSrc*     appsrc_   = nullptr;
    GstAppSink*    appsink_  = nullptr;
    GlVideoWidget* widget_   = nullptr;

    TouchCallback        touch_cb_;
    std::atomic<bool>    running_{false};
    std::thread          bus_thread_;
};

} // namespace qt
} // namespace platform
} // namespace aauto
