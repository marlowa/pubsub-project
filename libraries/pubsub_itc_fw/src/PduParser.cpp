// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstring>

#include <fmt/format.h>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduParser::PduParser(ByteStreamInterface& stream,
                     ApplicationThread& target_thread,
                     ExpandableSlabAllocator& slab_allocator,
                     std::function<void()> disconnect_handler)
    : stream_(stream)
    , target_thread_(target_thread)
    , slab_allocator_(slab_allocator)
    , disconnect_handler_(std::move(disconnect_handler))
    , header_bytes_{0}
    , header_validated_{false}
    , current_payload_size_{0}
    , current_pdu_id_{0}
    , payload_chunk_{nullptr}
    , payload_slab_id_{-1}
    , payload_bytes_received_{0}
{
}

std::tuple<bool, std::string> PduParser::receive()
{
    for (;;) {
        // Phase 1: accumulate header bytes.
        if (!header_validated_) {
            const size_t header_space = sizeof(PduHeader) - header_bytes_;

            if (header_space > 0) {
                auto [bytes_read, error] = stream_.receive(
                    utils::SimpleSpan<uint8_t>(header_buffer_ + header_bytes_, header_space));

                if (bytes_read == 0 && error.empty()) {
                    if (disconnect_handler_ != nullptr) {
                        disconnect_handler_();
                    }
                    return {false, ""};
                }

                if (bytes_read < 0) {
                    if (bytes_read == -EAGAIN || bytes_read == -EWOULDBLOCK) {
                        return {true, ""};
                    }
                    return {false, error};
                }

                header_bytes_ += static_cast<size_t>(bytes_read);
            }

            if (!has_complete_header()) {
                return {true, ""}; // Need more data; wait for next epoll event.
            }

            // Full header received — validate canary.
            const PduHeader* hdr = reinterpret_cast<const PduHeader*>(header_buffer_);
            if (ntohl(hdr->canary) != pdu_canary_value) {
                return {false, "PduParser: canary mismatch — wire corruption or framing error"};
            }

            current_payload_size_ = ntohl(hdr->byte_count);
            current_pdu_id_ = static_cast<int16_t>(ntohs(static_cast<uint16_t>(hdr->pdu_id)));

            if (current_payload_size_ == 0) {
                return {false, "PduParser: zero-length payload is not permitted"};
            }

            // Guard against payloads that exceed the inbound slab size before
            // calling allocate() — whose precondition is size <= slab_size().
            // Violating that precondition would throw PreconditionAssertion and
            // crash the reactor. Instead we return a descriptive error so the
            // reactor tears down this connection cleanly and stays alive for all
            // other connections. The fix is to increase
            // ReactorConfiguration::inbound_slab_size.
            if (current_payload_size_ > slab_allocator_.slab_size()) {
                return {false, fmt::format(
                    "PduParser: inbound PDU payload of {} bytes exceeds "
                    "inbound_slab_size of {} bytes — increase "
                    "ReactorConfiguration::inbound_slab_size",
                    current_payload_size_, slab_allocator_.slab_size())};
            }

            // Allocate a slab chunk to receive the payload directly — zero copy.
            auto [slab_id, chunk] = slab_allocator_.allocate(current_payload_size_);
            if (chunk == nullptr) {
                return {false, "PduParser: slab allocation failed for incoming PDU payload"};
            }

            payload_slab_id_        = slab_id;
            payload_chunk_          = chunk;
            payload_bytes_received_ = 0;
            header_validated_       = true;
        }

        // Phase 2: read payload bytes directly into the slab chunk.
        const size_t payload_remaining =
            current_payload_size_ - payload_bytes_received_;

        if (payload_remaining > 0) {
            uint8_t* dest = static_cast<uint8_t*>(payload_chunk_) + payload_bytes_received_;

            auto [bytes_read, error] = stream_.receive(
                utils::SimpleSpan<uint8_t>(dest, payload_remaining));

            if (bytes_read == 0 && error.empty()) {
                slab_allocator_.deallocate(payload_slab_id_, payload_chunk_);
                payload_chunk_   = nullptr;
                payload_slab_id_ = -1;
                if (disconnect_handler_ != nullptr) {
                    disconnect_handler_();
                }
                return {false, ""};
            }

            if (bytes_read < 0) {
                if (bytes_read == -EAGAIN || bytes_read == -EWOULDBLOCK) {
                    return {true, ""}; // Partial payload — resume on next epoll event.
                }
                slab_allocator_.deallocate(payload_slab_id_, payload_chunk_);
                payload_chunk_   = nullptr;
                payload_slab_id_ = -1;
                return {false, error};
            }

            payload_bytes_received_ += static_cast<size_t>(bytes_read);
        }

        if (payload_bytes_received_ < current_payload_size_) {
            return {true, ""}; // Payload not yet complete.
        }

        // Complete frame — dispatch to target thread.
        dispatch_pdu(payload_slab_id_, payload_chunk_);

        payload_chunk_          = nullptr;
        payload_slab_id_        = -1;
        payload_bytes_received_ = 0;
        reset_header();
    }
}

bool PduParser::has_complete_header() const
{
    return header_bytes_ >= sizeof(PduHeader);
}

void PduParser::dispatch_pdu(int slab_id, void* payload_chunk)
{
    const uint8_t* payload    = static_cast<const uint8_t*>(payload_chunk);
    const int      payload_size = static_cast<int>(current_payload_size_);

    EventMessage msg = EventMessage::create_framework_pdu_message(
        payload, payload_size, slab_id);

    target_thread_.get_queue().enqueue(std::move(msg));
}

void PduParser::reset_header()
{
    header_bytes_         = 0;
    header_validated_     = false;
    current_payload_size_ = 0;
    current_pdu_id_       = 0;
}

} // namespace pubsub_itc_fw
