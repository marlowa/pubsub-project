// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowerService.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace pubsub_itc_fw::tests {

namespace {

class RecordingLinks : public WalFollowerLinksInterface {
  public:
    void send_to_reading_thread(std::unique_ptr<WalReadingCommand> command) override {
        commands.push_back(*command);
    }
    void send_declined(const ConnectionID& to, const WalFollowDeclined& /*declined*/) override {
        declined.push_back(to.get_value());
    }
    void send_caught_up(const ConnectionID& to, const WalFollowCaughtUp& message) override {
        caught_up.emplace_back(to.get_value(), message);
    }
    void send_leader_changed(const ConnectionID& to, const WalLeaderChanged& /*changed*/) override {
        leader_changed.push_back(to.get_value());
    }

    std::vector<WalReadingCommand> commands;
    std::vector<int> declined;
    std::vector<std::pair<int, WalFollowCaughtUp>> caught_up;
    std::vector<int> leader_changed;
};

class RecordingEvents : public WalFollowerEventsInterface {
  public:
    void follower_handed_over(const ConnectionID& follower, int64_t through) override {
        handed_over.emplace_back(follower.get_value(), through);
    }
    void hand_over_falling_behind(const ConnectionID& follower) override {
        falling_behind.push_back(follower.get_value());
    }
    void declined_while_not_leading(const ConnectionID& follower) override {
        declined.push_back(follower.get_value());
    }
    void reading_stopped() override {
        ++stopped;
    }

    std::vector<std::pair<int, int64_t>> handed_over;
    std::vector<int> falling_behind;
    std::vector<int> declined;
    int stopped{0};
};

WalFollowRequestView request(int64_t last_seq_no) {
    WalFollowRequestView view{};
    view.follower_name = "follower";
    view.last_seq_no = last_seq_no;
    view.wants_new_records = true;
    return view;
}

// A report on the service's first serving of a connection, which is numbered 1, unless told otherwise.
EventMessage reached(const ConnectionID& connection, int64_t reached_seq_no, int64_t records_sent, uint64_t serving = 1) {
    auto report = std::make_unique<WalReadingReport>();
    report->serving = serving;
    report->kind = WalReadingReport::Kind::Reached;
    report->connection = connection;
    report->reached_seq_no = reached_seq_no;
    report->records_sent = records_sent;
    return carry(ThreadID{9}, std::move(report));
}

EventMessage stopped_all() {
    auto report = std::make_unique<WalReadingReport>();
    report->kind = WalReadingReport::Kind::StoppedAll;
    return carry(ThreadID{9}, std::move(report));
}

const ConnectionID follower_connection{7};
constexpr int converter = 0;

class WalFollowerServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = tests_common::make_scratch_directory("wal_follower_service");
        wal_.open(directory_, 1 << 20);
        const uint8_t byte = 1;
        for (int64_t seq_no = 1; seq_no <= 20; ++seq_no) {
            wal_.append(seq_no, 1, &byte, 1, 0, 1);
            positions_.publish_written(seq_no, wal_.write_position());
        }
        positions_.publish_committed(20);
    }
    void TearDown() override {
        tests_common::remove_scratch_directory(directory_);
    }

    tests_common::LoggerWithSink logger_;
    std::string directory_;
    Wal wal_;
    WalFollowingPositions positions_;
    RecordingLinks links_;
    RecordingEvents events_;
    WalFollowerService service_{logger_.logger, wal_, positions_, links_, events_};
};

} // un-named namespace

TEST_F(WalFollowerServiceTest, AnInstanceThatDoesNotLeadDeclinesAndTellsTheFollowerWhenItTakesTheLead) {
    service_.on_follow_request(follower_connection, request(5), false, FollowerLimit::LastCommitted, converter);
    EXPECT_EQ(links_.declined, std::vector<int>{7});
    EXPECT_EQ(events_.declined, std::vector<int>{7});
    EXPECT_TRUE(links_.commands.empty());

    service_.on_became_leader(3);
    EXPECT_EQ(links_.leader_changed, std::vector<int>{7});
}

TEST_F(WalFollowerServiceTest, TheLeaderHasTheReadingThreadSendTheRecordsAfterTheFollowersPosition) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    ASSERT_EQ(links_.commands.size(), 1U);
    const WalReadingCommand& command = links_.commands[0];
    EXPECT_EQ(command.kind, WalReadingCommand::Kind::Serve);
    EXPECT_EQ(command.connection.get_value(), 7);
    EXPECT_EQ(command.after_seq_no, 5);
    EXPECT_EQ(command.limit, FollowerLimit::LastCommitted);
    EXPECT_FALSE(service_.is_handed_over(follower_connection));
}

TEST_F(WalFollowerServiceTest, AFollowerIsHandedOverWhenTheReadingThreadReachesTheLimitAsItStandsNow) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));

    EXPECT_TRUE(service_.is_handed_over(follower_connection));
    ASSERT_EQ(links_.caught_up.size(), 1U);
    EXPECT_EQ(links_.caught_up[0].second.last_seq_no, 20);
    EXPECT_EQ(links_.caught_up[0].second.records_sent, 15);
    EXPECT_EQ(links_.caught_up[0].second.first_seq_no_held, 1);
    ASSERT_EQ(events_.handed_over.size(), 1U);
    EXPECT_EQ(events_.handed_over[0].second, 20);
}

TEST_F(WalFollowerServiceTest, AReportShortOfTheLimitNowSendsTheReadingThreadRoundAgain) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    positions_.publish_committed(21); // released while the reading thread was sending
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));

    EXPECT_FALSE(service_.is_handed_over(follower_connection));
    EXPECT_TRUE(links_.caught_up.empty());
    ASSERT_EQ(links_.commands.size(), 2U);
    EXPECT_EQ(links_.commands[1].kind, WalReadingCommand::Kind::Continue);

    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 21, 16)));
    EXPECT_TRUE(service_.is_handed_over(follower_connection));
}

TEST_F(WalFollowerServiceTest, TheApplicationIsToldOnceWhenAHandOverFallsShortThreeTimesInARow) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    for (int64_t limit = 21; limit <= 24; ++limit) {
        positions_.publish_committed(limit);
        ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, limit - 1, 0)));
    }
    EXPECT_EQ(events_.falling_behind, std::vector<int>{7});
}

TEST_F(WalFollowerServiceTest, AFollowerHoldingNothingIsHandedOverAtOnceAtTheNewestRecordItMayBeSent) {
    service_.on_follow_request(follower_connection, request(-1), true, FollowerLimit::LastCommitted, converter);
    EXPECT_TRUE(links_.commands.empty());
    EXPECT_TRUE(service_.is_handed_over(follower_connection));
    ASSERT_EQ(links_.caught_up.size(), 1U);
    EXPECT_EQ(links_.caught_up[0].second.last_seq_no, 20);
    EXPECT_EQ(links_.caught_up[0].second.records_sent, 0);
}

TEST_F(WalFollowerServiceTest, ALostConnectionStopsTheReadingThreadAndALateReportIsIgnored) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    service_.on_connection_lost(follower_connection);
    ASSERT_EQ(links_.commands.size(), 2U);
    EXPECT_EQ(links_.commands[1].kind, WalReadingCommand::Kind::Stop);

    EXPECT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));
    EXPECT_FALSE(service_.is_handed_over(follower_connection));
    EXPECT_TRUE(links_.caught_up.empty());
}

TEST_F(WalFollowerServiceTest, StoppingAllWaitsForTheReadingThreadToSayItHasStopped) {
    service_.on_follow_request(follower_connection, request(5), true, FollowerLimit::LastCommitted, converter);
    service_.stop_all();
    EXPECT_EQ(links_.commands.back().kind, WalReadingCommand::Kind::StopAll);
    EXPECT_FALSE(service_.reading_has_stopped());

    ASSERT_TRUE(service_.on_inter_thread_message(stopped_all()));
    EXPECT_TRUE(service_.reading_has_stopped());
    EXPECT_EQ(events_.stopped, 1);

    // The follower it was serving is told when this instance leads again, and asks again.
    service_.on_became_leader(5);
    EXPECT_EQ(links_.leader_changed, std::vector<int>{7});
}

TEST_F(WalFollowerServiceTest, AMessageThatIsNotAReportIsLeftForTheApplication) {
    const uint8_t bytes[4] = {1, 2, 3, 4};
    EXPECT_FALSE(service_.on_inter_thread_message(EventMessage::create_itc_message(ThreadID{9}, bytes, 4)));
}

TEST_F(WalFollowerServiceTest, AReportFromAnEarlierServingOfTheSameConnectionIsIgnored) {
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend);
    service_.serve(follower_connection, 8, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend); // afresh, numbered 2
    ASSERT_GE(links_.commands.size(), 3U);
    EXPECT_EQ(links_.commands.back().serving, 2U);

    // The first serving's report, still on its way, must not hand the follower over.
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15, 1)));
    EXPECT_FALSE(service_.is_handed_over(follower_connection));

    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 12, 2)));
    EXPECT_TRUE(service_.is_handed_over(follower_connection));
}

} // namespaces
