#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace aauto {
namespace session {

// One AAP frame as it arrives off the transport. The framer does NOT do
// reassembly or decryption — those are the caller's responsibility, matching
// aasdk's per-fragment cryptor->decrypt() model. Each fragment carries its
// own first/last flags so the caller can accumulate plaintext per channel
// and dispatch on the LAST/BULK fragment.
struct AapFragment {
    uint8_t              channel;
    bool                 is_first;
    bool                 is_last;
    bool                 encrypted;
    std::vector<uint8_t> ciphertext;  // raw payload (4-byte total_size already skipped)
};

// Splits a raw byte stream from the transport into individual AAP frames.
// Stateless beyond the inbound byte buffer — no cross-channel reassembly.
//
// Thread-safety: NOT thread-safe. Must be called from a single thread (ProcessLoop).
class MessageFramer {
   public:
    using FragmentCallback = std::function<void(AapFragment)>;

    // cb is invoked synchronously for each complete frame parsed from the buffer.
    explicit MessageFramer(FragmentCallback cb);

    // Feed raw bytes. May invoke cb zero or more times.
    void Feed(const std::vector<uint8_t>& data);

   private:
    void ProcessBuffer();

    FragmentCallback callback_;
    std::vector<uint8_t> buffer_;
    size_t read_offset_ = 0;
};

} // namespace session
} // namespace aauto
