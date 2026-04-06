// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstring>

#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduFramer::PduFramer(ByteStreamInterface& stream)
    : stream_(stream)
    , frame_size_{0}
    , send_offset_{0}
{
}

std::tuple<bool, std::string> PduFramer::send(int16_t pdu_id, int8_t version,
                                               const uint8_t* payload, uint32_t size)
{
    if (payload == nullptr) {
        throw PreconditionAssertion("PduFramer::send: payload must not be nullptr", __FILE__, __LINE__);
    }
    if (size == 0) {
        throw PreconditionAssertion("PduFramer::send: size must be greater than zero", __FILE__, __LINE__);
    }
    if (has_pending_data()) {
        throw PreconditionAssertion("PduFramer::send: previous frame not yet fully sent", __FILE__, __LINE__);
    }
    if (sizeof(PduHeader) + size > frame_buffer_size) {
        throw PreconditionAssertion("PduFramer::send: payload exceeds maximum frame buffer size", __FILE__, __LINE__);
    }

    // Write the header in network byte order.
    PduHeader* hdr = reinterpret_cast<PduHeader*>(frame_buffer_);
    hdr->byte_count = htonl(size);
    hdr->pdu_id     = htons(static_cast<uint16_t>(pdu_id));
    hdr->version    = version;
    hdr->filler_a   = 0;
    hdr->canary     = htonl(pdu_canary_value);
    hdr->filler_b   = 0;

    // Copy the payload immediately after the header.
    std::memcpy(frame_buffer_ + sizeof(PduHeader), payload, size);
    frame_size_   = sizeof(PduHeader) + size;
    send_offset_  = 0;

    return continue_send();
}

std::tuple<bool, std::string> PduFramer::continue_send()
{
    while (send_offset_ < frame_size_) {
        const uint8_t* buf  = frame_buffer_ + send_offset_;
        const size_t   remaining = frame_size_ - send_offset_;

        auto [bytes_sent, error] = stream_.send(
            utils::SimpleSpan<const uint8_t>(buf, remaining));

        if (bytes_sent < 0) {
            // EAGAIN/EWOULDBLOCK — socket buffer full, retry when writable.
            if (bytes_sent == -EAGAIN || bytes_sent == -EWOULDBLOCK) {
                return {true, ""};
            }
            // Non-recoverable error.
            frame_size_  = 0;
            send_offset_ = 0;
            return {false, error};
        }

        send_offset_ += static_cast<size_t>(bytes_sent);
    }

    // Frame fully sent.
    frame_size_  = 0;
    send_offset_ = 0;
    return {true, ""};
}

bool PduFramer::has_pending_data() const
{
    return send_offset_ < frame_size_;
}

} // namespace pubsub_itc_fw
