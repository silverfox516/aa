#define LOG_TAG "QtPlatform"
#include "aauto/platform/qt/QtPlatform.hpp"
#include "aauto/platform/qt/GstVideoOutput.hpp"
#include "aauto/platform/alsa/AlsaAudioOutput.hpp"

#include <QApplication>

namespace aauto {
namespace platform {
namespace qt {

bool QtPlatform::Initialize() {
    auto video = std::make_shared<GstVideoOutput>();
    if (!video->Initialize()) return false;
    video_output_ = video;
    audio_output_ = std::make_shared<alsa::AlsaAudioOutput>();
    return true;
}

std::shared_ptr<IVideoOutput> QtPlatform::GetVideoOutput() { return video_output_; }
std::shared_ptr<IAudioOutput> QtPlatform::GetAudioOutput() { return audio_output_; }

void QtPlatform::Run() {
    if (qApp) qApp->exec();
}

void QtPlatform::Stop() {
    if (qApp) qApp->quit();
}

} // namespace qt
} // namespace platform
} // namespace aauto
