# 0002 — App-driven service composition

## Context

현재 `ServiceFactory::CreateAll()` 이 8개 service(Audio×3 + Video + Input + Sensor + Microphone + Bluetooth)를 hardcode로 생성한다. 각 service의 옵션(audio sample rate, video resolution, input touch size/keycodes, sensor type 등) 도 service 클래스 또는 ServiceFactory 안에 hardcode되어 있다. 결과적으로:

- **앱이 platform capability를 표현할 수 없음**. JNI `nativeInit(btAddress)` 는 BT 주소만 받는다. Java 측이 "이 차에는 mic 없음 / GPS 있음 / display는 1920x720" 같은 정보를 native로 전달할 통로가 없다.
- **광고/송신 불일치 가능** — 예: `SensorService` 가 LOCATION/NIGHT_MODE 광고만 하고 송신은 driving_status만 (LOC1 이슈의 직접 원인). 한 곳에서 광고하고 다른 곳에서 송신을 결정하니 동기화 안 됨.
- **`ServiceFactory::CreateAll()` 의 hardcode** 가 서로 다른 platform/capability 지원을 막는다.

목표는 사용자 통찰을 정공법으로 실현:

> Service availability + options는 platform/디바이스에 의존한다. **앱이 결정**하고, **Service Discovery 응답은 등록된 set에서 자동으로 빌드**되며, **각 service가 자기 광고와 자기 송신/처리를 한 곳에서 책임**진다.

이미 `IService::FillServiceDefinition` 과 `SessionBuilder::AddService` 가 그 모델의 토대다 — 단지 ServiceFactory의 hardcode와 capability 표현 부재만 잘라내면 된다.

## 합의된 결정 (사용자 답변 기준)

1. **Scope**: Core 리팩터링 + JNI/Java capability 전달 (Step S1 + S2 + S3 = sub-step 3개).
2. **SensorService 분해 X** — sensor channel은 1개라 sub-source 분해하면 한 채널에 여러 service가 attach되어 복잡해짐. 통합 유지하고 `SensorServiceConfig` 로 광고/송신 분기.
3. **Service config struct는 각 service header 안에** 정의 (locality 우선).
4. **`HeadunitConfig` 는 identity 그대로 유지**, 새 `ServiceComposition` 신규 도입. 두 개를 `ServiceContext` 에 모두 담음.

## 비목표

- LOC1 자체 (Android `LocationManager` 통합 + sensor batch 송신) — 이번 변경은 그것을 위한 hook (config flag) 만 만들고, 실제 송신 구현은 별도 step.
- `SensorService` 의 sub-source 분해.
- service 채널 ID 자동 할당 정책 변경.
- 이미 진행 중인 PARKED 트랙(백그라운드 오디오, transport, BT dial 등).

## Design

### 1. 각 service header에 config struct 추가

각 service header 안 `aauto::service` namespace 안에 자기 config struct를 정의. service 생성자가 그것을 받음.

#### `core/include/aauto/service/AudioService.hpp`
```cpp
struct AudioServiceConfig {
    aap_protobuf::service::media::sink::message::AudioStreamType stream_type;
    uint32_t    sample_rate;
    uint8_t     channels;
    uint8_t     bits_per_sample = 16;
    std::string name;
};

class AudioService : public ServiceBase {
public:
    explicit AudioService(AudioServiceConfig config);
    // ... (기존)
};
```

#### `core/include/aauto/service/VideoService.hpp`
```cpp
struct VideoServiceConfig {
    aap_protobuf::service::media::sink::message::VideoCodecResolutionType resolution;
    aap_protobuf::service::media::sink::message::VideoFrameRateType       frame_rate;
    uint32_t width_margin   = 0;
    uint32_t height_margin  = 0;
    uint32_t density;            // dpi
};

class VideoService : public ServiceBase {
public:
    explicit VideoService(VideoServiceConfig config);
    // ...
};
```

#### `core/include/aauto/service/InputService.hpp`
```cpp
struct InputServiceConfig {
    uint32_t touch_width;
    uint32_t touch_height;
    aap_protobuf::service::inputsource::message::TouchScreenType touch_type
        = aap_protobuf::service::inputsource::message::CAPACITIVE;
    bool                  is_secondary = false;
    std::vector<int32_t>  supported_keycodes;
};

class InputService : public ServiceBase {
public:
    explicit InputService(InputServiceConfig config);
    // ...
};
```

#### `core/include/aauto/service/SensorService.hpp`
```cpp
struct SensorServiceConfig {
    bool driving_status = true;
    bool night_mode     = false;
    bool location       = false;
    // 이후 추가될 sensor type은 여기에 flag로
};

class SensorService : public ServiceBase {
public:
    explicit SensorService(SensorServiceConfig config);
    // ...
private:
    SensorServiceConfig config_;
    // FillServiceDefinition / Send* 가 config_ 를 보고 분기
};
```

#### `core/include/aauto/service/MicrophoneService.hpp`
```cpp
struct MicrophoneServiceConfig {
    uint32_t sample_rate     = 16000;
    uint8_t  channels        = 1;
    uint8_t  bits_per_sample = 16;
};

class MicrophoneService : public ServiceBase {
public:
    explicit MicrophoneService(MicrophoneServiceConfig config);
    // ...
};
```

#### `core/include/aauto/service/BluetoothService.hpp`
```cpp
struct BluetoothServiceConfig {
    std::string car_address;
};

class BluetoothService : public ServiceBase {
public:
    explicit BluetoothService(BluetoothServiceConfig config);
    // ...
};
```

### 2. `ServiceComposition` 신규 (`core/include/aauto/service/ServiceComposition.hpp`)

```cpp
#pragma once

#include <optional>
#include <vector>

#include "aauto/service/AudioService.hpp"
#include "aauto/service/VideoService.hpp"
#include "aauto/service/InputService.hpp"
#include "aauto/service/SensorService.hpp"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/service/BluetoothService.hpp"

namespace aauto {
namespace service {

// What services this head unit exposes to the phone, and with what options.
// Built by the app layer at engine creation time, then passed to ServiceFactory.
// Each field's presence (or vector size) controls whether that service is
// created and advertised in ServiceDiscoveryResponse.
struct ServiceComposition {
    std::vector<AudioServiceConfig>            audio_streams;  // empty = no audio
    std::optional<VideoServiceConfig>          video;
    std::optional<InputServiceConfig>          input;
    std::optional<SensorServiceConfig>         sensor;
    std::optional<MicrophoneServiceConfig>     microphone;
    std::optional<BluetoothServiceConfig>      bluetooth;
};

} // namespace service
} // namespace aauto
```

### 3. `ServiceContext` 확장

`core/include/aauto/service/ServiceFactory.hpp`:
```cpp
struct ServiceContext {
    core::HeadunitConfig                            identity;     // rename from 'config'
    ServiceComposition                              composition;  // NEW
    std::function<void(const session::PhoneInfo&)>  phone_info_cb;
};
```

### 4. `ServiceFactory::CreateAll()` 분기

`core/src/service/ServiceFactory.cpp`:
```cpp
std::vector<std::shared_ptr<IService>> ServiceFactory::CreateAll() const {
    std::vector<std::shared_ptr<IService>> peers;

    for (const auto& cfg : ctx_.composition.audio_streams) {
        peers.push_back(std::make_shared<AudioService>(cfg));
    }
    if (ctx_.composition.video) {
        peers.push_back(std::make_shared<VideoService>(*ctx_.composition.video));
    }
    if (ctx_.composition.input) {
        peers.push_back(std::make_shared<InputService>(*ctx_.composition.input));
    }
    if (ctx_.composition.sensor) {
        peers.push_back(std::make_shared<SensorService>(*ctx_.composition.sensor));
    }
    if (ctx_.composition.microphone) {
        peers.push_back(std::make_shared<MicrophoneService>(*ctx_.composition.microphone));
    }
    if (ctx_.composition.bluetooth) {
        peers.push_back(std::make_shared<BluetoothService>(*ctx_.composition.bluetooth));
    }

    auto control = CreateControl(peers);

    std::vector<std::shared_ptr<IService>> all;
    all.reserve(peers.size() + 1);
    all.push_back(std::move(control));
    all.insert(all.end(), peers.begin(), peers.end());
    return all;
}
```

기존 `CreateAudioMedia` / `CreateAudioGuidance` / `CreateAudioSystem` / `CreateVideo` / `CreateInput` / `CreateSensor` / `CreateMicrophone` / `CreateBluetooth` 헬퍼는 모두 제거 — 생성자 직접 호출이 더 명확.

`CreateControl` 만 유지 (peer_services 주입 + phone_info_cb 와이어링 책임).

### 5. `AAutoEngine` API 변경

`core/include/aauto/core/AAutoEngine.hpp`:
```cpp
class AAutoEngine {
public:
    AAutoEngine(HeadunitConfig identity, service::ServiceComposition composition);

    const HeadunitConfig& GetConfig() const { return identity_; }

    std::shared_ptr<session::Session> CreateSession(
        std::shared_ptr<transport::ITransport> transport,
        SessionCallbacks callbacks);

private:
    HeadunitConfig             identity_;
    service::ServiceComposition composition_;
};
```

`core/src/core/AAutoEngine.cpp` 의 `CreateSession`:
```cpp
service::ServiceContext ctx{
    identity_,
    composition_,
    std::move(callbacks.on_phone_info)
};
service::ServiceFactory factory(std::move(ctx));
// ... (기존)
```

### 6. JNI builder API

`app/android/jni/aauto_jni.cpp` 에 새 native 메서드 추가. AAutoEngine 생성을 늦춰서, Java 측이 capability를 모두 setting한 후 engine을 만든다.

새 흐름:
1. `nativeInit(btAddress, displayWidth, displayHeight, displayDensity)` — `EngineContext` 와 빈 `ServiceComposition` 만 만들고 engine 생성은 보류.
2. `nativeAddAudioStream(int streamType, int sampleRate, int channels)` × N — `composition_.audio_streams` 에 push.
3. `nativeSetVideoConfig(int resolutionEnum, int frameRateEnum, int density)` — `composition_.video` 설정.
4. `nativeSetInputConfig(int touchWidth, int touchHeight, int[] supportedKeycodes)` — `composition_.input` 설정.
5. `nativeSetSensorConfig(boolean drivingStatus, boolean nightMode, boolean location)` — `composition_.sensor` 설정.
6. `nativeSetMicrophoneConfig(int sampleRate, int channels)` — `composition_.microphone` 설정 (호출 안 하면 omit).
7. `nativeSetBluetoothConfig(String carAddress)` — `composition_.bluetooth` 설정.
8. `nativeFinalizeComposition()` — `AAutoEngine` 인스턴스 생성, composition을 transfer. 이후부터 USB/wireless 세션이 가능.

`EngineContext` 에 임시 storage 추가:
```cpp
struct EngineContext {
    // ... 기존
    core::HeadunitConfig            pending_identity;
    service::ServiceComposition     pending_composition;
    bool                            engine_built = false;
};
```

native 메서드들은 `engine_built == false` 일 때만 pending_* 를 수정. `nativeFinalizeComposition()` 이 `make_unique<AAutoEngine>(std::move(pending_identity), std::move(pending_composition))` 호출 후 `engine_built = true`.

### 7. Java 측 — `AaSessionService.java::onCreate`

```java
@Override
public void onCreate() {
    super.onCreate();
    Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
    startForeground(NOTIFICATION_ID, buildNotification());

    String btAddress = getBluetoothAddress();

    // 1. Init basic engine context (no engine yet)
    nativeInit(btAddress, /*display_w=*/1280, /*display_h=*/720, /*display_density=*/140);

    // 2. Build platform composition. This is where this head unit's
    //    capabilities are declared. Different products edit only this block.
    nativeAddAudioStream(STREAM_MEDIA, 48000, 2);
    nativeAddAudioStream(STREAM_GUIDANCE, 16000, 1);
    nativeAddAudioStream(STREAM_SYSTEM,   16000, 1);
    nativeSetVideoConfig(VIDEO_1280x720, FPS_30, 140);
    nativeSetInputConfig(1280, 720,
        new int[]{3, 4, 19, 20, 21, 22, 23, 66, 84, 85, 87, 88, 5, 6});
    nativeSetSensorConfig(true /*drivingStatus*/, false /*nightMode*/, false /*location*/);
    // microphone: not supported on this platform — skip the setMicrophoneConfig call
    nativeSetBluetoothConfig(btAddress);

    // 3. Finalize — engine is built and ready
    nativeFinalizeComposition();

    Log.i(TAG, "BT address: " + btAddress);
    btProfileGate_ = new BtProfileGate(this);
}
```

상수 (`STREAM_MEDIA` 등 enum int 값) 는 Java side에 별도 클래스로 두거나 `AaSessionService` 안에 `private static final int` 로 정의. 표준 AAP enum 값을 그대로 사용.

## 영향 파일 (총 ~17개)

### Core
- `core/include/aauto/service/AudioService.hpp` — `AudioServiceConfig` + ctor 시그니처
- `core/src/service/AudioService.cpp` — ctor 본문
- `core/include/aauto/service/VideoService.hpp` — `VideoServiceConfig` + ctor + member rename
- `core/src/service/VideoService.cpp` — ctor + `FillServiceDefinition` 가 config_ 사용
- `core/include/aauto/service/InputService.hpp` — `InputServiceConfig` + ctor + member rename
- `core/src/service/InputService.cpp` — ctor + `FillServiceDefinition` 가 config_ 사용
- `core/include/aauto/service/SensorService.hpp` — `SensorServiceConfig` + ctor + member 필드
- `core/src/service/SensorService.cpp` — `FillServiceDefinition` 가 flags 분기, `OnChannelOpened` / `HandleSensorStartRequest` 가 sensor type 분기
- `core/include/aauto/service/MicrophoneService.hpp` — `MicrophoneServiceConfig` + ctor + member
- `core/src/service/MicrophoneService.cpp` — ctor + `FillServiceDefinition` 가 config_ 사용
- `core/include/aauto/service/BluetoothService.hpp` — `BluetoothServiceConfig` + ctor + member
- `core/src/service/BluetoothService.cpp` — ctor 본문
- `core/include/aauto/service/ServiceComposition.hpp` — **NEW**
- `core/include/aauto/service/ServiceFactory.hpp` — `ServiceContext` 확장, helper 메서드 제거
- `core/src/service/ServiceFactory.cpp` — `CreateAll` composition-based, helper 제거
- `core/include/aauto/core/AAutoEngine.hpp` — ctor 시그니처 변경
- `core/src/core/AAutoEngine.cpp` — composition 멤버 + CreateSession 와이어링

### JNI
- `app/android/jni/aauto_jni.cpp` — `EngineContext::pending_*`, 새 native 메서드 7개

### Java
- `app/android/src/main/java/com/aauto/app/core/AaSessionService.java` — `onCreate` 흐름 변경, native 메서드 선언 7개 추가, 상수

### BuildInfo
- `app/android/src/main/java/com/aauto/app/BuildInfo.java` — +1

## 단계별 적용 순서

자기 service 한 묶음씩 빌드 통과 검증하며 진행. 각 sub-step 끝나면 빌드 + 동작 회귀 확인.

### Sub-step 1: Service config structs + 생성자 변경
- 6개 service header 에 config struct 추가
- 6개 service ctor 시그니처 변경
- 6개 service .cpp 에서 config_ 사용
- `ServiceFactory::Create*` helper 들이 임시로 hardcode default config 로 새 ctor 호출 (아직 composition 없이)
- **빌드 통과** + 기존 동작 회귀 없음 확인 (composition 없으니 동작은 동일해야 함)

### Sub-step 2: ServiceComposition + ServiceFactory + AAutoEngine
- `ServiceComposition.hpp` 신규
- `ServiceContext` 에 composition 추가, identity rename
- `ServiceFactory::CreateAll` composition 기반 분기, helper 제거
- `AAutoEngine` ctor 시그니처 변경, member 추가
- 호출처 (JNI nativeInit) 임시로 default composition 채워 전달
- **빌드 통과** + 기존 동작 회귀 없음

### Sub-step 3: JNI builder API + Java 측 composition build
- `EngineContext::pending_*` 추가
- 새 native 메서드 7개 (`nativeAddAudioStream`, `nativeSetVideoConfig`, `nativeSetInputConfig`, `nativeSetSensorConfig`, `nativeSetMicrophoneConfig`, `nativeSetBluetoothConfig`, `nativeFinalizeComposition`)
- `nativeInit` 시그니처 확장 (display 인자 추가)
- Java `AaSessionService::onCreate` 흐름 변경
- **빌드 통과** + 동작 회귀 없음 (composition 내용은 현재와 동일하게 전달하므로 동작 동일해야 함)

## Verification

각 sub-step:
1. **빌드 통과** — `Android.bp` 빌드.
2. **로그 검증** — `[ControlService] Sending ServiceDiscoveryResponse...` 로그에서 광고된 service 수가 변경 전후 동일한지 (예: audio×3 + video + input + sensor + bt = 7개).
3. **폰 연결 회귀** — USB 또는 wireless로 폰 연결, AA 화면 표출, 음악 재생, 영상 정상.

전체 완료 후:
- **다른 platform 시뮬레이션 가능 여부** — 가상으로 Java 측에서 mic 광고를 켜거나 sensor location flag를 토글하면 service discovery 응답이 그에 맞춰 변화하는지 (light 검증).
- **LOC1 후속 작업의 hook 존재 확인** — `nativeSetSensorConfig(..., true)` 만으로 location service definition이 응답에 포함되는지. 데이터 송신 자체는 별개 step이지만 광고만이라도 동작해야 함.

## Step Log 예약

이 plan 적용 시 `0001_p1_compare.md` 의 Step Log 에 다음 항목을 추가:

- Step 6 — Sub-step 1: Service config structs + 생성자 변경 (BUILD ?)
- Step 7 — Sub-step 2: ServiceComposition + ServiceFactory + AAutoEngine (BUILD ?)
- Step 8 — Sub-step 3: JNI builder API + Java composition build (BUILD ?)
