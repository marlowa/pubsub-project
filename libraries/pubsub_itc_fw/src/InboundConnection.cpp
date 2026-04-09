// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/InboundConnection.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

InboundConnection::InboundConnection(ConnectionID id,
                                     std::unique_ptr<TcpSocket> socket,
                                     ApplicationThread& target_thread,
                                     ExpandableSlabAllocator& inbound_allocator,
                                     ExpandableSlabAllocator& outbound_allocator,
                                     std::function<void()> disconnect_handler,
                                     std::string peer_description)
    : id_(id)
    , peer_description_(std::move(peer_description))
    , socket_(std::move(socket))
    , target_thread_id_(target_thread.get_thread_id())
    , inbound_allocator_(inbound_allocator)
    , outbound_allocator_(outbound_allocator)
    , target_thread_(target_thread)
{
    if (!socket_) {
        throw PreconditionAssertion(
            "InboundConnection: socket must not be null", __FILE__, __LINE__);
    }

    framer_ = std::make_unique<PduFramer>(*socket_);
    parser_ = std::make_unique<PduParser>(
        *socket_,
        target_thread_,
        inbound_allocator_,
        std::move(disconnect_handler));
}

int InboundConnection::get_fd() const
{
    return socket_ ? socket_->get_file_descriptor() : -1;
}

void InboundConnection::set_pending_send(int slab_id, void* chunk_ptr, uint32_t total_bytes)
{
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion(
            "InboundConnection::set_pending_send: chunk_ptr must not be null",
            __FILE__, __LINE__);
    }
    current_slab_id_     = slab_id;
    current_chunk_ptr_   = chunk_ptr;
    current_total_bytes_ = total_bytes;
}

void InboundConnection::clear_pending_send()
{
    current_slab_id_     = -1;
    current_chunk_ptr_   = nullptr;
    current_total_bytes_ = 0;
}

} // namespace pubsub_itc_fw
