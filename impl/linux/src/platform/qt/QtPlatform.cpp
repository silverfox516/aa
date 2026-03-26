#define LOG_TAG "QtPlatform"
#include "aauto/platform/qt/QtPlatform.hpp"
#include "aauto/platform/qt/GstVideoOutput.hpp"
#include "aauto/platform/qt/GstAudioOutput.hpp"

#include <QApplication>

namespace aauto {
namespace platform {
namespace qt {

bool QtPlatform::Initialize() {
    auto video = std::make_shared<GstVideoOutput>();
    if (!video->Initialize()) return false;
    video_output_ = video;
    return true;
}

std::shared_ptr<IVideoOutput> QtPlatform::GetVideoOutput() { return video_output_; }
std::shared_ptr<IAudioOutput> QtPlatform::GetAudioOutput() { return CreateAudioOutput(); }
std::shared_ptr<IAudioOutput> QtPlatform::CreateAudioOutput() {
    return std::make_shared<GstAudioOutput>();
}

void QtPlatform::Run() {
    if (qApp) qApp->exec();
}

void QtPlatform::Stop() {
    if (qApp) qApp->quit();
}

} // namespace qt
} // namespace platform
} // namespace aauto
