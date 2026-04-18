#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <memory>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Concrete protocol handler for raw byte streams (Strategy B).
 *
 * @ingroup reactor_subsystem
 *
 * This is the Strategy B implementation of ProtocolHandlerInterface. It is
 * intended for connections carrying foreign or alien protocols (e.g. ASCII FIX,
 * NMEA, custom binary) where the framework cannot impose PDU framing.
 *
 * Inbound path:
 *   Received bytes are read directly into an owned MirroredBuffer. After each
 *   successful read the handler enqueues a RawSocketCommunication EventMessage
 *   to the target ApplicationThread. The message carries a non-owning pointer
 *   into the MirroredBuffer (via read_ptr()) and the current bytes_available()
 *   count. Because the buffer uses virtual memory mirroring, the application
 *   always sees a contiguous view of unprocessed bytes with no split-packet
 *   edge cases.
 *
 *   Once the application has consumed N bytes it enqueues a CommitRawBytes
 *   command. The Reactor routes this to commit_bytes(N), which calls
 *   buffer_.advance_tail(N) to release those bytes from the ring.
 *
 *   Buffer-full policy (backpressure contract):
 *   If the buffer is full when on_data_ready() fires, the handler logs an error
 *   and invokes the disconnect handler, closing that connection. This is a
 *   deliberate backpressure policy, not a bug. The reactor thread must never
 *   block waiting for the application to consume data — doing so would stall
 *   every other connection and timer in the process. Disconnecting the slow or
 *   misbehaving peer is the correct enforcement mechanism.
 *
 *   Implications for callers:
 *   - Size buffer_capacity generously relative to the maximum burst size of
 *     the protocol in use. A well-behaved peer should never come close to
 *     filling the buffer between successive CommitRawBytes calls.
 *   - The application thread must call commit_raw_bytes() promptly after
 *     processing each RawSocketCommunication event to keep the buffer drained.
 *   - A peer that sends faster than the application can consume will be
 *     disconnected. A ConnectionLost event is delivered to the application
 *     thread. All other connections and threads in the reactor are unaffected.
 *
 * Outbound path:
 *   Identical to PduProtocolHandler: a PduFramer handles partial sends, slab
 *   chunk lifetime, and EPOLLOUT registration. send_prebuilt() accepts any
 *   pre-built frame, whether PDU-framed or raw.
 *
 * Threading model:
 *   All methods except commit_bytes() must be called from the reactor thread.
 *   commit_bytes() is called from the reactor thread in response to a
 *   CommitRawBytes command, so it is also reactor-thread only.
 *
 * Ownership:
 *   Owns the MirroredBuffer and PduFramer. Does not own the TcpSocket,
 *   ApplicationThread, or QuillLogger.
 */
class RawBytesProtocolHandler : public ProtocolHandlerInterface {
public:
    ~RawBytesProtocolHandler() override = default;

    RawBytesProtocolHandler(const RawBytesProtocolHandler&) = delete;
    RawBytesProtocolHandler& operator=(const RawBytesProtocolHandler&) = delete;

    /**
     * @brief Constructs a RawBytesProtocolHandler for an already-connected socket.
     *
     * @param[in] connection_id      The ConnectionID assigned to this connection.
     *                               Included in every RawSocketCommunication event
     *                               so the application can demultiplex messages
     *                               when multiple raw connections are active.
     * @param[in] socket             The connected TCP socket. Must outlive this object.
     * @param[in] target_thread      The ApplicationThread to receive
     *                               RawSocketCommunication events. Must outlive this object.
     * @param[in] buffer_capacity    Minimum capacity of the MirroredBuffer in bytes.
     *                               Rounded up to the nearest page size internally.
     * @param[in] disconnect_handler Called when the peer closes the connection,
     *                               a read error occurs, or the buffer overflows.
     * @param[in] logger             Logger for diagnostics. Must outlive this object.
     */
    RawBytesProtocolHandler(ConnectionID connection_id,
                             TcpSocket& socket,
                             ApplicationThread& target_thread,
                             int64_t buffer_capacity,
                             std::function<void()> disconnect_handler,
                             QuillLogger& logger);

    /**
     * @brief Services a readable socket event (EPOLLIN).
     *
     * Reads available bytes from the socket into the MirroredBuffer, then
     * enqueues a RawSocketCommunication EventMessage to the target thread
     * carrying a non-owning view of all currently unprocessed bytes.
     *
     * On peer disconnect (recv returns 0), read error, or buffer overflow
     * the disconnect handler is invoked and the method returns without
     * further access to this object.
     */
    void on_data_ready() override;

    /**
     * @brief Advances the ring buffer tail by the given number of bytes.
     *
     * Called by the Reactor in response to a CommitRawBytes command from the
     * application thread. Releases bytes that the application has finished
     * processing, making space for future reads.
     *
     * @param[in] bytes Number of bytes consumed by the application.
     * @throws PreconditionAssertion if bytes is negative or exceeds bytes_available().
     */
    void commit_bytes(int64_t bytes);

    /**
     * @brief Initiates a zero-copy send of a pre-built outbound frame.
     *
     * Delegates to PduFramer::send_prebuilt(). Partial-send state is recorded
     * internally; the Reactor will register EPOLLOUT and call continue_send()
     * as needed.
     *
     * @param[in] allocator   The slab allocator that owns chunk_ptr. Must not be nullptr.
     * @param[in] slab_id     Slab ID for deallocation when the send completes.
     * @param[in] chunk_ptr   Pointer to the start of the frame. Must not be nullptr.
     * @param[in] total_bytes Total frame size in bytes.
     */
    void send_prebuilt(ExpandableSlabAllocator* allocator,
                       int slab_id,
                       void* chunk_ptr,
                       uint32_t total_bytes) override;

    /**
     * @brief Returns true if a partial outbound send is in progress.
     */
    [[nodiscard]] bool has_pending_send() const override;

    /**
     * @brief Resumes a partial outbound send on EPOLLOUT.
     *
     * Delegates to PduFramer::continue_send(). When the send completes the
     * slab chunk is deallocated and pending-send state is cleared.
     */
    void continue_send() override;

    /**
     * @brief Deallocates any in-flight outbound slab chunk.
     *
     * Called by the Reactor during connection teardown when has_pending_send()
     * is true.
     */
    void deallocate_pending_send() override;

private:
    void release_pending_send();

    ConnectionID        connection_id_;
    TcpSocket&          socket_;
    ApplicationThread&  target_thread_;
    std::function<void()> disconnect_handler_;
    QuillLogger&        logger_;

    MirroredBuffer      buffer_;
    std::unique_ptr<PduFramer> framer_;

    ExpandableSlabAllocator* current_allocator_{nullptr};
    int      current_slab_id_{-1};
    void*    current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};
};

} // namespace pubsub_itc_fw
