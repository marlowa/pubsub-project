#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <string>
#include <variant>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/Message.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Represents a message used for inter-thread communication.
 *
 * This struct uses a `std::variant` as a discriminated union to identify the
 * type of event, allowing different data types to be passed through a single queue.
 * This is a safer and more modern alternative to a raw `void*`.
 */
struct EventMessage {
    /**
     * @brief Represents the type of a message event.
     */
    enum class EventType {
        InitialThread,
        AppReady,
        Termination,
        Timer,
        Socket,
        Message
    };

    /**
     * @brief Variant to hold the payload, ensuring type-safety.
     * We explicitly add a std::pair to handle messages with topics.
     */
    using Payload = std::variant<std::monostate, std::string, Message, std::pair<std::string, Message>>;

    /**
     * @brief Constructs a termination message.
     * @param [in] reason The reason for the thread termination.
     * @param [in] originating_thread_id The ID of the thread that initiated the termination.
     */
    EventMessage(const std::string& reason, ThreadID originating_thread_id)
        : event_type_(EventType::Termination),
          originating_thread_id_(originating_thread_id),
          payload_(reason) {}

    /**
     * @brief Constructs a generic event message with a `Message` payload.
     * @param [in] type The type of event.
     * @param [in] message The payload containing the event data.
     * @param [in] originating_thread_id The ID of the thread that initiated the event.
     */
    EventMessage(EventType type, Message message, ThreadID originating_thread_id)
        : event_type_(type),
          originating_thread_id_(originating_thread_id),
          payload_(message) {}

    /**
     * @brief Constructs a message with a topic and a message payload.
     * @param [in] type The type of event (should be EventType::Message).
     * @param [in] message_pair A pair containing the topic and the message data.
     * @param [in] originating_thread_id The ID of the thread that initiated the event.
     */
    EventMessage(EventType type, std::pair<std::string, Message> message_pair, ThreadID originating_thread_id)
        : event_type_(type),
          originating_thread_id_(originating_thread_id),
          payload_(message_pair) {}

    /**
     * @brief Constructs a generic event message without a payload.
     * @param [in] type The type of event.
     * @param [in] originating_thread_id The ID of the thread that initiated the event.
     */
    EventMessage(EventType type, ThreadID originating_thread_id)
        : event_type_(type),
          originating_thread_id_(originating_thread_id),
          payload_(std::monostate{}) {}

    EventType event_type_;
    ThreadID originating_thread_id_;
    Payload payload_;
};

} // namespace pubsub_itc_fw
