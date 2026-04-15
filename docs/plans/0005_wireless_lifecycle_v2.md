# 0005 — Wireless lifecycle v2 (replaces plan 0003)

## Context

Wireless AA 는 **BT RFCOMM (control) + WiFi TCP (data)** 이중 transport 구조.
둘 다 살아 있어야 session 이 유효하다. 현재 코드에서 두 transport 의
lifecycle 이 완전히 독립적이라 한 쪽이 끊겨도 다른 쪽이 모르는 문제가 있다.

기존 plan 0003 의 범위 (lifecycle 신호 propagation + RFCOMM accept 거부 +
BT/AP off teardown) 에 다음을 추가:
- `clientSocket_` 소유권 정리 (race 근본 해결)
- RFCOMM close → TCP session 종료 (plan 0003 과 동일, 명시화)
- TCP session closed → RFCOMM keep-alive 종료 (신규 — 양방향)

## 이중 transport lifecycle invariant

```
Wireless session 유효 ⟺ RFCOMM alive ∧ TCP alive
  RFCOMM close → TCP session 종료해야
  TCP close   → RFCOMM keep-alive 종료해야
  BT off      → RFCOMM close → TCP session 종료
  SoftAP off  → TCP close    → RFCOMM close
```

## Design

### 1. BluetoothWirelessManager — Listener 확장 + socket 소유권 정리

#### Listener 변경

```java
interface Listener {
    void onDeviceConnecting(String deviceId, String deviceName);
    void onDeviceReady(String deviceId, String deviceName);
    void onConnectionFailed(String deviceId, String reason);
    void onDeviceDisconnected(String deviceId, String reason);  // NEW
}
```

`waitForRfcommClose` 종료 시 `listener.onDeviceDisconnected` 호출.

#### clientSocket_ 소유권

현재: `clientSocket_` 가 class field (volatile). listenLoop 과 handshakeThread_ 가 공유.

변경: `clientSocket_` 를 **handshakeThread_ 의 local scope** 로 이동.
listenLoop 가 accept 한 socket 을 handshake thread 에 인자로 넘기고, 이후
listenLoop 는 socket reference 를 갖지 않음. close 책임은 handshake thread 만.

```java
// listenLoop
BluetoothSocket socket = serverSocket_.accept();
// ... 검증 ...
listener_.onDeviceConnecting(deviceId, name);
Thread t = new Thread(() -> runSession(socket, deviceId, name));
// ...

// class field clientSocket_ 제거
```

`stop()` 이 socket 을 close 해야 할 때는 `serverSocket_.close()` 로 accept 를
unblock + handshake thread interrupt. handshake thread 가 socket close.

#### 두 번째 accept 거부

`handshakeThread_` 가 살아 있으면 새 accept 즉시 close + log + continue.
(plan 0003 과 동일)

#### RFCOMM keep-alive 종료 API

WirelessMonitorService 가 TCP session closed 시 RFCOMM 종료 요청할 수 있도록:

```java
public void closeCurrentClient() {
    Thread ht = handshakeThread_;
    if (ht != null) ht.interrupt();
}
```

interrupt → `waitForRfcommClose` 의 `in.read()` → IOException → close +
onDeviceDisconnected.

### 2. WirelessMonitorService — 양방향 lifecycle

#### RFCOMM → TCP (plan 0003 과 동일)

```java
@Override
public void onDeviceDisconnected(String deviceId, String reason) {
    // RFCOMM 끊김 → TCP session 도 정리
    Intent intent = new Intent(this, AaSessionService.class);
    intent.setAction(AaSessionService.ACTION_WIRELESS_DEVICE_DETACHED);
    intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
    startForegroundService(intent);
}
```

#### TCP → RFCOMM (신규)

AaSessionService 의 `onSessionClosedFromNative` 가 wireless session 에 대해
broadcast 하면 WirelessMonitorService 가 수신 → RFCOMM close:

```java
// AaSessionService (기존) — ACTION_SESSION_ENDED broadcast 시 deviceId 포함
// WirelessMonitorService (신규) — SESSION_ENDED receiver
private final BroadcastReceiver sessionEndReceiver_ = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (wirelessManager_ != null) {
            wirelessManager_.closeCurrentClient();
        }
    }
};
```

또는 WirelessMonitorService 가 AaSessionService binder 의 session list 를
polling 하지 않고, **SESSION_ENDED broadcast 를 직접 수신**.

#### BT off / SoftAP off teardown (plan 0003 과 동일)

`btStateReceiver_` STATE_TURNING_OFF → `wirelessManager_.stop()` +
`teardownAllWirelessSessions("BT off")`.

`apStateReceiver_` WIFI_AP_STATE_DISABLED → `teardownAllWirelessSessions("SoftAP off")`.

### 3. AaSessionService — SESSION_ENDED broadcast 에 deviceId 포함

현재 `broadcastSessionEnded(handle)` 는 handle 만 전달. wireless session 의
transportId (BT MAC) 를 extra 로 추가해 WirelessMonitorService 가 어떤 device 인지 식별:

```java
private void broadcastSessionEnded(long handle, String transportId, boolean isWireless) {
    Intent intent = new Intent(ACTION_SESSION_ENDED);
    intent.putExtra(EXTRA_SESSION_ENDED_HANDLE, handle);
    intent.putExtra(EXTRA_DEVICE_ID, transportId);
    intent.putExtra("is_wireless", isWireless);
    intent.setPackage(getPackageName());
    sendBroadcast(intent);
}
```

## 영향 파일

| 파일 | 변경 |
|---|---|
| `BluetoothWirelessManager.java` | Listener.onDeviceDisconnected 추가, clientSocket_ 제거 (local scope), closeCurrentClient() 추가, 두 번째 accept 거부 |
| `WirelessMonitorService.java` | onDeviceDisconnected 구현, sessionEndReceiver_ 추가 (TCP→RFCOMM), BT/AP off teardown, teardownAllWirelessSessions helper |
| `AaSessionService.java` | broadcastSessionEnded 에 transportId/isWireless extra 추가 |
| `BuildInfo.java` | +1 |

## Verification

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | wireless 연결 후 **phone BT off** | RFCOMM close → onDeviceDisconnected → TCP session 종료 → list 에서 사라짐 |
| 2 | wireless 연결 후 **phone WiFi off** | TCP close → onSessionClosedFromNative → SESSION_ENDED broadcast → RFCOMM close → list 에서 사라짐 |
| 3 | wireless 연결 후 **HU BT off** | wirelessManager.stop + teardownAllWirelessSessions → 모두 정리 |
| 4 | wireless 연결 후 **HU SoftAP off** | teardownAllWirelessSessions → TCP close → RFCOMM close |
| 5 | wireless 연결 중 **phone BT 잠시 off→on** | 두 번째 accept 거부 + 기존 session (RFCOMM 끊김으로 종료) 정리 후 새 handshake 가능 |
| 6 | **회귀** — 정상 wireless 연결/해제 cycle | 깨끗한 connect → use → disconnect → reconnect |
| 7 | **회귀** — USB 정상 동작 | 영향 없음 |
