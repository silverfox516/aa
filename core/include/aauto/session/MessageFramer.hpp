#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace aauto {
namespace session {

// Parsed, reassembled AAP message ready for dispatch.
// payload is always the raw (possibly encrypted) body — Session handles decryption.
struct AapMessage {
    uint8_t              channel;
    bool                 encrypted;
    std::vector<uint8_t> payload;
};

// Accumulates raw bytes from the transport, parses AAP packet headers,
// and reassembles multi-fragment messages.
//
// Thread-safety: NOT thread-safe. Must be called from a single thread (ProcessLoop).
class MessageFramer {
   public:
    using MessageCallback = std::function<void(AapMessage)>;

    // cb is invoked synchronously for each complete, reassembled message.
    explicit MessageFramer(MessageCallback cb);

    // Feed raw bytes. May invoke cb zero or more times.
    void Feed(const std::vector<uint8_t>& data);

   private:
    void ProcessBuffer();

    struct FragmentState {
        std::vector<uint8_t>                          data;
        bool                                          encrypted   = false;
        std::chrono::steady_clock::time_point         started_at  = {};
        bool                                          in_progress = false;
    };

    static constexpr size_t kMaxFragmentBytes = 4 * 1024 * 1024;  // 4 MiB
    static constexpr auto   kFragmentTimeout  = std::chrono::seconds(5);

    void EvictStaleFragments();

    MessageCallback callback_;
    std::vector<uint8_t> buffer_;
    size_t read_offset_ = 0;
    std::unordered_map<uint8_t, FragmentState> fragment_buffers_;
};

} // namespace session
} // namespace aauto
