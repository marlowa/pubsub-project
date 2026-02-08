#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

// Factory method implementations
EventMessage EventMessage::create_reactor_event(EventType type)
{
    // Reactor events have no payload
    EventMessage msg(type, nullptr, 0);
    return msg;
}

EventMessage EventMessage::create_timer_event(TimerID timer_id)
{
    EventMessage msg(EventType(EventType::Timer), nullptr, 0);
    msg.header_.timer_id = timer_id;
    return msg;
}

EventMessage EventMessage::create_termination_event(const std::string& reason)
{
    EventMessage msg(EventType(EventType::Termination), nullptr, 0);
    msg.header_.reason = reason;
    return msg;
}

EventMessage EventMessage::create_pubsub_message(const uint8_t* data, int size)
{
    EventMessage msg(EventType(EventType::PubSubCommunication), data, size);
    return msg;
}

EventMessage EventMessage::create_itc_message(ThreadID originating_thread_id,
                                              const uint8_t* data,
                                              int size)
{
    EventMessage msg(EventType(EventType::InterthreadCommunication), data, size);
    msg.header_.originating_thread_id = originating_thread_id;
    return msg;
}

EventMessage EventMessage::create_raw_socket_message(const uint8_t* data, int size)
{
    EventMessage msg(EventType(EventType::RawSocketCommunication), data, size);
    return msg;
}

// Getter method implementations
EventType EventMessage::type() const noexcept
{
    return header_.type;
}

int EventMessage::payload_size() const noexcept
{
    return header_.payload_size;
}

TimerID EventMessage::timer_id() const noexcept
{
    return header_.timer_id;
}

const std::string& EventMessage::reason() const noexcept
{
    return header_.reason;
}

ThreadID EventMessage::originating_thread_id() const noexcept
{
    return header_.originating_thread_id;
}

const uint8_t* EventMessage::payload() const noexcept
{
    return payload_;
}

} // namespace pubsub_itc_fw
