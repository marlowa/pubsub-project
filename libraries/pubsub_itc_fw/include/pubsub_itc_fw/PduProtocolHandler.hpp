#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
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
 *   The PduParser reads framed PDUs from the socket. Failures (graceful
 *   disconnect, canary mismatch, slab allocation failure, read error) are
 *   reported to the caller via the return value of on_data_ready(). The owning
 *   manager is responsible for tearing the connection down.
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
 *   Does not own the TcpSocket, ApplicationThread, or ExpandableSlabAllocator.
 *   The caller is responsible for their lifetimes.
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
     * @param[in] logger             Logger forwarded to the PduParser for trace and
     *                               diagnostic output. Must outlive this object.
     * @param[in] connection_id      The ConnectionID of this connection, carried in
     *                               every FrameworkPdu EventMessage so that
     *                               on_framework_pdu_message() can identify the source.
     */
    PduProtocolHandler(TcpSocket& socket, ApplicationThread& target_thread, ExpandableSlabAllocator& inbound_allocator, QuillLogger& logger,
                       ConnectionID connection_id);

    /**
     * @brief Services a readable socket by draining available PDU frames.
     *
     * Called by the Reactor when epoll signals EPOLLIN on the underlying socket.
     * Delegates to PduParser::receive(), which reads header and payload bytes,
     * allocates slab chunks, and dispatches complete FrameworkPdu EventMessages
     * to the target ApplicationThread.
     *
     * Strategy A (framework PDUs) has its own backpressure mechanism via slab
     * allocation, so this handler never asks the manager to pause reads. The
     * third tuple element is always false.
     *
     * @return The tuple returned by PduParser::receive() augmented with a
     *         hard-coded false: {true, "", false} on a clean read (including
     *         no bytes available), {false, "", false} on graceful peer
     *         disconnect, or {false, error_string, false} on protocol failure.
     */
    [[nodiscard]] std::tuple<bool, std::string, bool> on_data_ready() override;

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
     *
     * @return {true, ""} if the send completed or progressed normally; {false,
     *         error_string} if the underlying send failed unrecoverably. The
     *         slab chunk is released before returning on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) override;

    /**
     * @brief Returns true if a partial outbound PDU send is in progress.
     */
    [[nodiscard]] bool has_pending_send() const override;

    /**
     * @brief Resumes a partial outbound PDU send on EPOLLOUT.
     *
     * Delegates to PduFramer::continue_send(). When the send completes the
     * slab chunk is deallocated and internal pending-send state is cleared.
     *
     * @return {true, ""} on progress or completion; {false, error_string} on
     *         unrecoverable send failure. The slab chunk is released before
     *         returning on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> continue_send() override;

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

    ExpandableSlabAllocator* current_allocator_{nullptr};
    int current_slab_id_{-1};
    void* current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};
};

} // namespace pubsub_itc_fw
