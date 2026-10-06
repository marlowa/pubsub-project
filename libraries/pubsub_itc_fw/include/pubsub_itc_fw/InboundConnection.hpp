#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/IdleTimeoutFlag.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Represents a single reactor-managed inbound TCP connection accepted
 *        from a remote peer.
 *
 * @ingroup reactor_subsystem
 *
 * An InboundConnection is created by the Reactor when TcpAcceptor::accept_connection()
 * returns a new connected socket. Unlike OutboundConnection there is no connecting
 * phase -- the socket is already established at construction time. The Reactor
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
     * @param[in] peer_description   Human-readable description of the remote peer
     *                               (e.g. "192.168.1.10:5001") for logging.
     * @param[in] idle_timeout           IdleTimeoutFlag::UseIdleTimeout (default) or
     *                                   IdleTimeoutFlag::BypassIdleTimeout for connections
     *                                   that do not exchange heartbeats.
     */
    InboundConnection(const ConnectionID& id, std::unique_ptr<TcpSocket> socket, ThreadID target_thread_id, std::unique_ptr<ProtocolHandlerInterface> handler,
                      std::string peer_description, IdleTimeoutFlag idle_timeout = IdleTimeoutFlag{IdleTimeoutFlag::UseIdleTimeout});

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
     * @brief Returns true if this connection is exempt from idle timeout teardown.
     *
     * Set at construction time from InboundListenerConfiguration::idle_timeout.
     * Exempt connections are never torn down by check_for_inactive_connections().
     */
    [[nodiscard]] bool idle_timeout_exempt() const {
        return idle_timeout_ == IdleTimeoutFlag::BypassIdleTimeout;
    }

    /**
     * @brief Services a readable socket event (EPOLLIN).
     *
     * Updates the last activity timestamp and delegates to the protocol
     * handler's on_data_ready(). Must be called by the Reactor when epoll
     * signals EPOLLIN on this connection's file descriptor.
     *
     * @return The tuple from the handler's on_data_ready(): {true, "", pause}
     *         on a clean read where pause indicates whether the handler wants
     *         EPOLLIN deregistered for backpressure, {false, "", false} on a
     *         graceful peer disconnect, or {false, error_string, false} on
     *         protocol failure. The caller is responsible for tearing down
     *         the connection on failure and for acting on the pause flag.
     */
    [[nodiscard]] std::tuple<bool, std::string, bool> handle_read();

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

    /**
     * @brief Stop watching this connection for incoming data, at the application's request.
     *
     * See ApplicationThread::pause_reading(). Kept apart from the handler's own pause for a full
     * buffer, so that neither can undo the other: the connection is watched for incoming data only
     * while neither has paused it.
     */
    void pause_reading_by_application() {
        reading_paused_by_application_ = true;
    }

    /// Undo pause_reading_by_application().
    void resume_reading_by_application() {
        reading_paused_by_application_ = false;
    }

    /// Whether the application has paused reading from this connection.
    [[nodiscard]] bool reading_paused_by_application() const {
        return reading_paused_by_application_;
    }

    /// The sends asked for on this connection while it was still writing an earlier one.
    [[nodiscard]] WaitingSendQueue& waiting_sends() {
        return waiting_sends_;
    }

    /**
     * @brief Records that the application asked to be told when this connection can take another
     * send, at a moment when it could not: it was still writing, or had sends waiting. The connection
     * manager tells the application once everything waiting has been written.
     */
    void want_writable_notification() {
        writable_notification_wanted_ = true;
    }

    /// Whether a writable notification is owed, clearing the record of it.
    [[nodiscard]] bool take_writable_notification_wanted() {
        const bool wanted = writable_notification_wanted_;
        writable_notification_wanted_ = false;
        return wanted;
    }

  private:
    ConnectionID id_;
    std::string peer_description_;
    ThreadID target_thread_id_;

    std::unique_ptr<TcpSocket> socket_;
    std::unique_ptr<ProtocolHandlerInterface> handler_;

    std::chrono::steady_clock::time_point last_activity_time_;
    IdleTimeoutFlag idle_timeout_;
    bool reading_paused_by_application_{false};
    WaitingSendQueue waiting_sends_;
    bool writable_notification_wanted_{false};
};

} // namespaces
