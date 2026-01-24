#include <utility>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

#if 0
// TODO not sure if we should allow EventMessage objects to be copied. Probably not.
// Implementation of the move constructor
EventMessage::EventMessage(EventMessage&& other) noexcept
    : header_(std::move(other.header_)),
      payload_(other.payload_) {
    // Set the moved-from object's payload to nullptr to prevent it from being used
    // and to leave it in a valid, empty state.
    other.payload_ = nullptr;
    other.header_.payload_size = 0;
}

// Implementation of the move assignment operator
EventMessage& EventMessage::operator=(EventMessage&& other) noexcept {
    if (this != &other) {
        // Transfer the header and payload from the other object
        header_ = std::move(other.header_);
        payload_ = other.payload_;

        // Invalidate the other object's state
        other.payload_ = nullptr;
        other.header_.payload_size = 0;
    }
    return *this;
}
#endif

// Factory method implementations
[[nodiscard]] EventMessage EventMessage::create_reactor_event(EventType type) {
    EventMessage msg;
    msg.header_.type = type;
    msg.header_.payload_size = 0;
    return msg;
}

[[nodiscard]] EventMessage EventMessage::create_timer_event(TimerID timer_id) {
    EventMessage msg;
    msg.header_.type = EventType(EventType::Timer);
    msg.header_.payload_size = 0;
    msg.header_.timer_id = timer_id;
    return msg;
}

[[nodiscard]] EventMessage EventMessage::create_termination_event(const std::string& reason) {
    EventMessage msg;
    msg.header_.type = EventType(EventType::Termination);
    msg.header_.payload_size = 0;
    msg.header_.reason = reason;
    return msg;
}

[[nodiscard]] EventMessage EventMessage::create_pubsub_message(const uint8_t* data, int size) {
    EventMessage msg;
    msg.header_.type = EventType(EventType::PubSubCommunication);
    msg.header_.payload_size = size;
    msg.payload_ = data;
    return msg;
}

[[nodiscard]] EventMessage EventMessage::create_itc_message(ThreadID originating_thread_id, const uint8_t* data, int size) {
    EventMessage msg;
    msg.header_.type = EventType(EventType::InterthreadCommunication);
    msg.header_.payload_size = size;
    msg.header_.originating_thread_id = originating_thread_id;
    msg.payload_ = data;
    return msg;
}

[[nodiscard]] EventMessage EventMessage::create_raw_socket_message(const uint8_t* data, int size) {
    EventMessage msg;
    msg.header_.type = EventType(EventType::RawSocketCommunication);
    msg.header_.payload_size = size;
    msg.payload_ = data;
    return msg;
}

// Getter method implementations
[[nodiscard]] EventType EventMessage::type() const noexcept {
    return header_.type;
}

[[nodiscard]] int EventMessage::payload_size() const noexcept {
    return header_.payload_size;
}

[[nodiscard]] TimerID EventMessage::timer_id() const noexcept {
    return header_.timer_id;
}

[[nodiscard]] const std::string& EventMessage::reason() const noexcept {
    return header_.reason;
}

[[nodiscard]] ThreadID EventMessage::originating_thread_id() const noexcept {
    return header_.originating_thread_id;
}

[[nodiscard]] const uint8_t* EventMessage::payload() const noexcept {
    return payload_;
}

} // namespace pubsub_itc_fw
