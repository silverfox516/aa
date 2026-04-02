#pragma once

namespace aauto {
namespace hw {

// Abstract interface for detecting physical/logical device connections
class IDeviceDetector {
   public:
    virtual ~IDeviceDetector() = default;

    // Initialize the detector (library/socket setup etc.)
    virtual bool Init() = 0;

    // Start the detection loop
    virtual bool Start() = 0;

    // Stop the detection loop and release resources
    virtual void Stop() = 0;
};

}  // namespace hw
}  // namespace aauto
