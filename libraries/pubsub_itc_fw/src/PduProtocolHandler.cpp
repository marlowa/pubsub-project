// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>

#include <string>
#include <tuple>

#include <pubsub_itc_fw/PduProtocolHandler.hpp>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

PduProtocolHandler::PduProtocolHandler(TcpSocket& socket, ApplicationThread& target_thread, ExpandableSlabAllocator& inbound_allocator, QuillLogger& logger,
                                       ConnectionID connection_id) {
    framer_ = std::make_unique<PduFramer>(socket);
    // nullptr is safe: InboundConnectionManager::on_data_ready checks the receive() return value
    // to detect disconnects and drives teardown via that path.
    parser_ = std::make_unique<PduParser>(socket, target_thread, inbound_allocator, logger, nullptr, connection_id);
}

std::tuple<bool, std::string, bool> PduProtocolHandler::on_data_ready() {
    auto [ok, error] = parser_->receive();
    return {ok, error, ok && parser_->stopped_for_messages_waiting()};
}

bool PduProtocolHandler::is_reads_paused() const {
    return parser_->stopped_for_messages_waiting();
}

bool PduProtocolHandler::counts_messages_waiting() const {
    return parser_->counts_messages_waiting();
}

bool PduProtocolHandler::limit_messages_waiting(std::shared_ptr<MessagesWaitingLimit> limit) {
    parser_->set_messages_waiting_limit(std::move(limit));
    return true;
}

std::tuple<bool, std::string> PduProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator, SlabHandle slab_id, void* chunk_ptr, uint32_t total_bytes) {
    if (allocator == nullptr) {
        throw PreconditionAssertion("PduProtocolHandler::send_prebuilt: allocator must not be nullptr", __FILE__, __LINE__);
    }
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion("PduProtocolHandler::send_prebuilt: chunk_ptr must not be nullptr", __FILE__, __LINE__);
    }

    current_allocator_ = allocator;
    current_slab_id_ = slab_id;
    current_chunk_ptr_ = chunk_ptr;
    current_total_bytes_ = total_bytes;

    const auto* frame = static_cast<const uint8_t*>(chunk_ptr);
    auto [success, error] = framer_->send_prebuilt(frame, total_bytes);
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
    return {true, ""};
}

PduFramer::GatheredOutcome PduProtocolHandler::send_gathered(const WaitingSend* sends, size_t count) {
    if (sends == nullptr || count == 0 || count > ByteStreamInterface::max_gathered_parts) {
        throw PreconditionAssertion("PduProtocolHandler::send_gathered: between one and max_gathered_parts sends must be given", __FILE__, __LINE__);
    }
    std::array<SimpleSpan<const uint8_t>, ByteStreamInterface::max_gathered_parts> frames{};
    for (size_t index = 0; index < count; ++index) {
        frames[index] = SimpleSpan<const uint8_t>(static_cast<const uint8_t*>(sends[index].chunk), sends[index].byte_count);
    }
    const PduFramer::GatheredOutcome outcome = framer_->send_gathered(frames.data(), count);
    for (size_t index = 0; index < outcome.written_whole; ++index) {
        sends[index].allocator->deallocate(sends[index].slab_id, sends[index].chunk);
    }
    if (outcome.front_cut) {
        // Held as the send in progress, exactly as send_prebuilt() holds one it could not finish.
        const WaitingSend& cut = sends[outcome.written_whole];
        current_allocator_ = cut.allocator;
        current_slab_id_ = cut.slab_id;
        current_chunk_ptr_ = cut.chunk;
        current_total_bytes_ = cut.byte_count;
    }
    return outcome;
}

bool PduProtocolHandler::has_pending_send() const {
    return framer_->has_pending_data();
}

std::tuple<bool, std::string> PduProtocolHandler::continue_send() {
    auto [success, error] = framer_->continue_send();
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
    return {true, ""};
}

void PduProtocolHandler::deallocate_pending_send() {
    // Called by the Reactor on teardown when has_pending_send() is true.
    // The send will never complete so we free the chunk now.
    release_pending_send();
}

void PduProtocolHandler::release_pending_send() {
    // Deallocates the in-flight slab chunk and resets all pending-send state.
    // Must only be called when current_allocator_ is non-null, i.e. a send
    // was initiated and the chunk has not yet been freed.
    if (current_allocator_ != nullptr) {
        current_allocator_->deallocate(current_slab_id_, current_chunk_ptr_);
        current_allocator_ = nullptr;
        current_slab_id_ = invalid_slab_handle;
        current_chunk_ptr_ = nullptr;
        current_total_bytes_ = 0;
    }
}

} // namespaces
