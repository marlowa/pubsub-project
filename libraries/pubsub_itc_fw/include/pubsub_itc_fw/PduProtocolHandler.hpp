#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <memory>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Concrete protocol handler for framework-native PDU streams.
 *
 * @ingroup reactor_subsystem
 *
 * This is the Strategy A implementation of ProtocolHandlerInterface. It
 * encapsulates a PduParser and PduFramer, which together provide the
 * zero-copy framing layer for connections carrying framework PDUs.
 *
 * Inbound path:
 *   The disconnect handler is stored at construction time and forwarded to
 *   the PduParser. The PduParser invokes it on graceful peer disconnect
 *   (recv returns 0). For protocol errors (canary mismatch, slab allocation
 *   failure, read error), the PduParser returns {false, error_string} without
 *   invoking the handler; on_data_ready() logs the error and invokes the
 *   handler itself so the Reactor tears down the connection in all failure cases.
 *
 * Outbound path:
 *   This handler owns the partial-send state (allocator pointer, slab ID,
 *   chunk pointer, total bytes). The Reactor calls send_prebuilt() to initiate
 *   a send, continue_send() to resume a partial send on EPOLLOUT, and
 *   deallocate_pending_send() during teardown if a send is in flight.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   Does not own the TcpSocket, ApplicationThread, ExpandableSlabAllocator,
 *   or QuillLogger. The caller is responsible for their lifetimes.
 *   The PduParser and PduFramer are owned exclusively by this handler.
 */
class PduProtocolHandler : public ProtocolHandlerInterface {
  public:
    ~PduProtocolHandler() override = default;

    PduProtocolHandler(const PduProtocolHandler&) = delete;
    PduProtocolHandler& operator=(const PduProtocolHandler&) = delete;

    /**
     * @brief Constructs a PduProtocolHandler for an already-connected socket.
     *
     * @param[in] socket             The connected TCP socket. Must outlive this object.
     * @param[in] target_thread      The ApplicationThread to which complete PDUs are
     *                               dispatched. Must outlive this object.
     * @param[in] inbound_allocator  Slab allocator for inbound PDU payload chunks.
     *                               Must outlive this object.
     * @param[in] disconnect_handler Called when the peer closes the connection or a
     *                               protocol error forces closure. Typically a lambda
     *                               calling Reactor::teardown_inbound_connection().
     * @param[in] logger             Logger for protocol error diagnostics.
     *                               Must outlive this object.
     */
    PduProtocolHandler(TcpSocket& socket, ApplicationThread& target_thread, ExpandableSlabAllocator& inbound_allocator,
                       std::function<void()> disconnect_handler, QuillLogger& logger);

    /**
     * @brief Services a readable socket by draining available PDU frames.
     *
     * Called by the Reactor when epoll signals EPOLLIN on the underlying socket.
     * Delegates to PduParser::receive(), which reads header and payload bytes,
     * allocates slab chunks, and dispatches complete FrameworkPdu EventMessages
     * to the target ApplicationThread.
     *
     * On graceful peer disconnect the PduParser invokes the disconnect handler
     * directly. On protocol errors this method logs the error and invokes the
     * disconnect handler before returning.
     */
    void on_data_ready() override;

    /**
     * @brief Initiates a zero-copy send of a pre-built PDU frame.
     *
     * Delegates to PduFramer::send_prebuilt(). If the send cannot complete
     * immediately the partial-send state is recorded internally.
     *
     * @param[in] allocator   The slab allocator that owns chunk_ptr. Must not be nullptr.
     * @param[in] slab_id     Slab ID for deallocation when the send completes.
     * @param[in] chunk_ptr   Pointer to the start of the PDU frame. Must not be nullptr.
     * @param[in] total_bytes Total frame size in bytes.
     */
    void send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) override;

    /**
     * @brief Returns true if a partial outbound PDU send is in progress.
     */
    [[nodiscard]] bool has_pending_send() const override;

    /**
     * @brief Resumes a partial outbound PDU send on EPOLLOUT.
     *
     * Delegates to PduFramer::continue_send(). When the send completes the
     * slab chunk is deallocated and internal pending-send state is cleared.
     */
    void continue_send() override;

    /**
     * @brief Deallocates any in-flight outbound slab chunk.
     *
     * Called during connection teardown when has_pending_send() is true.
     */
    void deallocate_pending_send() override;

  private:
    void release_pending_send();

    std::unique_ptr<PduParser> parser_;
    std::unique_ptr<PduFramer> framer_;
    std::function<void()> disconnect_handler_;
    QuillLogger& logger_;

    ExpandableSlabAllocator* current_allocator_{nullptr};
    int current_slab_id_{-1};
    void* current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};
};

} // namespace pubsub_itc_fw
