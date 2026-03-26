#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

using namespace pubsub_itc_fw;

namespace {

class EventMessageTest : public ::testing::Test {
public:
    EventMessageTest() = default;
    ~EventMessageTest() override = default;
};

} // unnamed namespace

// -----------------------------------------------------------------------------
// TEST 1: Reactor event creation
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, ReactorEventCreation)
{
    const EventMessage m = EventMessage::create_reactor_event(EventType(EventType::Initial));

    EXPECT_EQ(m.type().as_tag(), EventType::Initial);
    EXPECT_EQ(m.originating_thread_id().get_value(), system_thread_id_value);

    // Reactor events have no payload
    EXPECT_EQ(m.payload_size(), 0u);
    EXPECT_EQ(m.payload(), nullptr);
}

// -----------------------------------------------------------------------------
// TEST 2: ITC message creation with payload
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, ItcMessageWithPayload)
{
    const ThreadID origin(42);
    const uint8_t payload[4] = {10, 20, 30, 40};

    const EventMessage msg = EventMessage::create_itc_message(origin, payload, 4);

    EXPECT_EQ(msg.type().as_tag(), EventType::InterthreadCommunication);
    EXPECT_EQ(msg.originating_thread_id().get_value(), 42);

    ASSERT_EQ(msg.payload_size(), 4U);

    const auto* p = msg.payload();
    EXPECT_EQ(p[0], 10);
    EXPECT_EQ(p[1], 20);
    EXPECT_EQ(p[2], 30);
    EXPECT_EQ(p[3], 40);
}

// -----------------------------------------------------------------------------
// TEST 3: ITC message with zero-length payload
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, ItcMessageZeroLengthPayload)
{
    const ThreadID origin(7);

    const EventMessage msg = EventMessage::create_itc_message(origin, nullptr, 0);

    EXPECT_EQ(msg.type().as_tag(), EventType::InterthreadCommunication);
    EXPECT_EQ(msg.originating_thread_id().get_value(), 7);

    EXPECT_EQ(msg.payload_size(), 0u);
    EXPECT_EQ(msg.payload(), nullptr);
}

// -----------------------------------------------------------------------------
// TEST 4: Timer event creation
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, TimerEventCreation)
{
    const TimerID tid(12345);

    const EventMessage msg = EventMessage::create_timer_event(tid);

    EXPECT_EQ(msg.type().as_tag(), EventType::Timer);
    EXPECT_EQ(msg.timer_id().get_value(), 12345);

    EXPECT_EQ(msg.payload_size(), 0u);
    EXPECT_EQ(msg.payload(), nullptr);
}

// -----------------------------------------------------------------------------
// TEST 5: Termination event creation + reason()
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, TerminationEventReason)
{
    const EventMessage msg = EventMessage::create_termination_event("catastrophic failure");

    EXPECT_EQ(msg.type().as_tag(), EventType::Termination);
    EXPECT_EQ(msg.reason(), "catastrophic failure");
    EXPECT_EQ(msg.originating_thread_id().get_value(), system_thread_id_value);

    EXPECT_EQ(msg.payload_size(), 0u);
    EXPECT_EQ(msg.payload(), nullptr);
}

// -----------------------------------------------------------------------------
// TEST 6: Move semantics
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, MoveSemantics)
{
    const ThreadID origin(11);
    const uint8_t payload[2] = {9, 8};

    EventMessage original = EventMessage::create_itc_message(origin, payload, 2);
    const EventMessage moved = std::move(original);

    EXPECT_EQ(moved.type().as_tag(), EventType::InterthreadCommunication);
    EXPECT_EQ(moved.originating_thread_id().get_value(), 11);
    EXPECT_EQ(moved.payload_size(), 2u);

    const auto* p = moved.payload();
    EXPECT_EQ(p[0], 9);
    EXPECT_EQ(p[1], 8);
}

// -----------------------------------------------------------------------------
// TEST 8: EventType stringification
// -----------------------------------------------------------------------------
TEST_F(EventMessageTest, EventTypeToString)
{
    EXPECT_EQ(EventType(EventType::Initial).as_string(), "Initial");
    EXPECT_EQ(EventType(EventType::AppReady).as_string(), "AppReady");
    EXPECT_EQ(EventType(EventType::Timer).as_string(), "Timer");
    EXPECT_EQ(EventType(EventType::InterthreadCommunication).as_string(), "InterthreadCommunication");
    EXPECT_EQ(EventType(EventType::Termination).as_string(), "Termination");
}
