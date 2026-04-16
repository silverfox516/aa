# Android Auto Headunit — Architecture Review

> 작성 시점: 2026-04-16. 본 문서는 (a) 이상적 구조 제안, (b) 현재 코드와의
> 차이점, (c) 발견된 문제점을 정리한다. 코드 변경은 동반하지 않는다.

---

## Part A. 이상적 구조 (first-principles 설계)

### A.1 설계 원칙

1. **엄격한 단방향 의존**: app → impl → core. core는 platform 무지(無知).
   `#ifdef __ANDROID__` 는 core 어디에도 없어야 한다.
2. **인터페이스 분리**: sink, source, transport, crypto 모두 추상 인터페이스로
   분리. impl 계층이 구현체를 제공.
3. **의존성 주입**: Service는 sink/source를 직접 만들지 않는다. 외부에서 attach.
   Session은 transport를 직접 만들지 않는다. 외부에서 주입.
4. **Composition over hierarchy**: HU 제품마다 서비스 구성이 다르다. 이를
   `ServiceComposition` 같은 데이터로 표현하고, 하드코딩하지 않는다.
5. **세션 1:1 폰**: 폰 1대 = Session 1개 = transport 1개 = crypto context 1개.
   다대다 매핑 없음. 멀티 폰은 Session N개로 표현.
6. **Single-writer transport**: 한 transport에 동시에 쓰는 thread는 정확히 1개.
   여러 sender는 큐를 거쳐 직렬화.
7. **명시적 lifecycle**: Session/Service/Transport 모두 상태가 enum으로 명시되고,
   전이 trigger와 callback이 코드에 드러나야 한다. "암묵적 상태" 금지.
8. **Test seam**: core는 mock transport/sink/crypto 만으로 단위 테스트 가능해야
   한다. impl/app 없이도 protocol 로직을 검증할 수 있어야 한다.
9. **Thin JNI**: JNI 파일은 marshaling만. 비즈니스 로직(세션 활성화 정책,
   sink 라우팅 등)은 Java app layer 또는 C++ core에 둔다.
10. **명명과 위치의 일관성**: `core/`에 인터페이스, `impl/<platform>/`에 구현,
    `app/<platform>/`에 UI/lifecycle. 같은 종류 코드가 두 곳에 있으면 안 됨.

### A.2 레이어 다이어그램

```
┌──────────────────────────────────────────────────────────┐
│ App Layer (per platform — Android / Linux / macOS)        │
│   • UI (Activities / Widgets)                             │
│   • SessionManager (multi-device, active-session policy)  │
│   • DeviceRegistry (instance_id → 사용자 별명, 자동연결)  │
│   • OS lifecycle bridge (Service, BootReceiver, Daemon)   │
│   • Discovery wiring (USB/BT/WiFi watchers → transports)  │
└──────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────┐
│ Platform Adapter Layer (impl/<platform>/)                 │
│   • IAudioSink   ← MediaCodecAudio / OpenSL / ALSA / SDL  │
│   • IVideoSink   ← MediaCodecVideo / GStreamer / SDL      │
│   • IInputSource ← Touch/key 입력을 AAP 이벤트로          │
│   • ISensorSource ← GPS/IMU                               │
│   • ITransport   ← UsbTransport / TcpTransport / Composite│
│   • Discovery    ← UsbWatcher / BtScanner / mDNS responder│
└──────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────┐
│ Service Layer (core, protocol-aware, platform-agnostic)   │
│   IService base                                           │
│   ├ ControlService   (handshake, ping, byebye)            │
│   ├ AudioService×N   (MEDIA / GUIDANCE / SYSTEM)          │
│   ├ VideoService                                          │
│   ├ InputService                                          │
│   ├ SensorService                                         │
│   ├ MicrophoneService                                     │
│   ├ BluetoothService (페어링 정보 광고)                   │
│   └ NavigationService 등 확장                             │
└──────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────┐
│ Session Layer (core)                                      │
│   • Session: state machine (Disconnected→Handshake→       │
│              Connected→Disconnected), 3 worker threads    │
│   • Handshaker: version, SSL, AuthComplete, ServiceDisc   │
│   • MessageFramer: AAP fragment 조립/분해                 │
│   • SendQueue: bounded, single consumer = SendLoop        │
│   • PhoneInfo: SD_REQ에서 추출된 폰 식별자                │
└──────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────┐
│ Crypto Layer (core)                                       │
│   ICryptoStrategy ← TlsCryptoStrategy (OpenSSL)           │
│   AapKeys: HU 인증서/키 (빌드 시 주입, hardcode 지양)     │
└──────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────┐
│ Transport Layer (interface in core, impl in impl/)        │
│   ITransport: Connect / Disconnect / Send / Receive       │
│   • UsbTransport                                          │
│   • TcpTransport                                          │
│   • CompositeWirelessTransport (BT control + TCP data)    │
└──────────────────────────────────────────────────────────┘
```

### A.3 디렉토리 레이아웃

```
aa/
├── core/                          # platform-free C++17
│   ├── include/aauto/
│   │   ├── transport/             # ITransport, ITransportFactory
│   │   ├── crypto/                # ICryptoStrategy, AapKeys
│   │   ├── session/               # Session, Handshaker, Framer, PhoneInfo
│   │   ├── service/               # IService + 각 서비스 헤더
│   │   ├── sink/                  # IVideoSink, IAudioSink, IInputSource, ISensorSource
│   │   ├── core/                  # Engine, HeadunitConfig, Composition, DeviceRegistry(if portable)
│   │   └── utils/                 # Logger, ProtocolUtil
│   └── src/                       # 위 헤더 구현
│
├── impl/
│   ├── common/                    # 모든 OS 공통 (libusb 기반 USB transport 등)
│   ├── android/
│   │   ├── transport/             # AndroidUsb, Tcp, CompositeWireless
│   │   ├── sink/                  # MediaCodecVideoSink, OpenSLAudioSink
│   │   └── jni/                   # Thin JNI bridge (functions split by domain)
│   ├── linux/                     # Qt + GStreamer + ALSA
│   └── macos/                     # SDL2 등
│
├── app/
│   ├── android/
│   │   ├── service/               # AaSessionService (engine 소유)
│   │   ├── ui/                    # MainActivity, AaDisplayActivity
│   │   ├── usb/  bt/  wireless/   # discovery monitors
│   │   └── manager/               # SessionManager, DeviceRegistry (Android-specific persistence)
│   ├── linux/                     # Qt main + UI
│   └── macos/                     # Cocoa main
│
├── protobuf/                      # AAP .proto 정의 + 빌드 산출물
├── docs/
│   ├── architecture_review.md     # 본 문서
│   ├── plans/                     # 작업 계획 (CLAUDE.md 정책)
│   └── protocol_reference.md      # AAP wire 포맷 cheatsheet
└── tests/
    ├── core/                      # mock-based unit tests
    └── integration/               # 실제 transport 없이 fake phone 시나리오
```

### A.4 핵심 컴포넌트 책임

| Component | 책임 | 책임 아닌 것 |
|---|---|---|
| `Engine` | Session 생성 | Session 보관, 활성화 정책 |
| `Session` | transport ↔ services 중개, encryption, framing, lifecycle | UI 알림, 다른 Session 인지 |
| `Service` | AAP 채널 메시지 처리, sink/source 상태 캐시 | sink 생성, transport 직접 호출 |
| `Sink/Source` | 플랫폼 출력/입력 어댑터 | protocol 파싱 |
| `Transport` | 바이트 스트림 + connect/disconnect | 메시지 framing, encryption |
| `SessionManager` (app) | 여러 Session 트래킹, 활성 1개 정책, sink 라우팅 | protocol 처리 |
| `DeviceRegistry` (app) | instance_id → 사용자 별명 / 자동연결 정책 영속화 | 세션 lifecycle |
| `JNI bridge` | 타입 변환, callback 디스패치 | 비즈니스 결정 (활성화 등) |

### A.5 멀티-디바이스 모델

- **Session per phone**: 폰 N대 = Session N개. 각자 transport, crypto, threads, services.
- **Active session 한 개**: 차량 스피커/디스플레이는 하나뿐이므로 한 번에 한
  Session만 sink 부착. 나머지는 dormant (handshake/keepalive는 유지해도 됨,
  정책 결정).
- **DeviceRegistry**: 폰 instance_id를 영속 저장. "이전에 본 폰은 자동 활성"
  같은 정책 가능.
- **Background audio**: 화면을 떠도 active session의 audio sink는 유지. video sink만 detach.

### A.6 Test 전략

- **core 단위 테스트**: `MockTransport`(byte feed scriptable), `MockCryptoStrategy`(passthrough),
  `RecordingVideoSink/AudioSink` 로 protocol 시나리오 재현. 폰 없이 검증.
- **integration 테스트**: 실제 OpenSSL + recorded AAP byte stream(.bin) 으로
  end-to-end. CI에서도 실행 가능.
- **빌드 타깃**: gtest + CTest. Android.bp는 native_test 모듈, CMakeLists는 add_test.

---

## Part B. 현재 구조 vs 이상적 구조 비교

### B.1 잘 되어 있는 부분 (유지)

| 항목 | 평가 |
|---|---|
| Top-level layering (core/impl/app/protobuf) | ✅ 이상에 가깝다 |
| Core 헤더에 platform #ifdef 없음 | ✅ Logger.cpp 구현부에만 존재 — OK |
| Service들이 sink를 attach 받는 모델 (소유 X) | ✅ 과거 Phase 1 리팩터 결과로 정착 |
| ServiceComposition 으로 서비스 구성 데이터화 | ✅ Phase 2 리팩터 결과 |
| Session per transport, 3 worker threads (recv/proc/send) | ✅ |
| SendQueue로 single-writer 보장 | ✅ Phase 0004 결과 |
| 멀티 세션 지원 (handle 기반) | ✅ 동작은 하지만 위치가 문제 — B.2 참고 |
| AAP 메시지 카탈로그 / 매트릭스 분석 | ✅ 과거 0001 plan으로 축적 (지금은 코드/주석에 흡수) |

### B.2 이상과 다른 부분 (개선 후보)

| # | 항목 | 현재 | 이상 | 영향 |
|---|---|---|---|---|
| 1 | **JNI 비대화** | `aauto_jni.cpp` 665 LOC, 40+ JNI 메서드, 세션 활성화/sink 라우팅 등 비즈니스 로직 포함 | thin marshaling layer만 (~150 LOC). 정책은 Java SessionManager 또는 C++ Engine에 | 변경 시 전면 수정 위험, 테스트 어려움 |
| 2 | **SessionManager 부재** | Java `AaSessionService`(778 LOC)가 모든 책임 떠안음 — 세션 맵, 활성화, sink 라우팅, surface 관리, BT/USB ready 핸들러까지 | `SessionManager`(세션 트래킹+활성화), `SinkRouter`(surface↔video, audio focus), `LifecycleBridge`(Android Service) 분리 | 단일 클래스 변경 비용 큼, 회귀 위험 누적 |
| 3 | **DeviceRegistry 부재** | 폰 식별자가 휘발성 (재연결 시 기억 못함). 사용자 별명 저장 안 됨 | SharedPreferences 기반 `instance_id → DeviceProfile` (별명, 마지막 연결 시각, 자동활성 여부) | UX 떨어짐, Phase 4 미이월 항목 |
| 4 | **CompositeWirelessTransport 부재** | BT RFCOMM(control)+TCP(data) 조합이 Java(`BluetoothWirelessManager`+`WirelessMonitorService`)에서 choreography. core/impl는 TCP 한쪽만 봄 | core/impl에 `CompositeWirelessTransport` (BT control + TCP data)로 캡슐화. lifecycle invariant도 클래스 내부에서 보장 | RFCOMM 끊김 ↔ TCP 끊김 동기화가 plan 0005에서 다층 broadcast로 해결 — 본질적으로 transport 책임 |
| 5 | **Tests 0개** | 단위/통합 테스트 디렉토리 자체 없음 | core mock-based unit + recorded byte stream integration | 회귀 검증을 매번 실기기로. 디버깅 cycle 김 |
| 6 | **Hardcoded crypto identity** | `AapKeys.hpp`에 cert/key 하드코딩 | 빌드 시 주입 (Android.bp 변수, CMake option). 제품별 다른 키 가능 | 보안/배포 유연성 부족 |
| 7 | **ControlService → peer_services 역참조** | ControlService 생성자가 peer 서비스 vector를 받아 SD_RESP에 채움 | ControlService는 peer 서비스 정보를 callback이나 별도 SDProvider 인터페이스로 받기 | 서비스 등록 순서 의존성, 순환 참조 risk |
| 8 | **Session 재사용 불가** | DISCONNECTED 진입 후 재연결하려면 Session 객체 재생성 | Session::Reset() 또는 명시적 stateless 보장 | 객체 lifecycle 비용 (가벼우면 OK) |
| 9 | **MainActivity 513 LOC, AaSessionService 778 LOC** | 단일 책임 위반 | UI / adapter / event handling 분리 | 가독성, PR review 비용 |
| 10 | **docs/architecture 부재** | `docs/plans/`만 존재, 구조 설명 문서 없음 | architecture.md, protocol_reference.md, 신규 기여자 onboarding doc | 신규 작업자 onboarding 비용 |

### B.3 발견된 anti-patterns (요약)

1. **"God Service"**: `AaSessionService.java` 778 LOC — 세션 컨테이너 + sink 라우터 + lifecycle bridge + USB/wireless ready handler 전부.
2. **"God JNI"**: `aauto_jni.cpp` 665 LOC + 40 JNI 메서드 — domain별로 분리되지 않음.
3. **"Java가 protocol invariant 책임"**: BT-TCP lifecycle invariant(plan 0005)가 Java 측 broadcast 흐름에 의존. core가 알아야 할 정보를 app이 알게 됨.
4. **순환 의존성 risk**: ControlService ↔ peer services (생성자 인자로 vector). 새 서비스 추가 시 ControlService와의 결합도 증가.
5. **테스트 부재로 인한 "실기기 의존 디버깅"**: 매 변경마다 폰+USB/wireless로 회귀. SSL handshake, ping timeout 등 단위 테스트가 가능한 영역도 미커버.
6. **암묵적 상태머신**: Session::SessionState는 명시적 (3 enum), 그러나 AaSessionService.SessionState (CONNECTING/HANDSHAKING/READY/RUNNING/BACKGROUND_AUDIO)는 5 enum이고 native와의 매핑이 코드 흩어짐.

---

## Part C. 문제점 우선순위 (개선이 필요하다고 판단되는 순)

| 순위 | 문제 | 근거 |
|---|---|---|
| **P0** | 테스트 부재 | 회귀 비용이 모든 변경에 누적. 가장 ROI 높음 |
| **P0** | `AaSessionService` 책임 분리 (SessionManager / SinkRouter / LifecycleBridge) | 가장 큰 클래스, 다음 변경의 병목 |
| **P1** | DeviceRegistry 도입 | 사용자 가치 직결 (별명, 자동연결). 작업 범위 작음 |
| **P1** | JNI 분할 (domain별 .cpp: jni_engine, jni_session, jni_sink, jni_compose) | 가독성/안전성. 기능 변경 없음 |
| **P2** | CompositeWirelessTransport로 BT-TCP lifecycle 캡슐화 | plan 0005 후속. 책임 위치 정정 |
| **P2** | architecture.md / protocol_reference.md 작성 | 본 문서가 시작점. onboarding 효과 |
| **P3** | ControlService 의존성 역전 (SDProvider 인터페이스) | 새 서비스 추가 빈도가 낮으면 후순위 |
| **P3** | Crypto identity 빌드 주입화 | 배포 정책 정해질 때 |
| **P3** | Session 재사용 가능화 | 현 cost 미미, 우선순위 낮음 |

---

## Part D. 다음 단계 (제안, 사용자 승인 필요)

본 문서는 진단까지만 다룬다. 실제 개선은 P0 / P1 항목부터 별도 plan으로
`docs/plans/0001_*.md` ~ 분리하여 작성·실행한다. 가장 먼저 시작하기 좋은 단위:

- **0001 — Test harness 부트스트랩**: gtest + MockTransport + MockSink + 1개 시나리오(SD_REQ→SD_RESP) 를 통과시키는 최소 인프라. 후속 테스트가 추가될 토양.
- **0002 — AaSessionService 분리**: SessionManager / SinkRouter / 본체 LifecycleBridge로 3분할. 동작 동일.
- **0003 — DeviceRegistry**: SharedPreferences 기반 instance_id → DeviceProfile.

위 우선순위와 다음 단계는 모두 **제안**이며, 실제 진행 여부와 순서는 사용자
승인 후 결정한다.

---

## Part E. What we didn't question (가정 재검토)

Part A의 "이상적 구조"는 사실 현재 코드의 폴리시드 버전에 가깝다 — survey
결과를 먼저 보고 그 위에서 사고했기 때문(anchoring bias). 이 섹션은 우리가
무비판적으로 받아들인 가정들을 명시적으로 뒤집어 본다. 채택 권고가 아니라
**사고의 옵션 공간**을 넓히기 위한 도구.

### E.1 가정: "core는 C++로 짠다"

**왜 의심해야 하나**: openauto/aasdk가 C++라서 이어받았을 뿐, first-principle
근거가 약하다. 우리가 직접 짜는 부분(protocol 파서, 세션 상태머신, AAP 메시지
디스패치)은 **메모리 안전성 + 패턴 매칭 + 가벼운 동시성**이 더 가치 있다.

**대안들**:
- **Rust core**: 프로토콜 파서/framer는 unsafe 없이 작성 가능. lifetime이
  buffer ownership을 컴파일 타임에 검증 → "이중 free", "use-after-free" 류
  버그를 원천 차단. tokio 한 thread pool로 N 세션 async 처리.
- **Kotlin Multiplatform**: app과 core가 같은 언어. JNI 자체가 사라짐.
  AAOS(Android Automotive)면 native가 굳이 필요 없을 수도.
- **Zig**: C ABI 호환 + 빌드 단순. 임베디드 친화적.

**Trade-off**: Rust/Kotlin은 OpenSSL/MediaCodec 같은 기존 native 의존성을
FFI로 다리 놓아야 함. 학습 비용. 그러나 "AAP 프로토콜 버그가 메모리 안전성
때문에 디버깅 어려움" 같은 실제 비용과 비교해야.

### E.2 가정: "단일 프로세스에서 모든 세션을 돌린다"

**왜 의심**: 폰 한 대가 SSL 핸드셰이크 무한 retry loop에 빠지거나 디코더가
SIGSEGV를 내면 **다른 폰 세션도 같이 죽는다**. 자동차 환경에서 isolation은
보안·신뢰성 양쪽으로 가치가 큼.

**대안: process-per-session**
```
aa-supervisor (Android Service)
  ├─ fork → aa-session-worker (phone A)  ← BT MAC + USB FD pass
  ├─ fork → aa-session-worker (phone B)
  └─ Binder/Unix socket으로 audio/video PCM 데이터만 IPC
```

각 worker가 sandbox(SELinux domain) 내에서만 동작 → A 폰 SSL 버그가 B 영향 X.
Crash 시 supervisor가 자동 재기동.

**Trade-off**: IPC overhead (PCM/H.264 byte 전송 자체는 audio 1Mbps, video
10Mbps 정도라 modern Linux IPC로 충분). 코드 분리 비용. AAOS 아키텍처와
잘 맞음.

### E.3 가정: "세션마다 3개 worker thread (recv/proc/send)"

**왜 의심**: TCC803x 같은 임베디드에서 폰 4대 = 12 thread + heartbeat 4 +
sensor 4 + ... = OS thread 폭발. context switch 오버헤드.

**대안: async runtime**
- **C++**: boost::asio strand 기반 — io_context 하나로 N 세션 (single thread
  scheduling, lock-free intra-session)
- **Rust**: tokio multi-threaded runtime
- **Kotlin**: coroutines + Dispatchers.IO

각 세션은 thread가 아니라 **co-routine**이고, 동시성은 future/await로 표현.
`receive→process→send` 파이프라인이 그대로 read/decrypt/dispatch chain으로 표현.

**Trade-off**: 코드 가독성(코루틴/async에 익숙해야), 디버깅(stack trace가
runtime을 거침). 그러나 자원 효율은 한 자릿수 ms/MB 단위로 향상.

### E.4 가정: "JNI bridge가 필요하다"

**왜 의심**: JNI는 결합이 강하고 디버깅이 어렵고 ABI를 깨면 통째로 죽는다.
JNI 호출 자체가 ART에서 비싸다(VM stub 거침).

**대안 1: native daemon + Binder/AIDL**
- C++/Rust core를 별도 native 프로세스(`/system/bin/aa-engine`)로 실행
- Java app은 AIDL 인터페이스로 통신
- JNI 0 LOC. 양 쪽이 독립적으로 재시작/디버그 가능
- AAOS HAL 모델과 동일한 패턴

**대안 2: 전부 Java/Kotlin**
- AAP는 결국 byte 파싱 + OpenSSL + MediaCodec/AudioTrack 호출
- 모두 Java API 존재 (JCE, MediaCodec, ...)
- 성능 측정 필요하지만 modern hardware는 충분할 수도
- core/impl/jni 디렉토리 통째로 사라짐. 빌드/배포 단순

**Trade-off**: 1번은 IPC. 2번은 GC pause(audio jitter risk), 그리고 다른
플랫폼(Linux/macOS) 지원 포기. 우리 제품이 자동차 한정이면 후자가 합리적.

### E.5 가정: "Service는 채널당 클래스 (singleton)"

**왜 의심**: AAP 메시지는 결국 (channel, type, payload) tuple. 이걸 처리하는
함수들의 모음이 왜 클래스여야 하나? 상태(`session_id_`, `media_data_count_`)도
결국 메시지 처리 중간 결과일 뿐.

**대안: Reactive stream pipeline**
```
TransportStream (Flow<Bytes>)
  → DecryptStage (Flow<DecryptedFrame>)
  → DemuxStage (Flow<AapMessage>)
  → fanOut by channel
       ├─ Audio  (Flow<AudioFrame>)  → AudioSink.collect
       ├─ Video  (Flow<VideoFrame>)  → VideoSink.collect
       └─ Control (Flow<ControlMsg>) → handleControl
```

각 stage는 pure function (state는 `scan`/`fold`로 명시). 단위 테스트는
input flow → expected output flow 비교. mock 필요 없음.

**Trade-off**: 학습 곡선. 디버깅(어디서 뭐 잘못됐는지 stack 끊김). C++에서는
RxCpp나 직접 구현 필요 — 무게 큼. Rust(`futures`/`async-stream`), Kotlin
(`Flow`)에서는 자연.

### E.6 가정: "Sink/Source는 직접 호출 인터페이스 (push 모델)"

**왜 의심**: 현재 `IVideoSink::OnVideoFrame()`은 service가 sink를 직접 호출.
멀티 sink, 녹화, 디버그 tap 같은 케이스가 어려워진다.

**대안: pub/sub message bus**
- 모든 미디어 데이터는 in-process pub/sub(LCM, ZeroMQ inproc, 또는 단순
  `EventBus`) 토픽에 publish
- 토픽: `/session/{id}/video/h264`, `/session/{id}/audio/{channel}/pcm`
- consumer는 누구든 subscribe 가능: 디스플레이, 녹화기, 미리보기, 디버그 dump

**Trade-off**: byte buffer 복사 비용 (현 모델은 zero-copy shared_ptr). 진단/
확장성 가치와의 trade.

### E.7 가정: "Transport API는 동기 블로킹 (Send/Receive)"

**왜 의심**: `transport_->Receive()`가 블로킹이라 thread를 점유. cancellation은
`Disconnect()`로 우회 — 어색함.

**대안: 명시적 async**
```cpp
awaitable<vector<uint8_t>> Receive(stop_token);
awaitable<bool>             Send(span<const uint8_t>, stop_token);
```
또는 callback-based (`OnDataAvailable(callback)`).

C++20 coroutines 또는 Rust async에서 자연. cancellation 명시적, 타임아웃 명시적.

### E.8 가정: "HU 인증서/키는 빌드 타임에 박혀 있다"

**왜 의심**: AAP는 폰이 HU 인증서를 신뢰해야 동작. 리버스엔지된 reference 키를
영구히 쓰면 Google이 revoke할 위험. 또한 HU 제품마다 다른 키는 차량 fleet
관리에 유용.

**대안**:
- 빌드 시 `--cert-pem`/`--key-pem` argument로 주입
- TEE/HSM에 키 보관, 부팅 시 로드 (자동차급 보안)
- 인증서 회전(rotation) 메커니즘 — 원격 업데이트로 새 키 받기

### E.9 가정: "Discovery는 OS-level watcher가 한다"

**왜 의심**: USB는 udev/UsbManager, BT는 BluetoothBroadcastReceiver, WiFi는
WifiManager — 발견 메커니즘이 OS API에 결합. 멀티 플랫폼 코드를 똑같이 짜야.

**대안: unified Discovery service**
- 추상 `IDeviceDiscovery` 인터페이스 (`Stream<DiscoveryEvent>`)
- 플랫폼별 구현이 OS event를 표준 이벤트로 변환
- core가 transport 종류와 무관하게 "device appeared"를 받아 transport factory에 위임

이건 부분적으로 현재도 되어 있지만, 무선(BT+WiFi 짝)은 Java가 choreography해서
core가 무지함.

### E.10 가정: "모든 세션을 동일하게 다룬다"

**왜 의심**: 폰마다 capability 다름 (Android Auto vs CarPlay vs custom). 폰
모델별 quirk(예: 삼성은 SD_REQ를 두 번 보냄, 픽셀은 audio focus를 빨리 lose)는
현재 코드 어디에도 모델링 안 됨 — 발견되면 ad-hoc 분기.

**대안: PhoneCapabilities + QuirksProfile**
- `instance_id` 또는 `device_name` → `QuirksProfile` 매핑
- 각 quirk는 명시적 데이터 (timeout 값, retry 정책, 비활성 메시지 목록)
- 새 폰 모델 발견 시 데이터만 추가, 코드 안 건드림

### E.11 가정: "core/impl/app 3-tier 레이어가 자연스럽다"

**왜 의심**: 이 구조는 "생성자가 인터페이스를 알고, 런타임에 구현체를 주입"이라는
OOP 패러다임의 산물. Hexagonal architecture/Ports & Adapters는 다르게 본다:
**core가 ports(인터페이스)를 정의하고, adapter들이 양쪽에 붙는다 — 양방향**.

```
            ┌─────────────────────┐
 Driving →  │                     │ ← Driven
 (Discovery)│   Application Core  │ (Sinks)
            │   (Use cases)       │
            │                     │
            └─────────────────────┘
              ↑                 ↑
        Inbound port       Outbound port
        (e.g. ConnectPhone) (e.g. PlayAudio)
```

이렇게 보면 "ITransport는 inbound (data가 안으로 흐름), IAudioSink는 outbound
(data가 밖으로 흐름)" 이라는 비대칭이 명확해지고, 양쪽 모두 mock으로 교체 가능.
Test seam이 디자인의 부산물이 아니라 핵심.

---

## Part E 정리

위 11개 가정 중 **현실적으로 즉시 채택 가능한 것**은 별로 없다 — 대부분 언어
교체나 프로세스 모델 변경 같은 큰 결정이 필요. 그러나 다음 두 개는 우리 코드에
점진적으로 도입 가능:

- **E.10 QuirksProfile** — 폰 모델별 quirk를 데이터화. Phase 단위 plan으로
  실행 가능. P1 후보.
- **E.6 부분 적용** — VideoSink를 list로 받아 multi-cast (녹화 sink, 미리보기
  sink). 인터페이스 작은 변경만으로 가능. P3 후보.

나머지(언어 교체, 프로세스 분리, 전면 async, AIDL daemon화)는 **장기 비전 문서**
영역. 신규 product line이나 차세대 HU 설계 시 진지하게 검토할 것.
