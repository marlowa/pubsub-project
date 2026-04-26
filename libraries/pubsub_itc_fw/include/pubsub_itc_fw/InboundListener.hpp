#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/InboundListenerConfiguration.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>

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
 * This struct separates static configuration (held in configuration) from
 * runtime state (acceptor). The configuration fields are set once at
 * registration time and never change. The runtime state is managed exclusively
 * by the Reactor and InboundConnectionManager.
 *
 * Multiple concurrent connections:
 *   A listener accepts any number of concurrent inbound connections. There is no
 *   framework-level one-connection limit. Applications that require a dedicated
 *   point-to-point link may enforce that constraint themselves in
 *   on_connection_established() by disconnecting unexpected peers.
 *
 * Protocol type:
 *   Each listener is registered with a ProtocolType that determines which concrete
 *   ProtocolHandlerInterface is constructed for accepted connections. FrameworkPdu
 *   listeners construct a PduProtocolHandler; RawBytes listeners construct a
 *   RawBytesProtocolHandler backed by a MirroredBuffer of size raw_buffer_capacity.
 *   Using separate listening sockets per protocol type avoids any need to inspect
 *   stream content to determine framing -- the type is known at accept time.
 *
 * Routing:
 *   All inbound data from the accepted connection is routed to
 *   configuration.target_thread_id. The application thread receives
 *   ConnectionEstablished when the connection is accepted, FrameworkPdu or
 *   RawSocketCommunication messages as data arrives, and ConnectionLost when
 *   the peer disconnects or an error occurs.
 *
 * Lifecycle:
 *   Created and registered before run(). The TcpAcceptor is created during
 *   Reactor::initialize_listeners(), called from initialize_threads(). If the
 *   bind or listen fails, initialize_threads() returns false and the reactor
 *   shuts down immediately.
 */
struct InboundListener {
    // ----------------------------------------------------------------
    // Static configuration -- set once at registration, never changes.
    // ----------------------------------------------------------------
    InboundListenerConfiguration configuration;

    // ----------------------------------------------------------------
    // Runtime state -- managed exclusively by the Reactor.
    // ----------------------------------------------------------------

    /**
     * @brief The TcpAcceptor bound to configuration.address.
     * Created during reactor initialisation. Null before initialisation.
     */
    std::unique_ptr<TcpAcceptor> acceptor;
};

} // namespace pubsub_itc_fw
