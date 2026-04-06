// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstring>

#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduParser::PduParser(ByteStreamInterface& stream,
                     ApplicationThread& target_thread,
                     std::function<void()> disconnect_handler)
    : stream_(stream)
    , target_thread_(target_thread)
    , disconnect_handler_(std::move(disconnect_handler))
    , bytes_in_buffer_{0}
    , current_payload_size_{0}
    , header_validated_{false}
{
}

std::tuple<bool, std::string> PduParser::receive()
{
    for (;;) {
        // Fill the buffer with as many bytes as the socket will give us.
        const size_t space = receive_buffer_size - bytes_in_buffer_;
        if (space == 0) {
            // Buffer is full — should not happen with correctly sized PDUs.
            return {false, "PduParser: receive buffer full — PDU exceeds maximum size"};
        }

        auto [bytes_read, error] = stream_.receive(
            utils::SimpleSpan<uint8_t>(buffer_ + bytes_in_buffer_, space));

        if (bytes_read == 0 && error.empty()) {
            // Peer closed the connection gracefully.
            if (disconnect_handler_ != nullptr) {
                disconnect_handler_();
            }
            return {false, ""};
        }

        if (bytes_read < 0) {
            if (bytes_read == -EAGAIN || bytes_read == -EWOULDBLOCK) {
                // No more data available right now — done for this epoll event.
                return {true, ""};
            }
            return {false, error};
        }

        bytes_in_buffer_ += static_cast<size_t>(bytes_read);

        // Dispatch as many complete frames as the buffer contains.
        for (;;) {
            if (!header_validated_) {
                if (!has_complete_header()) {
                    break; // Need more data.
                }

                // Validate the canary.
                const PduHeader* hdr = reinterpret_cast<const PduHeader*>(buffer_);
                if (ntohl(hdr->canary) != pdu_canary_value) {
                    return {false, "PduParser: canary mismatch — wire corruption or framing error"};
                }

                current_payload_size_ = ntohl(hdr->byte_count);

                if (current_payload_size_ > max_payload_size) {
                    return {false, "PduParser: payload size exceeds maximum"};
                }

                header_validated_ = true;
            }

            if (!has_complete_payload()) {
                break; // Need more data.
            }

            dispatch_pdu();
            consume_frame();
        }
    }
}

bool PduParser::has_complete_header() const
{
    return bytes_in_buffer_ >= sizeof(PduHeader);
}

bool PduParser::has_complete_payload() const
{
    return bytes_in_buffer_ >= sizeof(PduHeader) + current_payload_size_;
}

void PduParser::dispatch_pdu()
{
    const PduHeader* hdr = reinterpret_cast<const PduHeader*>(buffer_);
    const uint8_t* payload = buffer_ + sizeof(PduHeader);
    const int payload_size = static_cast<int>(current_payload_size_);

    EventMessage msg = EventMessage::create_framework_pdu_message(payload, payload_size);
    target_thread_.get_queue().enqueue(std::move(msg));

    // Suppress unused variable warning when pdu_id is not yet used.
    [[maybe_unused]] int16_t pdu_id = static_cast<int16_t>(ntohs(
        static_cast<uint16_t>(hdr->pdu_id)));
}

void PduParser::consume_frame()
{
    const size_t frame_size = sizeof(PduHeader) + current_payload_size_;
    const size_t remaining  = bytes_in_buffer_ - frame_size;

    if (remaining > 0) {
        std::memmove(buffer_, buffer_ + frame_size, remaining);
    }

    bytes_in_buffer_      = remaining;
    current_payload_size_ = 0;
    header_validated_     = false;
}

void PduParser::reset_buffer()
{
    bytes_in_buffer_      = 0;
    current_payload_size_ = 0;
    header_validated_     = false;
}

} // namespace pubsub_itc_fw
