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
 *   AddTimer    — owner_thread_id_, timer_id_, timer_name_, interval_, timer_type_
 *   CancelTimer — owner_thread_id_, timer_id_
 *   Connect     — requesting_thread_id_, service_name_
 *   Disconnect  — connection_id_
 *   SendPdu     — connection_id_, allocator_, slab_id_, pdu_chunk_ptr_, pdu_byte_count_
 */
class ReactorControlCommand {
public:
    enum CommandTag {
        AddTimer,
        CancelTimer,
        Connect,    ///< Request the reactor to establish an outbound TCP connection.
        Disconnect, ///< Request the reactor to close an established connection.
        SendPdu     ///< Request the reactor to send a framed PDU on an established connection.
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
    // Disconnect / SendPdu payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Identifies the target connection.
     *
     * Used by Disconnect and SendPdu commands. Must be a valid ConnectionID
     * previously delivered via a ConnectionEstablished event.
     */
    ConnectionID connection_id_{};

    // ----------------------------------------------------------------
    // SendPdu payload fields
    // ----------------------------------------------------------------

    /**
     * @brief Pointer to the ExpandableSlabAllocator that owns the PDU chunk.
     *
     * Each ApplicationThread owns its own outbound ExpandableSlabAllocator.
     * When enqueuing a SendPdu command the thread sets this field to a pointer
     * to its own allocator. The reactor calls allocator_->deallocate(slab_id_,
     * pdu_chunk_ptr_) after the frame has been fully transmitted.
     *
     * This pointer is never sent over the network — it is purely local
     * bookkeeping so the reactor knows which allocator to return the chunk to.
     * ExpandableSlabAllocator::deallocate() is thread-safe so the reactor
     * may call it from the reactor thread without synchronisation.
     *
     * Must not be nullptr when the SendPdu command is processed.
     */
    ExpandableSlabAllocator* allocator_{nullptr};

    /**
     * @brief Slab ID of the chunk holding the complete PDU frame.
     *
     * Passed to allocator_->deallocate() alongside pdu_chunk_ptr_ after
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
