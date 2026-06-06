// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

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
    return {ok, error, false};
}

std::tuple<bool, std::string> PduProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) {
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
        current_slab_id_ = -1;
        current_chunk_ptr_ = nullptr;
        current_total_bytes_ = 0;
    }
}

} // namespace pubsub_itc_fw
