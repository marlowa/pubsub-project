#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <memory>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

class ApplicationThread;

/**
 * @brief Represents a single reactor-managed inbound TCP connection accepted
 *        from a remote peer.
 *
 * @ingroup reactor_subsystem
 *
 * An `InboundConnection` is created by the Reactor when `TcpAcceptor::accept_connection()`
 * returns a new connected socket. Unlike `OutboundConnection`, there is no connecting
 * phase — the socket is already established at construction time. The reactor registers
 * the socket with epoll for `EPOLLIN` immediately.
 *
 * Ownership and threading:
 *   All methods are called exclusively from the reactor thread. No locking required.
 *
 * Lifecycle:
 *
 *   Construction:
 *     Created with a fully connected `TcpSocket` returned by `TcpAcceptor::accept_connection()`.
 *     `PduFramer` and `PduParser` are constructed immediately. The reactor registers
 *     the socket fd with epoll for `EPOLLIN | EPOLLERR`.
 *     A `ConnectionEstablished` event is delivered to the target ApplicationThread.
 *
 *   Receiving PDUs:
 *     When epoll signals `EPOLLIN`, the reactor calls `parser()->receive()`. The
 *     `PduParser` reads the frame header and allocates a slab chunk from the inbound
 *     slab allocator, reading the payload bytes directly into it (zero copy). It
 *     enqueues a `FrameworkPdu` `EventMessage` carrying the slab pointer and slab_id
 *     into the target ApplicationThread's queue. The ApplicationThread must call
 *     `inbound_slab_allocator_.deallocate(msg.slab_id(), msg.payload())` after processing.
 *
 *   Sending PDUs:
 *     Identical to `OutboundConnection`: the application thread pre-builds a complete
 *     frame (PduHeader + payload) in a slab-allocated chunk and enqueues a `SendPdu`
 *     command. The reactor calls `framer()->send_prebuilt()` (zero copy). Partial writes
 *     are tracked in `current_*` fields and completed via `EPOLLOUT`.
 *
 *   Teardown:
 *     On `Disconnect` command, peer disconnect, or unrecoverable error, the reactor
 *     deregisters the fd from epoll, delivers `ConnectionLost` to the target thread,
 *     and destroys the `InboundConnection`. Any in-flight outbound slab chunk is
 *     deallocated before destruction.
 *
 * Reactor maps:
 *   The reactor maintains two maps for `InboundConnection` objects:
 *
 *     inbound_connections_       : ConnectionID → unique_ptr<InboundConnection>  (owns)
 *     inbound_connections_by_fd_ : int fd → InboundConnection*  (non-owning, epoll dispatch)
 *
 * Distinction from OutboundConnection:
 *   `OutboundConnection` is fred initiating a connection to joe (client side).
 *   `InboundConnection` is joe accepting a connection from fred (server side).
 *   Both use the same PDU framing layer and slab allocators once established.
 *   The key difference is that `InboundConnection` has no connecting phase,
 *   no `ServiceRegistry` lookup, no secondary endpoint retry, and no connect timeout.
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
     * The socket must be fully connected (returned by `TcpAcceptor::accept_connection()`).
     * `PduFramer` and `PduParser` are constructed immediately. The disconnect handler
     * is called by `PduParser` when the peer closes the connection gracefully.
     *
     * @param[in] id                   ConnectionID assigned by the reactor.
     * @param[in] socket               The accepted connected socket. Ownership transferred.
     * @param[in] target_thread        ApplicationThread to which inbound PDUs are dispatched.
     *                                 Must outlive this object.
     * @param[in] inbound_allocator    Slab allocator for inbound PDU payloads.
     *                                 Must outlive this object.
     * @param[in] outbound_allocator   Slab allocator for outbound PDU chunks.
     *                                 Must outlive this object.
     * @param[in] disconnect_handler   Called by PduParser on graceful peer disconnect.
     *                                 Typically a lambda calling Reactor::teardown_inbound_connection().
     * @param[in] peer_description     Human-readable description of the remote peer
     *                                 (e.g. "192.168.1.10:5001") for logging.
     */
    InboundConnection(ConnectionID id,
                      std::unique_ptr<TcpSocket> socket,
                      ApplicationThread& target_thread,
                      ExpandableSlabAllocator& inbound_allocator,
                      ExpandableSlabAllocator& outbound_allocator,
                      std::function<void()> disconnect_handler,
                      std::string peer_description);

    /**
     * @brief Returns the ConnectionID assigned to this connection.
     */
    [[nodiscard]] ConnectionID id() const { return id_; }

    /**
     * @brief Returns a human-readable description of the remote peer.
     */
    [[nodiscard]] const std::string& peer_description() const { return peer_description_; }

    /**
     * @brief Returns the file descriptor of the underlying socket.
     */
    [[nodiscard]] int get_fd() const;

    /**
     * @brief Returns the TcpSocket for this connection.
     */
    [[nodiscard]] TcpSocket* socket() const { return socket_.get(); }

    /**
     * @brief Returns the PduFramer for this connection.
     */
    [[nodiscard]] PduFramer* framer() const { return framer_.get(); }

    /**
     * @brief Returns the PduParser for this connection.
     */
    [[nodiscard]] PduParser* parser() const { return parser_.get(); }

    /**
     * @brief Returns true if a PDU send is partially complete.
     *
     * When true the reactor has registered `EPOLLOUT` and will call
     * `framer()->continue_send()` when the socket becomes writable.
     */
    [[nodiscard]] bool has_pending_send() const { return current_chunk_ptr_ != nullptr; }

    /**
     * @brief Records the state of a partial outbound PDU send.
     *
     * @param[in] slab_id     Slab ID for deallocation when send completes.
     * @param[in] chunk_ptr   Pointer to the start of the PDU frame.
     * @param[in] total_bytes Total frame size in bytes.
     */
    void set_pending_send(int slab_id, void* chunk_ptr, uint32_t total_bytes);

    /**
     * @brief Clears the partial send state after a send completes.
     */
    void clear_pending_send();

    /**
     * @brief Returns the slab ID of the currently in-flight PDU chunk.
     * Valid only when has_pending_send() is true.
     */
    [[nodiscard]] int current_slab_id() const { return current_slab_id_; }

    /**
     * @brief Returns the pointer to the currently in-flight PDU chunk.
     * Valid only when has_pending_send() is true.
     */
    [[nodiscard]] void* current_chunk_ptr() const { return current_chunk_ptr_; }

    /**
     * @brief Returns the total byte count of the currently in-flight PDU frame.
     * Valid only when has_pending_send() is true.
     */
    [[nodiscard]] uint32_t current_total_bytes() const { return current_total_bytes_; }

private:
    ConnectionID id_;
    std::string  peer_description_;

    std::unique_ptr<TcpSocket>  socket_;
    std::unique_ptr<PduFramer>  framer_;
    std::unique_ptr<PduParser>  parser_;

    // Partial send state
    int      current_slab_id_{-1};
    void*    current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};

    // Not owned
    ExpandableSlabAllocator& inbound_allocator_;
    ExpandableSlabAllocator& outbound_allocator_;
    ApplicationThread&       target_thread_;
};

} // namespace pubsub_itc_fw
