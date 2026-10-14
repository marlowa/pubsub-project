// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/WalFollowerClient.hpp>

namespace pubsub_itc_fw::tests {

namespace {

struct SentRequest {
    int connection;
    int64_t last_seq_no;
    bool wants_new_records;
};

class RecordingLinks : public WalFollowerClientLinksInterface {
  public:
    void send_follow_request(const ConnectionID& to, const WalFollowRequest& request) override {
        sent.push_back(SentRequest{to.get_value(), request.last_seq_no, request.wants_new_records});
    }
    std::vector<SentRequest> sent;
};

WalFollowCaughtUpView caught_up(int64_t last, int64_t records_sent, int64_t first_held = 1) {
    WalFollowCaughtUpView view{};
    view.last_seq_no = last;
    view.records_sent = records_sent;
    view.first_seq_no_held = first_held;
    return view;
}

const ConnectionID primary{1};
const ConnectionID secondary{2};

} // un-named namespace

TEST(WalFollowerClientTest, AsksEachInstanceItConnectsToFromTheLastRecordItHolds) {
    RecordingLinks links;
    WalFollowerClient client("engine", true, 41, links);
    client.on_connected(primary);
    client.on_connected(secondary);
    ASSERT_EQ(links.sent.size(), 2U);
    EXPECT_EQ(links.sent[0].connection, 1);
    EXPECT_EQ(links.sent[1].connection, 2);
    EXPECT_EQ(links.sent[0].last_seq_no, 41);
    EXPECT_TRUE(links.sent[0].wants_new_records);
}

TEST(WalFollowerClientTest, AcceptsTheNextRecordAndDropsOnesItAlreadyHolds) {
    RecordingLinks links;
    WalFollowerClient client("publisher", true, 10, links);
    client.on_connected(primary);
    EXPECT_EQ(client.on_record(primary, 9), WalFollowerClient::RecordVerdict::AlreadyHeld);
    EXPECT_EQ(client.on_record(primary, 10), WalFollowerClient::RecordVerdict::AlreadyHeld);
    EXPECT_EQ(client.on_record(primary, 11), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.on_record(primary, 12), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.last_seq_no(), 12);
    EXPECT_EQ(links.sent.size(), 1U); // asked once, on connecting
}

TEST(WalFollowerClientTest, AGapIsRefusedAndTheClientAsksAgainFromTheLastRecordItHolds) {
    RecordingLinks links;
    WalFollowerClient client("publisher", true, 10, links);
    client.on_connected(primary);
    EXPECT_EQ(client.on_record(primary, 11), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.on_record(primary, 13), WalFollowerClient::RecordVerdict::Gap);
    EXPECT_EQ(client.last_seq_no(), 11);
    ASSERT_EQ(links.sent.size(), 2U);
    EXPECT_EQ(links.sent[1].last_seq_no, 11);
}

TEST(WalFollowerClientTest, CaughtUpOnlyWhenItReceivedExactlyTheRecordsTheInstanceSays) {
    RecordingLinks links;
    WalFollowerClient client("engine", false, 0, links);
    client.on_connected(primary);
    EXPECT_EQ(client.on_record(primary, 1), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.on_record(primary, 2), WalFollowerClient::RecordVerdict::Accept);

    // Three said, two received: not caught up, and it asks again.
    EXPECT_EQ(client.on_caught_up(primary, caught_up(2, 3)), WalFollowerClient::CaughtUpVerdict::CountMismatch);
    EXPECT_FALSE(client.caught_up());
    ASSERT_EQ(links.sent.size(), 2U);
    EXPECT_EQ(links.sent[1].last_seq_no, 2);

    // Asked again, it is sent nothing more and told so, with the count since the new request.
    EXPECT_EQ(client.on_caught_up(primary, caught_up(2, 0, 1)), WalFollowerClient::CaughtUpVerdict::CaughtUp);
    EXPECT_TRUE(client.caught_up());
    EXPECT_EQ(client.first_seq_no_held(), 1);
}

TEST(WalFollowerClientTest, RecordsItAlreadyHeldAreCountedBecauseTheInstanceCountsEveryRecordItSent) {
    RecordingLinks links;
    WalFollowerClient client("engine", true, 5, links);
    client.on_connected(primary);
    EXPECT_EQ(client.on_record(primary, 5), WalFollowerClient::RecordVerdict::AlreadyHeld);
    EXPECT_EQ(client.on_record(primary, 6), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.on_caught_up(primary, caught_up(6, 2)), WalFollowerClient::CaughtUpVerdict::CaughtUp);
}

TEST(WalFollowerClientTest, AFollowerHoldingNothingIsPlacedWhereTheInstanceSays) {
    RecordingLinks links;
    WalFollowerClient client("publisher", true, -1, links);
    client.on_connected(primary);
    EXPECT_EQ(links.sent[0].last_seq_no, -1);
    EXPECT_EQ(client.on_caught_up(primary, caught_up(500, 0)), WalFollowerClient::CaughtUpVerdict::CaughtUp);
    EXPECT_EQ(client.last_seq_no(), 500);
    EXPECT_EQ(client.on_record(primary, 501), WalFollowerClient::RecordVerdict::Accept);
}

TEST(WalFollowerClientTest, AChangeOfLeaderMakesItAskAgainFromTheLastRecordItHolds) {
    RecordingLinks links;
    WalFollowerClient client("publisher", true, 0, links);
    client.on_connected(primary);
    client.on_connected(secondary);
    client.on_declined(secondary);
    EXPECT_EQ(client.on_record(primary, 1), WalFollowerClient::RecordVerdict::Accept);
    EXPECT_EQ(client.on_caught_up(primary, caught_up(1, 1)), WalFollowerClient::CaughtUpVerdict::CaughtUp);

    client.on_leader_changed(secondary);
    ASSERT_EQ(links.sent.size(), 3U);
    EXPECT_EQ(links.sent[2].connection, 2);
    EXPECT_EQ(links.sent[2].last_seq_no, 1);
    EXPECT_FALSE(client.caught_up());
}

} // namespaces
