#pragma once

namespace aauto {
namespace hw {

// 물리적/논리적 디바이스 연결을 감지하는 추상화 인터페이스
class IDeviceDetector {
   public:
    virtual ~IDeviceDetector() = default;

    // 감지기 초기화 (필요한 라이브러리/소켓 초기화 등)
    virtual bool Init() = 0;

    // 감지 루프 시작
    virtual bool Start() = 0;

    // 감지 루프 종료 및 리소스 해제
    virtual void Stop() = 0;
};

}  // namespace hw
}  // namespace aauto
