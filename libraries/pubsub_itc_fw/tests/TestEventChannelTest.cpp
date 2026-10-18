// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <unistd.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/SlabHandle.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/TestEventsConfiguration.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include <test_events.hpp>

namespace pubsub_itc_fw::tests {

namespace {

struct SentMessage {
    ConnectionID connection;
    int16_t pdu_id{0};
    std::vector<uint8_t> bytes;
};

class RecordingLinks : public TestEventLinksInterface {
  public:
    void send_test_event(const ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override {
        sent.push_back(SentMessage{connection, pdu_id, std::vector<uint8_t>(bytes, bytes + size)});
    }
    std::vector<SentMessage> sent;
};

template <typename View> View decoded(const SentMessage& message) {
    std::array<uint8_t, 256> arena_storage{};
    BumpAllocator arena(arena_storage.data(), arena_storage.size());
    View view{};
    size_t consumed = 0;
    size_t arena_needed = 0;
    EXPECT_TRUE(decode(view, message.bytes.data(), message.bytes.size(), consumed, arena, arena_needed));
    EXPECT_EQ(consumed, message.bytes.size());
    return view;
}

class TestEventChannelTest : public ::testing::Test {
  protected:
    RecordingLinks links_;
    TestEventChannel channel_{links_, "sequencer_primary", 1, "SequencerThread"};
    const ConnectionID to_test_{7, "test_events"};
    const ConnectionID elsewhere_{8, "peer"};

    EventMessage checkpoint_request(const ConnectionID& connection, int64_t number) {
        TestCheckpointRequest request{};
        request.number = number;
        size_t needed = 0;
        size_t written = 0;
        EXPECT_TRUE(encode(request, request_bytes_.data(), request_bytes_.size(), written, needed));
        return EventMessage::create_framework_pdu_message(request_bytes_.data(), static_cast<int>(written), invalid_slab_handle, connection,
                                                          TestCheckpointRequest::message_pdu_id, 0);
    }

  private:
    std::array<uint8_t, 64> request_bytes_{};
};

} // un-named namespace

TEST_F(TestEventChannelTest, WithNoConnectionAnEventSendsNothing) {
    channel_.role_changed("sequencer", 1, LeadershipRole::Unknown, LeadershipRole::Leader, 1);
    EXPECT_FALSE(channel_.connected());
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(TestEventChannelTest, AConnectionToAnotherServiceIsNotClaimed) {
    EXPECT_FALSE(channel_.on_connection_established(elsewhere_));
    EXPECT_FALSE(channel_.connected());
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(TestEventChannelTest, TheConnectionToTheTestIsClaimedAndTheThreadIntroducesItself) {
    EXPECT_TRUE(channel_.on_connection_established(to_test_));
    EXPECT_TRUE(channel_.connected());
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent[0].connection, to_test_);
    EXPECT_EQ(links_.sent[0].pdu_id, TestEventsHello::message_pdu_id);
    const auto hello = decoded<TestEventsHelloView>(links_.sent[0]);
    EXPECT_EQ(hello.component, "sequencer_primary");
    EXPECT_EQ(hello.instance, 1);
    EXPECT_EQ(hello.thread, "SequencerThread");
}

TEST_F(TestEventChannelTest, AChangeOfRoleIsSentWithEveryField) {
    channel_.on_connection_established(to_test_);
    channel_.role_changed("sequencer", 2, LeadershipRole::Follower, LeadershipRole::Leader, 14);
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(links_.sent[1].pdu_id, RoleChanged::message_pdu_id);
    const auto event = decoded<RoleChangedView>(links_.sent[1]);
    EXPECT_EQ(event.owner, "sequencer");
    EXPECT_EQ(event.instance, 2);
    EXPECT_EQ(event.from_role, LeadershipRole::Follower);
    EXPECT_EQ(event.to_role, LeadershipRole::Leader);
    EXPECT_EQ(event.epoch, 14);
}

TEST_F(TestEventChannelTest, TheIntroductionCarriesTheProcessId) {
    channel_.on_connection_established(to_test_);
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(decoded<TestEventsHelloView>(links_.sent[0]).process_id, static_cast<int64_t>(getpid()));
}

TEST_F(TestEventChannelTest, AChangeOfLeaseIsSentWithEveryField) {
    channel_.on_connection_established(to_test_);
    channel_.lease_changed("SequencerThread", 2, LeaseChange::Granted, Voter::ThirdVoter, 9);
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(links_.sent[1].pdu_id, LeaseChanged::message_pdu_id);
    const auto event = decoded<LeaseChangedView>(links_.sent[1]);
    EXPECT_EQ(event.owner, "SequencerThread");
    EXPECT_EQ(event.instance, 2);
    EXPECT_EQ(event.change, LeaseChange::Granted);
    EXPECT_EQ(event.granted_by, Voter::ThirdVoter);
    EXPECT_EQ(event.epoch, 9);
}

TEST_F(TestEventChannelTest, AGrantByAThirdVoterIsSentWithEveryField) {
    channel_.on_connection_established(to_test_);
    channel_.lease_granted_to(4, 1, 17);
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(links_.sent[1].pdu_id, LeaseGrantedTo::message_pdu_id);
    const auto event = decoded<LeaseGrantedToView>(links_.sent[1]);
    EXPECT_EQ(event.pair_id, 4);
    EXPECT_EQ(event.instance, 1);
    EXPECT_EQ(event.epoch, 17);
}

TEST_F(TestEventChannelTest, AnotherConnectionMadeOrLostIsReportedOnceTheTestIsConnected) {
    EXPECT_FALSE(channel_.on_connection_established(elsewhere_)); // before the test connects: not reported
    channel_.on_connection_established(to_test_);
    EXPECT_FALSE(channel_.on_connection_lost(elsewhere_));
    EXPECT_FALSE(channel_.on_connection_established(elsewhere_));
    ASSERT_EQ(links_.sent.size(), 3U);
    const auto lost = decoded<ConnectionChangedView>(links_.sent[1]);
    EXPECT_EQ(links_.sent[1].pdu_id, ConnectionChanged::message_pdu_id);
    EXPECT_EQ(lost.service, "peer");
    EXPECT_EQ(lost.connection, 8);
    EXPECT_FALSE(lost.made);
    EXPECT_TRUE(decoded<ConnectionChangedView>(links_.sent[2]).made);
}

TEST_F(TestEventChannelTest, ACheckpointRequestIsAnsweredWithItsNumber) {
    channel_.on_connection_established(to_test_);
    EXPECT_TRUE(channel_.on_framework_pdu_message(checkpoint_request(to_test_, 42)));
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(links_.sent[1].pdu_id, TestCheckpointReached::message_pdu_id);
    EXPECT_EQ(decoded<TestCheckpointReachedView>(links_.sent[1]).number, 42);
}

TEST_F(TestEventChannelTest, AMessageOnAnotherConnectionIsLeftToTheOwner) {
    channel_.on_connection_established(to_test_);
    EXPECT_FALSE(channel_.on_framework_pdu_message(checkpoint_request(elsewhere_, 1)));
    EXPECT_EQ(links_.sent.size(), 1U);
}

TEST_F(TestEventChannelTest, OnceTheConnectionIsLostNothingMoreIsSent) {
    channel_.on_connection_established(to_test_);
    EXPECT_FALSE(channel_.on_connection_lost(elsewhere_)); // reported, as another connection lost
    EXPECT_TRUE(channel_.on_connection_lost(to_test_));
    EXPECT_FALSE(channel_.connected());
    const size_t sent_while_connected = links_.sent.size();
    EXPECT_EQ(sent_while_connected, 2U);
    channel_.role_changed("sequencer", 1, LeadershipRole::Leader, LeadershipRole::Follower, 2);
    channel_.lease_changed("SequencerThread", 1, LeaseChange::RanOut, Voter::NoVoter, 2);
    channel_.lease_granted_to(1, 1, 2);
    EXPECT_FALSE(channel_.on_connection_established(elsewhere_));
    EXPECT_FALSE(channel_.on_connection_lost(elsewhere_));
    EXPECT_FALSE(channel_.on_framework_pdu_message(checkpoint_request(to_test_, 3)));
    EXPECT_EQ(links_.sent.size(), sent_while_connected);
}

namespace {

TestEventsConfiguration loaded(std::string_view text) {
    TomlConfiguration toml;
    const auto [parsed, error] = toml.load_string(text);
    EXPECT_TRUE(parsed) << error;
    return TestEventsConfigurationLoader::load(toml);
}

} // un-named namespace

TEST(TestEventsConfigurationTest, WithNoSectionNoTestEventsAreSent) {
    EXPECT_FALSE(loaded("[reactor]\nx = 1\n").enabled);
}

TEST(TestEventsConfigurationTest, AFullSectionGivesTheListenerAndTheNameTheTestKnowsTheComponentBy) {
    const auto configuration = loaded("[test_events]\nhost = \"127.0.0.1\"\nport = 45123\ncomponent = \"matching_engine\"\n");
    EXPECT_TRUE(configuration.enabled);
    EXPECT_EQ(configuration.host, "127.0.0.1");
    EXPECT_EQ(configuration.port, 45123);
    EXPECT_EQ(configuration.component, "matching_engine");
}

TEST(TestEventsConfigurationTest, ASectionWithoutTheComponentIsRefused) {
    EXPECT_THROW(loaded("[test_events]\nhost = \"127.0.0.1\"\nport = 45123\n"), ConfigurationException);
}

TEST(TestEventsConfigurationTest, AnEmptyComponentIsRefused) {
    EXPECT_THROW(loaded("[test_events]\nhost = \"127.0.0.1\"\nport = 45123\ncomponent = \"\"\n"), ConfigurationException);
}

} // namespaces
