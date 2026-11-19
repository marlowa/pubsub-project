// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <endian.h>

#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduFramer::PduFramer(ByteStreamInterface& stream) : stream_(stream), active_frame_ptr_{nullptr}, frame_size_{0}, send_offset_{0} {}

std::tuple<bool, std::string> PduFramer::send(int16_t pdu_id, int8_t version, int64_t seq_no, const uint8_t* payload, uint32_t size) {
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
        throw PreconditionAssertion("PduFramer::send: payload exceeds max_payload_size", __FILE__, __LINE__);
    }

    // Build the frame into the internal buffer.
    auto hdr = reinterpret_cast<PduHeader*>(frame_buffer_);
    hdr->byte_count = htonl(size);
    hdr->pdu_id = htons(static_cast<uint16_t>(pdu_id));
    hdr->version = version;
    hdr->filler_a = 0;
    hdr->seq_no = static_cast<int64_t>(htobe64(static_cast<uint64_t>(seq_no)));
    hdr->canary = htonl(pdu_canary_value);
    hdr->filler_b = 0;
    std::memcpy(frame_buffer_ + sizeof(PduHeader), payload, size);

    active_frame_ptr_ = frame_buffer_;
    frame_size_ = sizeof(PduHeader) + size;
    send_offset_ = 0;

    return continue_send();
}

std::tuple<bool, std::string> PduFramer::send_prebuilt(const uint8_t* frame, uint32_t total_bytes) {
    if (frame == nullptr) {
        throw PreconditionAssertion("PduFramer::send_prebuilt: frame must not be nullptr", __FILE__, __LINE__);
    }
    if (total_bytes <= sizeof(PduHeader)) {
        throw PreconditionAssertion("PduFramer::send_prebuilt: total_bytes must be greater than sizeof(PduHeader)", __FILE__, __LINE__);
    }
    if (has_pending_data()) {
        throw PreconditionAssertion("PduFramer::send_prebuilt: previous frame not yet fully sent", __FILE__, __LINE__);
    }

    // Zero-copy: point directly at the caller's slab chunk.
    active_frame_ptr_ = frame;
    frame_size_ = total_bytes;
    send_offset_ = 0;

    return continue_send();
}

PduFramer::GatheredOutcome PduFramer::send_gathered(const SimpleSpan<const uint8_t>* frames, size_t count) {
    if (frames == nullptr || count == 0 || count > ByteStreamInterface::max_gathered_parts) {
        throw PreconditionAssertion("PduFramer::send_gathered: between one and max_gathered_parts frames must be given", __FILE__, __LINE__);
    }
    if (has_pending_data()) {
        throw PreconditionAssertion("PduFramer::send_gathered: previous frame not yet fully sent", __FILE__, __LINE__);
    }
    for (size_t index = 0; index < count; ++index) {
        if (frames[index].size() <= sizeof(PduHeader)) {
            throw PreconditionAssertion("PduFramer::send_gathered: every frame must be larger than a PduHeader", __FILE__, __LINE__);
        }
    }

    GatheredOutcome outcome;
    auto [taken, error] = stream_.send_gathered(frames, count);
    if (taken < 0) {
        if (taken == -EAGAIN || taken == -EWOULDBLOCK) {
            return outcome;
        }
        if (taken == -ENOBUFS || taken == -ENOMEM) {
            outcome.short_of_memory = true;
            return outcome;
        }
        outcome.ok = false;
        outcome.error = error;
        return outcome;
    }

    // The stream took a prefix: as many whole frames as it covers, then perhaps part of the next.
    auto remaining = static_cast<size_t>(taken);
    while (outcome.written_whole < count && remaining >= frames[outcome.written_whole].size()) {
        remaining -= frames[outcome.written_whole].size();
        ++outcome.written_whole;
    }
    if (remaining > 0) {
        const SimpleSpan<const uint8_t>& cut = frames[outcome.written_whole];
        active_frame_ptr_ = cut.data();
        frame_size_ = cut.size();
        send_offset_ = remaining;
        outcome.front_cut = true;
    }
    return outcome;
}

std::tuple<bool, std::string> PduFramer::continue_send() {
    while (send_offset_ < frame_size_) {
        const uint8_t* buf = active_frame_ptr_ + send_offset_;
        const size_t remaining = frame_size_ - send_offset_;

        auto [bytes_sent, error] = stream_.send(SimpleSpan<const uint8_t>(buf, remaining));

        if (bytes_sent < 0) {
            if (bytes_sent == -EAGAIN || bytes_sent == -EWOULDBLOCK) {
                // Socket buffer full -- reactor must wait for EPOLLOUT.
                return {true, ""};
            }
            // Non-recoverable error -- reset state.
            active_frame_ptr_ = nullptr;
            frame_size_ = 0;
            send_offset_ = 0;
            return {false, error};
        }

        send_offset_ += static_cast<size_t>(bytes_sent);
    }

    // Frame fully sent -- reset state.
    active_frame_ptr_ = nullptr;
    frame_size_ = 0;
    send_offset_ = 0;
    return {true, ""};
}

bool PduFramer::has_pending_data() const {
    return send_offset_ < frame_size_;
}

} // namespaces
