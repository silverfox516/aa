# 0003 — Wireless lifecycle 정리

## Context

Wireless 연결 환경에서 두 가지 lifecycle 결함이 보고됨:

1. **Stale 항목 잔존** — phone 측 BT 또는 WiFi 가 끊겨도 `MainActivity` 의 device list 에서 그 wireless session 항목이 사라지지 않는다. 다음 native ping timeout 까지 stuck 또는 영원히 stale.

2. **RFCOMM 재연결 후 AA 동작 멈춤** — wireless session 이 정상 동작 중에 phone 측이 알 수 없는 이유로 RFCOMM 을 다시 연결한다. HU 가 새 RFCOMM accept 후 handshake 진행 → phone 이 "WiFi 이미 연결됨" 응답 → HU 는 재시도 안 함. 그러나 이 시점 이후 첫 AAP session 자체도 더 이상 동작하지 않는다.

이 문서는 두 결함의 root cause 가설과 fix plan 을 정리한다.

## 현재 wireless lifecycle 흐름 (코드 트레이스)

### 정상 path

`WirelessMonitorService.onCreate` →
`startListeningIfReady` →
`preBoundTcpSocket()` → `tcpServerFd_` 에 5277 port pre-bind →
`BluetoothWirelessManager.startListening()` →
`listenLoop` 가 RFCOMM accept loop.

Phone 연결 시:
- `serverSocket_.accept()` → `closeClientSocket()` (이전 socket 닫음) → `clientSocket_ = newSocket` → `listener.onDeviceConnecting(...)` → `WirelessMonitorService.onDeviceConnecting` → `ACTION_WIRELESS_CONNECTING` intent → `AaSessionService.pending_` 에 entry 추가.
- handshake thread 시작: `runHandshake` → VERSION_REQUEST / START_REQUEST / INFO_REQUEST/RESPONSE / START_RESPONSE 교환 → 성공 시 `listener.onDeviceReady(...)` → `WirelessMonitorService.onDeviceReady` → `AaSessionService.onWirelessDeviceReady(deviceId, deviceName, fd)` (binder 직접 호출).
- `onWirelessDeviceReady` 가 dedupe 후 `nativeOnWirelessDeviceReady` 로 native session 생성 → `pending_` 의 entry 를 `sessions_` 로 promote.
- `runHandshake` 의 마지막 단계가 `waitForRfcommClose(in, deviceId)` — RFCOMM input stream 에서 read 하며 무한 block. 의도: phone 이 RFCOMM 을 닫을 때까지 keep-alive (메모리 `feedback_aaw_rfcomm_keepalive.md` — RFCOMM 닫으면 phone 재연결 폭주).
- 이 사이에 native AAP session 은 별도 TCP transport 로 정상 동작.

### 끊김 시나리오 — 현재 path

| 트리거 | 처리 | session list 정리 여부 |
|---|---|---|
| **BT off** (`BluetoothAdapter.STATE_OFF` 직전 `STATE_TURNING_OFF`) | `WirelessMonitorService.btStateReceiver_` 가 `wirelessManager_.stop()` 만 호출. RFCOMM listener 종료. | ❌ **AaSessionService 에 알림 없음.** native session 은 TCP 별개라 그대로 살아 있음. UI list 에 stale entry. |
| **SoftAP off** | `apStateReceiver_` 가 `hotspotConfig_ = null`. RFCOMM listener 와 native session 둘 다 그대로. | ❌ 알림 없음. stale. |
| **Phone 측 RFCOMM peer close** (BT 끊김 등) | `waitForRfcommClose` 의 `in.read()` 가 -1 또는 IOException → finally `closeClientSocket()` → `listenLoop` 다음 iteration 으로 (다음 accept). | ❌ listener 에 onDisconnected 통지 없음. native session 그대로. |
| **Phone 측 WiFi 끊김** | TCP socket 의 OS-level keep-alive 까지 우리가 알 수 없음 (수 분 ~ 시간). | ❌ AAP layer ping 이 timeout 검출해야만 정리. |
| **Native AAP ping timeout** (B2 기능, BUILD 89~) | `ControlService::HeartbeatLoop` 가 timeout 검출 → `TriggerSessionClose` → `Session::Stop` → transport disconnect → `onSessionClosedFromNative` → `sessions_.remove`. | ✅ 정리됨. 단 wireless 측 TCP backpressure 로 ping 송신 자체가 block 되면 timeout 검출 못 함. |
| **USB detach** | `ACTION_USB_DEVICE_DETACHED` intent → `stopAndRemoveByTransportId`. | ✅ 정상 (USB only). |
| **새 RFCOMM accept** (phone 재연결) | `listenLoop` 에서 `closeClientSocket()` (이전 client socket 강제 종료) → 새 socket → 새 handshake thread. | ❓ — 문제 2 의 핵심 영역. 아래 분석. |

### 문제 1 — root cause

세 곳에서 wireless lifecycle 종료 신호가 AaSessionService 까지 propagate 안 됨:

1. `BluetoothWirelessManager.waitForRfcommClose` 가 RFCOMM peer close 검출 시 listener 에 통지 안 함 (`closeClientSocket` 만 호출하고 조용히 listenLoop 로 복귀).
2. `WirelessMonitorService.btStateReceiver_` 가 BT off 시 wireless session 모두를 정리하지 않음 (RFCOMM listener 만 stop).
3. `WirelessMonitorService.apStateReceiver_` 가 SoftAP off 시에도 정리하지 않음.

native AAP ping timeout 이 fallback 으로 동작해야 하는데 wireless TCP backpressure 시 send 가 block 되어 timeout 자체가 안 움직일 수 있음 (B2 가 main thread 에서 호출되지 않더라도, transport sync send 가 mutex 에서 long-block).

### 문제 2 — root cause 가설

`listenLoop` 의 새 accept path:

```java
socket = serverSocket_.accept();         // (a)
String deviceId = ...;
closeClientSocket();                      // (b) — 이전 client socket 강제 종료
clientSocket_ = socket;                   // (c)
listener_.onDeviceConnecting(...);
Thread t = new Thread(() -> runHandshake(socket, ...));
handshakeThread_ = t; t.start();
t.join();                                 // (d) — 이전 handshake thread 와 직렬
```

`(b)` 가 핵심: **이전 client socket 을 강제로 close**. 그 시점 첫 RFCOMM thread 는 `waitForRfcommClose` 의 `in.read()` 에서 block 중. socket close 로 read 가 IOException → first thread 의 finally `closeClientSocket()` 호출 → 그 시점 `clientSocket_` 변수는 이미 새 socket 으로 덮어 씌었으므로 **새 socket 이 또 close 됨**. 그러면 새 handshake thread 의 read 도 IOException → `onConnectionFailed` → loop 다음 accept.

악화 시나리오:
1. (b) 단계에서 `closeClientSocket()` 호출 시점에 `clientSocket_` 가 첫 socket. 첫 socket close.
2. (c) 에서 `clientSocket_ = newSocket`.
3. 첫 thread 의 `waitForRfcommClose` 가 close 로 인한 IOException → finally `closeClientSocket()` 호출 → 새 socket close.
4. 새 thread `runHandshake` 시작했지만 이미 socket close 라 read 시 즉시 IOException → handshake fail → `onConnectionFailed`.
5. listenLoop 다음 accept. phone 이 retry. (3) 부터 반복 — 폭주.

이 race 때문에 새 RFCOMM 이 들어와도 정상 handshake 되지 않고 이전 RFCOMM 은 강제 종료. 그 사이에 첫 native AAP session 은 RFCOMM 종료와 무관하게 동작해야 정상이지만, 두 가지 부수 효과로 깨질 수 있음:

- `WirelessMonitorService.onDeviceReady` 가 `rebindTcpServerFdOnly()` 호출 — 새 TCP server fd 만들고 이전 server fd close. 단 이전 server fd 는 이미 첫 session 시 native 로 transfer 됐으므로 영향 X. 단 새 fd 가 dedupe 시 close되어 다음 wireless 시도 시 fd 없음 → 새 phone 시도 fail.
- 두 번째 RFCOMM handshake 가 success 까지 갔을 경우 (예: phone 이 status=0 으로 응답) → `onDeviceReady` → `AaSessionService.onWirelessDeviceReady` → `findSessionByTransportId(deviceId) != null` 이라 dedupe → fd close. 첫 session 은 살아 있음. 그러나 새 RFCOMM client socket 은 keep-alive 모드로 들어가는데, **첫 RFCOMM keep-alive 가 강제 종료된 상태에서 사용자가 본 "동작 안 함"** 이 곧 첫 session 의 TCP transport 가 어떤 이유로 stuck 인지 또는 phone-side AAP 가 새 RFCOMM keep-alive 를 새 session 시작으로 해석해 첫 session 의 video stream 을 일방적으로 stop 했을 가능성.

이 가설은 사용자 보고와 합치하지만 100% 확정 아님 — phone-side 동작에 의존. 추가 진단 권고는 verification 섹션 참조.

## Design

두 결함을 한 묶음으로 수정. 핵심 원칙: **wireless lifecycle 변화는 항상 AaSessionService 까지 propagate**.

### 1. `BluetoothWirelessManager.Listener` 에 disconnect callback 추가

```java
interface Listener {
    void onDeviceConnecting(String deviceId, String deviceName);
    void onDeviceReady(String deviceId, String deviceName);
    void onConnectionFailed(String deviceId, String reason);
    void onDeviceDisconnected(String deviceId, String reason);  // NEW
}
```

호출 지점:
- `waitForRfcommClose` 가 peer close (`in.read() == -1`) 또는 IOException 검출 시 — finally 직전에 `listener_.onDeviceDisconnected(deviceId, reason)` 호출.
- 단, 새 RFCOMM accept 가 첫 socket 을 강제 close 한 케이스(=`closeClientSocket()` 외부 호출 직후 IOException) 와 phone peer close 케이스를 구분 — 후자만 통지. 구분 기준은 새 멤버 `boolean externallyClosed_` flag 또는 cause type.

### 2. `WirelessMonitorService.onDeviceDisconnected` 구현

```java
@Override
public void onDeviceDisconnected(String deviceId, String reason) {
    Log.i(TAG, "Wireless peer disconnected: " + deviceId + " (" + reason + ")");
    Intent intent = new Intent(this, AaSessionService.class);
    intent.setAction(AaSessionService.ACTION_WIRELESS_DEVICE_DETACHED);
    intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
    startForegroundService(intent);
}
```

이미 `AaSessionService` 측 `ACTION_WIRELESS_DEVICE_DETACHED` 핸들러가 `stopAndRemoveByTransportId` 호출하도록 되어 있음 (line 349-354). 즉 new path 만 wire 하면 정리 자동.

### 3. BT off / SoftAP off 시 wireless session 정리

`btStateReceiver_` 가 `STATE_TURNING_OFF` 검출 시 + `apStateReceiver_` 가 `WIFI_AP_STATE_DISABLED` 검출 시:

```java
private void teardownAllWirelessSessions(String reason) {
    if (sessionService_ == null) return;
    for (AaSessionService.SessionEntry e : sessionService_.getSessionList()) {
        if (!e.isWireless) continue;
        Log.i(TAG, "Tearing down wireless session " + e.transportId + " (" + reason + ")");
        if (e.handle != 0) {
            sessionService_.disconnectSession(e.handle);
        } else {
            sessionService_.disconnectPending(e.transportId);
        }
    }
}
```

이미 `AaSessionService.disconnectSession(long handle)` 와 `disconnectPending(String transportId)` API 가 있음 — 그대로 활용.

### 4. 새 RFCOMM accept 시 race 회피 — 첫 session 보호

문제 2 의 race 가 해결하려면 새 RFCOMM accept 시 자동으로 첫 socket 을 강제 close 하지 말아야 함. 그러면 첫 keep-alive 가 살아 있음.

선택지 (a): **두 번째 RFCOMM 거부**
- listenLoop accept 후 `clientSocket_ != null` 또는 `handshakeThread_ != null && handshakeThread_.isAlive()` 면 즉시 `socket.close()` + log + listen 다음 iteration. 첫 keep-alive 보호.
- 이 모델은 "한 번에 한 phone" 가정. 우리는 이미 `AaSessionService.onWirelessDeviceReady` 에서 dedupe 로 같은 가정. consistent.

선택지 (b): **두 번째 RFCOMM 을 첫 client 으로 substitute**
- 첫 client socket 을 graceful 하게 종료 + 새 client 로 substitute. AaSessionService 에 새 session 으로 promote? 또는 transport id 가 같으면 dedupe 가 처리.
- 더 복잡. (a) 가 더 단순.

권고: **(a) — 두 번째 RFCOMM accept 즉시 거부**.

```java
while (!Thread.currentThread().isInterrupted()) {
    BluetoothSocket socket;
    try {
        socket = serverSocket_.accept();
    } catch (IOException e) { ... break; }

    // If we already have a live RFCOMM client / handshake, refuse the new
    // connection. Tearing down the existing client mid-keep-alive races
    // with its read thread and produces stuck duplicate sessions.
    if (clientSocket_ != null) {
        Log.w(TAG, "Second RFCOMM connection ignored — existing client still alive");
        try { socket.close(); } catch (IOException ignored) {}
        continue;
    }

    // ... 기존 path
}
```

### 5. 정리: 변경 파일 요약

| 파일 | 변경 |
|---|---|
| `app/android/src/main/java/com/aauto/app/wireless/BluetoothWirelessManager.java` | `Listener.onDeviceDisconnected` 추가, `waitForRfcommClose` 가 호출, `listenLoop` 가 두 번째 accept 거부 |
| `app/android/src/main/java/com/aauto/app/wireless/WirelessMonitorService.java` | `onDeviceDisconnected` 구현, `btStateReceiver_` / `apStateReceiver_` 가 BT off / SoftAP off 시 `teardownAllWirelessSessions` 호출 |
| `app/android/src/main/java/com/aauto/app/core/AaSessionService.java` | 변경 없음 (기존 `ACTION_WIRELESS_DEVICE_DETACHED` / `disconnectSession` / `disconnectPending` API 그대로 활용) |
| `app/android/src/main/java/com/aauto/app/BuildInfo.java` | +1 |

## 비목표

- 두 wireless phone 동시 지원 — 현재 dedupe 모델 그대로 유지 (한 번에 한 wireless device).
- TCP transport 의 backpressure 자체 해결 (`AndroidUsbTransport` / `TcpTransport` 의 `Send` 가 send mutex sync block 인 점) — 별도 step.
- AAP `BYEBYE_REQUEST` 송신 측 (HU → phone) — B4 가 수신 측만 처리. graceful disconnect 송신은 별도.
- phone-side 가 새 RFCOMM 을 보내는 trigger 자체 — phone 동작이라 우리 측 통제 밖.

## Verification

수동 시나리오:

1. **Stale 정리 — phone 측 BT 끊김**
   - HU + phone 무선 연결, AA 화면 진입.
   - phone 측 BT off.
   - 기대: 1~2초 내 MainActivity device list 에서 해당 wireless session 사라짐.

2. **Stale 정리 — phone 측 WiFi 끊김** (BT 는 살아있음)
   - HU + phone 무선 연결.
   - phone 측 WiFi off.
   - 기대: phone-side AA 가 RFCOMM 으로 disconnect 신호 보내거나 RFCOMM peer close → 자동 정리.

3. **Stale 정리 — HU 측 BT off**
   - HU 측에서 `MainActivity` 의 BT 토글로 BT off.
   - 기대: 모든 wireless session 정리, list 비워짐.

4. **Stale 정리 — HU 측 SoftAP off**
   - HU 측에서 SoftAP 토글 off.
   - 기대: 모든 wireless session 정리.

5. **RFCOMM 재연결 race 회피**
   - HU + phone 무선 연결 정상.
   - phone 측 BT 잠시 off → on (RFCOMM 새로 시도하도록 유도).
   - 기대: 두 번째 RFCOMM accept 시 "Second RFCOMM connection ignored" 로그. 첫 RFCOMM keep-alive 그대로 유지. 첫 native AAP session 영향 없음.

6. **회귀**: USB 정상 동작, 단일 wireless 연결 정상 동작.

## 진단 (필요 시)

만약 가설 (race 때문에 첫 session 깨짐) 이 fix 후에도 재현되면 다음 확인:
- `listenLoop` 새 accept 직전에 `clientSocket_` 가 null 인지 not-null 인지 로그.
- `waitForRfcommClose` exit 시점의 stack — externally close vs peer close 구분.
- native AAP session 의 transport disconnect 시점 vs RFCOMM disconnect 시점 비교.

## Step Log 예약

이 plan 적용 시 `0001_p1_compare.md` Step Log 에 추가:

- Step 11 — Wireless lifecycle (BluetoothWirelessManager + WirelessMonitorService) (BUILD ?)
