// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <memory>

#include <pubsub_itc_fw/OutboundConnection.hpp>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/TlsContext.hpp>
#include <pubsub_itc_fw/TlsRawBytesProtocolHandler.hpp>

namespace pubsub_itc_fw {

OutboundConnection::OutboundConnection(ConnectionID id, ThreadID requesting_thread_id, std::string service_name, ServiceEndpoints endpoints,
                                       std::unique_ptr<TcpConnector> connector, ExpandableSlabAllocator& inbound_allocator, ApplicationThread& target_thread,
                                       QuillLogger& logger)
    : id_(id)
    , requesting_thread_id_(requesting_thread_id)
    , service_name_(std::move(service_name))
    , endpoints_(std::move(endpoints))
    , connector_(std::move(connector))
    , inbound_allocator_(inbound_allocator)
    , target_thread_(target_thread)
    , logger_(logger) {
    if (!connector_) {
        throw PreconditionAssertion("OutboundConnection: connector must not be null", __FILE__, __LINE__);
    }
    if (endpoints_.tls.has_value()) {
        const TlsClientConfiguration& tls_config = *endpoints_.tls;
        auto [ctx, ctx_error] = TlsContext::create_client(tls_config.ca_path, tls_config.certificate_path, tls_config.private_key_path);
        if (!ctx) {
            throw PreconditionAssertion("OutboundConnection: TlsContext::create_client failed: " + ctx_error, __FILE__, __LINE__);
        }
        tls_context_ = std::move(ctx);
    }
}

void OutboundConnection::on_connected(std::unique_ptr<TcpSocket> socket) {
    if (!socket) {
        throw PreconditionAssertion("OutboundConnection::on_connected: socket must not be null", __FILE__, __LINE__);
    }
    if (!is_connecting()) {
        throw PreconditionAssertion("OutboundConnection::on_connected: must be in connecting phase", __FILE__, __LINE__);
    }

    socket_ = std::move(socket);
    connector_.reset();

    if (tls_context_) {
        const int64_t buffer_capacity = endpoints_.tls->raw_buffer_capacity;
        protocol_handler_ = std::make_unique<TlsRawBytesProtocolHandler>(
            id_, *socket_, target_thread_, buffer_capacity, *tls_context_, /*is_server=*/false);
        // data_exchange_ready_ stays false until the TLS handshake completes.
    } else {
        framer_ = std::make_unique<PduFramer>(*socket_);
        // nullptr is safe: OutboundConnectionManager::on_data_ready checks the receive() return
        // value to detect disconnects and drives teardown via that path.
        parser_ = std::make_unique<PduParser>(*socket_, target_thread_, inbound_allocator_, logger_, nullptr, id_);
        data_exchange_ready_ = true;
    }
}

void OutboundConnection::retry_with_secondary(std::unique_ptr<TcpConnector> connector) {
    if (!connector) {
        throw PreconditionAssertion("OutboundConnection::retry_with_secondary: connector must not be null", __FILE__, __LINE__);
    }
    trying_secondary_ = true;
    connector_ = std::move(connector);
}

int OutboundConnection::get_fd() const {
    if (connector_) {
        return connector_->get_fd();
    }
    if (socket_) {
        return socket_->get_file_descriptor();
    }
    return -1;
}

void OutboundConnection::set_pending_send(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) {
    if (allocator == nullptr) {
        throw PreconditionAssertion("OutboundConnection::set_pending_send: allocator must not be null", __FILE__, __LINE__);
    }
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion("OutboundConnection::set_pending_send: chunk_ptr must not be null", __FILE__, __LINE__);
    }
    current_allocator_ = allocator;
    current_slab_id_ = slab_id;
    current_chunk_ptr_ = chunk_ptr;
    current_total_bytes_ = total_bytes;
}

void OutboundConnection::clear_pending_send() {
    current_allocator_ = nullptr;
    current_slab_id_ = -1;
    current_chunk_ptr_ = nullptr;
    current_total_bytes_ = 0;
}

} // namespaces
