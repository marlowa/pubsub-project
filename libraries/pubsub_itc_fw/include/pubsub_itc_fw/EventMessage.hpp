#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <string>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/Message.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A unified, low-latency message envelope for inter-thread and reactor events.
 *
 * This class is designed as a lightweight, non-owning wrapper around event data.
 * It uses raw pointers for its payload to avoid heap allocations and overhead
 * in the critical path. All messages are created via static factory methods to
 * ensure consistent state and initialization.
 *
 * **Payload Ownership:**
 * THIS CLASS DOES NOT OWN THE PAYLOAD MEMORY. The lifetime of the payload
 * buffer is managed by the producing component (e.g., the Reactor). The consuming
 * component is responsible for processing the data before the buffer is reused.
 *
 * **Type Safety:**
 * This class relies on explicit type checking and casting. The `get_as()` method
 * is highly unsafe and must only be used after verifying the message type.
 */
class EventMessage {
public:
    /**
     * @brief Header structure containing message metadata.
     */
    struct Header {
        EventType type;
        int payload_size;
        TimerID timer_id;                   ///< Used for Timer events
        std::string reason;                 ///< Used for Termination events
        ThreadID originating_thread_id; ///< Used for InterthreadCommunication events
    };

private:
    Header header_;
    const uint8_t* payload_;

    /**
     * @brief Private constructor to enforce use of static factory methods.
     */
    EventMessage() = default;

    /**
     * @brief Private constructor for events with a payload.
     * @param [in] type The type of the event.
     * @param [in] payload_data The raw pointer to the payload data.
     * @param [in] size The size of the payload in bytes.
     */
    EventMessage(EventType type, const uint8_t* payload_data, int size)
        : header_{type, size, TimerID(), "", ThreadID()}, payload_(payload_data) {}

public:
    // Ensure all messages are moved, not copied, to prevent unintended sharing
    // of the raw payload pointer. TODO maybe we should just delete these functions.
    EventMessage(EventMessage&& other) noexcept;
    EventMessage& operator=(EventMessage&& other) noexcept;

    // Disallow copy operations to prevent unintended sharing of the raw pointer.
    EventMessage(const EventMessage&) = delete;
    EventMessage& operator=(const EventMessage&) = delete;

    /**
     * @brief Factory method for reactor internal events (Initial, AppReady).
     * @param [in] type Event type.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_reactor_event(EventType type);

    /**
     * @brief Factory method for timer events.
     * @param [in] timer_id The ID of the expired timer.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_timer_event(TimerID timer_id);

    /**
     * @brief Factory method for termination events.
     * @param [in] reason Termination reason string.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_termination_event(const std::string& reason);

    /**
     * @brief Factory method for Pub/Sub communication messages.
     *
     * @param [in] data Raw pointer to the protocol packet data.
     * @param [in] size Size of the data in bytes.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_pubsub_message(const uint8_t* data, int size);

    /**
     * @brief Factory method for inter-thread communication messages.
     *
     * @param [in] originating_thread_id The ID of the thread that created the message.
     * @param [in] data Raw pointer to the serialized C++ object data.
     * @param [in] size Size of the data in bytes.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_itc_message(ThreadID originating_thread_id, const uint8_t* data, int size);

    /**
     * @brief Factory method for raw socket data (e.g., for foreign protocols).
     *
     * @param [in] data Raw pointer to the socket data.
     * @param [in] size Size of the data in bytes.
     * @return EventMessage instance.
     */
    [[nodiscard]] static EventMessage create_raw_socket_message(const uint8_t* data, int size);

    /**
     * @brief Gets the event type.
     * @return The event type.
     */
    [[nodiscard]] EventType type() const noexcept;

    /**
     * @brief Gets the payload size in bytes.
     * @return The size of the payload, or 0 if no payload.
     */
    [[nodiscard]] int payload_size() const noexcept;

    /**
     * @brief Gets the timer ID.
     *
     * This method is only valid for Timer events.
     * @return The timer ID.
     */
    [[nodiscard]] TimerID timer_id() const noexcept;

    /**
     * @brief Gets the termination reason string.
     *
     * This method is only valid for Termination events.
     * @return A const reference to the reason string.
     */
    [[nodiscard]] const std::string& reason() const noexcept;

    /**
     * @brief Gets the ID of the thread that sent the message.
     *
     * This method is only valid for InterthreadCommunication events.
     * @return The originating thread ID.
     */
    [[nodiscard]] ThreadID originating_thread_id() const noexcept;

    /**
     * @brief Gets read-only access to the payload data.
     *
     * @warning This method returns a raw pointer. The caller must not
     * attempt to free or modify the memory.
     * @return A const pointer to the payload bytes, or `nullptr` if no payload.
     */
    [[nodiscard]] const uint8_t* payload() const noexcept;

    /**
     * @brief Casts the payload to the specified type.
     *
     * @warning This function is extremely unsafe. It performs a `reinterpret_cast`
     * and relies entirely on the caller to ensure the underlying data is of type `T`.
     * It should only be used in performance-critical code after the `type()`
     * method has been checked.
     *
     * @tparam T The type to cast the payload to.
     * @return A const reference to the payload cast as type T.
     */
    template<typename T>
    [[nodiscard]] const T& get_as() const noexcept {
        return *reinterpret_cast<const T*>(payload_);
    }
};

} // namespace pubsub_itc_fw
