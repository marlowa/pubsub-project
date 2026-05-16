#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <tuple>

namespace pubsub_itc_fw {

class ExpandableSlabAllocator;

/**
 * @brief Strategy interface for transforming raw socket streams into application events.
 *
 * @ingroup reactor_subsystem
 *
 * This interface decouples the Reactor's connection management (epoll handling,
 * idle timeouts, and socket lifecycle) from the protocol-specific framing logic.
 *
 * Each concrete implementation is constructed with all the resources it needs
 * (socket, target thread, allocator, logger) bound at construction time. The
 * handler owns all protocol-specific state including outbound partial-send
 * bookkeeping, so InboundConnection remains a thin transport shell.
 *
 * Lifecycle and responsibilities of each implementation:
 *   - Read:       call receive() or equivalent on the bound socket.
 *   - Frame:      identify message boundaries according to the specific protocol.
 *   - Package:    wrap data in EventMessage envelopes.
 *   - Route:      dispatch messages to the target thread via the Vyukov queue.
 *   - Disconnect: signal failure to the caller via the return value of
 *                 on_data_ready, send_prebuilt, or continue_send. The owning
 *                 manager is responsible for tearing the connection down.
 *   - Send:       manage outbound framing and partial-send state.
 *
 * Two concrete implementations exist:
 *   - PduProtocolHandler (Strategy A): framework-native PDU framing via
 *     PduParser and PduFramer; delivers FrameworkPdu EventMessages.
 *   - RawBytesProtocolHandler (Strategy B): raw byte streaming via
 *     MirroredBuffer; delivers RawSocketCommunication EventMessages and
 *     exposes commit_bytes() for application-driven tail advancement.
 */
class ProtocolHandlerInterface {
  public:
    /**
     * @brief Virtual destructor to ensure correct cleanup of protocol-specific
     *        resources such as MirroredBuffers or PduParsers.
     */
    virtual ~ProtocolHandlerInterface() = default;

    /**
     * @brief Service routine called by the Reactor when the underlying socket
     *        has data available to read (EPOLLIN).
     *
     * Implementations must drain the socket or read as much as internal
     * buffering allows, performing in-place framing where possible to
     * minimise copies.
     *
     * @return A tuple of { success, error_string, pause_reads }.
     *         success is true if the read completed cleanly (including the case
     *         where no bytes were available). success is false if the connection
     *         must be closed; error_string is empty for a graceful peer
     *         disconnect and contains a description on protocol failure.
     *         pause_reads is true if, as a result of this read, the handler
     *         wants the manager to deregister EPOLLIN on this socket as a
     *         backpressure signal. Only RawBytesProtocolHandler ever sets this;
     *         PDU handlers always return false. The manager must re-register
     *         EPOLLIN only when commit_bytes() subsequently returns true.
     */
    [[nodiscard]] virtual std::tuple<bool, std::string, bool> on_data_ready() = 0;

    /**
     * @brief Initiates a zero-copy send of a pre-built frame.
     *
     * Called by the Reactor to transmit a complete outbound frame that has
     * been pre-built by the ApplicationThread in a slab-allocated chunk.
     * If the send cannot complete immediately the implementation must record
     * the partial-send state internally so that continue_send() can resume it.
     *
     * The allocator and slab_id are stored by the implementation so that the
     * slab chunk can be returned to the correct allocator when the send
     * completes or when deallocate_pending_send() is called on teardown.
     *
     * @param[in] allocator   The slab allocator that owns chunk_ptr. Must not be nullptr.
     * @param[in] slab_id     Slab ID for deallocation when the send completes.
     * @param[in] chunk_ptr   Pointer to the start of the frame. Must not be nullptr.
     * @param[in] total_bytes Total frame size in bytes.
     *
     * @return A tuple of { success, error_string }. success is false on
     *         unrecoverable send failure; the implementation will have already
     *         released the slab chunk in that case.
     */
    [[nodiscard]] virtual std::tuple<bool, std::string> send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) = 0;

    /**
     * @brief Returns true if a partial outbound send is in progress.
     *
     * When true the Reactor must have registered EPOLLOUT and will call
     * continue_send() when the socket becomes writable again.
     */
    [[nodiscard]] virtual bool has_pending_send() const = 0;

    /**
     * @brief Resumes a partial outbound send.
     *
     * Called by the Reactor when epoll signals EPOLLOUT and has_pending_send()
     * is true. The implementation must attempt to write the remaining bytes.
     * When the send completes it must deallocate the slab chunk and clear its
     * internal pending-send state.
     *
     * @return A tuple of { success, error_string }. success is false on
     *         unrecoverable send failure; the implementation will have already
     *         released the slab chunk in that case.
     */
    [[nodiscard]] virtual std::tuple<bool, std::string> continue_send() = 0;

    /**
     * @brief Deallocates any in-flight outbound slab chunk.
     *
     * Called by the Reactor during connection teardown when has_pending_send()
     * is true. Ensures the slab chunk is returned to the allocator even if the
     * send never completes.
     */
    virtual void deallocate_pending_send() = 0;

    /**
     * @brief Advances the inbound ring buffer tail by the given number of bytes.
     *
     * Called by the Reactor in response to a CommitRawBytes command. Only
     * meaningful for RawBytesProtocolHandler; the default implementation is a
     * no-op so that PduProtocolHandler does not need to override it.
     *
     * @param[in] bytes Number of bytes the application has finished processing.
     * @return true if, as a result of this commit, the handler wants the
     *         manager to re-register EPOLLIN on this socket (i.e. backpressure
     *         has been released). Only RawBytesProtocolHandler ever returns
     *         true; PDU handlers always return false.
     */
    virtual bool commit_bytes([[maybe_unused]] int64_t bytes) { return false; }

    /**
     * @brief Returns true if the handler is currently asking the manager to
     *        keep EPOLLIN deregistered for backpressure.
     *
     * The manager queries this whenever it constructs an epoll_event for
     * EPOLL_CTL_MOD on this socket (e.g. when adding or removing EPOLLOUT for
     * a pending send) so that it does not inadvertently re-enable EPOLLIN
     * while reads are paused.
     *
     * Default implementation returns false. Only RawBytesProtocolHandler
     * overrides this.
     */
    [[nodiscard]] virtual bool is_reads_paused() const { return false; }
};

} // namespace pubsub_itc_fw
