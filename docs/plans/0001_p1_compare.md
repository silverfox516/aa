# 작업 트랙

이 파일은 **현재 활성 분석/구현 트랙**과 **PARKED 트랙**을 함께 보관합니다.
사용자 요청에 따라 백그라운드 오디오 구현 작업은 일단 보류 상태로 두고,
P1(aasdk + openauto) reference 코드 분석 작업을 우선 진행합니다.

---

# [ACTIVE] P1 Reference 대조 — 서비스/메시지 영역

## Context

`Z:/work/aauto/P1/` 에 두 reference 코드베이스가 있다:
- `P1_aasdk_opencardev` — AAP protocol library. `core/` 의 protocol 부분 대응.
- `P1_openauto_opencardev` — aasdk를 사용하는 헤드유닛 앱 (Qt). `app/android/` + `core/service/` 비즈니스 로직 대응.

이미 audio focus 검증으로 두 가지가 드러났다:
1. 우리 코드가 `AudioFocusRequestNotification`을 쓰는데 aasdk 표준은 `AudioFocusRequest`다 (wire-compatible이지만 비표준 선택).
2. 응답 매핑은 우리가 openauto보다 더 정교(transient mirror).

audio focus는 빙산의 일각이다. 본격적으로 코드를 쌓아가기 전에 message handler 단위로 차이를 체계적으로 매핑해 둔다.

## 범위 — 서비스/메시지에 한정

| # | 영역 | 상태 |
|---|---|---|
| **D** | Session lifecycle (Control channel handshake류) | 분석 완료 |
| **E** | Control channel services (focus/ping/voice session/battery 등) | 분석 완료 |
| **F** | Media services (Audio/Video/Mic) | 분석 완료 |
| **G** | Input service | 분석 완료 |
| **H** | Sensor service | 분석 완료 |
| A | Protocol/Wire 카탈로그 | (D~H 진행 중 자연 흡수) |
| **B** | Cryptor & Frame | **부분 처리** — `MessageFramer` 단순화 + 산술 버그 fix + aasdk fragment-by-fragment decrypt 모델로 전환 (Step Log 참고) |
| C | Transport (USB/TCP) | PARKED |
| I | Bluetooth wireless dial | PARKED |
| J | Application lifecycle (Java 측) | PARKED |

PARKED 영역은 별도 트랙에서 다룬다 (이 plan file의 마지막 PARKED 섹션 참고).

## 진행 순서 (사용자 합의)

1. **D + E** — Control channel 전체 (handshake류 + 부수 control 메시지). control channel은 다른 모든 service의 lifecycle을 결정하므로 먼저 정착시킨다.
2. **F** — Media services (audio 3종 + video + mic). 데이터 양이 가장 많고 sink 모델과 직결됨.
3. **G + H** — Input + Sensor. 가장 작은 표면적, 마지막에.

각 단계 끝마다 사용자 검토 → action item 합의 → 다음 단계.

## 대조 단위 & 깊이 (사용자 합의)

- **단위**: 메시지 ID 한 줄 = 매트릭스 한 행. 한 service 안의 sub-message도 분리해서 적는다 (예: PING_REQUEST와 PING_RESPONSE 별도, MEDIA_SETUP/START/STOP/DATA 각각).
- **깊이**: **logic 수준**. 메시지 ID/시그니처/proto 필드 일치 여부 + 핸들러 본문 로직(응답 정책, 부수 동작)까지. error/race/엣지 케이스 같은 behavioral은 실제 버그가 보일 때 가서 다룬다.

## 매트릭스 컬럼 정의

```
| msg ID | 이름 | ch | 방향 | reference 핸들러 | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
```

- `ch` — 채널 (0=Control / V=Video / AM=AudioMedia / AG=AudioGuidance / AS=AudioSystem / I=Input / S=Sensor / B=Bluetooth)
- `방향` — `→` (phone→HU) / `←` (HU→phone) / `↔` (양방향)
- `reference 핸들러` / `우리 핸들러` — `파일:라인` 형식
- `proto` — payload protobuf 메시지 이름. 없으면 `—`
- `reference 정책` / `우리 정책` — 한 줄 요약 (응답 매핑, 부수 동작)
- `평가` — `✅` 일치 / `⚠️` 단순화 / `❌` 누락 / `🔧` 잘못 / `➕` 의도적 개선
- `action` — 이 행에 대해 할 일 (`정리 후보` / `즉시 수정` / `문서화` / `보류` / `없음`)

## Reference 사실 카탈로그 (누적)

대조 작업 중 reference 코드에서 확인된 사실을 영역별로 누적한다. 결과 매트릭스의 1차 입력 자료.

### A. Protocol 카탈로그

- **Audio focus message ID** (aasdk `ControlMessageType.proto:76-77`): `MESSAGE_AUDIO_FOCUS_REQUEST = 18` (0x12), `MESSAGE_AUDIO_FOCUS_NOTIFICATION = 19` (0x13). 단일 request/response 쌍.
- **Focus 명명 컨벤션** (Nav/Audio/Video 모두): phone→HU 요청은 `XxxRequestNotification`, HU→phone 응답은 `XxxNotification`. Audio만 `AudioFocusRequest.proto`라는 outlier가 추가로 존재 (aasdk도 정의는 보유하나 코드에서 미사용).
- **Service ID 정의 위치**: `protobuf/aap_protobuf/service/control/ControlMessageType.proto`. 모든 control 메시지 ID가 여기 enum으로 정의됨.

### B. Cryptor & Frame (PARKED 영역, SSL decrypt 디버깅 중 노출)

#### Reference (aasdk) 사실

- **Frame wire format** (aasdk `FrameHeader.cpp:26-31`, `FrameSize.cpp:36-47`, `MessageInStream.cpp:59-128`):
  - 2 byte `FrameHeader` (channel + flags)
  - 2 byte `FrameSize` SHORT (MIDDLE/LAST/BULK) 또는 6 byte EXTENDED (FIRST: 2 byte frameSize + 4 byte totalSize)
  - `frameSize` byte의 ciphertext (또는 plain payload)
- **`FrameType` enum** (`FrameType.hpp:27-32`): `MIDDLE=0`, `FIRST=1<<0`, `LAST=1<<1`, `BULK=FIRST|LAST`. `EncryptionType::ENCRYPTED = 1<<3`. 우리 `FLAG_FIRST=0x01`, `FLAG_LAST=0x02`, `FLAG_ENCRYPTED=0x08`과 일치.
- **Decrypt 호출 모델 — 핵심**: aasdk `MessageInStream::receiveFramePayloadHandler:131-145`는 **각 fragment를 받는 즉시** `cryptor_->decrypt(message_->getPayload(), buffer, frameSize_)` 호출. plaintext 결과를 channel별 message buffer에 누적. LAST/BULK 시 promise resolve.
- **TLS state는 Cryptor 인스턴스가 보유** — partial record는 SSL state machine이 자체 관리, 다음 fragment에서 record가 완성되면 그 호출에서 plaintext 반환.
- **한 frame = 한 SSL record** 가 표준 가정 (확정 spec은 미확인이나 reference 모델이 그렇게 동작).

#### 우리 코드 (수정 전) 사실

- **`MessageFramer.cpp` (수정 전)**: byte stream을 받아 multi-fragment를 **자체적으로 reassemble** 후 `AapMessage{channel, payload}` callback. `fragment_buffers_` map으로 channel별 in-progress fragment 관리, 5초 timeout으로 stale 제거.
- **`Session::ProcessLoop` (수정 전)**: callback 받은 reassembled buffer 전체를 한 번에 `crypto_->DecryptData(payload)` 호출.
- **`CryptoManager::Decrypt` (수정 전)**: `BIO_write` 후 `SSL_read` while loop. 결과 빈 vector면 caller가 fail로 처리.
- **모델 차이**: 우리 = reassemble-then-decrypt, aasdk = fragment-by-fragment decrypt. 두 모델은 wire 호환은 가능하나 SSL state machine 관점에서 buffer boundary 처리가 다름.

#### 발견한 버그 (수정 전 문제점)

1. **🔧 `MessageFramer::ProcessBuffer` buffer overrun** — multi-first frame의 4 byte total_size를 buffer-size guard에 포함하지 않음. `aap_packet_len = HEADER_SIZE + payload_len`만 검사 후 통과 → 그 다음 `extra_skip += 4`로 늘림 → `payload_ptr = hdr + 8` 에서 `payload_len` byte 읽기 → wire에 있는 byte 수보다 4 byte 초과 가능. 누적 corruption 가능성.
2. **🔧 단일 호출 reassemble-then-decrypt 모델의 잠재 비호환** — 폰이 한 frame에 여러 TLS record를 packing할 가능성 또는 한 record가 여러 frame에 split될 때, 우리 모델은 SSL_read while loop가 여러 record plaintext를 한 결과로 concat 반환 → ProcessLoop가 한 message로 dispatch → 잘못된 message type 파싱 → 그 다음 frame부터 sequence 어긋남 → `BAD_DECRYPT` 폭발.

#### 증상

- 동작하다가 100~200 frame 후 `SSL decrypt failed (1): error:1e000065 BAD_DECRYPT` + `error:1000008b DECRYPTION_FAILED_OR_BAD_RECORD_MAC` 발생, 이후 모든 channel 1 (video) 메시지가 drop.

#### 수정 (Step 1~4, Step Log 참고)

aasdk와 동일한 fragment-by-fragment decrypt 모델로 전환:
- `MessageFramer` 단순화: reassembly state 제거, fragment 단위 callback (`AapFragment`)
- `MessageFramer::ProcessBuffer` 의 buffer-size guard에 multi-first 4 byte 포함 (산술 버그 fix)
- `CryptoManager::DecryptData` 시그니처: `(input, output&) -> bool` — partial record(`WANT_READ` 등)는 `true` 반환하며 output은 0 byte append. 진짜 fatal 에러만 `false`.
- `Session::ProcessLoop` 에서 채널별 plaintext 누적 + LAST/BULK 시 dispatch.

#### 결과

- 동작 중 SSL decrypt failed 사라짐 (사용자 검증, 2026-04-08).
- 장시간 안정성/회귀는 추가 검증 진행 중.

### D/E. Control channel

#### Reference (aasdk + openauto) 사실

- **aasdk `ControlServiceChannel`**: 모든 control channel 메시지의 단일 dispatcher. `messageHandler` switch (`ControlServiceChannel.cpp:189-236`)가 ID별로 handler 호출.
- **aasdk가 receive에서 dispatch하는 메시지 (11종)**:
  - VERSION_RESPONSE → `handleVersionResponse:239` (4-byte big-endian: major/minor/status)
  - ENCAPSULATED_SSL → `onHandshake` (parser 없이 raw payload 전달)
  - SERVICE_DISCOVERY_REQUEST → `handleServiceDiscoveryRequest:254`
  - PING_REQUEST → `handlePingRequest:332`
  - PING_RESPONSE → `handlePingResponse:343`
  - AUDIO_FOCUS_REQUEST → `handleAudioFocusRequest:265` (`AudioFocusRequest` proto 사용)
  - NAV_FOCUS_REQUEST → `handleNavigationFocusRequest:321` (`NavFocusRequestNotification` proto 사용)
  - VOICE_SESSION_NOTIFICATION → `handleVoiceSessionRequest:276`
  - BATTERY_STATUS_NOTIFICATION → `handleBatteryStatusNotification:287`
  - BYEBYE_REQUEST → `handleShutdownRequest:299`
  - BYEBYE_RESPONSE → `handleShutdownResponse:310`
- **aasdk가 send 메서드 가지는 메시지**: VERSION_REQUEST, ENCAPSULATED_SSL, AUTH_COMPLETE, SERVICE_DISCOVERY_RESPONSE, AUDIO_FOCUS_NOTIFICATION, BYEBYE_REQUEST(`sendShutdownRequest`), BYEBYE_RESPONSE(`sendShutdownResponse`), NAV_FOCUS_NOTIFICATION, PING_REQUEST, PING_RESPONSE.
- **VOICE_SESSION_NOTIFICATION 응답 메서드**: `sendVoiceSessionFocusResponse` (`ControlServiceChannel.cpp:147-152`)는 **빈 함수** — 파일에 정의는 있으나 본문이 로그만 출력하고 send 안 함. notification이라 응답이 필요 없는 듯.
- **CHANNEL_OPEN_REQUEST/RESPONSE는 ControlServiceChannel에 없음**. 별도 ServiceChannel base 또는 service 측에서 처리. (확인 필요 — 우리 ServiceBase 모델과 비교 가치)
- **openauto `AndroidAutoEntity`**: aasdk의 ControlServiceChannel event handler 인터페이스를 구현. 각 핸들러:
  - `onVersionResponse:109` — 버전 mismatch면 quit, 아니면 `cryptor_->doHandshake()` 시작
  - `onHandshake:138` — SSL handshake 진행, 완료 시 `sendAuthComplete` 호출
  - `onServiceDiscoveryRequest:172` — 모든 service의 `fillFeatures()`를 호출해 응답 빌드, ping config(tracked_count=5, timeout=3000ms, interval=1000ms, high_latency=200ms) 포함
  - `onAudioFocusRequest:216` — RELEASE→LOSS / 그 외→GAIN
  - `onByeByeRequest:255` — `sendShutdownResponse` 보내고 `triggerQuit()`
  - `onByeByeResponse:268` — `triggerQuit()`
  - `onNavigationFocusRequest:274` — 항상 `NAV_FOCUS_PROJECTED`(=2) 응답
  - `onBatteryStatusNotification:296` — 로깅만
  - `onPingRequest:301` — **응답 안 보냄** (그냥 receive 다시 호출만). HU는 outgoing ping의 받는 입장이지 폰의 ping을 처리할 의무 없음으로 본 듯
  - `onVoiceSessionRequest:306` — 로깅만
  - `onPingResponse:312` — `pinger_->pong()` 호출해 liveness 추적
- **openauto Pinger 모델**: `schedulePing:343` 가 주기적으로 `sendPing:361` 호출, `pinger_->ping(promise)` 의 promise가 timeout에 fail하면 `triggerQuit()`. 즉 liveness check를 outgoing ping ↔ pong rendezvous로 구현.
- **NavFocusType enum** (우리 proto와 동일): `NAV_FOCUS_NATIVE=1`, `NAV_FOCUS_PROJECTED=2`. openauto는 PROJECTED(2) 응답 — "AA가 nav focus 잡고 있음".
- **ByeByeReason enum**: USER_SELECTION=1, DEVICE_SWITCH=2, NOT_SUPPORTED=3, NOT_CURRENTLY_SUPPORTED=4, PROBE_SUPPORTED=5.

#### 우리 코드 사실

- **handshake류 분리**: VERSION_REQUEST/RESPONSE, SSL_HANDSHAKE, AUTH_COMPLETE는 ControlService가 아니라 별도 모듈 `core/src/session/AapHandshaker.cpp`에서 처리.
  - `Run()` (L16): version → SSL → auth_complete 3단계 순차.
  - `DoVersionExchange:70` — version payload `{0,1,0,1}` 즉 v1.1, plain flags(0x03)로 송신, response 도착 시까지 drain.
  - `DoSslHandshake:97` — version 후 500ms sleep, 그 다음 OpenSSL handshake 반복(최대 20회). SSL 메시지를 channel 0 type SSL_HANDSHAKE flags 0x03으로 송수신.
  - `SendAuthComplete:148` — `AuthResponse` proto에 status=0(OK) 채워 SSL_AUTH_COMPLETE flags 0x03으로 송신.
- **CHANNEL_OPEN_REQUEST 처리**: ControlService가 아니라 `ServiceBase::DispatchChannelOpen` (`ServiceBase.cpp:30`)이 처리. 모든 service가 자기 channel의 channel-open을 직접 받아 OnChannelOpened() callback + ChannelOpenResponse(STATUS_SUCCESS) 자동 송신.
  - 즉 aasdk/openauto와 모델이 다름: aasdk는 별도 dispatcher가 channel open만 처리, 우리는 service 자체가 자기 channel open을 처리.
- **ControlService.cpp가 RegisterHandler로 등록한 메시지 (5종)**:
  - SERVICE_DISCOVERY_REQ → `:28` — phone info 콜백 호출 + `SendServiceDiscoveryResponse()`
  - NAV_FOCUS_REQUEST → `:48` — payload **무시**, `SendNavFocusNotification(1)` 호출 (=NAV_FOCUS_NATIVE!)
  - AUDIO_FOCUS_REQUEST → `:51` — `AudioFocusRequestNotification` proto로 파싱(비표준), transient mirror 정책으로 응답
  - PING_REQUEST → `:89` — `PingRequest` 파싱해 timestamp echo로 PING_RESPONSE 송신
  - PING_RESPONSE → `:100` — **no-op** (liveness 추적 없음)
- **HeartbeatLoop**: `:160` 50회 × 100ms = 5초 주기로 SendPing 호출. timeout 검사 없음.
- **`SendServiceDiscoveryResponse:103`**: hu_info 채우고, `set_driver_position(DRIVER_POSITION_LEFT)`, `set_session_configuration(0)`, peer_services 순회하며 channel + service definition 추가. **ping_configuration / display_name / probe_for_support / connection_configuration 등 일체 없음**. openauto는 ping_config를 명시적으로 채움.
- **`SendPing:148`**: timestamp는 `system_clock::now()` 의 milliseconds (openauto는 `high_resolution_clock` microseconds — 둘 다 단조증가 보장 안 되거나 단위 다름).
- **누락된 핸들러 (11종 dispatch에 비해)**:
  - BYEBYE_REQUEST/RESPONSE — graceful disconnect 전파 없음
  - VOICE_SESSION_NOTIFICATION — 무시되어 unhandled 경고 log
  - BATTERY_STATUS_NOTIFICATION — 무시
  - CAR_CONNECTED_DEVICES_REQUEST/RESPONSE — 무시
  - USER_SWITCH_REQUEST/RESPONSE — 무시
  - CALL_AVAILABILITY_STATUS — 무시
  - SERVICE_DISCOVERY_UPDATE — 무시
  - CHANNEL_CLOSE_NOTIFICATION — 무시
- **누락된 send**: BYEBYE_REQUEST 송신 (HU가 graceful shutdown을 트리거할 수 없음).

## 메시지 대조 매트릭스

### Stage 1 — Control channel (D + E)

행 단위는 메시지 ID, 행 폭이 좁아지도록 정책은 짧게 요약하고 상세는 위 카탈로그 참조.

방향 기호: `→` phone→HU, `←` HU→phone.

#### D. Session lifecycle 메시지

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x0001 | VERSION_REQUEST | ← | openauto `AndroidAutoEntity::start:46` | `AapHandshaker.cpp:70` | 4-byte raw (major/minor) | major/minor 양방향 전송, big-endian | `{0,1,0,1}` 즉 v1.1, big-endian | ⚠️ aasdk는 `AASDK_MAJOR/MINOR` 매크로 사용, 우리는 하드코딩 v1.1. **버전이 reference보다 옛 가능성** — aasdk 현재 매크로값 확인 필요 | 확인 후 결정 |
| 0x0002 | VERSION_RESPONSE | → | aasdk `handleVersionResponse:239`, openauto `onVersionResponse:109` | `AapHandshaker.cpp:84` (DrainUntil) | 6-byte raw (major/minor/status) | status가 `STATUS_NO_COMPATIBLE_VERSION`이면 quit | status 검사 없음, drain만 | 🔧 status 무시 — 폰이 incompat 응답 보내도 우리는 그냥 SSL 진행 → 에러는 그 후 단계에서 | status 파싱 + incompat 시 abort |
| 0x0003 | SSL_HANDSHAKE | ↔ | aasdk dispatch `:202` → openauto `onHandshake:138` | `AapHandshaker.cpp:97` (DoSslHandshake) | raw SSL bytes | OpenSSL `doHandshake` 반복, 완료 시 sendAuthComplete | 동일 모델, 20회 재시도 + 500ms 사전 sleep | ✅ 일치, ⚠️ 우리는 사전 500ms sleep 추가 (일부 폰 호환성) | 주석으로 sleep 이유 명시 |
| 0x0004 | AUTH_COMPLETE | ← | openauto `onHandshake:155` (sendAuthComplete) | `AapHandshaker.cpp:148` | `AuthResponse` (status) | status=SUCCESS | status=0 (=SUCCESS) | ✅ | 없음 |
| 0x0005 | SERVICE_DISCOVERY_REQUEST | → | aasdk `:254`, openauto `onServiceDiscoveryRequest:172` | `ControlService.cpp:28` | `ServiceDiscoveryRequest` | request 파싱(`device_name`/`label_text`), 모든 서비스 `fillFeatures()` 호출, ping_config 포함 응답 | request 파싱(`device_name`/`label_text`/`phone_info`), peer_services 순회 응답 | ➕ 우리는 `phone_info.instance_id`/`lifetime_id`까지 추출(openauto는 안 함) — 멀티 디바이스용. 단 응답에 ping_config/display_name/probe_for_support 누락 | ping_configuration 채우기, display_name/probe_for_support 검토 |
| 0x0006 | SERVICE_DISCOVERY_RESPONSE | ← | openauto `onServiceDiscoveryRequest:178-212` (build), aasdk `sendServiceDiscoveryResponse:78` | `ControlService.cpp:103` | `ServiceDiscoveryResponse` | hu_info + driver_position + display_name + ping_config + connection_configuration + 모든 service channels | hu_info + driver_position(LEFT) + session_configuration(0) + service channels. **ping_config 등 누락** | ⚠️ 필수 필드 누락 — 일부 폰이 ping_config 없으면 자체 default를 사용하거나 timeout이 어긋날 수 있음 | ping_configuration 추가 (tracked=5, timeout=3000ms, interval=1000ms, high_latency=200ms) |
| 0x0007 | CHANNEL_OPEN_REQUEST | → | aasdk는 ControlServiceChannel이 처리하지 않음 — 별도 ServiceChannel base에서 (아직 미확인) | `ServiceBase.cpp:30` `DispatchChannelOpen` | `ChannelOpenRequest` | (확인 필요) | 모든 service가 자기 channel의 open을 직접 수신, OnChannelOpened 콜백 + 자동 응답 | ➕ 의도적 단순화 — service-local 처리. aasdk와 모델은 다르지만 결과는 같음 | aasdk의 channel open 처리 위치 확인 후 동등성 검증 |
| 0x0008 | CHANNEL_OPEN_RESPONSE | ← | (확인 필요) | `ServiceBase.cpp:36-44` | `ChannelOpenResponse` | (확인 필요) | status=STATUS_SUCCESS 자동 응답 | ➕ 자동 OK 응답 | 동등성 검증 |
| 0x0009 | CHANNEL_CLOSE_NOTIFICATION | ← | (양쪽 모두 미사용) | (없음) | `ChannelCloseNotification` | aasdk dispatch에 없음, openauto 핸들러도 없음 | 우리도 없음 | ✅ 양쪽 모두 미사용 일치 | 없음 |
| 0x000F | BYEBYE_REQUEST | ↔ | aasdk `handleShutdownRequest:299`, openauto `onByeByeRequest:255` | (없음 — `ServiceBase`의 unhandled 경고) | `ByeByeRequest` (reason) | 폰이 보내면 `sendShutdownResponse` + `triggerQuit`. HU도 능동 송신 가능 (`sendShutdownRequest`) | 미구현 — 무시 | ❌ 누락. 폰이 ByeBye 보내면 우리 세션이 정리 안 되고 transport 끊김으로만 정리됨 | ByeByeRequest 핸들러 추가 (response 송신 + Session::Stop 트리거) |
| 0x0010 | BYEBYE_RESPONSE | ↔ | aasdk `handleShutdownResponse:310`, openauto `onByeByeResponse:268` | (없음) | `ByeByeResponse` | 우리가 BYEBYE_REQUEST를 보내고 응답 받으면 quit | 미구현 (송신 자체가 없으므로) | ❌ 누락 (BYEBYE_REQUEST 송신과 짝) | BYEBYE 송신 경로 추가 시 함께 |
| 0x001A | SERVICE_DISCOVERY_UPDATE | → | (양쪽 모두 미사용 — aasdk dispatch에도 없음) | (없음) | `ServiceDiscoveryUpdate` | 미사용 | 미사용 | ✅ 일치 | 없음 |

#### E. Control channel services

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x000B | PING_REQUEST | ↔ | aasdk `handlePingRequest:332`, openauto `onPingRequest:301` | `ControlService.cpp:89` (수신), `:148` (송신, HeartbeatLoop) | `PingRequest` (timestamp) | **HU도 폰의 ping에 응답하지 않음** (openauto는 logging만, sendPingResponse 호출 X). HU 자체가 5초 주기 outgoing ping. timestamp 단위 microseconds (high_res_clock). | 폰의 PING_REQUEST에 PING_RESPONSE echo. HU는 5초 주기 outgoing ping. timestamp 단위 milliseconds (system_clock). | ⚠️ openauto는 폰 ping에 응답 안 함 / 우리는 응답함 — 우리가 더 친절. 단, system_clock은 wall-clock(NTP 점프 영향) — `steady_clock`이 더 적합 | timestamp clock을 `steady_clock`로 변경 (마이너) |
| 0x000C | PING_RESPONSE | ↔ | aasdk `handlePingResponse:343`, openauto `onPingResponse:312` | `ControlService.cpp:100` | `PingResponse` (timestamp) | `pinger_->pong()` 호출 — outgoing ping의 promise를 resolve하여 liveness 추적. 응답 없으면 timeout fail → triggerQuit | **no-op** — 응답을 받든 안 받든 무시 | 🔧 liveness 추적 없음 — 폰이 죽어도 우리는 계속 ping만 보내고 transport 끊김으로만 감지 | ping/pong rendezvous 추가 — 응답 timeout 시 session 종료 트리거 |
| 0x000D | NAV_FOCUS_REQUEST | → | aasdk `handleNavigationFocusRequest:321` (`NavFocusRequestNotification` 파싱), openauto `onNavigationFocusRequest:274` | `ControlService.cpp:48` | `NavFocusRequestNotification` (focus_type 필드) | request 파싱(현재 무시), 항상 `NAV_FOCUS_PROJECTED`(=2) 응답 — "AA가 nav focus 잡고 있음" | **payload 파싱 안 함**, 항상 `NAV_FOCUS_NATIVE`(=1) 응답 — "HU가 nav focus 잡고 있음" | 🔧 **잘못된 응답값**! NATIVE는 HU 자체 navigation을 의미. AA navigation을 화면에 띄우려면 PROJECTED여야 함. 폰이 안내음/안내 화면을 띄우는 경로에 영향 가능 | `SendNavFocusNotification(2)` (PROJECTED)로 변경. 인자 hardcode가 아니라 enum 사용 권고 |
| 0x000E | NAV_FOCUS_NOTIFICATION | ← | openauto `:285` (`sendNavigationFocusResponse`) | `ControlService.cpp:168` `SendNavFocusNotification` | `NavFocusNotification` (focus_type) | NAV_FOCUS_PROJECTED 송신 | NAV_FOCUS_NATIVE(int=1) 송신 | 🔧 위와 동일 | 위와 함께 |
| 0x0011 | VOICE_SESSION_NOTIFICATION | → | aasdk `handleVoiceSessionRequest:276`, openauto `onVoiceSessionRequest:306` (logging만) | (없음 — unhandled) | `VoiceSessionNotification` (status) | 폰의 음성 세션 시작/종료 알림. openauto는 logging만 — 후속 동작 없음 | 무시 → unhandled 경고 | ⚠️ 양쪽 모두 본격 처리 안 하지만, 우리는 핸들러 자체가 없어서 매번 경고 log | 빈 핸들러 등록(=의도적 무시) — 경고 제거 |
| 0x0012 | AUDIO_FOCUS_REQUEST | → | aasdk `handleAudioFocusRequest:265` (`AudioFocusRequest` 파싱), openauto `onAudioFocusRequest:216` | `ControlService.cpp:51` | `AudioFocusRequest` (aasdk) / `AudioFocusRequestNotification` (우리) — wire-compat | RELEASE→LOSS / 그 외→GAIN (일괄) | RELEASE→LOSS / GAIN_TRANSIENT→GAIN_TRANSIENT / MAY_DUCK→GAIN_TRANSIENT_GUIDANCE_ONLY / 나머지→GAIN (mirror) | ➕ 정책은 우리가 더 정교 + 🔧 proto 이름 비표준 | proto를 `AudioFocusRequest`로 통일, `AudioFocusRequestNotification.proto` 삭제 |
| 0x0013 | AUDIO_FOCUS_NOTIFICATION | ← | openauto `:245` (`sendAudioFocusResponse`) | `ControlService.cpp:81-87` | `AudioFocusNotification` (focus_state, unsolicited) | response.set_focus_state, unsolicited 미설정(=false 기본) | 위와 동일 (`unsolicited=false` 명시 set) | ✅ 일치 | 없음 |
| 0x0014 | CAR_CONNECTED_DEVICES_REQUEST | → | (양쪽 모두 미사용 — aasdk dispatch에도 없음) | (없음) | `CarConnectedDevicesRequest` | 미사용 | 미사용 | ✅ 일치 | 없음 (PARKED) |
| 0x0015 | CAR_CONNECTED_DEVICES_RESPONSE | ← | (미사용) | (없음) | `CarConnectedDevices` | 미사용 | 미사용 | ✅ | 없음 (PARKED) |
| 0x0016 | USER_SWITCH_REQUEST | → | (미사용) | (없음) | `UserSwitchRequest` | 미사용 | 미사용 | ✅ | 없음 (PARKED) |
| 0x0017 | BATTERY_STATUS_NOTIFICATION | → | aasdk `handleBatteryStatusNotification:287`, openauto `onBatteryStatusNotification:296` (logging만) | (없음 — unhandled) | `BatteryStatusNotification` | logging만, UI에 노출 안 함 | 무시 → unhandled 경고 | ⚠️ 양쪽 모두 본격 처리 안 함, 우리는 핸들러 부재 | 빈 핸들러 등록(=의도적 무시) — 경고 제거. 미래에 UI 전달 시 확장 |
| 0x0018 | CALL_AVAILABILITY_STATUS | → | (미사용 — aasdk dispatch에도 없음) | (없음) | `CallAvailabilityStatus` | 미사용 | 미사용 | ✅ | 없음 (PARKED) |
| 0x0019 | USER_SWITCH_RESPONSE | ← | (미사용) | (없음) | `UserSwitchResponse` | 미사용 | 미사용 | ✅ | 없음 (PARKED) |

#### Stage 1 종합 action 우선순위

**즉시 수정 (🔧)**
1. **NAV_FOCUS 응답값을 `NAV_FOCUS_PROJECTED`(=2)로 변경** — 현재 NATIVE는 잘못된 시맨틱. 폰의 navigation 안내가 영향받을 수 있음.
2. **PING_RESPONSE liveness 추적 추가** — 폰이 죽어도 ping timeout으로 감지하지 못함. session 자동 종료 경로가 transport 끊김에만 의존.
3. **VERSION_RESPONSE의 status 검증** — 현재 status 무시. incompat 폰 연결 시 SSL 단계에서야 fail함.

**정리/표준화 (🔧 + ➕ 혼합)**
4. AUDIO_FOCUS_REQUEST proto를 `AudioFocusRequest`로 통일, `AudioFocusRequestNotification.proto` 삭제. 정책 mirror는 유지.

**누락 추가 (❌)**
5. **BYEBYE_REQUEST 핸들러 추가** — 폰 측 graceful disconnect 처리.
6. (선택) BYEBYE_REQUEST 송신 경로 추가 — HU 측 graceful shutdown.

**소소한 정리 (⚠️)**
7. SERVICE_DISCOVERY_RESPONSE에 `ping_configuration` 채우기 (tracked=5, timeout=3000ms, interval=1000ms, high_latency=200ms).
8. PING timestamp clock을 `system_clock`(wall) → `steady_clock`(monotonic)로 변경.
9. VOICE_SESSION_NOTIFICATION, BATTERY_STATUS_NOTIFICATION에 빈 핸들러 등록 (의도적 무시 명시 + unhandled 경고 제거).
10. VERSION 매크로 — 우리 v1.1 하드코딩 vs aasdk `AASDK_MAJOR/MINOR` 비교 필요. 현재 aasdk Version.hpp 값 확인 필요.
11. NAV_FOCUS_REQUEST 핸들러에서 payload(focus_type) 파싱 (현재 무시) — 미래에 폰이 NATIVE를 요구할 때 처리할 수 있도록.

**보류**
- USER_SWITCH, CAR_CONNECTED_DEVICES, CALL_AVAILABILITY_STATUS 등 reference도 미사용인 메시지들.
- CHANNEL_OPEN의 aasdk 측 처리 위치 확인 — Stage 2 직전에.

### Stage 2 — Media services (F)

#### Reference (aasdk + openauto) 사실 — F. Media

- **aasdk media sink 모델**: Audio와 Video는 거의 동일한 channel base에 video focus만 추가. Mic는 별도 `MediaSource/Audio/MicrophoneAudioChannel`.
- **aasdk `AudioMediaSinkService::messageHandler`** (`AudioMediaSinkService.cpp:91`): switch dispatch — CHANNEL_OPEN_REQUEST / SETUP / START / STOP / CODEC_CONFIG (raw onMediaIndication) / DATA (handleMediaWithTimestampIndication).
- **aasdk `VideoMediaSinkService::messageHandler`** (`VideoMediaSinkService.cpp:103`): 위 + VIDEO_FOCUS_REQUEST.
- **aasdk timestamp 처리** (`handleMediaWithTimestampIndication`): MEDIA_DATA payload의 첫 8바이트가 `Timestamp` (microseconds), 나머지가 raw audio/video bytes. **audio와 video 양쪽 동일하게 8바이트 떼어 timestamp + buffer로 분리해 sink로 전달**.
- **aasdk MEDIA_MESSAGE enum 값** (`MediaMessageId.proto`):
  - DATA=0, CODEC_CONFIG=1, SETUP=32768(0x8000), START=32769(0x8001), STOP=32770(0x8002), CONFIG=32771(0x8003), ACK=32772(0x8004)
  - **MICROPHONE_REQUEST=32773(0x8005), MICROPHONE_RESPONSE=32774(0x8006)**
  - VIDEO_FOCUS_REQUEST=32775(0x8007), VIDEO_FOCUS_NOTIFICATION=32776(0x8008)
  - **UPDATE_UI_CONFIG_REQUEST=32777(0x8009), UPDATE_UI_CONFIG_REPLY=32778(0x800A), AUDIO_UNDERFLOW_NOTIFICATION=32779(0x800B)**
- **aasdk send 메서드**:
  - Audio: sendChannelOpenResponse, sendChannelSetupResponse(`Config`), sendMediaAckIndication(`Ack`)
  - Video: 위 + sendVideoFocusIndication(`VideoFocusNotification`)
- **aasdk Audio sink는 `onMediaIndication`(timestamp 없음)을 받지만 dispatch는 MEDIA_MESSAGE_CODEC_CONFIG에서만 호출** — 즉 audio도 CODEC_CONFIG를 받을 수 있음(PCM이 아닌 codec일 수 있도록).
- **openauto MediaAudioService/GuidanceAudioService/SystemAudioService/TelephonyAudioService**: 각각 별도 stream type. (자세한 정책은 아직 미확인 — 필요 시 다음 단계에서)

#### 우리 코드 사실 — F. Media

- **우리 message ID 정의** (`AapProtocol.hpp::msg`):
  - MEDIA_DATA=0x0000, MEDIA_CODEC_CONFIG=0x0001 ✅
  - MEDIA_SETUP=0x8000, MEDIA_START=0x8001, MEDIA_STOP=0x8002, MEDIA_CONFIG=0x8003, MEDIA_ACK=0x8004 ✅
  - VIDEO_FOCUS_REQUEST=0x8007, VIDEO_FOCUS_NOTIFICATION=0x8008 ✅
  - **MIC_REQUEST=0x800A, MIC_RESPONSE=0x800B** 🔧 **잘못된 값** (표준은 0x8005/0x8006)
  - **누락**: 0x8005/0x8006(microphone), 0x8009(UPDATE_UI_CONFIG_REQUEST), 0x800A(UPDATE_UI_CONFIG_REPLY), 0x800B(AUDIO_UNDERFLOW_NOTIFICATION)
- **AudioService.cpp**: stream_type/sample_rate/channels/name 매개변수화. PCM 16-bit hardcode.
  - 핸들러: MEDIA_DATA, MEDIA_SETUP, MEDIA_START, MEDIA_STOP, MEDIA_ACK
  - **MEDIA_CODEC_CONFIG 핸들러 등록 안 함** — PCM이라 보통 안 오지만 unhandled 경고는 발생할 수 있음.
  - MEDIA_DATA 처리: ack 먼저 송신 → 8-byte timestamp prefix(`kAudioTimestampBytes`) 떼어 `pts_us` 추출 → sink->OnAudioData(data, size, pts_us). ✅ aasdk와 일치.
  - HandleSetupRequest: Config(STATUS_READY, max_unacked=4, configuration_indices=[0]) 응답.
  - SetSink: 캐시된 format을 sink에 OnAudioFormat으로 replay.
- **VideoService.cpp**: 단일 인스턴스, config_의 width/height 사용. fps=30 hardcode.
  - 핸들러: MEDIA_CODEC_CONFIG, MEDIA_DATA, MEDIA_SETUP, MEDIA_START, MEDIA_STOP, VIDEO_FOCUS_REQUEST, MEDIA_ACK
  - MEDIA_CODEC_CONFIG: payload(SPS/PPS H264 raw)를 codec_data로 캐시, sink->OnVideoConfig 호출, ack 송신.
  - MEDIA_DATA 처리: sink->OnVideoFrame(payload, size, pts_us=**0**, is_keyframe=**false**), ack. **🔧 timestamp prefix 안 떼어냄!** aasdk는 audio도 video도 모두 8 byte timestamp 분리. 우리는 video frame의 timestamp가 항상 0 → sink가 디코더로 PTS를 0으로 넘기면 디코더가 모든 frame을 같은 시각에 표출하려 할 수 있음. + raw payload에 timestamp 8 byte가 그대로 포함된 채 H.264 NAL unit으로 전달 → **잘못된 NAL bytes** → 디코더가 계속 SPS/PPS 재요구하거나 첫 keyframe까지 느려질 수 있음.
  - VIDEO_FOCUS_REQUEST: payload 파싱 후 **로그만**. 응답 없음. 폰이 이 메시지를 보냈을 때 우리가 응답을 안 주면 폰이 다음 단계로 못 갈 수 있음.
  - SendVideoFocusGain: VIDEO_FOCUS_PROJECTED, unsolicited=**false**. ⚠️ unsolicited 의미 — 폰의 요청 없이 HU가 능동적으로 보낸 거면 `true`가 더 정확. SetSink 시점에 발송되는 게 unsolicited.
  - SendVideoFocusLoss: VIDEO_FOCUS_NATIVE, unsolicited=false.
  - HandleSetupRequest: Config(STATUS_READY, max_unacked=10) → 즉시 SendVideoFocusLoss 호출. 폰의 stream 송출 보류 → SetSink 호출 시 SendVideoFocusGain.
  - FillServiceDefinition: VIDEO_1280x720 hardcode, FPS_30, density는 config_에서.
- **MicrophoneService.cpp**:
  - 핸들러: MIC_REQUEST(=잘못된 0x800A) → log만, 실제 audio capture 미구현
  - MIC_RESPONSE 송신 메서드 없음
  - FillServiceDefinition: PCM 16kHz 16bit mono.
  - **현재 상태**: message ID 잘못된 데다 capture 미구현이라 마이크 기능 자체가 작동 안 하는 dead code.

#### Stage 2 매트릭스

#### F-1. Audio (per-stream channels: AM, AG, AS)

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x0007 | CHANNEL_OPEN_REQUEST (audio ch) | → | aasdk `AudioMediaSinkService::handleChannelOpenRequest:125` | `ServiceBase.cpp:30` `DispatchChannelOpen` (공통) | `ChannelOpenRequest` | service-local 파싱, eventHandler->onChannelOpenRequest | service-local 파싱, OnChannelOpened 콜백 + 자동 OK 응답 | ✅ **모델 일치** (Stage 1 미해결 항목 — aasdk도 service-local 처리) | 없음 |
| 0x8000 | MEDIA_SETUP | → | aasdk `handleChannelSetupRequest:136` | `AudioService.cpp:67,106` | `Setup` (type) | service-local 파싱 → eventHandler에 위임 | type 로깅, Config(STATUS_READY, max_unacked=4, indices=[0]) 즉시 응답 | ✅ 응답 wire format 일치, 정책 단순함 | 없음 |
| 0x8001 | MEDIA_START | → | aasdk `handleStartIndication:147` | `AudioService.cpp:68` | `Start` (session_id, configuration_index) | indication만 전달 | session_id 캐시, 카운터 리셋 | ✅ | 없음 |
| 0x8002 | MEDIA_STOP | → | aasdk `handleStopIndication:158` | `AudioService.cpp:77` | `Stop` | indication만 전달 | log only | ✅ | 없음 |
| 0x8003 | MEDIA_CONFIG | ← | aasdk `sendChannelSetupResponse` | `AudioService.cpp:117-121` | `Config` (status, max_unacked, configuration_indices) | reference도 동일 wire | 동일 | ✅ | 없음 |
| 0x8004 | MEDIA_ACK | ↔ | aasdk `sendMediaAckIndication`, dispatch는 audio side에서 안 받음 | `AudioService.cpp:46-49` (송신), `:81` (수신 no-op) | `Ack` (session_id, ack=1) | aasdk Audio sink는 ack 송신만 (MEDIA_DATA 처리 후 자동) | 동일 — MEDIA_DATA 받자마자 ack 송신 | ✅ | 없음 |
| 0x0001 | MEDIA_CODEC_CONFIG | → | aasdk `messageHandler:112` → `onMediaIndication` (raw payload 전달) | (없음) | (raw bytes) | aasdk Audio sink는 CODEC_CONFIG dispatch 가지지만 PCM에선 보통 안 옴 | 핸들러 없음 → unhandled 경고 | ⚠️ PCM이라 동작은 OK, unhandled 경고만 log 오염 | 빈 핸들러 등록(=의도적 무시) |
| 0x0000 | MEDIA_DATA (audio) | → | aasdk `handleMediaWithTimestampIndication:169` | `AudioService.cpp:35-66` | (8-byte timestamp prefix + PCM bytes) | timestamp 분리 후 buffer 전달 | 동일 — 8-byte timestamp 분리, ack 먼저 송신, sink 콜백 | ✅ | 없음 |

#### F-2. Video (channel V)

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x0007 | CHANNEL_OPEN_REQUEST (video) | → | aasdk `VideoMediaSinkService::handleChannelOpenRequest:171` | `ServiceBase.cpp:30` (공통) | `ChannelOpenRequest` | 동일 | 동일 | ✅ | 없음 |
| 0x8000 | MEDIA_SETUP (video) | → | aasdk `handleChannelSetupRequest:138` | `VideoService.cpp:34,125` | `Setup` (type) | indication만 전달 | Config(max_unacked=10) 응답 + 즉시 SendVideoFocusLoss → SetSink 시점에 Gain | ➕ 우리는 sink-driven focus 모델 — 의도적, 합리적 | 주석 명시 |
| 0x8001 | MEDIA_START (video) | → | aasdk `handleStartIndication:149` | `VideoService.cpp:35,147` | `Start` | indication만 전달 | session_id 캐시 | ✅ | 없음 |
| 0x8002 | MEDIA_STOP (video) | → | aasdk `handleStopIndication:160` | `VideoService.cpp:36` | `Stop` | indication만 전달 | log only | ✅ | 없음 |
| 0x8003 | MEDIA_CONFIG (video) | ← | aasdk `sendChannelSetupResponse` | `VideoService.cpp:131-140` | `Config` | reference 동일 wire | 동일 | ✅ | 없음 |
| 0x8004 | MEDIA_ACK (video) | ← | aasdk `sendMediaAckIndication` | `VideoService.cpp:181` (송신 only) | `Ack` | reference 동일 | MEDIA_CODEC_CONFIG와 MEDIA_DATA 받을 때마다 ack | ✅ | 없음 |
| 0x0001 | MEDIA_CODEC_CONFIG (video) | → | aasdk `messageHandler:122` → `onMediaIndication` (raw payload 전달, 파싱 없음) | `VideoService.cpp:32,83` | (raw H264 SPS/PPS bytes) | raw payload sink로 전달 | payload 캐시 → sink->OnVideoConfig + ack | ✅ 일치 + ➕ 캐시(SetSink replay용) | 없음 |
| 0x0000 | MEDIA_DATA (video) | → | aasdk `handleMediaWithTimestampIndication:182` | `VideoService.cpp:33,100` | **(8-byte timestamp prefix + H.264 NAL bytes)** | **timestamp 8 byte 분리, buffer만 sink로** | **timestamp 분리 안 함! payload 그대로 전달, pts_us=0 hardcode, is_keyframe=false hardcode** | 🔧 **잠재 critical bug** — H.264 디코더에 timestamp 8 byte가 NAL prefix로 섞여 들어감. 디코더가 garbage byte를 만나 SPS 재동기까지 frame drop, 또는 첫 keyframe까지 검은 화면 길어질 수 있음. PTS=0이면 모든 frame을 동시각으로 처리 → 디코더 wallclock 동작 불일치. | **즉시 수정**: `AudioService`처럼 8-byte timestamp prefix 떼고 sink로 전달, pts 추출, 가능하면 keyframe 검출(NAL type 5) |
| 0x8007 | VIDEO_FOCUS_REQUEST | → | aasdk `handleVideoFocusRequest:195` (`VideoFocusRequestNotification` 파싱) | `VideoService.cpp:42` | `VideoFocusRequestNotification` (mode, reason) | indication만 전달, openauto는 별도 응답 정책 | 파싱 후 **로그만, 응답 없음** | 🔧 응답 누락 — 폰이 일부 시나리오에서 video stream을 보류할 수 있음 (sink-driven 모델로 가려져 있을 수도) | VIDEO_FOCUS_REQUEST 핸들러에서 mode에 맞춰 VIDEO_FOCUS_NOTIFICATION 응답 |
| 0x8008 | VIDEO_FOCUS_NOTIFICATION | ← | aasdk `sendVideoFocusIndication` | `VideoService.cpp:157,169` `SendVideoFocusGain/Loss` | `VideoFocusNotification` (focus, unsolicited) | reference 동일 wire | PROJECTED/NATIVE, **`unsolicited=false`** 둘 다 | ⚠️ SetSink 시점 송신은 폰 요청 없이 보내는 거라 `unsolicited=true`가 의미적으로 정확. VIDEO_FOCUS_REQUEST 응답 시에만 false. | unsolicited 값 분리 |

#### F-3. Microphone (channel direction reversed: HU is source)

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| **0x8005** | **MICROPHONE_REQUEST** (표준) | → | aasdk MediaSource Audio channel | `MicrophoneService.cpp:13` 가 **0x800A** 로 등록 | (1 byte: open/close) | mic open/close indication | log only, 실제 capture 미구현 | 🔧 **CRITICAL — 잘못된 message ID**. 우리 0x800A는 표준에서 `MEDIA_MESSAGE_UPDATE_UI_CONFIG_REPLY`임. 폰이 mic request를 보내도 우리 service가 수신 못 함. 마이크 기능 자체가 dead. | **즉시 수정**: `AapProtocol.hpp::msg::MIC_REQUEST = 0x8005` 로 변경 |
| **0x8006** | **MICROPHONE_RESPONSE** (표준) | ← | aasdk MediaSource Audio | (없음 — 송신 미구현) | (status) | mic open/close 응답 | 송신 안 함 | 🔧 우리 정의는 0x800B (=AUDIO_UNDERFLOW_NOTIFICATION). 송신 시 폰이 다른 메시지로 해석. 미구현이라 영향 X. | `MIC_RESPONSE = 0x8006` 로 수정 |
| 0x0000 | MEDIA_DATA (mic, HU→phone) | ← | aasdk MediaSource | (없음) | (timestamp prefix + PCM) | HU가 captured PCM을 phone에 송신 | 미구현 | ❌ 누락 (마이크 기능 전체 미구현) | 마이크 기능이 필요해질 때 구현 |

#### F-4. 우리 protocol 정의 누락 (Media)

aasdk MediaMessageId enum의 다음이 우리 `AapProtocol.hpp::msg`에 없거나 잘못 매핑됨:

| aasdk 상수 | 표준 값 | 우리 정의 | 상태 |
|---|---|---|---|
| MICROPHONE_REQUEST | 0x8005 | (없음) | ❌ 추가 필요 |
| MICROPHONE_RESPONSE | 0x8006 | (없음) | ❌ 추가 필요 |
| UPDATE_UI_CONFIG_REQUEST | 0x8009 | (없음) | ❌ 추가 (또는 PARKED — UI config는 미사용 가능성) |
| UPDATE_UI_CONFIG_REPLY | 0x800A | **MIC_REQUEST에 충돌!** | 🔧 ID 충돌 해결 |
| AUDIO_UNDERFLOW_NOTIFICATION | 0x800B | **MIC_RESPONSE에 충돌!** | 🔧 ID 충돌 해결 |

#### Stage 2 종합 action 우선순위

**즉시 수정 (🔧, critical)**
1. **VIDEO MEDIA_DATA의 8-byte timestamp prefix 분리 누락** (`VideoService.cpp:100`) — 디코더에 garbage byte 주입 가능성. AudioService와 동일한 패턴으로 수정. 동시에 pts_us 추출 + (선택) keyframe 검출.
2. **MIC_REQUEST/RESPONSE message ID 잘못됨** — `AapProtocol.hpp` 의 `MIC_REQUEST = 0x8005`, `MIC_RESPONSE = 0x8006` 으로 수정. 마이크 기능 자체는 아직 구현 안 했지만, ID가 다른 메시지(UPDATE_UI_CONFIG_REPLY/AUDIO_UNDERFLOW_NOTIFICATION)와 충돌해 있어 미래에 어느 한 쪽이라도 사용하면 즉시 버그.
3. **VIDEO_FOCUS_REQUEST 응답 누락** (`VideoService.cpp:42`) — 폰의 명시적 요청에 응답 없음. 현재 sink-driven 모델이 가리고 있을 수 있지만 표준 준수 차원에서 응답 보내기.

**누락 (❌)**
4. `AUDIO_UNDERFLOW_NOTIFICATION` (0x800B) 처리 — phone이 audio underflow를 알릴 수 있음. 현재 수신 핸들러 없음. 빈 핸들러로라도 등록.
5. (선택) `UPDATE_UI_CONFIG_REQUEST/REPLY` — UI config 협상. reference도 사용 빈도 낮음, PARKED 가능.

**표준화/정리 (⚠️)**
6. `VIDEO_FOCUS_NOTIFICATION`의 `unsolicited` 값 분리 — request 응답일 때 false, SetSink 트리거일 때 true.
7. AudioService에 MEDIA_CODEC_CONFIG 빈 핸들러 등록 (PCM에선 안 오지만 unhandled 경고 방지).

**보존 (➕)**
8. `VideoService::SetSink` → SendVideoFocusGain 트리거 (sink-driven focus 모델) — 의도적 단순화. 주석으로 명시.
9. `VideoService::HandleCodecConfig`의 codec_data 캐시 (SetSink 시 replay) — sink hot-swap 지원하기 위함.
10. `AudioService::SetSink` → format replay — 동일.

### Stage 3 — Input + Sensor (G + H)

#### Reference (aasdk + openauto) 사실 — G/H

- **aasdk `InputSourceService::messageHandler`** (`InputSourceService.cpp:84`): switch dispatch — KEY_BINDING_REQUEST, CHANNEL_OPEN_REQUEST. INPUT_REPORT는 dispatch에 **없음** (HU→phone outbound only).
- **aasdk InputMessageId enum**: INPUT_REPORT=32769(0x8001), KEY_BINDING_REQUEST=32770(0x8002), KEY_BINDING_RESPONSE=32771(0x8003), INPUT_FEEDBACK=32772(0x8004).
- **aasdk `SensorSourceService::messageHandler`** (`SensorSourceService.cpp:57`): switch dispatch — SENSOR_REQUEST, CHANNEL_OPEN_REQUEST.
- **aasdk SensorMessageId enum**: REQUEST=32769(0x8001), RESPONSE=32770(0x8002), BATCH=32771(0x8003), ERROR=32772(0x8004).
- **aasdk send 메서드**:
  - Input: sendInputReport(`InputReport`), sendKeyBindingResponse(`KeyBindingResponse`), sendChannelOpenResponse
  - Sensor: sendSensorEventIndication(`SensorBatch`), sendSensorStartResponse(`SensorStartResponseMessage`), sendChannelOpenResponse
- **aasdk InputReport는 `KeyBindingRequest` proto의 `keycodes` 배열을 사용** — 즉 폰이 channel open 후 KeyBindingRequest로 어떤 keycode를 처리할지 알리고, HU는 KeyBindingResponse로 ACK.
- **aasdk Sensor의 SensorRequest 모델**: 폰이 type별로 sensor 시작 요청. HU는 type별로 시작/거부 응답.

#### 우리 코드 사실 — G/H

- **우리 message ID 정의**:
  - **Input**: INPUT_EVENT=0x8001 (=표준 INPUT_REPORT), INPUT_BINDING_REQUEST=0x8002 (=KEY_BINDING_REQUEST), INPUT_BINDING_RESPONSE=0x8003 (=KEY_BINDING_RESPONSE) — **값은 모두 일치**, 이름만 우리 식. INPUT_FEEDBACK(0x8004)는 누락.
  - **Sensor**: SENSOR_START_REQUEST=0x8001 (=REQUEST), SENSOR_START_RESPONSE=0x8002 (=RESPONSE), SENSOR_EVENT=0x8003 (=BATCH) — **값은 모두 일치**. SENSOR_ERROR(0x8004) 누락.
- **InputService.cpp**:
  - 핸들러: `INPUT_BINDING_REQUEST`, `INPUT_EVENT`
  - **INPUT_EVENT 핸들러는 dead code**: INPUT_EVENT는 HU→phone outbound 메시지. 폰이 보내는 게 아님. 우리가 RegisterHandler 한 게 무의미. aasdk dispatch에도 없음.
  - HandleBindingRequest: KeyBindingRequest 파싱(keycodes 개수 로깅) → KeyBindingResponse(STATUS_SUCCESS) 즉시 응답.
  - SendTouchEvent: InputReport 빌드 (timestamp microseconds via steady_clock, single pointer만 add_pointer_data — multi-touch 표현 불가능). action을 PointerAction enum으로 cast — Android MotionEvent action 값과 PointerAction enum 값이 같지 않을 수 있음 (확인 필요).
  - **SendKeyEvent 메서드 없음** — 키패드/스티어링 휠 키 이벤트 송신 기능 미구현.
  - FillServiceDefinition: keycodes_supported = `{3, 4, 19, 20, 21, 22, 23, 66, 84, 85, 87, 88, 5, 6}` — **Android keycode 값들**(HOME=3, BACK=4, DPAD_*=19~23, ENTER=66, SEARCH=84, MEDIA_*=85/87/88, CALL=5, ENDCALL=6). AAP는 자체 keycode enum을 정의하는 표준이므로 Android keycode를 그대로 광고하면 폰이 잘못 해석할 가능성. 단, Android Auto는 사실 Android keycode를 그대로 받기로 알려진 부분도 있음 — 검증 필요.
  - touch screen 광고: width/height/CAPACITIVE/is_secondary=false. ✅
- **SensorService.cpp**:
  - 핸들러: `SENSOR_START_REQUEST`만
  - HandleSensorStartRequest: SensorRequest 파싱 → **type에 무관하게** SensorResponse(STATUS_SUCCESS) 응답 + 즉시 SendDrivingStatus 호출. 즉 폰이 NIGHT_MODE 시작을 요청해도 우리는 driving_status_data를 보냄.
  - OnChannelOpened: SendDrivingStatus 한 번 송신.
  - SendDrivingStatus: SensorBatch에 `driving_status_data {status: 0}` (UNRESTRICTED) 한 개. 항상 같은 값.
  - **NIGHT_MODE/LOCATION 데이터는 송신 안 함** (광고만 함).
  - FillServiceDefinition: DRIVING_STATUS_DATA, NIGHT_MODE, LOCATION 세 sensor type 광고.

#### Stage 3 매트릭스

#### G. Input service (channel I)

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x0007 | CHANNEL_OPEN_REQUEST (input) | → | aasdk `handleChannelOpenRequest:116` | `ServiceBase.cpp:30` (공통) | `ChannelOpenRequest` | service-local 처리 | 동일 | ✅ | 없음 |
| 0x8001 | INPUT_EVENT (=INPUT_REPORT) | ← | aasdk `sendInputReport:44` | `InputService.cpp:44-65` `SendTouchEvent` | `InputReport` (timestamp + touch_event/key_event) | reference도 동일 wire | timestamp microseconds (steady_clock), single pointer | ✅ wire ID 일치 / ⚠️ multi-touch 미지원 (`add_pointer_data` 1회만) | multi-touch 지원 추가 (필요 시) |
| 0x8001 | INPUT_EVENT 수신 핸들러 (잘못 등록) | (없음) | aasdk dispatch에 없음 | `InputService.cpp:23` (등록만, 폰이 안 보냄) | — | — | log only | ⚠️ dead code | 핸들러 등록 제거 |
| 0x8002 | INPUT_BINDING_REQUEST (=KEY_BINDING_REQUEST) | → | aasdk `handleKeyBindingRequest:105` | `InputService.cpp:22,28` | `KeyBindingRequest` (keycodes[]) | indication만 전달 | keycodes 개수 logging → KeyBindingResponse(STATUS_SUCCESS) 즉시 응답 | ✅ | 없음 |
| 0x8003 | INPUT_BINDING_RESPONSE (=KEY_BINDING_RESPONSE) | ← | aasdk `sendKeyBindingResponse:58` | `InputService.cpp:34-41` | `KeyBindingResponse` (status) | reference도 동일 wire | STATUS_SUCCESS | ✅ | 없음 |
| **0x8004** | **INPUT_FEEDBACK** | ↔ | aasdk enum에 정의(미사용) | (없음 — 우리 정의에도 없음) | (확인 필요) | reference도 dispatch/send 없음 | 정의 자체가 없음 | ✅ 양쪽 모두 미사용 | (PARKED) — 미래에 필요해지면 |

##### Input service definition (FillServiceDefinition)

| 항목 | reference | 우리 | 평가 | action |
|---|---|---|---|---|
| keycodes_supported 종류 | aasdk-side 광고는 미확인 (확인 필요) | Android keycode 값 14개 | ⚠️ AAP가 자체 keycode enum을 표준으로 두고 있는지 확인 필요. Android keycode 그대로면 폰이 다른 의미로 해석 가능 | KeyCode mapping 확인 |
| touch screen | width/height + CAPACITIVE | 동일 | ✅ | 없음 |

#### H. Sensor service (channel S)

| msg ID | 이름 | 방향 | reference (aasdk/openauto) | 우리 핸들러 | proto | reference 정책 | 우리 정책 | 평가 | action |
|---|---|---|---|---|---|---|---|---|---|
| 0x0007 | CHANNEL_OPEN_REQUEST (sensor) | → | aasdk `handleChannelOpenRequest:116` | `ServiceBase.cpp:30` (공통) | `ChannelOpenRequest` | service-local | 동일 + OnChannelOpened에서 SendDrivingStatus 한 번 송신 | ✅ + ➕ proactive driving status | 없음 |
| 0x8001 | SENSOR_START_REQUEST (=SENSOR_REQUEST) | → | aasdk `handleSensorStartRequest:105` | `SensorService.cpp:18,26` | `SensorRequest` (type, min_update_period) | indication만 전달 | type 무관 SUCCESS 응답 + DrivingStatus 송신 | ⚠️ type 무시 — 폰이 NIGHT_MODE 시작 요청해도 우리는 driving_status로만 응답. 광고한 NIGHT_MODE/LOCATION 데이터 송신 경로 없음 | type 분기 + 각 sensor type별 송신 메서드 추가 (또는 광고에서 NIGHT_MODE/LOCATION 제거) |
| 0x8002 | SENSOR_START_RESPONSE (=SENSOR_RESPONSE) | ← | aasdk `sendSensorStartResponse:91` | `SensorService.cpp:33-40` | `SensorStartResponseMessage` (status) | reference 동일 wire | STATUS_SUCCESS 항상 | ✅ wire 일치 | 없음 |
| 0x8003 | SENSOR_EVENT (=SENSOR_BATCH) | ← | aasdk `sendSensorEventIndication:78` | `SensorService.cpp:45-54` `SendDrivingStatus` | `SensorBatch` (sensor type별 oneof) | reference 동일 wire | driving_status_data 한 개만, status=0(UNRESTRICTED) 고정 | ⚠️ 항상 같은 값 — 실제 차량 상태 반영 없음 (현재는 stub) | 추후 차량 게이지 통합 시 (PARKED) |
| **0x8004** | **SENSOR_ERROR** | ↔ | aasdk enum에 정의(미사용) | (없음) | (확인 필요) | reference dispatch/send 없음 | 정의 없음 | ✅ 양쪽 미사용 | PARKED |

##### Sensor service definition (FillServiceDefinition)

| 항목 | reference | 우리 | 평가 | action |
|---|---|---|---|---|
| 광고 sensor types | (확인 필요 — openauto는 fake/real 혼합) | DRIVING_STATUS_DATA + NIGHT_MODE + LOCATION | ⚠️ NIGHT_MODE/LOCATION 광고 후 데이터 송신 경로 없음 — 폰이 데이터 기대했다가 timeout 가능 | 둘 중 선택: ① NIGHT_MODE/LOCATION 데이터 송신 경로 추가, ② 광고에서 제거 |

### Stage 1.5 — Service 단위 누락 (D~H 매트릭스 보완)

매트릭스가 메시지 ID 단위라 **service 자체가 통째로 없거나 dispatch 미구현**인 경우가 잘 안 보임. 별도 카탈로그.

#### 우리에게 없거나 미구현인 service

| service | reference 위치 | dispatch 메시지 | reference 광고 여부 | 우리 상태 | 영향도 | action |
|---|---|---|---|---|---|---|
| **Bluetooth** | aasdk `Channel/Bluetooth/`, openauto `Service/Bluetooth/` | `onBluetoothPairingRequest`, `onBluetoothAuthenticationResult` (+ onChannelOpenRequest) | service definition에 `bluetooth_service` 추가, `car_address` 채움 | **광고만** (`BluetoothService.cpp:11-14` car_address). **dispatch 없음** | ⚠️ USB 모드에서 폰이 자동 BT pairing 요청하면 무시됨. wireless 모드는 별도 dial 흐름이라 영향 없음 | BluetoothPairingRequest 핸들러 추가 (BT 페어링 트리거) |
| **GenericNotification** | aasdk `Channel/GenericNotification/` | (handler에 onChannelOpenRequest만 — placeholder) | (확인 필요) | **service 자체 없음**, 광고도 안 함 | ⚠️ reference도 메시지 dispatch 없는 placeholder. 광고만 안 해도 영향 적을 듯. 단 폰이 service 없다고 보면 일부 알림 기능 비활성화 가능 | (낮은 우선순위) placeholder service 추가하고 광고만 |
| **MediaBrowser** | aasdk `Channel/MediaBrowser/` | (placeholder, onChannelOpenRequest만) | (확인 필요) | **없음** | ⚠️ 동일 — 폰의 미디어 라이브러리 탐색 기능. 차량 디스플레이에서 음악 라이브러리 브라우징 시 필요 | 우선순위 낮음 — UI 통합 시 |
| **MediaPlaybackStatus** | aasdk `Channel/MediaPlaybackStatus/` | **`onMetadataUpdate`**(MediaPlaybackMetadata: title/artist/album/art), **`onPlaybackUpdate`**(MediaPlaybackStatus: state/position) | (확인 필요) | **없음** | 🔧 **실제 dispatch가 있는 service** — 폰이 "지금 듣는 곡" 메타데이터를 보내는데 우리가 받지 못함. 차량 디스플레이/클러스터에 곡 정보 표출 못 함 | NavigationStatus와 묶어서 우선 검토 — UI 통합 가치 큼 |
| **NavigationStatus** | aasdk `Channel/NavigationStatus/` | **`onStatusUpdate`**(NavigationStatus), **`onTurnEvent`**(NavigationNextTurnEvent: 다음 회전), **`onDistanceEvent`**(NavigationNextTurnDistanceEvent: 거리) | (확인 필요) | **없음** | 🔧 **실제 dispatch가 있는 service** — 폰이 다음 turn 방향/거리/안내문을 보내는데 우리가 받지 못함. 차량 클러스터에 네비 표시 못 함 | 운전자 가치 가장 큼. NAV_FOCUS 응답값 수정과 함께 검토 |
| **PhoneStatus** | aasdk `Channel/PhoneStatus/` | (placeholder, onChannelOpenRequest만) | (확인 필요) | **없음** | ⚠️ reference도 placeholder. 폰의 통화 상태/신호 강도 등을 표출하는 자리 같으나 dispatch 미정의. 광고 차원에서만 추가 고려 | 낮음 |
| **Radio** | aasdk `Channel/Radio/` | (placeholder, onChannelOpenRequest만) | (확인 필요) | **없음** | ⚠️ 차량 라디오를 폰에 노출하는 service. HU가 라디오 기능 가지면 의미 있음. 우리 헤드유닛이 라디오 통합할지에 따라 다름 | 낮음 — HU 라디오 통합 시 |
| **VendorExtension** | aasdk `Channel/VendorExtension/` | (placeholder) | (확인 필요) | **없음** | ⚠️ 제조사별 확장 메시지 통로. OEM custom 통신 필요할 때 | 낮음 — 필요 시 |
| **WifiProjection** | aasdk `Channel/WifiProjection/` | **`onWifiCredentialsRequest`**(WifiCredentialsRequest) | (확인 필요) | **없음** | 🔧 **실제 dispatch가 있는 service** — 폰이 wireless 연결 중 WiFi 자격 증명을 재요청하는 path. 우리 wireless dial은 RFCOMM 단계에서 WiFi 정보 교환(BluetoothWirelessManager)이라 AAP 세션 안의 이 메시지를 사용하는지 확인 필요. 만약 폰이 보내는데 우리가 못 받으면 wireless 재연결 시 실패 가능 | wireless 동작 검증 + 필요 시 핸들러 추가. PARKED 영역(I)와 연계 |

#### Service 누락 정리

**우리가 가진 service (8)**: Control, Bluetooth(광고만), MediaSink-Audio(3 stream)/Video, MediaSource-Mic(잘못된 ID), Input, Sensor

**reference 대비 통째 누락 (8)**: GenericNotification, MediaBrowser, MediaPlaybackStatus, NavigationStatus, PhoneStatus, Radio, VendorExtension, WifiProjection

**부분 미구현 (1)**: BluetoothService (광고만, dispatch 없음)

**평가 카테고리**:

- **🔧 실제 dispatch 있는 service 누락 (3개)** — 폰이 실제로 메시지를 보내는 service:
  - **NavigationStatus** — 클러스터 네비 안내 (운전자 가치 가장 큼)
  - **MediaPlaybackStatus** — "지금 듣는 곡" 표출
  - **WifiProjection** — wireless 연결 안정성에 영향 가능
- **⚠️ Placeholder service 누락 (5개)** — reference도 channel만 열고 메시지 dispatch 없음:
  - GenericNotification, MediaBrowser, PhoneStatus, Radio, VendorExtension
  - reference도 광고 후 메시지 거의 안 받음. 우리는 광고도 안 함 → 폰이 service "없음"으로 인식. 일부 폰 기능이 차량에 노출 안 될 수 있음. 광고만 추가해도 됨.
- **🔧 부분 미구현 (1개)**:
  - **Bluetooth** — 광고만, BluetoothPairingRequest dispatch 없음. USB 모드에서 폰이 자동 BT pairing을 요청하는 경우 처리 못 함.

#### Stage 3 종합 action 우선순위

**즉시 수정 (🔧)**
없음 — Stage 3는 wire ID 매핑이 모두 일치하고, critical wire bug 없음.

**누락/불일치 정리 (⚠️)**
1. `InputService.cpp:23` 의 INPUT_EVENT 수신 핸들러 등록 제거 — dead code (INPUT_EVENT는 outbound only).
2. `SensorService::HandleSensorStartRequest` 에서 SensorRequest의 `type` 필드 분기 — 현재 type 무관하게 driving_status만 송신.
3. SensorService FillServiceDefinition의 NIGHT_MODE/LOCATION 광고와 송신 경로 불일치 해결 — 광고 제거하거나 송신 추가.
4. InputService keycodes_supported가 Android keycode 값을 그대로 광고함 — AAP 표준 keycode와의 매핑 일치 여부 확인. 만약 다르면 변환 layer 추가.
5. `InputService::SendTouchEvent`가 single pointer만 처리 — multi-touch가 필요해질 때 `pointer_data` 배열 확장.

**누락 (❌)**
6. **`InputService::SendKeyEvent` 메서드 부재** — 스티어링 휠/IR 리모컨 키 이벤트 송신 경로 없음. 우리가 광고한 keycodes를 실제로 보낼 방법이 없는 dead 광고.
7. INPUT_FEEDBACK / SENSOR_ERROR — reference도 미사용. PARKED.

**보존 (➕)**
8. SensorService::OnChannelOpened에서 SendDrivingStatus 한 번 송신 — channel open 직후 폰에게 즉시 driving_status를 알리는 proactive 패턴, 합리적.

## 단계별 작업 흐름

각 stage마다:

1. **Reference 인덱싱** — aasdk + openauto에서 해당 영역의 모든 핸들러/proto 위치 수집. 결과를 "Reference 사실 카탈로그" 해당 섹션에 추가.
2. **우리 코드 인덱싱** — 우리 `core/src/service/`와 `core/include/aauto/service/`에서 대응 핸들러 수집.
3. **메시지 단위 매핑** — 매트릭스 행을 하나씩 채움. 양쪽 핸들러를 짧게 읽고 정책을 한 줄 요약.
4. **평가** — 행마다 `✅/⚠️/❌/🔧/➕` 부여.
5. **Action 도출** — 행마다 후속 조치. 즉시 수정 / 정리 / 문서화 / 보류 / 없음.
6. **사용자 검토** — Stage 끝나면 매트릭스를 사용자에게 보여주고 action 우선순위 합의.
7. 다음 stage로.

## 산출물 위치

- **1차 저장소**: 본 plan file의 매트릭스와 카탈로그 섹션. plan mode 안에서 유일하게 편집 가능한 파일이라.
- **plan mode 종료 후**: 사용자 결정에 따라 별도 `docs/p1_compare/*.md` 로 옮길 수도 있고, plan file 그대로 두고 작업이 끝나면 archive로 옮길 수도. 현 시점에서는 plan file 누적이 기본.

## 비목표 (이번 분석에서 다루지 않음)

- aasdk/openauto의 빌드 시스템, 패키징, Qt UI, gstreamer pipeline 세부.
- boost::asio 비동기 I/O 패턴 차이.
- C++20 modernization 차이.
- 라이선스/저작권 처리.
- PARKED 영역 (B/C/I/J): wire framing, crypto, transport, BT dial, app lifecycle Java 측.

## 완료 기준

- D+E (Stage 1) 매트릭스의 모든 행이 채워지고, 평가/action이 부여되어 있다.
- F (Stage 2) 매트릭스의 모든 행이 채워져 있다.
- G+H (Stage 3) 매트릭스의 모든 행이 채워져 있다.
- 사용자가 각 stage 끝의 action 리스트를 검토하고 우선순위를 매겼다.
- "Reference 사실 카탈로그"가 영역별로 충분한 1차 자료를 가지고 있어, 후속 작업(코드 정리/구현)에서 다시 reference를 뒤지지 않아도 된다.

## Step Log (수정 진행 이력)

분석에서 도출된 action을 step별로 적용한 이력. 각 항목: BUILD_VERSION, 변경 파일, 변경 요지, 검증 결과.

### Step 0 — 분석 (BUILD_VERSION 60, 분석만)

- D~H 매트릭스 + service 단위 누락 카탈로그 작성. 아래 step의 입력.

### Step 1 — C1 첫 시도 (BUILD 61, **revert됨**)

- 변경: `core/src/service/VideoService.cpp` `HandleMediaData`에서 8 byte timestamp prefix 분리 + `pts_us` 추출.
- 결과: 사용자가 SSL decrypt failed 에러 보고. **이 변경과는 무관함이 확인됨**(C1 함수는 application-layer, payload는 const ref).
- BUILD 62 — **revert** (코드는 60과 동일, 빌드 식별 위해 +1).

### Step 2 — B 영역 fragment-by-fragment decrypt 전환 (BUILD 63)

PARKED 영역(B Cryptor & Frame)에서 SSL decrypt failed의 원인 추적.

- **진단**: `MessageFramer::ProcessBuffer`에 multi-first 4 byte total_size를 buffer-size guard가 누락하는 산술 버그(잠재 OOB) + reassemble-then-decrypt 모델이 폰의 record packing 패턴과 어긋날 가능성.
- **변경**:
  - `core/include/aauto/crypto/CryptoManager.hpp` — `ICryptoStrategy::Decrypt`/`TlsCryptoStrategy::Decrypt`/`CryptoManager::DecryptData` 시그니처를 `(input, output&) -> bool` 로 변경. partial record는 `true`+0 append, fatal SSL 에러만 `false`.
  - `core/src/crypto/CryptoManager.cpp` — `Decrypt` 본문 변경. `WANT_READ`/`WANT_WRITE`/`ZERO_RETURN`은 `return true`.
  - `core/include/aauto/session/MessageFramer.hpp` — `AapMessage` 제거 → `AapFragment` 신규. `FragmentState`/`fragment_buffers_`/`EvictStaleFragments`/timeout 등 reassembly state 모두 제거.
  - `core/src/session/MessageFramer.cpp` — `ProcessBuffer` 단순화. multi-first의 4 byte를 buffer-size guard에 포함 (`total_packet = HEADER_SIZE + extra_skip + payload_len`). reassembly 코드 제거. fragment 단위 callback.
  - `core/src/session/Session.cpp` — `<unordered_map>` include. `ProcessLoop`에서 `channel_payloads` 맵 도입. 매 fragment 콜백마다: `is_first`면 clear → `crypto_->DecryptData(ciphertext, payload)` 또는 plain pass-through → `is_last`면 dispatch + clear.
  - `BuildInfo.java` 62 → 63.
- **검증**: 사용자가 SSL decrypt failed 사라졌음 확인 (2026-04-08).
- **남은 검증**: 장시간 안정성(5분+), 회귀(글리치/딜레이/USB·wireless 모두), 핫스왑 등.

### Step 3 — C1 재적용 (BUILD 64)

SSL이 안정된 상태에서 video frame timestamp 분리를 다시 적용.

- **변경**:
  - `core/src/service/VideoService.cpp` — `<cstring>` include + `kVideoTimestampBytes = 8` 상수 추가. `HandleMediaData`에서 첫 8 byte를 `pts_us`로 추출, 나머지를 sink에 전달. ack 위치는 그대로.
  - `BuildInfo.java` 63 → 64.
- **검증**: 빌드 + 동작 확인 완료 (사용자 보고).

### Step 4 — C2: MIC message ID 표준화 (BUILD 65)

- **변경**:
  - `core/include/aauto/session/AapProtocol.hpp` — `MIC_REQUEST` 0x800A → 0x8005, `MIC_RESPONSE` 0x800B → 0x8006. 표준(`MEDIA_MESSAGE_MICROPHONE_REQUEST/RESPONSE`)과 일치. 기존 0x800A/B는 `UPDATE_UI_CONFIG_REPLY`/`AUDIO_UNDERFLOW_NOTIFICATION` 슬롯과 충돌하던 것을 해소.
  - `BuildInfo.java` 64 → 65.
- **동작 영향**: 마이크 capture 자체가 미구현 dead code라 즉시 동작 변화 없음. 미래 마이크 구현 시 정확한 ID로 등록되도록 한 사전 정리.
- **검증**: 빌드 통과만 확인 (mic 미사용).

### Step 5 — B1: NAV_FOCUS 응답 NATIVE → PROJECTED (BUILD 66 → 67 → 68)

- **변경**:
  - `core/include/aauto/service/ControlService.hpp` — `NavFocusType.pb.h` include + `SendNavFocusNotification` 시그니처 `int` → `NavFocusType` enum.
  - `core/src/service/ControlService.cpp` —
    - 핸들러가 `NAV_FOCUS_PROJECTED` enum 값으로 호출 (이전 raw `1` = `NATIVE` → 폰이 "HU 자체 nav 우선"으로 해석하던 잠재 버그).
    - `SendNavFocusNotification` 본문에서 static_cast 제거.
    - 핸들러 위에 의도/이유 주석.
  - `BuildInfo.java` 65 → 66.
- **빌드 fail 2회 (BUILD 67, 68)**: `NavFocusType_Name` 헬퍼가 protobuf-lite 빌드에는 포함되지 않아 fully-qualify 시도 후에도 unresolved → 최종적으로 `static_cast<int>(type)` 으로 단순 출력. 로그에서 enum 이름 대신 정수 값 (`type=2` 형태) 출력.
- **검증**: 빌드 통과 + 폰 연결 후 navigation 화면 표출 확인 (사용자 보고). 좌표/지도 데이터가 안 와서 지도 자체는 비어 있음 — 이건 별개 이슈로 SensorService의 LOCATION 송신 미구현 (Stage 3 매트릭스 #A9 / #6 — 아래 다음 step 후보 참고).

### Step 6 — Sub-step 1: Service config structs + 생성자 변경 (BUILD 69)

`docs/plans/0002_app_driven_services.md` 의 Sub-step 1.

- **변경**: 6개 service header에 config struct 추가, 생성자 시그니처를 `(config struct)` 로 통일.
  - `BluetoothServiceConfig{car_address}`
  - `MicrophoneServiceConfig{sample_rate, channels, bits_per_sample}`
  - `SensorServiceConfig{driving_status, night_mode, location}` — 광고/송신 둘 다 flag로 분기 → 일관성 확보
  - `AudioServiceConfig{stream_type, sample_rate, channels, bits_per_sample, name}`
  - `VideoServiceConfig{resolution, frame_rate, width_margin, height_margin, density, width, height, fps}`
  - `InputServiceConfig{touch_width, touch_height, touch_type, is_secondary, supported_keycodes}`
  - 6개 .cpp 본문이 `config_` 사용. `VideoService` 가 `HeadunitConfig` 의존 제거.
- `ServiceFactory::Create*` 헬퍼들이 임시로 hardcode default config 채워 새 ctor 호출 (sub-step 2 직전 상태).
- **동작 변화**: Sensor 광고가 3개(DRIVING/NIGHT/LOCATION) → 1개(DRIVING) 로 축소 (의도된 변화 — 광고/송신 일관성).
- **검증**: 빌드 + 폰 연결 동작 확인 (사용자 보고).

### Step 7 — Sub-step 2: ServiceComposition + ServiceFactory + AAutoEngine (BUILD 70)

- **변경**:
  - `core/include/aauto/service/ServiceComposition.hpp` (NEW) — `vector<AudioServiceConfig>` + `optional<VideoServiceConfig>` + ... .
  - `ServiceContext` 에 `composition` 추가, `config` → `identity` rename. `ServiceFactory.hpp` 의 helper 메서드 8개 모두 제거.
  - `ServiceFactory::CreateAll()` 가 composition 기반 분기 (audio 순회 + optional 분기).
  - `AAutoEngine` ctor `(HeadunitConfig)` → `(HeadunitConfig identity, ServiceComposition composition)`. member rename.
  - JNI `nativeInit` 이 hardcode default composition 빌드해 새 ctor 호출 (회귀 0 보장).
- **검증**: 빌드 + 동작 확인 (사용자 보고).

### Step 8 — Sub-step 3 + mic 광고 복구 (BUILD 71 → 72)

- **변경 (BUILD 71)**:
  - `EngineContext` 에 `pending_identity` / `pending_composition` / `finalized` 추가.
  - `nativeInit` 시그니처 변경: `(btAddress, displayWidth, displayHeight, displayDensity)`. engine 생성 X — pending_identity만 stage.
  - 신규 native 메서드 7개: `nativeAddAudioStream`, `nativeSetVideoConfig`, `nativeSetInputConfig`, `nativeSetSensorConfig`, `nativeSetMicrophoneConfig`, `nativeSetBluetoothConfig`, `nativeFinalizeComposition`.
  - Java `AaSessionService::onCreate` 흐름: nativeInit → 8개 builder calls → nativeFinalizeComposition. AAP wire enum 상수 (`AUDIO_STREAM_*`, `VIDEO_RES_1280x720`, `VIDEO_FPS_30`) Java side 정의. Platform capability 상수(`DISPLAY_WIDTH/HEIGHT/DENSITY`, `SUPPORTED_KEYCODES`).
- **회귀 발견 (BUILD 71)**: mic 광고를 의도적으로 omit 했더니 폰(Samsung Galaxy Note 20)이 channel open 자체를 안 보냄. ServiceDiscoveryResponse는 잘 갔으나 그 이후 모든 송신이 USB write timeout. **광고에 microphone source service가 있어야 폰이 협상을 진행**.
- **fix (BUILD 72)**: `nativeSetMicrophoneConfig(16000, 1)` 호출 추가. capture는 여전히 미구현이지만 광고는 유지. 동작 회복 (사용자 보고).
- **검증**: 빌드 + 동작 회복 확인 (사용자 보고).
- **새 발견 사실** (P1 카탈로그에 추가 가치): Samsung 계열 폰은 ServiceDiscoveryResponse에 `media_source_service` 항목이 필수. 광고가 빠지면 channel open 단계 자체가 진행 안 됨. 향후 service 광고 셀프-축소 시 주의.

### Sub-step 1~3 결과 — App-driven service composition 완성

이제 platform capability를 변경하려면 `AaSessionService.java::onCreate` 한 블록만 손보면 됩니다:
- 마이크 capture 추가 시: 광고는 이미 있고 sample rate만 platform에 맞춰 변경.
- LOC1 해결 시: `nativeSetSensorConfig(true, false, true)` (location=true) + Android `LocationManager` 통합 + `SensorService` 의 location 송신 경로 추가.
- 다른 디스플레이 해상도: `DISPLAY_*` 상수만 변경.
- TELEPHONY audio 추가: `nativeAddAudioStream(4, 8000, 1);` 한 줄.

core/native에는 어떤 service set이 default인지에 대한 지식 없음. **app이 결정 → native가 그대로 빌드** 모델 완성.

### Step 9 — LOC1: Sensor LOCATION pipeline + simulator (BUILD 76 → 93)

여러 sub-step으로 나뉘어 진행. 이 트랙에서 발견한 사실/회귀가 많아 시간 흐름대로 정리.

- **목표**: 차량 GPS → phone-side AA navigation 안의 차량 marker. HU에 GPS chipset이 없으므로 시뮬레이터로 대체.
- **architecture**: `LocationSimulator` (polyline tick) → `FixListener.onFix(...)` → `nativeSendLocation` → 모든 active session의 `SensorService::SendLocationFix` → `SensorBatch{location_data + driving_status_data}` 송신. 동일한 lambda를 real `LocationManager` listener와 mock simulator가 모두 호출 — Real/Mock 분기는 `startLocationPipeline` 한 곳.
- **mock provider 우회**: 처음엔 `LocationManager.addTestProvider(GPS_PROVIDER)` 모델로 시도했으나 Android 10에서 `OP_MOCK_LOCATION` AppOps gating이 setMode 후에도 silent fail. simulator가 LocationManager 우회하고 직접 callback 호출하는 모델로 pivot.
- **LocationData wire 조정**:
  - `timestamp` (proto에서 deprecated이지만 일부 phone이 stale 검증에 사용) — 항상 set.
  - `driving_status_data`를 같은 batch에 동시 송신 — `UNRESTRICTED(=정지)` + 100km/h location 모순 회피.
- **HandleSensorStartRequest type 분기**: 이전에 type 무시하고 항상 `SendDrivingStatus` 호출하던 것을 type 별로 분기. LOCATION 요청에 driving 응답을 보내던 mismatch 해소.
- **OSRM polyline**: 강남역 273점 round trip → 서울↔부산 95점 round trip @ 100km/h. 진짜 도로 위 좌표.

#### 발견 사항 (사용자 검증)

- **wired (USB)**: navigation app idle 모드에서도 차량 marker가 polyline 따라 정상 이동.
- **wireless (TCP)**: 같은 navigation app, 안내 모드 시작해야만 movement 보임. 가설 — phone-side AA 가 wireless 모드에서 차량 sensor location을 secondary로 두고 phone GPS chip을 우선 사용. AAW protocol design 자체로 보임. 우리 측에서 fix 불가.
- **결론**: wire path는 정상. wireless 측 한계는 phone-side 정책.

#### 회귀: USB write timeout deadlock (commit 7c25cf1)

- LOC1 commit 적용 후 5분 안에 USB write timeout 15회 재시도 → connection timed out.
- **원인**: `LocationSimulator` 가 `Handler(Looper.getMainLooper())` 사용. tick 1Hz callback → JNI → `SensorService::SendLocationFix` → `SendEncrypted` → USB write. **USB write가 main thread에서 동기 호출**되어 phone-side back-pressure 시 main thread block. broadcast receiver / lifecycle / BT 이벤트 등 다른 main thread work 도 누적 정체. 5분이 누적 임계점.
- **fix**: `LocationSimulator` 가 `HandlerThread("LocationSimulator")` 의 looper 사용. tick + USB write가 worker thread 에서. main thread 는 free.
- **검증**: 20분+ 안정 동작 (사용자 보고).

#### 중간 정리 (BUILD 87)

- 진단 과정에서 추가했던 권한들 제거: AndroidManifest의 `ACCESS_MOCK_LOCATION` / `UPDATE_APP_OPS_STATS` / `MANAGE_APP_OPS_MODES`, privapp xml의 같은 항목들 — mock provider 우회 후 미사용.
- 진단 로그 (`tickCount_`, `locationFixCount_`, `LocationFix #`) 제거.

### Step 10 — B2/B3/B4: Control channel 정리 (BUILD 89 → 91 → 92)

세 가지 작은 변경 한 묶음.

- **B2 — PING liveness 추적**:
  - `ControlService` 에 `last_pong_ns_` (atomic), `close_triggered_` (atomic), `session_close_cb_` 추가.
  - `PING_RESPONSE` 핸들러 — `last_pong_ns_` update.
  - `OnChannelOpened` — last_pong seed (handshake 직후 false-positive 방지).
  - `HeartbeatLoop` — 매 ping 송신 후 timeout 검사. `kPingInterval=5s`, `kPingTimeout=10s` (2 ping miss).
  - `Session::Start` — `GetService(CONTROL)` 후 `SetSessionCloseCallback([this]{ Stop(); })` install.
  - 처음엔 `kPingInterval=1s` (openauto reference align) 시도했으나 USB write back-pressure 누적 의심으로 5s 로 되돌림. main thread fix 후에는 재검토 가능.
- **B3 — VERSION_RESPONSE status 검증**:
  - `AapHandshaker::DoVersionExchange` 가 payload 6 byte 파싱: `major(2) | minor(2) | int16 status`.
  - `status != 0` (= `STATUS_SUCCESS`) 이면 abort + 명확한 로그.
  - payload < 6 byte 옛 firmware 케이스는 warning + accept (호환).
- **B4 — BYEBYE_REQUEST/RESPONSE 핸들러**:
  - `AapProtocol.hpp::msg::BYEBYE_REQUEST = 0x000F`, `BYEBYE_RESPONSE = 0x0010` 추가 (이전 미정의).
  - `ControlService` 에 두 핸들러 등록. `BYEBYE_REQUEST` 받으면 `ByeByeResponse` ack 송신 후 `TriggerSessionClose("ByeByeRequest")`. `BYEBYE_RESPONSE` 도 `TriggerSessionClose`.
- **검증**: 정상 동작 회귀 없음. PING timeout / BYEBYE 실 trigger는 reproduce 어려워 별도 검증 안 함.

### 다음 step 후보

분석에서 도출된 항목 중 아직 미적용. 우선순위가 높아진 신규 항목 표시.

- **🆕 LOC1 — Sensor LOCATION 송신** (사용자 발견, 2026-04-08): 네비 화면은 PROJECTED 응답 후 표출되지만 좌표가 없어 지도가 비어 있음. `SensorService::FillServiceDefinition`에서 `SENSOR_LOCATION` 광고하지만 데이터 송신 경로가 없음 (Stage 3 #A9). GPS 데이터를 (Android `LocationManager` 또는 stub) 폰에 sensor batch로 송신해야 지도가 표시됨. 운전자 가치 가장 큼.
- **B2** PING_RESPONSE liveness 추적 추가 (`ControlService.cpp:100`).
- **B3** VERSION_RESPONSE status 검증 (`AapHandshaker.cpp:84`).
- **B4** BYEBYE_REQUEST 핸들러 추가 (`ControlService.cpp`).
- **B5** VIDEO_FOCUS_REQUEST 응답 송신 (`VideoService.cpp:42`).
- **A1** AUDIO_FOCUS proto를 `AudioFocusRequest`로 표준화.
- **A2~A11** 정리/표준화 항목 11건 (Stage 1~3 매트릭스 참고).
- **N1** NavigationStatus service 신규 추가 — 폰의 turn-by-turn 안내 정보 수신.
- **N2** MediaPlaybackStatus service 신규 추가 — "지금 듣는 곡" 메타데이터.
- WifiProjection 검증 + Bluetooth pairing 핸들러 추가.
- Placeholder service 5개 일괄 광고 추가.

---

# [PARKED] 백그라운드 오디오 + 멀티 디바이스 시나리오 정리 및 구현

> **상태**: 보류. 위 P1 reference 분석이 끝난 후 재개. 분석 결과로
> sink 모델/audio focus 정책에 변경이 생기면 이 플랜도 갱신 후 실행.

## Context

현재 `AaDisplayActivity.onStop()`이 `AaSessionService.deactivateAll()`을 호출해서
활성 세션의 비디오 + 오디오 sink 3종(MEDIA/GUIDANCE/SYSTEM)을 모두 detach하고
`activeHandle_`를 0으로 만든다. 이 결과로 사용자가 AA 화면에서 홈 버튼이나
다른 앱으로 빠지면 음악이 즉시 끊긴다.

목표: AA 화면을 떠나도 활성 세션이 살아 있고 음악이 계속 흐르도록 하되,
멀티 디바이스 환경에서 정책이 모호해지지 않도록 모델을 명확히 한다.

## 합의된 정책

| # | 갈림길 | 결정 |
|---|---|---|
| Q1 | 백그라운드 오디오 동시 세션 수 | **한 세션만** (자동차 스피커는 한 폰의 소리만) |
| Q2 | 디스플레이 세션 vs 오디오 세션 분리 | **묶음** — 같은 세션 |
| Q3 | 백그라운드 오디오 중 다른 세션 활성화 | **즉시 전환** (이전 audio 죽임) |
| Q4 | 백그라운드 오디오 중 새 디바이스 연결 | **MainActivity에 추가만**, 자동 활성화 없음 |

## 도출 모델

`AaSessionService`는 이미 단일 `activeHandle_`을 가진다. 여기에 "비디오가
표면(surface)에 묶여 있는가"라는 한 비트만 추가하면 위 정책이 모두 표현된다.

```
DisplayBinding (활성 세션의 sink 부착 상태)
  NONE         : activeHandle_ == 0
  AUDIO_ONLY   : activeHandle_ != 0, audio sink 부착, video 없음 (백그라운드 오디오)
  AUDIO_VIDEO  : activeHandle_ != 0, audio + video 부착 (포그라운드)
```

세션 단위 상태(`SessionState`)에는 `BACKGROUND_AUDIO` 신규 enum 추가하여
MainActivity 리스트에서 사용자가 한눈에 구분할 수 있게 한다.

### 시나리오 매트릭스

| 트리거 | 사전 상태 | 후속 동작 |
|---|---|---|
| `activate(h)` (다른 세션 탭) | 기존 활성 X | h를 활성, surface 있으면 AUDIO_VIDEO, 없으면 AUDIO_ONLY |
| `activate(h)` (다른 세션 탭) | 기존 활성 O (FG/BG 무관) | 기존 audio+video 모두 detach → h로 교체 (Q3 즉시 전환) |
| `onSurfaceReady` | 활성 세션 AUDIO_ONLY | video만 attach → AUDIO_VIDEO |
| `onSurfaceReady` | 활성 세션 AUDIO_VIDEO 또는 NONE | (NONE이면 no-op; AUDIO_VIDEO면 view-size만 갱신) |
| `onSurfaceDestroyed` | AUDIO_VIDEO | video만 detach → AUDIO_ONLY (오디오 유지) |
| `AaDisplayActivity.onStop` | (어떤 상태든) | 서비스 unbind만. sink 변경 없음 — surfaceDestroyed가 이미 video를 떼어둔 상태 |
| 명시적 disconnect (롱프레스) | 어느 상태든 | `disconnectSession(h)` 그대로 — native 세션 stop, 정리 자동 |
| 새 디바이스 ready (USB/wireless) | 활성 세션 AUDIO_ONLY | sessions_에 추가, MainActivity 전면화. 백그라운드 오디오 유지 (Q4) |
| native 세션 종료 | 종료된 세션이 활성 | `activeHandle_=0`, AUDIO_ONLY였더라도 자연 정리 |
| MainActivity에서 동일 활성 세션 탭 | AUDIO_ONLY | activate는 no-op (handle == activeHandle_), 그 다음 AaDisplayActivity 진입 → onSurfaceReady → AUDIO_VIDEO 승격 |

핵심 invariant:
- `activeHandle_ != 0` 이면 audio sink 3종은 항상 부착되어 있다.
- video sink 부착 여부는 `currentSurface_` 가 non-null 인 동안에만.

## 구현 변경

### 파일: `app/android/src/main/java/com/aauto/app/core/AaSessionService.java`

1. **새 enum 값** `SessionState.BACKGROUND_AUDIO` 추가 (UI 표시용).
2. **헬퍼 분리** — 현재 `attachActive()` / `detachActive()`를 다음으로 쪼갠다:
   - `bindAudioToActive()` — audio sink 3종만 attach
   - `bindVideoToActive()` — video sink만 attach (currentSurface_ 필요)
   - `unbindAudioFromActive()` — audio sink 3종 detach
   - `unbindVideoFromActive()` — video sink detach
3. **`activate(long handle)`** 변경:
   - 기존 활성이 있으면 video → audio 순으로 모두 unbind (현재 detachActive 순서 유지: audio 먼저).
   - `activeHandle_ = handle`, 세션 state는 surface 유무에 따라 RUNNING 또는 BACKGROUND_AUDIO.
   - `bindAudioToActive()` 항상 호출, `currentSurface_ != null`이면 `bindVideoToActive()` 추가.
4. **`onSurfaceReady`** (`AaSessionService.java:291`):
   - 동일 surface 재발사 가드 (`currentSurface_ == surface`) 그대로 유지 (디코더 보존, 메모리 L302-307의 이유).
   - 새 surface일 때 `currentSurface_ = surface; if (activeHandle_ != 0) { bindVideoToActive(); state = RUNNING; broadcast; }`.
5. **`onSurfaceDestroyed`** (`AaSessionService.java:313`):
   - `if (activeHandle_ != 0) { unbindVideoFromActive(); state = BACKGROUND_AUDIO; broadcast; }`
   - `currentSurface_ = null` (오디오는 손대지 않음).
6. **`deactivateAll()`** (`AaSessionService.java:277`):
   - 의미 변경: "활성 세션 통째로 종료" — audio + video 모두 detach, `activeHandle_ = 0`. (지금 동작과 동일하지만 호출 지점이 줄어든다.)
   - **이 경로는 더 이상 AaDisplayActivity.onStop 에서는 호출하지 않는다.** 명시적 "종료"가 필요한 곳이 생기면 사용하기 위해 API는 남겨둔다. 호출자가 없어진다면 제거 가능 — 구현 시 grep으로 확인 후 결정.
7. **세션 종료 경로** (`onSessionClosedFromNative`, `stopAndRemoveByHandle`):
   - 종료된 세션이 활성이면 `unbindAudioFromActive()` 도 함께 호출 (현재는 detachActive에 video도 묶여 있어서 의도적으로 호출 안 했음 — `AaSessionService.java:434-440` 주석 참고). 분리된 후에는 audio만 detach해주는 게 정확. 또는 안전하게 둘 다 호출.

### 파일: `app/android/src/main/java/com/aauto/app/AaDisplayActivity.java`

1. **`onStop()`** (`AaDisplayActivity.java:126`):
   - `service.deactivateAll()` 호출 **제거**.
   - 서비스 unbind, receiver unregister만 유지.
   - surfaceDestroyed 콜백이 (이미 안드로이드 라이프사이클에 의해) onStop 직전에 발사되어 서비스가 video unbind를 처리하므로 onStop은 추가 작업 없음.
2. **`sessionEndReceiver_`** (`AaDisplayActivity.java:80`): 그대로 유지. 활성 세션이 사라졌을 때만 finish — 백그라운드 오디오 모드에서도 activeHandle_ 살아 있으면 finish 안 함.

### 파일: `app/android/src/main/java/com/aauto/app/MainActivity.java`

1. **DeviceAdapter.getView**: `BACKGROUND_AUDIO` 케이스 추가 — "Audio only" 같은 라벨과 별도 색상으로 표시 (예: 시안 톤).

### BuildInfo

- `BuildInfo.BUILD_VERSION` +1 (메모리 규칙: 매 코드 수정 시 증가).

## 명시적으로 다루지 않는 것 (의도된 비목표)

- 시스템 `AudioManager.requestAudioFocus` 통합. 현재 코드에 없고 이번 변경 범위 밖.
- AAP `AudioFocusRequest`에 LOSS 응답 보내기. `ControlService.cpp:51`의 default-grant 정책 유지.
- 비디오 백그라운드 시 폰에 video stream stop 요청. Native sink가 없을 때 데이터 drop으로 충분하며, 폰별 호환성 위험 회피.

## 검증 (end-to-end)

수동 시나리오:
1. **단일 디바이스 백그라운드 오디오**
   - USB 또는 wireless로 폰 1대 연결, 음악 재생, AA 화면 진입.
   - 홈 버튼 → 음악이 끊기지 않는지 확인.
   - 다시 AA 앱 (런처 또는 MainActivity → 해당 세션 탭) 진입 → 비디오가 다시 표출되는지, 오디오 글리치가 없는지.
   - 백그라운드 상태에서 MainActivity 리스트에 "Audio only" 표시되는지.
2. **백그라운드 오디오 중 두 번째 디바이스 연결 (Q4)**
   - 1번 상태(폰A 백그라운드 오디오)에서 폰B 연결.
   - MainActivity가 전면화되지만 폰A의 음악은 끊기지 않음.
   - 폰B는 READY 상태로 리스트에 추가됨.
3. **즉시 전환 (Q3)**
   - 2번 상태에서 폰B를 탭 → 폰A 음악 즉시 정지, 폰B 비디오+오디오 표출.
   - 폰A는 READY로 강등.
4. **명시적 종료**
   - 백그라운드 오디오 중인 세션을 MainActivity에서 롱프레스 → 세션 종료, 음악 정지.
5. **트랜스포트 갑작스런 끊김**
   - 백그라운드 오디오 중에 USB 뽑기 / wireless RFCOMM 끊기.
   - 음악이 정지하고 세션이 리스트에서 사라지는지.
6. **포그라운드 동작 회귀 없음**
   - 일반적인 AA 사용 흐름에서 비디오/오디오에 글리치 없는지.
   - 세션 전환 (활성 세션 A → B) 동작이 기존과 동일한지.
7. **BT cycling 회귀 없음** (메모리 `feedback_session_service_locking.md` 참고):
   - `onWirelessDeviceReady`에 락을 추가하지 않았는지 확인.

빌드:
- `BuildInfo.BUILD_VERSION` 증가 확인.
- 안드로이드 빌드 통과 확인.

## 영향 범위 요약

수정 파일 3개:
- `app/android/src/main/java/com/aauto/app/core/AaSessionService.java` (모델/헬퍼)
- `app/android/src/main/java/com/aauto/app/AaDisplayActivity.java` (onStop에서 deactivateAll 제거)
- `app/android/src/main/java/com/aauto/app/MainActivity.java` (UI 라벨)
- `app/android/src/main/java/com/aauto/app/BuildInfo.java` (버전 증가)

native 변경 없음. C++ 레이어의 sink 모델/audio focus 정책은 그대로.
