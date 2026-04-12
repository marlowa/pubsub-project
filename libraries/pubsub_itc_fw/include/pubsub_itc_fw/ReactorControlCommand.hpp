#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A command sent from an ApplicationThread to the Reactor via the
 *        lock-free command queue.
 *
 * All socket I/O and timer management is performed exclusively by the reactor
 * thread. ApplicationThreads request these operations by enqueuing a
 * ReactorControlCommand and signalling the reactor's wakeup_fd.
 *
 * Each CommandTag has a dedicated set of payload fields. Fields that do not
 * belong to a given tag are ignored by the reactor.
 *
 * Tag / payload field mapping:
 *
 *   AddTimer       — owner_thread_id_, timer_id_, timer_name_, interval_, timer_type_
 *   CancelTimer    — owner_thread_id_, timer_id_
 *   Connect        — requesting_thread_id_, service_name_
 *   Disconnect     — connection_id_
 *   SendPdu        — connection_id_, allocator_, slab_id_, pdu_chunk_ptr_, pdu_byte_count_
 *   SendRaw        — connection_id_, allocator_, slab_id_, raw_chunk_ptr_, raw_byte_count_
 *   CommitRawBytes — connection_id_, bytes_consumed_
 *
 * SendPdu vs SendRaw:
 *   SendPdu is for framework-native PDU connections (PduProtocolHandler). The
 *   reactor prepends a PduHeader and the total wire size is
 *   sizeof(PduHeader) + pdu_byte_count_.
 *
 *   SendRaw is for raw-bytes connections (RawBytesProtocolHandler). The chunk
 *   contains the complete outbound bytes exactly as they should appear on the
 *   wire. No header is prepended. The total wire size is raw_byte_count_.
 */
class ReactorControlCommand {
public:
    enum CommandTag {
        AddTimer,
        CancelTimer,
        Connect,         ///< Request the reactor to establish an outbound TCP connection.
        Disconnect,      ///< Request the reactor to close an established connection.
        SendPdu,         ///< Request the reactor to send a framed PDU on a PDU connection.
        SendRaw,         ///< Request the reactor to send raw bytes on a raw-bytes connection.
        CommitRawBytes   ///< Notify the reactor that the application has consumed N bytes from
                         ///< the MirroredBuffer of a RawBytesProtocolHandler connection.
    };

public:
    explicit ReactorControlCommand(CommandTag tag)
        : tag_(tag)
    {
    }

    CommandTag as_tag() const {
        return tag_;
    }

    std::string as_string() const {
        if (tag_ == AddTimer) {
            return "AddTimer";
        }
        if (tag_ == CancelTimer) {
            return "CancelTimer";
        }
        if (tag_ == Connect) {
            return "Connect";
        }
        if (tag_ == Disconnect) {
            return "Disconnect";
        }
        if (tag_ == SendPdu) {
            return "SendPdu";
        }
        if (tag_ == SendRaw) {
            return "SendRaw";
        }
        if (tag_ == CommitRawBytes) {
            return "CommitRawBytes";
        }
        return fmt::format("unknown ({})", static_cast<int>(tag_));
    }

    bool is_equal(const ReactorControlCommand& rhs) const {
        return tag_ == rhs.tag_;
    }

public:
    // ----------------------------------------------------------------
    // AddTimer / CancelTimer payload fields
    // ----------------------------------------------------------------

    ThreadID owner_thread_id_{};
    TimerID timer_id_{};
    std::string timer_name_;
    std::chrono::microseconds interval_{0};
    TimerType timer_type_{TimerType::SingleShot};

    // ----------------------------------------------------------------
    // Connect payload fields
    // ----------------------------------------------------------------

    /**
     * @brief The thread to notify with ConnectionEstablished or ConnectionFailed.
     *
     * The reactor delivers an EventMessage of type ConnectionEstablished
     * (carrying the assigned ConnectionID) or ConnectionFailed (carrying a
     * reason string) to this thread's queue once the non-blocking connect
     * attempt completes.
     */
    ThreadID requesting_thread_id_{};

    /**
     * @brief The logical name of the service to connect to.
     *
     * The reactor resolves this name via the ServiceRegistry it was constructed
     * with, obtaining the primary and secondary endpoints to try.
     */
    std::string service_name_;

    // ----------------------------------------------------------------
    // Disconnect / SendPdu / SendRaw / CommitRawBytes payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Identifies the target connection.
     *
     * Used by Disconnect, SendPdu, SendRaw, and CommitRawBytes commands. Must
     * be a valid ConnectionID previously delivered via a ConnectionEstablished
     * event.
     */
    ConnectionID connection_id_{};

    // ----------------------------------------------------------------
    // SendPdu payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Pointer to the ExpandableSlabAllocator that owns the PDU or raw chunk.
     *
     * Each ApplicationThread owns its own outbound ExpandableSlabAllocator.
     * When enqueuing a SendPdu or SendRaw command the thread sets this field to
     * a pointer to its own allocator. The reactor calls
     * allocator_->deallocate(slab_id_, chunk_ptr) after the frame has been fully
     * transmitted.
     *
     * This pointer is never sent over the network — it is purely local
     * bookkeeping so the reactor knows which allocator to return the chunk to.
     * ExpandableSlabAllocator::deallocate() is thread-safe so the reactor
     * may call it from the reactor thread without synchronisation.
     *
     * Must not be nullptr when the SendPdu or SendRaw command is processed.
     */
    ExpandableSlabAllocator* allocator_{nullptr};

    /**
     * @brief Slab ID of the chunk holding the outbound frame.
     *
     * Passed to allocator_->deallocate() alongside the chunk pointer after
     * the frame has been fully transmitted.
     */
    int slab_id_{-1};

    /**
     * @brief Pointer to the start of the PDU frame in the slab chunk.
     *
     * Points to the PduHeader, which is immediately followed by the encoded
     * payload. The reactor must not modify or free this memory until
     * transmission is complete.
     */
    void* pdu_chunk_ptr_{nullptr};

    /**
     * @brief Size of the PDU payload in bytes, excluding the PduHeader.
     *
     * The total frame size transmitted on the wire is
     * sizeof(PduHeader) + pdu_byte_count_.
     */
    uint32_t pdu_byte_count_{0};

    // ----------------------------------------------------------------
    // SendRaw payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Pointer to the start of the raw outbound bytes in the slab chunk.
     *
     * The chunk contains the complete wire bytes exactly as they should be
     * transmitted — no header is prepended by the reactor. The total number
     * of bytes transmitted is raw_byte_count_.
     *
     * The same allocator_ and slab_id_ fields used by SendPdu are reused for
     * slab ownership bookkeeping. The reactor calls
     * allocator_->deallocate(slab_id_, raw_chunk_ptr_) after transmission
     * completes.
     *
     * Must not be nullptr when the SendRaw command is processed.
     */
    void* raw_chunk_ptr_{nullptr};

    /**
     * @brief Total number of raw bytes to transmit.
     *
     * Unlike pdu_byte_count_, this is the complete wire size — no header
     * arithmetic is applied.
     */
    uint32_t raw_byte_count_{0};

    // ----------------------------------------------------------------
    // CommitRawBytes payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Number of bytes the application has finished processing.
     *
     * The reactor calls RawBytesProtocolHandler::commit_bytes(bytes_consumed_)
     * which advances the MirroredBuffer tail by this amount, freeing space for
     * future reads. Must be > 0 and <= MirroredBuffer::bytes_available() at the
     * time the command is processed.
     */
    int64_t bytes_consumed_{0};

private:
    CommandTag tag_{AddTimer};
};

inline bool operator==(const ReactorControlCommand& lhs,
                       const ReactorControlCommand& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const ReactorControlCommand& lhs,
                       const ReactorControlCommand& rhs)
{
    return !lhs.is_equal(rhs);
}

} // namespace pubsub_itc_fw
