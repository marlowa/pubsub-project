// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/SlabHandle.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>

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
    EXPECT_FALSE(channel_.on_connection_lost(elsewhere_));
    EXPECT_TRUE(channel_.on_connection_lost(to_test_));
    EXPECT_FALSE(channel_.connected());
    channel_.role_changed("sequencer", 1, LeadershipRole::Leader, LeadershipRole::Follower, 2);
    EXPECT_FALSE(channel_.on_framework_pdu_message(checkpoint_request(to_test_, 3)));
    EXPECT_EQ(links_.sent.size(), 1U);
}

} // namespaces
