#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Represents a single registered inbound TCP listener managed by the Reactor.
 *
 * @ingroup reactor_subsystem
 *
 * An InboundListener is created when the application calls
 * Reactor::register_inbound_listener() before run(). It owns a TcpAcceptor
 * bound to the configured address and port. The reactor registers the acceptor's
 * listening socket fd with epoll for EPOLLIN.
 *
 * One-connection contract:
 *   Each InboundListener enforces a strict one-connection-at-a-time rule.
 *   Framework-to-framework PDU communication uses unicast connections between
 *   known peers. Only one peer should ever connect to a given listener port.
 *   If a second peer attempts to connect while a connection is already established,
 *   the reactor accepts the socket (to prevent the OS from queuing it), immediately
 *   closes it, logs a Warning identifying the offending peer address, and does NOT
 *   deliver any event to the application thread. This constitutes a framework misuse
 *   by the connecting application and is treated as a configuration or programming
 *   error on their side. The Warning log is the signal to the operator that something
 *   is wrong.
 *
 * Protocol type:
 *   Each listener is registered with a ProtocolType that determines which concrete
 *   ProtocolHandlerInterface is constructed for accepted connections. FrameworkPdu
 *   listeners construct a PduProtocolHandler; RawBytes listeners construct a
 *   RawBytesProtocolHandler backed by a MirroredBuffer of size raw_buffer_capacity.
 *   Using separate listening sockets per protocol type avoids any need to inspect
 *   stream content to determine framing — the type is known at accept time.
 *
 * Routing:
 *   All inbound data from the accepted connection is routed to target_thread_id.
 *   The application thread receives ConnectionEstablished when the connection is
 *   accepted, FrameworkPdu or RawSocketCommunication messages as data arrives, and
 *   ConnectionLost when the peer disconnects or an error occurs.
 *
 * Lifecycle:
 *   Created and registered before run(). The TcpAcceptor is created during
 *   Reactor::initialize_listeners(), called from initialize_threads(). If the
 *   bind or listen fails, initialize_threads() returns false and the reactor
 *   shuts down immediately. When the established connection is lost, has_connection()
 *   is cleared and the listener resumes accepting.
 */
struct InboundListener {
    /**
     * @brief The address and port this listener binds to.
     */
    NetworkEndpointConfiguration address;

    /**
     * @brief The ApplicationThread that receives all events and data from
     *        connections accepted on this listener.
     */
    ThreadID target_thread_id;

    /**
     * @brief Determines which protocol handler is constructed for accepted connections.
     *
     * FrameworkPdu — constructs PduProtocolHandler (default).
     * RawBytes     — constructs RawBytesProtocolHandler.
     */
    ProtocolType protocol_type{ProtocolType::FrameworkPdu};

    /**
     * @brief Minimum capacity of the MirroredBuffer in bytes for RawBytes listeners.
     *
     * Ignored for FrameworkPdu listeners. For RawBytes listeners this is passed
     * directly to the RawBytesProtocolHandler constructor and rounded up to the
     * nearest page size internally. Must be greater than zero when
     * protocol_type == RawBytes.
     */
    int64_t raw_buffer_capacity{0};

    /**
     * @brief The TcpAcceptor bound to address.
     * Created during reactor initialisation. Null before initialisation.
     */
    std::unique_ptr<TcpAcceptor> acceptor;

    /**
     * @brief The ConnectionID of the currently established inbound connection,
     *        or ConnectionID{} (invalid) if no connection is currently active.
     *
     * Used to enforce the one-connection rule: if has_connection() returns true
     * and a new peer attempts to connect, the new connection is rejected.
     */
    ConnectionID current_connection_id;

    /**
     * @brief Returns true if this listener currently has an established connection.
     */
    [[nodiscard]] bool has_connection() const {
        return current_connection_id.get_value() != 0;
    }
};

} // namespace pubsub_itc_fw
