#include "aauto/session/MessageFramer.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"

#define LOG_TAG "AA.Framer"

namespace aauto {
namespace session {

MessageFramer::MessageFramer(MessageCallback cb) : callback_(std::move(cb)) {}

void MessageFramer::Feed(const std::vector<uint8_t>& data) {
    EvictStaleFragments();
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    ProcessBuffer();
    // Compact consumed bytes
    if (read_offset_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + read_offset_);
        read_offset_ = 0;
    }
}

void MessageFramer::EvictStaleFragments() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = fragment_buffers_.begin(); it != fragment_buffers_.end(); ) {
        auto& frag = it->second;
        if (frag.in_progress && (now - frag.started_at) > kFragmentTimeout) {
            AA_LOG_W() << "[MessageFramer] Ch:" << (int)it->first
                       << " multi-fragment timeout — dropping " << frag.data.size() << " bytes";
            it = fragment_buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

void MessageFramer::ProcessBuffer() {
    while (buffer_.size() - read_offset_ >= aap::HEADER_SIZE) {
        const uint8_t* hdr = buffer_.data() + read_offset_;

        uint16_t payload_len     = (hdr[2] << 8) | hdr[3];
        size_t   aap_packet_len  = aap::HEADER_SIZE + payload_len;

        if (buffer_.size() - read_offset_ < aap_packet_len) break;

        uint8_t channel      = hdr[0];
        uint8_t flags        = hdr[1];
        bool    is_first     = (flags & 0x01) != 0;
        bool    is_last      = (flags & 0x02) != 0;
        bool    is_encrypted = (flags & 0x08) != 0;

        // Multi-fragment first packet carries a 4-byte total_size field after the header
        bool   is_multi_first = is_first && !is_last;
        size_t extra_skip     = is_multi_first ? 4 : 0;
        aap_packet_len        += extra_skip;

        const uint8_t* payload_ptr  = hdr + aap::HEADER_SIZE + extra_skip;
        size_t         payload_size = payload_len;

        auto& frag = fragment_buffers_[channel];
        if (is_first) {
            frag.data.clear();
            frag.encrypted    = is_encrypted;
            frag.in_progress  = !is_last;  // true only for multi-fragment
            frag.started_at   = std::chrono::steady_clock::now();
        }

        // Guard against unbounded accumulation
        if (frag.data.size() + payload_size > kMaxFragmentBytes) {
            AA_LOG_E() << "[MessageFramer] Ch:" << (int)channel
                       << " fragment size exceeded (" << kMaxFragmentBytes << " bytes) — dropping";
            fragment_buffers_.erase(channel);
            read_offset_ += aap_packet_len;
            continue;
        }

        frag.data.insert(frag.data.end(), payload_ptr, payload_ptr + payload_size);

        read_offset_ += aap_packet_len;

        if (!is_last) continue;  // wait for more fragments

        // Full message assembled — fire callback
        frag.in_progress = false;
        callback_(AapMessage{channel, frag.encrypted, std::move(frag.data)});
        frag.data.clear();
    }
}

} // namespace session
} // namespace aauto
