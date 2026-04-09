#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/TcpConnector.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

class ApplicationThread;

/**
 * @brief Represents a single reactor-managed outbound TCP connection from this
 *        process to a named remote service.
 *
 * @ingroup reactor_subsystem
 *
 * An `OutboundConnection` is created by the Reactor when an ApplicationThread
 * calls `connect_to_service(service_name)`. It encapsulates the full lifecycle
 * of one client-side TCP connection — from the initial non-blocking connect
 * attempt through to steady-state PDU exchange and eventual teardown.
 *
 * Ownership and threading:
 *   All methods on `OutboundConnection` are called exclusively from the reactor
 *   thread. No locking is required. ApplicationThreads interact with the
 *   connection only indirectly, by enqueuing `ReactorControlCommand` objects
 *   (`Connect`, `SendPdu`, `Disconnect`) into the reactor's command queue.
 *
 * Lifecycle phases:
 *
 *   Phase 1 — Connecting:
 *     The `OutboundConnection` is created with a `TcpConnector` that has
 *     initiated a non-blocking `connect()` to the primary endpoint of the
 *     named service. The reactor registers the connector's socket file
 *     descriptor with epoll for `EPOLLOUT` to detect connect completion.
 *     During this phase `is_connecting()` returns true and `is_established()`
 *     returns false.
 *
 *     If `finish_connect()` fails on the primary endpoint, the reactor
 *     automatically retries using the secondary endpoint from the
 *     `ServiceRegistry`. If both fail, a `ConnectionFailed` event is
 *     delivered to the requesting ApplicationThread and the
 *     `OutboundConnection` is destroyed.
 *
 *   Phase 2 — Established:
 *     When `finish_connect()` succeeds, the `TcpConnector` is released, and
 *     the `TcpSocket` it was managing is transferred to this connection. A
 *     `PduFramer` and `PduParser` are constructed at this point. The reactor
 *     re-registers the socket file descriptor with epoll for `EPOLLIN`. A
 *     `ConnectionEstablished` event carrying the `ConnectionID` is delivered
 *     to the requesting ApplicationThread.
 *     During this phase `is_connecting()` returns false and
 *     `is_established()` returns true.
 *
 *   Sending PDUs (phase 2 only):
 *     When the reactor processes a `SendPdu` command, it calls
 *     `framer->send()`. If the kernel send buffer is full, the send is
 *     partial and the remaining bytes are tracked in the `current_*` fields.
 *     The reactor registers `EPOLLOUT` on the socket and calls
 *     `framer->continue_send()` when the socket becomes writable again.
 *     `has_pending_send()` indicates whether a partial send is in flight.
 *     Once the send completes, the reactor deallocates the slab chunk via
 *     `outbound_slab_allocator_.deallocate(current_slab_id_, current_chunk_ptr_)`.
 *
 *   Receiving PDUs (phase 2 only):
 *     When epoll signals `EPOLLIN` on the socket, the reactor calls
 *     `parser->receive()`. The `PduParser` reads the frame header and then
 *     allocates a slab chunk from the inbound slab allocator, reading the
 *     payload bytes directly into it (zero copy). It then enqueues a
 *     `FrameworkPdu` `EventMessage` carrying the slab pointer and slab ID
 *     into the target ApplicationThread's queue. The ApplicationThread must
 *     call `inbound_slab_allocator_.deallocate(msg.slab_id(), msg.payload())`
 *     after processing the message.
 *
 *   Teardown:
 *     On `Disconnect` command, peer disconnect, or unrecoverable socket
 *     error, the reactor destroys the `OutboundConnection`. If the
 *     connection was established, a `ConnectionLost` event is delivered to
 *     the requesting ApplicationThread before destruction. Any slab chunk
 *     currently tracked in `current_*` is deallocated by the reactor before
 *     destroying the connection.
 *
 * Reactor maps:
 *   The reactor maintains two maps for `OutboundConnection` objects:
 *
 *     connections_by_id_  : ConnectionID → OutboundConnection*
 *       Used when processing `SendPdu` and `Disconnect` commands.
 *
 *     connections_by_fd_  : int (fd) → OutboundConnection*
 *       Used when dispatching epoll events (`EPOLLIN`, `EPOLLOUT`, `EPOLLERR`).
 *
 *   Ownership of all `OutboundConnection` instances lies with a third map:
 *
 *     connections_        : ConnectionID → unique_ptr<OutboundConnection>
 */
class OutboundConnection {
public:
    ~OutboundConnection() = default;

    OutboundConnection(const OutboundConnection&) = delete;
    OutboundConnection& operator=(const OutboundConnection&) = delete;
    OutboundConnection(OutboundConnection&&) = delete;
    OutboundConnection& operator=(OutboundConnection&&) = delete;

    /**
     * @brief Constructs an OutboundConnection in the connecting phase.
     *
     * The caller must have already called `connector->connect()` successfully
     * before constructing this object. The reactor is responsible for
     * registering the connector's socket fd with epoll for `EPOLLOUT`.
     *
     * @param[in] id                   The ConnectionID assigned by the reactor.
     * @param[in] requesting_thread_id The ApplicationThread to notify on
     *                                 establishment or failure.
     * @param[in] service_name         The logical service name, for logging.
     * @param[in] connector            The TcpConnector with an active connect
     *                                 attempt in progress. Ownership is transferred.
     * @param[in] inbound_allocator    The slab allocator for inbound PDU payloads.
     *                                 Must outlive this object.
     * @param[in] outbound_allocator   The slab allocator for outbound PDU chunks.
     *                                 Must outlive this object.
     * @param[in] target_thread        The ApplicationThread to which inbound PDUs
     *                                 are dispatched. Must outlive this object.
     */
    OutboundConnection(ConnectionID id,
                       ThreadID requesting_thread_id,
                       std::string service_name,
                       ServiceEndpoints endpoints,
                       std::unique_ptr<TcpConnector> connector,
                       ExpandableSlabAllocator& inbound_allocator,
                       ExpandableSlabAllocator& outbound_allocator,
                       ApplicationThread& target_thread);

    /**
     * @brief Transitions from connecting phase to established phase.
     *
     * Called by the reactor after `finish_connect()` succeeds. Transfers
     * socket ownership from the connector, constructs the PduFramer and
     * PduParser, and clears the connector.
     *
     * @param[in] socket             The connected TcpSocket. Ownership is transferred.
     * @param[in] disconnect_handler Called by PduParser when the peer closes the
     *                               connection gracefully. Typically a lambda that
     *                               calls Reactor::teardown_connection().
     */
    void on_connected(std::unique_ptr<TcpSocket> socket,
                      std::function<void()> disconnect_handler);

    /**
     * @brief Returns the file descriptor of the underlying socket.
     *
     * Valid in both connecting and established phases. Returns -1 if
     * no socket is currently held.
     */
    [[nodiscard]] int get_fd() const;

    /**
     * @brief Returns the ConnectionID assigned to this connection.
     */
    [[nodiscard]] ConnectionID id() const { return id_; }

    /**
     * @brief Returns the ThreadID of the ApplicationThread that requested
     *        this connection.
     */
    [[nodiscard]] ThreadID requesting_thread_id() const { return requesting_thread_id_; }

    /**
     * @brief Returns the logical service name this connection targets.
     */
    [[nodiscard]] const std::string& service_name() const { return service_name_; }

    /**
     * @brief Returns the service endpoints (primary and secondary addresses).
     */
    [[nodiscard]] const ServiceEndpoints& endpoints() const { return endpoints_; }

    /**
     * @brief Returns true if the reactor is currently attempting the secondary endpoint.
     *
     * False during the initial primary attempt. Set to true by the reactor
     * before retrying with the secondary endpoint after a primary failure.
     */
    [[nodiscard]] bool is_trying_secondary() const { return trying_secondary_; }

    /**
     * @brief Marks that the reactor is now attempting the secondary endpoint.
     *
     * Called by the reactor before calling connector()->connect() with the
     * secondary address. Resets the connector to a new TcpConnector for
     * the retry attempt.
     *
     * @param[in] connector A new TcpConnector with the secondary connect in progress.
     */
    void retry_with_secondary(std::unique_ptr<TcpConnector> connector);

    /**
     * @brief Returns the TcpConnector during the connecting phase.
     *
     * Returns nullptr once the connection is established.
     */
    [[nodiscard]] TcpConnector* connector() const { return connector_.get(); }

    /**
     * @brief Returns the TcpSocket during the established phase.
     *
     * Returns nullptr during the connecting phase.
     */
    [[nodiscard]] TcpSocket* socket() const { return socket_.get(); }

    /**
     * @brief Returns the PduFramer for this connection.
     *
     * Returns nullptr during the connecting phase.
     */
    [[nodiscard]] PduFramer* framer() const { return framer_.get(); }

    /**
     * @brief Returns the PduParser for this connection.
     *
     * Returns nullptr during the connecting phase.
     */
    [[nodiscard]] PduParser* parser() const { return parser_.get(); }

    /**
     * @brief Returns the time at which the connect attempt was started.
     *
     * Used by the reactor's housekeeping tick to detect timed-out connections.
     * Valid in both connecting and established phases.
     */
    [[nodiscard]] MillisecondClock::time_point connect_started_at() const { return connect_started_at_; }

    /**
     * @brief Returns true if the connection is in the connecting phase.
     *
     * Connecting phase: `connect()` has been called but `finish_connect()`
     * has not yet succeeded.
     */
    [[nodiscard]] bool is_connecting() const { return connector_ != nullptr; }

    /**
     * @brief Returns true if the connection is fully established.
     *
     * Established phase: `finish_connect()` has succeeded and the socket,
     * framer, and parser are all active.
     */
    [[nodiscard]] bool is_established() const { return socket_ != nullptr; }

    /**
     * @brief Returns true if a PDU send is partially complete.
     *
     * When true, the reactor has registered `EPOLLOUT` on this socket and
     * will call `framer()->continue_send()` when it becomes writable.
     * No further `SendPdu` commands for this connection should be dequeued
     * from the command queue until this clears.
     */
    [[nodiscard]] bool has_pending_send() const { return current_chunk_ptr_ != nullptr; }

    /**
     * @brief Records the state of a partial outbound PDU send.
     *
     * Called by the reactor when `framer()->send()` or `continue_send()`
     * returns with data still pending. The reactor registers `EPOLLOUT` on
     * the socket after calling this.
     *
     * @param[in] slab_id     Slab ID of the chunk, for deallocation when complete.
     * @param[in] chunk_ptr   Pointer to the start of the PDU frame in the slab chunk.
     * @param[in] total_bytes Total frame size in bytes (sizeof(PduHeader) + payload).
     */
    void set_pending_send(int slab_id, void* chunk_ptr, uint32_t total_bytes);

    /**
     * @brief Clears the partial send state after a send completes.
     *
     * Called by the reactor when `framer()->continue_send()` indicates the
     * frame has been fully transmitted. The reactor deallocates the slab
     * chunk and deregisters `EPOLLOUT` after calling this.
     */
    void clear_pending_send();

    /**
     * @brief Returns the slab ID of the currently in-flight PDU chunk.
     *
     * Valid only when `has_pending_send()` returns true.
     */
    [[nodiscard]] int current_slab_id() const { return current_slab_id_; }

    /**
     * @brief Returns the pointer to the currently in-flight PDU chunk.
     *
     * Valid only when `has_pending_send()` returns true.
     */
    [[nodiscard]] void* current_chunk_ptr() const { return current_chunk_ptr_; }

    /**
     * @brief Returns the total byte count of the currently in-flight PDU frame.
     *
     * Valid only when `has_pending_send()` returns true.
     */
    [[nodiscard]] uint32_t current_total_bytes() const { return current_total_bytes_; }

private:
    // --- Identity ---
    ConnectionID id_;
    ThreadID     requesting_thread_id_;
    std::string  service_name_;

    // --- Resolved service endpoints ---
    ServiceEndpoints endpoints_;

    // --- Phase 1: connecting ---
    bool trying_secondary_{false};
    MillisecondClock::time_point connect_started_at_{MillisecondClock::now()};
    std::unique_ptr<TcpConnector> connector_;

    // --- Phase 2: established ---
    std::unique_ptr<TcpSocket>  socket_;
    std::unique_ptr<PduFramer>  framer_;
    std::unique_ptr<PduParser>  parser_;

    // --- Partial send state ---
    int      current_slab_id_{-1};
    void*    current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};

    // --- Allocators and target thread (not owned) ---
    ExpandableSlabAllocator& inbound_allocator_;
    ExpandableSlabAllocator& outbound_allocator_;
    ApplicationThread&       target_thread_;
};

} // namespace pubsub_itc_fw
