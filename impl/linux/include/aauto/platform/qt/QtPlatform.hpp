#pragma once

#include <memory>

#include "aauto/platform/IPlatform.hpp"

namespace aauto {
namespace platform {
namespace qt {

class GstVideoOutput;

class QtPlatform : public IPlatform {
   public:
    bool Initialize() override;
    std::shared_ptr<IVideoOutput> GetVideoOutput() override;
    std::shared_ptr<IAudioOutput> GetAudioOutput() override;
    std::shared_ptr<IAudioOutput> CreateAudioOutput() override;
    void Run() override;
    void Stop() override;

   private:
    std::shared_ptr<GstVideoOutput> video_output_;
};

} // namespace qt
} // namespace platform
} // namespace aauto
