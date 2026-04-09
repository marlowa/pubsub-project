#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Represents a single registered inbound TCP listener managed by the Reactor.
 *
 * @ingroup reactor_subsystem
 *
 * An `InboundListener` is created when the application calls
 * `Reactor::register_inbound_listener()` before `run()`. It owns a `TcpAcceptor`
 * bound to the configured address and port. The reactor registers the acceptor's
 * listening socket fd with epoll for `EPOLLIN`.
 *
 * One-connection contract:
 *   Each `InboundListener` enforces a strict one-connection-at-a-time rule.
 *   Framework-to-framework PDU communication uses unicast connections between
 *   known peers. Only one peer should ever connect to a given listener port.
 *   If a second peer attempts to connect while a connection is already established,
 *   the reactor:
 *     1. Accepts the socket (to prevent the OS from queuing it).
 *     2. Immediately closes it.
 *     3. Logs a Warning identifying the offending peer address.
 *     4. Does NOT deliver any event to the application thread.
 *   This constitutes a framework misuse by the connecting application and is
 *   treated as a configuration or programming error on their side, not on ours.
 *   The Warning log is the signal to the operator that something is wrong.
 *
 * Routing:
 *   All inbound PDUs from the accepted connection are routed to `target_thread_id`.
 *   The application thread receives `ConnectionEstablished` when the connection
 *   is accepted, `FrameworkPdu` messages as PDUs arrive, and `ConnectionLost`
 *   when the peer disconnects or an error occurs.
 *
 * Lifecycle:
 *   Created and registered before `run()`. The `TcpAcceptor` is created during
 *   `Reactor::initialize_listeners()`, called from `initialize_threads()`. If the
 *   bind or listen fails, `initialize_threads()` returns false and the reactor
 *   shuts down immediately.
 *
 *   When the established connection is lost, `has_connection` is cleared and the
 *   listener resumes accepting — the next peer to connect on this port will be
 *   accepted normally.
 */
struct InboundListener {
    /**
     * @brief The address and port this listener binds to.
     */
    NetworkEndpointConfig address;

    /**
     * @brief The ApplicationThread that receives all events and PDUs from
     *        connections accepted on this listener.
     */
    ThreadID target_thread_id;

    /**
     * @brief The TcpAcceptor bound to `address`.
     * Created during reactor initialisation. Null before initialisation.
     */
    std::unique_ptr<TcpAcceptor> acceptor;

    /**
     * @brief The ConnectionID of the currently established inbound connection,
     *        or ConnectionID{} (invalid) if no connection is currently active.
     *
     * Used to enforce the one-connection rule: if `has_connection()` returns
     * true and a new peer attempts to connect, the new connection is rejected.
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
