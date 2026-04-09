// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <pubsub_itc_fw/OutboundConnection.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

OutboundConnection::OutboundConnection(ConnectionID id,
                                       ThreadID requesting_thread_id,
                                       std::string service_name,
                                       ServiceEndpoints endpoints,
                                       std::unique_ptr<TcpConnector> connector,
                                       ExpandableSlabAllocator& inbound_allocator,
                                       ExpandableSlabAllocator& outbound_allocator,
                                       ApplicationThread& target_thread)
    : id_(id)
    , requesting_thread_id_(requesting_thread_id)
    , service_name_(std::move(service_name))
    , endpoints_(std::move(endpoints))
    , connector_(std::move(connector))
    , inbound_allocator_(inbound_allocator)
    , outbound_allocator_(outbound_allocator)
    , target_thread_(target_thread)
{
    if (!connector_) {
        throw PreconditionAssertion(
            "OutboundConnection: connector must not be null", __FILE__, __LINE__);
    }
}

void OutboundConnection::on_connected(std::unique_ptr<TcpSocket> socket,
                                       std::function<void()> disconnect_handler)
{
    if (!socket) {
        throw PreconditionAssertion(
            "OutboundConnection::on_connected: socket must not be null", __FILE__, __LINE__);
    }
    if (!is_connecting()) {
        throw PreconditionAssertion(
            "OutboundConnection::on_connected: must be in connecting phase", __FILE__, __LINE__);
    }

    socket_    = std::move(socket);
    connector_.reset();

    framer_ = std::make_unique<PduFramer>(*socket_);
    parser_ = std::make_unique<PduParser>(
        *socket_,
        target_thread_,
        inbound_allocator_,
        std::move(disconnect_handler));
}

void OutboundConnection::retry_with_secondary(std::unique_ptr<TcpConnector> connector)
{
    if (!connector) {
        throw PreconditionAssertion(
            "OutboundConnection::retry_with_secondary: connector must not be null", __FILE__, __LINE__);
    }
    trying_secondary_ = true;
    connector_ = std::move(connector);
}

int OutboundConnection::get_fd() const
{
    if (connector_) {
        return connector_->get_fd();
    }
    if (socket_) {
        return socket_->get_file_descriptor();
    }
    return -1;
}

void OutboundConnection::set_pending_send(int slab_id, void* chunk_ptr, uint32_t total_bytes)
{
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion(
            "OutboundConnection::set_pending_send: chunk_ptr must not be null", __FILE__, __LINE__);
    }
    current_slab_id_     = slab_id;
    current_chunk_ptr_   = chunk_ptr;
    current_total_bytes_ = total_bytes;
}

void OutboundConnection::clear_pending_send()
{
    current_slab_id_     = -1;
    current_chunk_ptr_   = nullptr;
    current_total_bytes_ = 0;
}

} // namespace pubsub_itc_fw
