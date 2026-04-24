#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

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
 * (socket, target thread, allocator, disconnect handler, logger) bound at
 * construction time. The handler owns all protocol-specific state including
 * outbound partial-send bookkeeping, so InboundConnection remains a thin
 * transport shell.
 *
 * Lifecycle and responsibilities of each implementation:
 *   - Read:       call receive() or equivalent on the bound socket.
 *   - Frame:      identify message boundaries according to the specific protocol.
 *   - Package:    wrap data in EventMessage envelopes.
 *   - Route:      dispatch messages to the target thread via the Vyukov queue.
 *   - Disconnect: invoke the stored disconnect handler on EOF or protocol error.
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
     */
    virtual void on_data_ready() = 0;

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
     */
    virtual void send_prebuilt(ExpandableSlabAllocator* allocator,
                               int slab_id,
                               void* chunk_ptr,
                               uint32_t total_bytes) = 0;

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
     */
    virtual void continue_send() = 0;

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
     */
    virtual void commit_bytes([[maybe_unused]] int64_t bytes) {}

    /**
     * @brief Returns the number of bytes currently buffered and not yet committed.
     *
     * Only meaningful for RawBytesProtocolHandler. The default returns 0.
     */
    [[nodiscard]] virtual int64_t bytes_buffered() const { return 0; }

    /**
     * @brief Returns a pointer to the start of the buffered data, or nullptr.
     *
     * Only meaningful for RawBytesProtocolHandler. The default returns nullptr.
     */
    [[nodiscard]] virtual const uint8_t* buffered_read_ptr() const { return nullptr; }

    /**
     * @brief Returns true if on_data_ready() has enqueued a delivery that
     *        has not yet been acknowledged by a commit_bytes() call.
     *
     * Used by InboundConnectionManager::deliver_pending_redeliveries() to
     * suppress re-delivery when the original on_data_ready() message is still
     * unprocessed in the application thread's queue, preventing duplicate
     * deliveries. The default returns false.
     */
    [[nodiscard]] virtual bool has_fresh_delivery_pending() const { return false; }

    /**
     * @brief Atomically checks and clears the pending re-delivery flag.
     *
     * Set by commit_bytes() when bytes remain in the buffer after tail
     * advancement. Cleared by this method and by on_data_ready().
     * Used by InboundConnectionManager::deliver_pending_redeliveries() which
     * is called once per process_control_commands() pass, after the entire
     * command queue has been drained, so that all CommitRawBytes commands for
     * a given burst are processed before any re-delivery is enqueued.
     * The default returns false.
     */
    [[nodiscard]] virtual bool take_pending_redelivery() { return false; }
};

} // namespace pubsub_itc_fw
