#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
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
 *   When the MirroredBuffer fill level reaches the high-water mark
 *   (high_water_numerator/water_denominator of capacity) the handler asks the
 *   manager to deregister EPOLLIN on the socket. The TCP receive window then
 *   closes naturally as the kernel buffer fills, applying backpressure all the
 *   way to the peer. When the application has drained the buffer below the
 *   low-water mark (low_water_numerator/water_denominator of capacity) the
 *   handler asks the manager to re-register EPOLLIN.
 *
 *   The pause/resume signals are carried in the return values of
 *   on_data_ready() and commit_bytes() respectively. Hysteresis between the
 *   high-water and low-water marks prevents flapping.
 *
 *   If the buffer somehow fills entirely (which should not happen with the
 *   pause logic in place but is defended against), on_data_ready() returns
 *   {false, "buffer full ..."} and the manager tears down the connection.
 *   This is a defensive fallback only.
 *
 *   Implications for callers:
 *   - Size buffer_capacity generously relative to the maximum burst size of
 *     the protocol in use. The high-water mark is at 75% of capacity, so a
 *     burst that fits in 75% of capacity will never cause TCP backpressure.
 *   - The application thread must call commit_raw_bytes() promptly after
 *     processing each RawSocketCommunication event so that reads can resume
 *     after backpressure-induced pauses.
 *
 * Outbound path:
 *   The handler owns its own raw send loop. send_prebuilt() accepts a
 *   slab-allocated chunk of arbitrary raw bytes (no framework header is
 *   prepended) and writes it to the socket. Partial writes are tracked
 *   internally: if the socket cannot accept all bytes immediately, the
 *   reactor registers EPOLLOUT and calls continue_send() when the socket
 *   becomes writable. When the send completes the slab chunk is released.
 *
 *   This handler must not delegate to PduFramer. PduFramer is the
 *   strategy-A framer and produces a PduHeader-prefixed wire format;
 *   strategy B is by definition header-free.
 *
 * Threading model:
 *   All methods except commit_bytes() must be called from the reactor thread.
 *   commit_bytes() is called from the reactor thread in response to a
 *   CommitRawBytes command, so it is also reactor-thread only.
 *
 * Ownership:
 *   Holds the MirroredBuffer via shared_ptr so that enqueued
 *   RawSocketCommunication EventMessages can each share ownership too. This
 *   prevents the buffer from being unmapped while events that reference it
 *   are still pending in the application thread's queue. Does not own the
 *   TcpSocket or ApplicationThread.
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
     */
    RawBytesProtocolHandler(ConnectionID connection_id, TcpSocket& socket, ApplicationThread& target_thread, int64_t buffer_capacity);

    /**
     * @brief Services a readable socket event (EPOLLIN).
     *
     * Reads available bytes from the socket into the MirroredBuffer, then
     * enqueues a RawSocketCommunication EventMessage to the target thread
     * carrying a non-owning view of all currently unprocessed bytes.
     *
     * After the read, the MirroredBuffer fill level is compared against the
     * high-water mark. If at or above high-water, the pause_reads element of
     * the returned tuple is set true, asking the manager to deregister EPOLLIN
     * on this socket. The pause signal is edge-triggered: it is only set on
     * the false-to-true transition of reads_paused_, never while already
     * paused.
     *
     * @return {true, "", pause} on a clean read where pause is true on the
     *         transition into the paused state, {false, "", false} on a
     *         graceful peer disconnect, or {false, error_string, false} on
     *         read error or (defensively) buffer overflow.
     */
    [[nodiscard]] std::tuple<bool, std::string, bool> on_data_ready() override;

    /**
     * @brief Advances the ring buffer tail by the given number of bytes.
     *
     * Called by the Reactor in response to a CommitRawBytes command from the
     * application thread. Releases bytes that the application has finished
     * processing, making space for future reads.
     *
     * If reads are currently paused and the fill level after the advance is
     * below the low-water mark, returns true to ask the manager to re-register
     * EPOLLIN on this socket. The resume signal is edge-triggered: it is only
     * set on the true-to-false transition of reads_paused_, never while
     * already resumed.
     *
     * @param[in] bytes Number of bytes consumed by the application.
     * @return true if reads should be resumed (EPOLLIN re-registered), false
     *         otherwise.
     */
    bool commit_bytes(int64_t bytes) override;

    /**
     * @brief Initiates a zero-copy send of an outbound raw byte chunk.
     *
     * Writes the chunk to the socket. If the socket cannot accept all bytes
     * immediately, the unsent remainder is tracked internally; the Reactor
     * will register EPOLLOUT and call continue_send() as needed. The chunk
     * is *not* framed -- no PduHeader is prepended.
     *
     * @param[in] allocator   The slab allocator that owns chunk_ptr. Must not be nullptr.
     * @param[in] slab_id     Slab ID for deallocation when the send completes.
     * @param[in] chunk_ptr   Pointer to the start of the frame. Must not be nullptr.
     * @param[in] total_bytes Total frame size in bytes.
     *
     * @return {true, ""} if the send completed or progressed normally; {false,
     *         error_string} on unrecoverable send failure. The slab chunk is
     *         released before returning on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) override;

    /**
     * @brief Returns true if a partial outbound send is in progress.
     */
    [[nodiscard]] bool has_pending_send() const override;

    /**
     * @brief Returns true if reads are currently paused for backpressure.
     *
     * Used by InboundConnectionManager when modifying this connection's epoll
     * registration for unrelated reasons (e.g. adding or removing EPOLLOUT
     * around a send) so it can avoid re-enabling EPOLLIN while paused.
     */
    [[nodiscard]] bool is_reads_paused() const override {
        return reads_paused_;
    }

    /**
     * @brief Resumes a partial outbound send on EPOLLOUT.
     *
     * Continues writing the unsent remainder of the in-flight chunk. When the
     * send completes the slab chunk is deallocated and pending-send state is
     * cleared.
     *
     * @return {true, ""} on progress or completion; {false, error_string} on
     *         unrecoverable send failure. The slab chunk is released before
     *         returning on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> continue_send() override;

    /**
     * @brief Deallocates any in-flight outbound slab chunk.
     *
     * Called by the Reactor during connection teardown when has_pending_send()
     * is true.
     */
    void deallocate_pending_send() override;

  private:
    // High-water and low-water thresholds for read-side backpressure, as
    // fractions of MirroredBuffer capacity. When bytes_available reaches
    // 3/4 of capacity we ask the manager to deregister EPOLLIN; when it
    // drops to 1/2 of capacity (while paused) we ask the manager to
    // re-register EPOLLIN. The gap provides hysteresis to prevent flapping.
    //
    // Expressed as numerator/denominator so the comparisons can be done in
    // integer arithmetic: bytes_available * denominator >= capacity * numerator.
    static constexpr int64_t water_denominator = 4;
    static constexpr int64_t high_water_numerator = 3; // 75%
    static constexpr int64_t low_water_numerator = 2;  // 50%

    /**
     * @brief Writes as much of the in-flight chunk as the socket will accept.
     *
     * Loops calling TcpSocket::send() until either the chunk is fully written
     * (send_offset_ reaches frame_size_) or the socket reports EAGAIN. Updates
     * send_offset_ as bytes are written.
     *
     * @return {true, ""} on full or partial progress (including EAGAIN);
     *         {false, error_string} on unrecoverable send error.
     */
    [[nodiscard]] std::tuple<bool, std::string> attempt_send_remaining();

    void release_pending_send();

    ConnectionID connection_id_;
    TcpSocket& socket_;
    ApplicationThread& target_thread_;

    // The MirroredBuffer is held as a shared_ptr (not a value member) so that
    // each enqueued RawSocketCommunication EventMessage can also hold a
    // shared_ptr to it. This extends the buffer's lifetime past the lifetime
    // of this handler in the case where the reactor tears down the connection
    // (e.g. buffer-full backpressure) while events for that connection are
    // still queued for the application thread. Without this, the buffer's
    // mmap region would be unmapped and the events' payload pointers would
    // dangle, causing a SEGV when the application thread reads from them.
    std::shared_ptr<MirroredBuffer> buffer_;

    // Outbound send state. While a send is in flight, active_frame_ptr_ points
    // into the caller's slab chunk (held alive by current_chunk_ptr_), frame_size_
    // is the total chunk size, and send_offset_ is the number of bytes already
    // written. When send_offset_ reaches frame_size_ the chunk is deallocated
    // and these are reset.
    const uint8_t* active_frame_ptr_{nullptr};
    uint32_t frame_size_{0};
    uint32_t send_offset_{0};

    ExpandableSlabAllocator* current_allocator_{nullptr};
    int current_slab_id_{-1};
    void* current_chunk_ptr_{nullptr};
    uint32_t current_total_bytes_{0};

    // True when reads have been paused because the MirroredBuffer crossed the
    // high-water mark. Used to make pause/resume signals edge-triggered: the
    // pause signal is emitted only on the false->true transition in
    // on_data_ready(), the resume signal only on the true->false transition
    // in commit_bytes().
    bool reads_paused_{false};
};

} // namespace pubsub_itc_fw
