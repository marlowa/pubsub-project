// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>

#include <cstring>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include <pubsub_itc_fw/PduParser.hpp>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduParser::PduParser(ByteStreamInterface& stream, ApplicationThread& target_thread, ExpandableSlabAllocator& slab_allocator, QuillLogger& logger,
                     std::function<void()> disconnect_handler, ConnectionID connection_id)
    : stream_(stream)
    , target_thread_(target_thread)
    , slab_allocator_(slab_allocator)
    , logger_(logger)
    , disconnect_handler_(std::move(disconnect_handler))
    , connection_id_(connection_id) {}

std::tuple<bool, std::string> PduParser::receive() {
    for (;;) {
        // Phase 1: accumulate header bytes.
        if (!header_validated_) {
            const size_t header_space = sizeof(PduHeader) - header_bytes_;

            if (header_space > 0) {
                auto [bytes_read, error] = stream_.receive(utils::SimpleSpan<uint8_t>(header_buffer_ + header_bytes_, header_space));

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
            const auto* hdr = reinterpret_cast<const PduHeader*>(header_buffer_);
            if (ntohl(hdr->canary) != pdu_canary_value) {
                return {false, "PduParser: canary mismatch — wire corruption or framing error"};
            }

            current_payload_size_ = ntohl(hdr->byte_count);
            current_pdu_id_ = static_cast<int16_t>(ntohs(static_cast<uint16_t>(hdr->pdu_id)));
            current_seq_no_ = static_cast<int64_t>(be64toh(static_cast<uint64_t>(hdr->seq_no)));

            PUBSUB_LOG(logger_, FwLogLevel::Debug,
                       "TRACE PduParser::receive: connection_id={} decoded header: "
                       "canary=0x{:08x} byte_count={} pdu_id={} version={}",
                       connection_id_.get_value(), ntohl(hdr->canary), current_payload_size_, current_pdu_id_, static_cast<int>(hdr->version));

            PUBSUB_LOG(logger_, FwLogLevel::Debug,
                       "TRACE PduParser::receive: header bytes: "
                       "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} "
                       "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} "
                       "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                       header_buffer_[0],  header_buffer_[1],  header_buffer_[2],  header_buffer_[3],
                       header_buffer_[4],  header_buffer_[5],  header_buffer_[6],  header_buffer_[7],
                       header_buffer_[8],  header_buffer_[9],  header_buffer_[10], header_buffer_[11],
                       header_buffer_[12], header_buffer_[13], header_buffer_[14], header_buffer_[15],
                       header_buffer_[16], header_buffer_[17], header_buffer_[18], header_buffer_[19],
                       header_buffer_[20], header_buffer_[21], header_buffer_[22], header_buffer_[23]);

            if (current_payload_size_ == 0) {
                return {false, "PduParser: zero-length payload is not permitted"};
            }

            // Guard against payloads that exceed the inbound slab size before
            // calling allocate() — whose precondition is size <= slab_size().
            // Violating that precondition would throw PreconditionAssertion and
            // crash the reactor. Instead, we return a descriptive error so the
            // reactor tears down this connection cleanly and stays alive for all
            // other connections. The fix is to increase
            // ReactorConfiguration::inbound_slab_size.
            if (current_payload_size_ > slab_allocator_.slab_size()) {
                return {false, fmt::format("PduParser: inbound PDU payload of {} bytes exceeds "
                                           "inbound_slab_size of {} bytes — increase "
                                           "ReactorConfiguration::inbound_slab_size",
                                           current_payload_size_, slab_allocator_.slab_size())};
            }

            // Allocate a slab chunk to receive the payload directly — zero copy.
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "TRACE PduParser::receive: connection_id={} about to allocate {} bytes from inbound slab allocator",
                       connection_id_.get_value(), current_payload_size_);
            auto [slab_id, chunk] = slab_allocator_.allocate(current_payload_size_);
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "TRACE PduParser::receive: connection_id={} allocate returned slab_id={} chunk={}",
                       connection_id_.get_value(), slab_id, fmt::ptr(chunk));
            if (chunk == nullptr) {
                return {false, "PduParser: slab allocation failed for incoming PDU payload"};
            }

            payload_slab_id_ = slab_id;
            payload_chunk_ = chunk;
            payload_bytes_received_ = 0;
            header_validated_ = true;
        }

        // Phase 2: read payload bytes directly into the slab chunk.
        const size_t payload_remaining = current_payload_size_ - payload_bytes_received_;

        if (payload_remaining > 0) {
            uint8_t* dest = static_cast<uint8_t*>(payload_chunk_) + payload_bytes_received_;

            auto [bytes_read, error] = stream_.receive(utils::SimpleSpan<uint8_t>(dest, payload_remaining));

            if (bytes_read == 0 && error.empty()) {
                slab_allocator_.deallocate(payload_slab_id_, payload_chunk_);
                payload_chunk_ = nullptr;
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
                payload_chunk_ = nullptr;
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

        payload_chunk_ = nullptr;
        payload_slab_id_ = -1;
        payload_bytes_received_ = 0;
        reset_header();
    }
}

bool PduParser::has_complete_header() const {
    return header_bytes_ >= sizeof(PduHeader);
}

void PduParser::dispatch_pdu(int slab_id, void* payload_chunk) {
    const auto* payload = static_cast<const uint8_t*>(payload_chunk);
    const int payload_size = static_cast<int>(current_payload_size_);

    // Hex-dump up to the first 96 bytes of the payload so each hop's
    // wire content can be compared against what the encoder should
    // have produced. The header trace above already captured the
    // PduHeader; this captures what comes after it.
    //
    // The string is assembled only when Debug logging is actually enabled;
    // should_log_statement() is a cheap inline check on the Quill log level
    // so this guard costs nothing on Info/Warning-level production runs.
    if (logger_.quill_logger()->should_log_statement(quill::LogLevel::Debug)) {
        const int dump_limit = (payload_size < 96) ? payload_size : 96;
        std::string hex_bytes;
        hex_bytes.reserve(static_cast<size_t>(dump_limit) * 3);
        for (int i = 0; i < dump_limit; ++i) {
            hex_bytes += fmt::format("{:02x} ", payload[i]);
        }
        PUBSUB_LOG(logger_, FwLogLevel::Debug,
                   "TRACE PduParser::dispatch_pdu: connection_id={} pdu_id={} "
                   "payload_size={} payload_bytes (first {}): {}",
                   connection_id_.get_value(), current_pdu_id_, payload_size, dump_limit, hex_bytes);
    }

    EventMessage msg = EventMessage::create_framework_pdu_message(payload, payload_size, slab_id, connection_id_, current_pdu_id_, current_seq_no_);

    target_thread_.get_queue().enqueue(std::move(msg));
}

void PduParser::reset_header() {
    header_bytes_ = 0;
    header_validated_ = false;
    current_payload_size_ = 0;
    current_pdu_id_ = 0;
    current_seq_no_ = 0;
}

} // namespace pubsub_itc_fw
