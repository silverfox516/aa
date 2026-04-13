# 0004 — Session send queue

## Context

현재 HU → phone 송신은 `Session::SendEncrypted` 가 호출자의 thread 에서 직접 `crypto.Encrypt → transport.Send` 를 동기 호출한다. `transport.Send` 는 `send_mutex_` 로 직렬화되어 있어 여러 thread 가 동시에 보내면 경합이 발생한다.

실제 sender thread:
- **process thread** — Media ACK (~80/s), control 응답, video focus 등
- **heartbeat thread** — ping (0.2 Hz)
- **sensor worker thread** — location fix (1 Hz, LOC1)

process thread 가 이미 80 send/s 를 처리하는 상태에서 sensor thread 가 send_mutex_ 를 잡으면 process thread 의 ACK 가 밀린다. sensor thread 의 USB write 가 1초 timeout 되면 send_mutex_ 가 15초 점유되어 모든 ACK 가 막힘 → phone-side stream stall → USB endpoint dead (feedback loop, 40분~2분 후 connection lost).

이전 시도:
- TrySend (non-blocking trylock + 10ms USB timeout) — 역효과. drop 비율 높아져 phone-side 가 sensor stream stale 로 판단, endpoint 더 빨리 dead.
- Process thread piggyback (SetPendingFix + FlushPendingFix) — sensor 만 해결, 근본 구조 안 바뀜.

## Design

Receive 쪽이 이미 queue 모델 (`ReceiveLoop → message_queue_ → ProcessLoop`). Send 쪽도 같은 패턴:

```
모든 sender (process / sensor / heartbeat)
    │ SendEncrypted(ch, type, payload) → send_queue_ push (non-blocking)
    ▼
send_queue_ (bounded deque, mutex + condition_variable)
    │ send_thread_ pop (wait)
    ▼
crypto.Encrypt → transport.Send (blocking — 이 thread 만 transport 접근)
```

### Session 멤버 추가

```cpp
// Session.hpp
std::mutex                            send_queue_mutex_;
std::condition_variable               send_queue_cv_;
std::deque<SendItem>                  send_queue_;
std::thread                           send_thread_;

struct SendItem {
    uint8_t              channel;
    uint16_t             msg_type;
    std::vector<uint8_t> payload;   // plaintext (type prefix + service payload)
};
```

### SendEncrypted 변경

현재:
```cpp
bool Session::SendEncrypted(ch, type, payload) {
    auto packet = Pack(ch, type, payload);
    auto encrypted = crypto_->EncryptData(plain);
    return transport_->Send(encrypted_packet);  // blocking
}
```

변경:
```cpp
bool Session::SendEncrypted(ch, type, payload) {
    if (state_ == DISCONNECTED) return false;
    auto packet = Pack(ch, type, payload);
    std::vector<uint8_t> plain(packet.begin() + HEADER_SIZE, packet.end());

    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        if (send_queue_.size() >= kMaxSendQueueSize) {
            AA_LOG_W() << "Send queue full, dropping";
            return false;
        }
        send_queue_.push_back({ch, type, std::move(plain)});
    }
    send_queue_cv_.notify_one();
    return true;
}
```

호출자는 queue push 후 즉시 반환. **non-blocking**.

### SendLoop (새 thread)

```cpp
void Session::SendLoop() {
    while (state_ != DISCONNECTED) {
        SendItem item;
        {
            std::unique_lock<std::mutex> lock(send_queue_mutex_);
            send_queue_cv_.wait(lock, [this] {
                return !send_queue_.empty() || state_ == DISCONNECTED;
            });
            if (state_ == DISCONNECTED && send_queue_.empty()) break;
            item = std::move(send_queue_.front());
            send_queue_.pop_front();
        }

        auto encrypted = crypto_->EncryptData(item.plain);
        // Rebuild AAP header with encrypted length
        uint16_t enc_len = static_cast<uint16_t>(encrypted.size());
        std::vector<uint8_t> packet(aap::HEADER_SIZE + enc_len);
        packet[0] = item.channel;
        packet[1] = aap::ComputeFlags(item.channel, item.msg_type);
        packet[2] = (enc_len >> 8) & 0xFF;
        packet[3] =  enc_len       & 0xFF;
        std::copy(encrypted.begin(), encrypted.end(), packet.begin() + aap::HEADER_SIZE);

        if (!transport_->Send(packet)) {
            AA_LOG_E() << "[SendLoop] transport.Send failed";
            // Transport failure — session will be torn down by ReceiveLoop or ping timeout.
        }
    }
}
```

### Queue 정책

- `kMaxSendQueueSize = 256` — bounded. ACK 80/s + ping + sensor = ~82/s. 256 items ≈ 3초 burst buffer. 
- queue full 시 oldest drop 또는 newest drop. ACK 는 drop 되면 안 됨 (phone-side max_unacked stall). **newest reject** (push 실패) 가 더 안전 — 오래된 ACK 가 먼저 나감.
- 또는 sensor 만 latest-only slot. ACK/ping 은 항상 enqueue. 이건 queue item 에 priority 구분 필요 — 복잡. **일단 uniform queue + bounded + newest reject** 로 시작. 문제 생기면 priority 추가.

### SensorService 변경

piggyback 모델 (SetPendingFix / FlushPendingFix) **되돌리기**. 원래 `SendLocationFix` → `send_cb_` (= `SendEncrypted`) 직접 호출로 복원. SendEncrypted 가 이제 non-blocking queue push 이므로 sensor thread 가 block 안 됨.

### Thread lifecycle

```
Session::Start():
    ... handshake ...
    send_thread_ = std::thread(&Session::SendLoop, this);
    receive_thread_ = ...
    process_thread_ = ...

Session::Stop():
    state_ = DISCONNECTED;
    send_queue_cv_.notify_all();   // wake SendLoop
    ... join all threads ...
```

### Encryption thread-safety

`crypto_->EncryptData` 는 `CryptoManager::mutex_` 로 보호됨 (기존). SendLoop 만 호출하므로 단일 consumer — CryptoManager 의 mutex 경합 감소.

## 영향 파일

| 파일 | 변경 |
|---|---|
| `core/include/aauto/session/Session.hpp` | `SendItem` struct, `send_queue_*` 멤버, `send_thread_`, `SendLoop` 선언. `TrySendEncrypted` 제거. |
| `core/src/session/Session.cpp` | `SendEncrypted` 를 queue push 로, `SendLoop` 구현, `Start`/`Stop` 에 thread lifecycle, ProcessLoop 의 FlushPendingFix 호출 제거. |
| `core/include/aauto/service/SensorService.hpp` | piggyback 멤버들 (fix_mutex_, fix_pending_, ...) 제거. `SendLocationFix` 복원. |
| `core/src/service/SensorService.cpp` | `SetPendingFix`/`FlushPendingFix`/`DoSendLocationBatch` → `SendLocationFix` 복원 (원래 blocking send_cb_ 호출, 이제 non-blocking). |
| `app/android/jni/aauto_jni.cpp` | `SetPendingFix` → `SendLocationFix` 복원. |
| `app/android/src/main/java/com/aauto/app/BuildInfo.java` | +1 |

Transport layer (AndroidUsbTransport, TcpTransport, ITransport) — **변경 없음**. TrySend/SendLocked 추가했던 것은 되돌림 (사용 안 하므로).

## 비목표

- Queue item 별 priority (ACK > sensor > ping). 현재 uniform queue. 문제 생기면 별도 step.
- Transport layer queue (transport 는 blocking Send 그대로 유지).
- Encryption pipeline (encrypt 를 queue push 전에 vs 후에). 현재 SendLoop 에서 encrypt.

## Verification

1. 빌드 통과.
2. 로그: `LocationFix stats: pended=N sent=N` (pended ≈ sent, drop 거의 0).
3. aging 10시간+ — USB write timeout 없음.
4. 회귀: 일반 동작 (audio, video, touch, navigation) 정상.
5. queue full log (`Send queue full, dropping`) 가 정상 동작 중 안 찍힘.
