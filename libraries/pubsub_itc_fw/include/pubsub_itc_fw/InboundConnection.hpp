#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Represents a single reactor-managed inbound TCP connection accepted
 *        from a remote peer.
 *
 * @ingroup reactor_subsystem
 *
 * An InboundConnection is created by the Reactor when TcpAcceptor::accept_connection()
 * returns a new connected socket. Unlike OutboundConnection there is no connecting
 * phase — the socket is already established at construction time. The Reactor
 * registers the socket with epoll for EPOLLIN immediately after construction.
 *
 * Protocol handling is fully delegated to a ProtocolHandlerInterface implementation
 * supplied at construction time. InboundConnection itself is a thin transport shell
 * responsible only for:
 *   - Holding the socket and its file descriptor for epoll registration.
 *   - Recording the target thread ID for ConnectionLost delivery.
 *   - Tracking the last activity time for idle timeout enforcement.
 *   - Delegating read events, outbound sends, and teardown to the handler.
 *
 * Ownership and threading:
 *   All methods are called exclusively from the reactor thread. No locking required.
 *
 * Reactor maps:
 *   inbound_connections_       : ConnectionID -> unique_ptr<InboundConnection>  (owns)
 *   inbound_connections_by_fd_ : int fd -> InboundConnection*  (non-owning, epoll dispatch)
 *
 * Distinction from OutboundConnection:
 *   OutboundConnection is the client side (initiates the connect).
 *   InboundConnection is the server side (accepts the connect).
 *   Both delegate protocol-specific work to a ProtocolHandlerInterface.
 */
class InboundConnection {
  public:
    ~InboundConnection() = default;

    InboundConnection(const InboundConnection&) = delete;
    InboundConnection& operator=(const InboundConnection&) = delete;
    InboundConnection(InboundConnection&&) = delete;
    InboundConnection& operator=(InboundConnection&&) = delete;

    /**
     * @brief Constructs an InboundConnection from an already-connected socket.
     *
     * The handler must be fully constructed before being passed here. The
     * Reactor builds the concrete ProtocolHandlerInterface (PduProtocolHandler
     * or RawBytesProtocolHandler) and transfers ownership to this connection.
     *
     * @param[in] id               ConnectionID assigned by the Reactor.
     * @param[in] socket           The accepted connected socket. Ownership transferred.
     * @param[in] target_thread_id ThreadID of the ApplicationThread that receives
     *                             events from this connection.
     * @param[in] handler          The protocol handler for this connection.
     *                             Ownership transferred.
     * @param[in] peer_description Human-readable description of the remote peer
     *                             (e.g. "192.168.1.10:5001") for logging.
     */
    InboundConnection(ConnectionID id, std::unique_ptr<TcpSocket> socket, ThreadID target_thread_id, std::unique_ptr<ProtocolHandlerInterface> handler,
                      std::string peer_description);

    /**
     * @brief Returns the ConnectionID assigned to this connection.
     */
    [[nodiscard]] ConnectionID id() const {
        return id_;
    }

    /**
     * @brief Returns a human-readable description of the remote peer.
     */
    [[nodiscard]] const std::string& peer_description() const {
        return peer_description_;
    }

    /**
     * @brief Returns the file descriptor of the underlying socket.
     */
    [[nodiscard]] int get_fd() const;

    /**
     * @brief Returns the ThreadID of the ApplicationThread that receives
     *        events and PDUs from this connection.
     */
    [[nodiscard]] ThreadID target_thread_id() const {
        return target_thread_id_;
    }

    /**
     * @brief Returns the time point of the most recent inbound data activity.
     *
     * Used by the Reactor's idle timeout sweep to detect zombie connections.
     * Updated by handle_read() on every call regardless of whether any bytes
     * were actually received.
     */
    [[nodiscard]] std::chrono::steady_clock::time_point last_activity_time() const {
        return last_activity_time_;
    }

    /**
     * @brief Services a readable socket event (EPOLLIN).
     *
     * Updates the last activity timestamp and delegates to the protocol
     * handler's on_data_ready(). Must be called by the Reactor when epoll
     * signals EPOLLIN on this connection's file descriptor.
     */
    void handle_read();

    /**
     * @brief Returns the protocol handler for this connection.
     *
     * The Reactor uses this to call send_prebuilt(), has_pending_send(),
     * continue_send(), and deallocate_pending_send(). Ownership remains
     * with this connection.
     *
     * @return A non-owning pointer to the handler. Never nullptr after construction.
     */
    [[nodiscard]] ProtocolHandlerInterface* handler() const {
        return handler_.get();
    }

  private:
    ConnectionID id_;
    std::string peer_description_;
    ThreadID target_thread_id_;

    std::unique_ptr<TcpSocket> socket_;
    std::unique_ptr<ProtocolHandlerInterface> handler_;

    std::chrono::steady_clock::time_point last_activity_time_;
};

} // namespace pubsub_itc_fw
