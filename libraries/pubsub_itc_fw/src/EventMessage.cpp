// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <string>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

namespace pubsub_itc_fw {

// Factory method implementations
EventMessage EventMessage::create_reactor_event(EventType type)
{
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

EventMessage EventMessage::create_raw_socket_message(ConnectionID connection_id,
                                                      const uint8_t* data,
                                                      int size)
{
    EventMessage msg(EventType(EventType::RawSocketCommunication), data, size);
    msg.header_.connection_id = connection_id;
    return msg;
}

EventMessage EventMessage::create_framework_pdu_message(const uint8_t* data, int size, int slab_id)
{
    EventMessage msg(EventType(EventType::FrameworkPdu), data, size);
    msg.slab_id_ = slab_id;
    return msg;
}

EventMessage EventMessage::create_connection_established_event(ConnectionID connection_id)
{
    EventMessage msg(EventType(EventType::ConnectionEstablished), nullptr, 0);
    msg.header_.connection_id = connection_id;
    return msg;
}

EventMessage EventMessage::create_connection_failed_event(const std::string& reason)
{
    EventMessage msg(EventType(EventType::ConnectionFailed), nullptr, 0);
    msg.header_.reason = reason;
    return msg;
}

EventMessage EventMessage::create_connection_lost_event(ConnectionID connection_id,
                                                        const std::string& reason)
{
    EventMessage msg(EventType(EventType::ConnectionLost), nullptr, 0);
    msg.header_.connection_id = connection_id;
    msg.header_.reason = reason;
    return msg;
}

// Getter method implementations
EventType EventMessage::type() const
{
    return header_.type;
}

int EventMessage::payload_size() const
{
    return header_.payload_size;
}

TimerID EventMessage::timer_id() const
{
    return header_.timer_id;
}

const std::string& EventMessage::reason() const
{
    return header_.reason;
}

ThreadID EventMessage::originating_thread_id() const
{
    return header_.originating_thread_id;
}

ConnectionID EventMessage::connection_id() const
{
    return header_.connection_id;
}

int EventMessage::slab_id() const
{
    return slab_id_;
}

const uint8_t* EventMessage::payload() const
{
    return payload_;
}

} // namespace pubsub_itc_fw
