#include "aauto/session/MessageFramer.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"

#define LOG_TAG "AA.CORE.Framer"

namespace aauto {
namespace session {

MessageFramer::MessageFramer(FragmentCallback cb) : callback_(std::move(cb)) {}

void MessageFramer::Feed(const std::vector<uint8_t>& data) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    ProcessBuffer();
    // Compact consumed bytes
    if (read_offset_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + read_offset_);
        read_offset_ = 0;
    }
}

void MessageFramer::ProcessBuffer() {
    while (buffer_.size() - read_offset_ >= aap::HEADER_SIZE) {
        const uint8_t* hdr = buffer_.data() + read_offset_;

        uint8_t  channel      = hdr[0];
        uint8_t  flags        = hdr[1];
        bool     is_first     = (flags & 0x01) != 0;
        bool     is_last      = (flags & 0x02) != 0;
        bool     is_encrypted = (flags & 0x08) != 0;
        bool     is_multi_first = is_first && !is_last;

        uint16_t payload_len  = (hdr[2] << 8) | hdr[3];
        size_t   extra_skip   = is_multi_first ? 4 : 0;
        size_t   total_packet = aap::HEADER_SIZE + extra_skip + payload_len;

        // Bound check after extra_skip is added — multi-first frames carry an
        // additional 4-byte total_size after the header that the previous
        // implementation forgot to include in the buffer-size guard.
        if (buffer_.size() - read_offset_ < total_packet) break;

        const uint8_t* payload_ptr = hdr + aap::HEADER_SIZE + extra_skip;

        AapFragment frag;
        frag.channel    = channel;
        frag.is_first   = is_first;
        frag.is_last    = is_last;
        frag.encrypted  = is_encrypted;
        frag.ciphertext.assign(payload_ptr, payload_ptr + payload_len);

        read_offset_ += total_packet;

        callback_(std::move(frag));
    }
}

} // namespace session
} // namespace aauto
