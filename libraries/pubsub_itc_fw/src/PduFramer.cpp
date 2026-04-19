// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstring>

#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduFramer::PduFramer(ByteStreamInterface& stream)
    : stream_(stream)
    , active_frame_ptr_{nullptr}
    , frame_size_{0}
    , send_offset_{0}
{
}

std::tuple<bool, std::string> PduFramer::send(int16_t pdu_id, int8_t version,
                                               const uint8_t* payload, uint32_t size)
{
    if (payload == nullptr) {
        throw PreconditionAssertion("PduFramer::send: payload must not be nullptr",
                                    __FILE__, __LINE__);
    }
    if (size == 0) {
        throw PreconditionAssertion("PduFramer::send: size must be greater than zero",
                                    __FILE__, __LINE__);
    }
    if (has_pending_data()) {
        throw PreconditionAssertion("PduFramer::send: previous frame not yet fully sent",
                                    __FILE__, __LINE__);
    }
    if (sizeof(PduHeader) + size > frame_buffer_size) {
        throw PreconditionAssertion("PduFramer::send: payload exceeds max_payload_size",
                                    __FILE__, __LINE__);
    }

    // Build the frame into the internal buffer.
    auto  hdr = reinterpret_cast<PduHeader*>(frame_buffer_);
    hdr->byte_count = htonl(size);
    hdr->pdu_id     = htons(static_cast<uint16_t>(pdu_id));
    hdr->version    = version;
    hdr->filler_a   = 0;
    hdr->canary     = htonl(pdu_canary_value);
    hdr->filler_b   = 0;
    std::memcpy(frame_buffer_ + sizeof(PduHeader), payload, size);

    active_frame_ptr_ = frame_buffer_;
    frame_size_       = sizeof(PduHeader) + size;
    send_offset_      = 0;

    return continue_send();
}

std::tuple<bool, std::string> PduFramer::send_prebuilt(const uint8_t* frame,
                                                        uint32_t total_bytes)
{
    if (frame == nullptr) {
        throw PreconditionAssertion("PduFramer::send_prebuilt: frame must not be nullptr",
                                    __FILE__, __LINE__);
    }
    if (total_bytes <= sizeof(PduHeader)) {
        throw PreconditionAssertion(
            "PduFramer::send_prebuilt: total_bytes must be greater than sizeof(PduHeader)",
            __FILE__, __LINE__);
    }
    if (has_pending_data()) {
        throw PreconditionAssertion(
            "PduFramer::send_prebuilt: previous frame not yet fully sent",
            __FILE__, __LINE__);
    }

    // Zero-copy: point directly at the caller's slab chunk.
    active_frame_ptr_ = frame;
    frame_size_       = total_bytes;
    send_offset_      = 0;

    return continue_send();
}

std::tuple<bool, std::string> PduFramer::continue_send()
{
    while (send_offset_ < frame_size_) {
        const uint8_t* buf       = active_frame_ptr_ + send_offset_;
        const size_t   remaining = frame_size_ - send_offset_;

        auto [bytes_sent, error] = stream_.send(
            utils::SimpleSpan<const uint8_t>(buf, remaining));

        if (bytes_sent < 0) {
            if (bytes_sent == -EAGAIN || bytes_sent == -EWOULDBLOCK) {
                // Socket buffer full — reactor must wait for EPOLLOUT.
                return {true, ""};
            }
            // Non-recoverable error — reset state.
            active_frame_ptr_ = nullptr;
            frame_size_        = 0;
            send_offset_       = 0;
            return {false, error};
        }

        send_offset_ += static_cast<size_t>(bytes_sent);
    }

    // Frame fully sent — reset state.
    active_frame_ptr_ = nullptr;
    frame_size_        = 0;
    send_offset_       = 0;
    return {true, ""};
}

bool PduFramer::has_pending_data() const
{
    return send_offset_ < frame_size_;
}

} // namespace pubsub_itc_fw
