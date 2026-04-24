#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

namespace pubsub_itc_fw {

/** @ingroup messaging_subsystem */

/**
 * @brief A unified, low-latency message envelope for inter-thread and reactor events.
 *
 * This class is designed as a lightweight, non-owning wrapper around event data.
 * It uses raw pointers for its payload to avoid heap allocations and overhead
 * in the critical path. All messages are created via static factory methods to
 * ensure consistent state and initialisation.
 *
 * Payload ownership:
 *   The payload applies to ITC messages and FrameworkPdu messages, not to system
 *   event messages such as INIT or TERM. In those system event cases the payload
 *   pointer is nullptr.
 *
 *   For FrameworkPdu messages, the payload points into a slab chunk allocated by
 *   the reactor's inbound slab allocator. The slab_id() accessor returns the
 *   corresponding slab ID. The receiving ApplicationThread must call
 *   release_pdu_payload() (or deallocate directly) after processing the payload.
 *   Failure to do so leaks the slab chunk.
 *
 *   For all other messages with a non-null payload, the buffer is managed by the
 *   allocator of the receiving component. The receiving thread is responsible
 *   for freeing it.
 *
 * Type safety:
 *   This class relies on explicit type checking and casting. The get_as()
 *   method is highly unsafe and must only be used after verifying the message
 *   type.
 */
class EventMessage {
public:
    /**
     * @brief Header structure containing message metadata.
     */
    struct Header {
        EventType type;
        int payload_size;
        int64_t tail_position;               ///< Used for RawSocketCommunication events.
        TimerID timer_id;                    ///< Used for Timer events.
        std::string reason;                  ///< Used for Termination and ConnectionFailed/ConnectionLost events.
        ThreadID originating_thread_id;      ///< Used for InterthreadCommunication events.
        ConnectionID connection_id;          ///< Used for ConnectionEstablished and ConnectionLost events.
    };

private:
    Header header_;
    const uint8_t* payload_{nullptr};
    int itc_message_type_{-1};
    int pdu_id_{-1};
    int slab_id_{-1};  ///< Slab ID for FrameworkPdu messages. -1 means not slab-allocated.

    /**
     * @brief Private constructor to enforce use of static factory methods.
     */
    EventMessage() = default;

    /**
     * @brief Private constructor for events with a payload.
     * @param[in] type         The type of the event.
     * @param[in] payload_data The raw pointer to the payload data.
     * @param[in] size         The size of the payload in bytes.
     */
    EventMessage(EventType type, const uint8_t* payload_data, int size)
        : header_{type, size, 0, TimerID(), "", ThreadID(), ConnectionID()}
        , payload_(payload_data) {}

public:
    // Disallow copy, allow move.
    EventMessage(const EventMessage&) = delete;
    EventMessage& operator=(const EventMessage&) = delete;
    EventMessage(EventMessage&& other) = default;
    EventMessage& operator=(EventMessage&& other) = default;

    /**
     * @brief Gets the ITC message subtype.
     *
     * Meaningful only when the event type is EventType::InterthreadCommunication.
     * Identifies the specific ITC message variant as defined by the receiving
     * thread's local ITC type registry.
     *
     * @return The ITC message subtype as a signed integer.
     */
    [[nodiscard]] int itc_message_type() const {
        return itc_message_type_;
    }

    /**
     * @brief Gets the PDU message identifier.
     *
     * Meaningful only when the event type is EventType::FrameworkPdu. Identifies
     * the specific PDU type as defined by the DSL-generated message ID constants.
     * The ApplicationThread uses this value to select the correct decode function
     * for the incoming PDU payload.
     *
     * @return The PDU identifier as a signed integer.
     */
    [[nodiscard]] int pdu_id() const {
        return pdu_id_;
    }

    /**
     * @brief Factory method for reactor internal events (Initial, AppReady).
     * @param[in] type Event type.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_reactor_event(EventType type);

    /**
     * @brief Factory method for timer events.
     * @param[in] timer_id The ID of the expired timer.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_timer_event(TimerID timer_id);

    /**
     * @brief Factory method for termination events.
     * @param[in] reason Termination reason string.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_termination_event(const std::string& reason);

    /**
     * @brief Factory method for pub/sub communication messages.
     * @param[in] data Raw pointer to the protocol packet data.
     * @param[in] size Size of the data in bytes.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_pubsub_message(const uint8_t* data, int size);

    /**
     * @brief Factory method for inter-thread communication messages.
     * @param[in] originating_thread_id The ID of the thread that created the message.
     * @param[in] data                  Raw pointer to the serialised object data.
     * @param[in] size                  Size of the data in bytes.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_itc_message(ThreadID originating_thread_id, const uint8_t* data, int size);

    /**
     * @brief Factory method for raw socket data (e.g. for foreign protocols).
     *
     * The tail_position parameter records the MirroredBuffer tail at the moment
     * of enqueue. The receiving ApplicationThread compares this against its last
     * seen tail to detect unambiguously whether the tail advanced between
     * deliveries, without relying on the fragile available < last_available_
     * heuristic.
     *
     * @param[in] connection_id  The connection on which the data was received.
     * @param[in] data           Raw pointer to the socket data.
     * @param[in] size           Size of the data in bytes.
     * @param[in] tail_position  MirroredBuffer tail at enqueue time.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_raw_socket_message(ConnectionID connection_id,
                                                                 const uint8_t* data,
                                                                 int size,
                                                                 int64_t tail_position);
    /**
     * @brief Factory method for framework PDU messages.
     *
     * These messages represent fully packetised PDUs decoded by the reactor's
     * framing layer. The payload is a slab-allocated chunk containing the raw
     * PDU bytes after header removal. The caller must pass the slab_id returned
     * by ExpandableSlabAllocator::allocate() so the receiving thread can free
     * the chunk after processing.
     *
     * @param[in] data     Pointer to the slab-allocated PDU payload. Must not be nullptr.
     * @param[in] size     Size of the PDU payload in bytes.
     * @param[in] slab_id  Slab ID from ExpandableSlabAllocator::allocate().
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_framework_pdu_message(const uint8_t* data, int size, int slab_id);

    /**
     * @brief Factory method for a successful outbound connection event.
     *
     * Delivered to the ApplicationThread that requested the connection when the
     * reactor has fully established the TCP connection and assigned a ConnectionID.
     *
     * @param[in] connection_id The ConnectionID assigned by the reactor.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_connection_established_event(ConnectionID connection_id);

    /**
     * @brief Factory method for a failed outbound connection attempt.
     *
     * Delivered to the ApplicationThread that requested the connection when the
     * TCP connect attempt fails (e.g. connection refused, timeout).
     *
     * @param[in] reason Human-readable description of the failure.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_connection_failed_event(const std::string& reason);

    /**
     * @brief Factory method for an unexpected connection loss event.
     *
     * Delivered to the ApplicationThread associated with a connection when the
     * reactor detects that the TCP connection has been dropped.
     *
     * @param[in] connection_id The ConnectionID of the lost connection.
     * @param[in] reason        Human-readable description of why the connection was lost.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_connection_lost_event(ConnectionID connection_id, const std::string& reason);

    /**
     * @brief Gets the MirroredBuffer tail position at enqueue time.
     *
     * Valid only for RawSocketCommunication events. The ApplicationThread
     * uses this to detect unambiguously whether the tail advanced between
     * two deliveries.
     *
     * @return Tail position in bytes at the time the message was enqueued.
     */
    [[nodiscard]] int64_t tail_position() const;

    /**
     * @brief Gets the event type.
     * @return The event type.
     */
    [[nodiscard]] EventType type() const;

    /**
     * @brief Gets the payload size in bytes.
     * @return The size of the payload, or 0 if no payload.
     */
    [[nodiscard]] int payload_size() const;

    /**
     * @brief Gets the timer ID.
     *
     * Valid only for Timer events.
     * @return The timer ID.
     */
    [[nodiscard]] TimerID timer_id() const;

    /**
     * @brief Gets the reason string.
     *
     * Valid for Termination, ConnectionFailed, and ConnectionLost events.
     * @return A const reference to the reason string.
     */
    [[nodiscard]] const std::string& reason() const;

    /**
     * @brief Gets the ID of the thread that sent the message.
     *
     * Valid only for InterthreadCommunication events.
     * @return The originating thread ID.
     */
    [[nodiscard]] ThreadID originating_thread_id() const;

    /**
     * @brief Gets the connection ID.
     *
     * Valid for ConnectionEstablished and ConnectionLost events.
     * @return The ConnectionID.
     */
    [[nodiscard]] ConnectionID connection_id() const;

    /**
     * @brief Gets the slab ID for FrameworkPdu messages.
     *
     * Valid only for FrameworkPdu events. Returns -1 for all other event types.
     * The receiving ApplicationThread must pass this value to
     * ExpandableSlabAllocator::deallocate() along with payload() after processing.
     *
     * @return The slab ID, or -1 if not a slab-allocated payload.
     */
    [[nodiscard]] int slab_id() const;

    /**
     * @brief Gets read-only access to the payload data.
     *
     * @warning Returns a raw pointer. The caller must not attempt to free
     * or modify the memory.
     * @return A const pointer to the payload bytes, or nullptr if no payload.
     */
    [[nodiscard]] const uint8_t* payload() const;

    /**
     * @brief Casts the payload to the specified type.
     *
     * @warning Extremely unsafe. Performs a reinterpret_cast and relies
     * entirely on the caller to ensure the underlying data is of type T.
     * Must only be used in performance-critical code after type() has been
     * checked.
     *
     * @tparam T The type to cast the payload to.
     * @return A const reference to the payload cast as type T.
     */
    template<typename T>
    [[nodiscard]] const T& get_as() const {
        return *reinterpret_cast<const T*>(payload_);
    }
};

} // namespace pubsub_itc_fw
