// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/TechnicalEventsSubscription.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include <leader_follower.hpp>

namespace pubsub_itc_fw {
namespace {

TechnicalEventsSubscription load(const std::string& section) {
    TomlConfiguration toml;
    const auto [ok, error] = toml.load_string(section);
    EXPECT_TRUE(ok) << error;
    return TechnicalEventsSubscription::load(toml, "TechnicalEventsSubscriptionTest");
}

// The bytes of a TechnicalEvent entering @p phase on @p trading_day, kept alive for the message that borrows them.
std::vector<uint8_t> technical_event(TradingPhase phase, int32_t trading_day) {
    TechnicalEvent event{};
    event.phase = phase;
    event.trading_day = trading_day;
    size_t written = 0;
    size_t needed = 0;
    static_cast<void>(encode(event, nullptr, 0, written, needed));
    std::vector<uint8_t> bytes(written + needed + 64);
    EXPECT_TRUE(encode(event, bytes.data(), bytes.size(), written, needed));
    bytes.resize(written);
    return bytes;
}

EventMessage delivered(const std::vector<uint8_t>& bytes, int16_t pdu_id = TechnicalEvent::message_pdu_id) {
    return EventMessage::create_pubsub_message(bytes.data(), static_cast<int>(bytes.size()), pdu_id, 42);
}

} // namespaces

TEST(TechnicalEventsSubscriptionTest, ReadsBothPublishersAndRegistersEach) {
    const TechnicalEventsSubscription subscription = load(R"(
[technical_events]
primary_host = "127.0.0.1"
primary_port = 11046
secondary_host = "127.0.0.2"
secondary_port = 11047
)");
    EXPECT_EQ(subscription.primary_port, 11'046);
    EXPECT_EQ(subscription.secondary_host, "127.0.0.2");
    ServiceRegistry registry;
    const std::vector<std::string> services = subscription.register_publishers(registry);
    EXPECT_EQ(services, (std::vector<std::string>{"technical_events_primary", "technical_events_secondary"}));
    EXPECT_EQ(services, subscription.publisher_services());
    const auto [endpoints, error] = registry.lookup("technical_events_secondary");
    EXPECT_TRUE(error.empty()) << error;
}

// A publisher the environment does not start has an empty host: it is not registered, and its port is not checked.
TEST(TechnicalEventsSubscriptionTest, APublisherWithNoHostIsNotConnectedTo) {
    const TechnicalEventsSubscription subscription = load(R"(
[technical_events]
primary_host = "127.0.0.1"
primary_port = 11046
secondary_host = ""
secondary_port = 0
)");
    ServiceRegistry registry;
    EXPECT_EQ(subscription.register_publishers(registry), (std::vector<std::string>{"technical_events_primary"}));
    const auto [endpoints, error] = registry.lookup("technical_events_secondary");
    EXPECT_FALSE(error.empty());
}

TEST(TechnicalEventsSubscriptionTest, APortOutOfRangeForAPublisherThatIsRunIsRefused) {
    EXPECT_THROW(load(R"(
[technical_events]
primary_host = "127.0.0.1"
primary_port = 0
secondary_host = ""
secondary_port = 0
)"),
                 ConfigurationException);
}

TEST(TechnicalEventsSubscriptionTest, RecognisesTheEndOfDayEndOfItsOwnTradingDay) {
    const std::vector<uint8_t> end = technical_event(TradingPhase::EndOfDayEnd, 20'261'119);
    EXPECT_TRUE(TechnicalEventsSubscription::is_end_of_day_end(delivered(end), 20'261'119));
}

// A component started for one day stops only for that day's end.
TEST(TechnicalEventsSubscriptionTest, TheEndOfAnotherDayIsNotItsOwn) {
    const std::vector<uint8_t> end = technical_event(TradingPhase::EndOfDayEnd, 20'261'118);
    EXPECT_FALSE(TechnicalEventsSubscription::is_end_of_day_end(delivered(end), 20'261'119));
}

TEST(TechnicalEventsSubscriptionTest, AnotherPhaseOrAnotherMessageIsNotTheEnd) {
    const std::vector<uint8_t> begin = technical_event(TradingPhase::EndOfDayBegin, 20'261'119);
    EXPECT_FALSE(TechnicalEventsSubscription::is_end_of_day_end(delivered(begin), 20'261'119));
    const std::vector<uint8_t> end = technical_event(TradingPhase::EndOfDayEnd, 20'261'119);
    EXPECT_FALSE(TechnicalEventsSubscription::is_end_of_day_end(delivered(end, 1000), 20'261'119));
}

} // namespaces
