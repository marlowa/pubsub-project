// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <optional>
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
    void follower_handed_over(const ConnectionID& follower, int64_t through, int64_t records_sent, WalPosition /*resume_at*/) override {
        handed_over.emplace_back(follower.get_value(), through);
        handed_over_records_sent.push_back(records_sent);
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
    void follower_cannot_be_served(const ConnectionID& follower, int64_t damaged_seq_no) override {
        cannot_be_served.emplace_back(follower.get_value(), damaged_seq_no);
    }

    std::vector<std::pair<int, int64_t>> handed_over;
    std::vector<int64_t> handed_over_records_sent;
    std::vector<int> falling_behind;
    std::vector<int> declined;
    int stopped{0};
    std::vector<std::pair<int, int64_t>> cannot_be_served;
};

WalFollowRequestView request(int64_t last_seq_no, int64_t request_number = 1) {
    WalFollowRequestView view{};
    view.follower_name = "follower";
    view.last_seq_no = last_seq_no;
    view.wants_new_records = true;
    view.request_number = request_number;
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

// The reading thread's report that record damaged_seq_no could not be read for the connection's first serving.
EventMessage damaged(const ConnectionID& connection, int64_t damaged_seq_no, int64_t records_sent) {
    auto report = std::make_unique<WalReadingReport>();
    report->serving = 1;
    report->kind = WalReadingReport::Kind::Damaged;
    report->connection = connection;
    report->reached_seq_no = damaged_seq_no;
    report->records_sent = records_sent;
    return carry(ThreadID{9}, std::move(report));
}

EventMessage stopped_all() {
    auto report = std::make_unique<WalReadingReport>();
    report->kind = WalReadingReport::Kind::StoppedAll;
    return carry(ThreadID{9}, std::move(report));
}

EventMessage confirmed() {
    auto report = std::make_unique<WalReadingReport>();
    report->kind = WalReadingReport::Kind::Confirmed;
    return carry(ThreadID{9}, std::move(report));
}

const ConnectionID follower_connection{7};
constexpr int converter = 0;
constexpr int64_t no_application_value = 0;

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
    service_.on_follow_request(follower_connection, request(5), Leading::DoesNotLead, FollowerLimit::LastCommitted, converter, no_application_value);
    EXPECT_EQ(links_.declined, std::vector<int>{7});
    EXPECT_EQ(events_.declined, std::vector<int>{7});
    EXPECT_TRUE(links_.commands.empty());

    service_.on_became_leader(3);
    EXPECT_EQ(links_.leader_changed, std::vector<int>{7});
}

TEST_F(WalFollowerServiceTest, TheLeaderHasTheReadingThreadSendTheRecordsAfterTheFollowersPosition) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    ASSERT_EQ(links_.commands.size(), 1U);
    const WalReadingCommand& command = links_.commands[0];
    EXPECT_EQ(command.kind, WalReadingCommand::Kind::Serve);
    EXPECT_EQ(command.connection.get_value(), 7);
    EXPECT_EQ(command.after_seq_no, 5);
    EXPECT_EQ(command.limit, FollowerLimit::LastCommitted);
    EXPECT_FALSE(service_.is_handed_over(follower_connection));
}

TEST_F(WalFollowerServiceTest, AFollowerIsHandedOverWhenTheReadingThreadReachesTheLimitAsItStandsNow) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));

    EXPECT_TRUE(service_.is_handed_over(follower_connection));
    ASSERT_EQ(links_.caught_up.size(), 1U);
    EXPECT_EQ(links_.caught_up[0].second.last_seq_no, 20);
    EXPECT_EQ(links_.caught_up[0].second.records_sent, 15);
    EXPECT_EQ(links_.caught_up[0].second.first_seq_no_held, 1);
    ASSERT_EQ(events_.handed_over.size(), 1U);
    EXPECT_EQ(events_.handed_over[0].second, 20);
    EXPECT_EQ(events_.handed_over_records_sent, std::vector<int64_t>{15});
}

TEST_F(WalFollowerServiceTest, AReportShortOfTheLimitNowSendsTheReadingThreadRoundAgain) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
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
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    for (int64_t limit = 21; limit <= 24; ++limit) {
        positions_.publish_committed(limit);
        ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, limit - 1, 0)));
    }
    EXPECT_EQ(events_.falling_behind, std::vector<int>{7});
}

// Through the reading thread, so that the start of the answer reaches the follower after anything an earlier serving of
// the connection was still sending.
TEST_F(WalFollowerServiceTest, AFollowerHoldingNothingIsServedFromTheNewestRecordItMayBeSentAndSentNothingOlder) {
    service_.on_follow_request(follower_connection, request(-1), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_EQ(links_.commands[0].kind, WalReadingCommand::Kind::Serve);
    EXPECT_EQ(links_.commands[0].after_seq_no, 20);
    ASSERT_TRUE(links_.commands[0].started.has_value());
    EXPECT_EQ(links_.commands[0].started->message.after_seq_no, -1) << "the position the request named is echoed";

    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 0)));
    EXPECT_TRUE(service_.is_handed_over(follower_connection));
    ASSERT_EQ(links_.caught_up.size(), 1U);
    EXPECT_EQ(links_.caught_up[0].second.last_seq_no, 20);
    EXPECT_EQ(links_.caught_up[0].second.records_sent, 0);
}

// The reading thread sends it before the first record; the request's number comes back on it and on WalFollowCaughtUp, so
// that the follower can tell this answer from an earlier one still on its way.
TEST_F(WalFollowerServiceTest, TheAnswerToARequestStartsWithItsNumberAndTheApplicationsValueAndEndsWithItsNumber) {
    constexpr int64_t request_number = 4;
    constexpr int64_t application_value = 17;
    service_.on_follow_request(follower_connection, request(5, request_number), Leading::Leads, FollowerLimit::LastCommitted, converter, application_value);
    ASSERT_EQ(links_.commands.size(), 1U);
    ASSERT_TRUE(links_.commands[0].started.has_value());
    const WalFollowStarted& started = links_.commands[0].started->message;
    EXPECT_FALSE(links_.commands[0].started->application_value_from_wal) << "a value given is sent as given";
    EXPECT_EQ(started.request_number, request_number);
    EXPECT_EQ(started.after_seq_no, 5);
    EXPECT_EQ(started.serving_through_seq_no, 20);
    EXPECT_EQ(started.application_value, application_value);

    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));
    ASSERT_EQ(links_.caught_up.size(), 1U);
    EXPECT_EQ(links_.caught_up[0].second.request_number, request_number);
}

// A follower that does not ask, such as the follower sequencer, is sent no start.
TEST_F(WalFollowerServiceTest, AServingWithNoStartSendsNone) {
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_FALSE(links_.commands[0].started.has_value());
}

TEST_F(WalFollowerServiceTest, ALostConnectionStopsTheReadingThreadAndALateReportIsIgnored) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    service_.on_connection_lost(follower_connection);
    ASSERT_EQ(links_.commands.size(), 2U);
    EXPECT_EQ(links_.commands[1].kind, WalReadingCommand::Kind::Stop);

    EXPECT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));
    EXPECT_FALSE(service_.is_handed_over(follower_connection));
    EXPECT_TRUE(links_.caught_up.empty());
}

TEST_F(WalFollowerServiceTest, StoppingAllWaitsForTheReadingThreadToSayItHasStopped) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
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

TEST_F(WalFollowerServiceTest, AConnectionIsBeingCaughtUpFromItsServingUntilItIsHandedOver) {
    EXPECT_FALSE(service_.is_being_caught_up(follower_connection));
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    EXPECT_TRUE(service_.is_being_caught_up(follower_connection));
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));
    EXPECT_FALSE(service_.is_being_caught_up(follower_connection));
    EXPECT_TRUE(service_.is_handed_over(follower_connection));

    service_.serve(follower_connection, 20, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    EXPECT_TRUE(service_.is_being_caught_up(follower_connection)) << "served afresh, it is being caught up again";
    service_.on_connection_lost(follower_connection);
    EXPECT_FALSE(service_.is_being_caught_up(follower_connection));
}

TEST_F(WalFollowerServiceTest, StoppingOneFollowerLeavesTheOthersServedAndWaitsForTheReadingThreadToConfirm) {
    const ConnectionID other_connection{8};
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    service_.serve(other_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    const size_t commands_before = links_.commands.size();

    service_.stop_serving(follower_connection);
    ASSERT_EQ(links_.commands.size(), commands_before + 2);
    EXPECT_EQ(links_.commands[commands_before].kind, WalReadingCommand::Kind::Stop);
    EXPECT_EQ(links_.commands[commands_before].connection.get_value(), 7);
    EXPECT_EQ(links_.commands[commands_before + 1].kind, WalReadingCommand::Kind::Confirm);
    EXPECT_FALSE(service_.reading_has_stopped());
    EXPECT_FALSE(service_.is_being_caught_up(follower_connection));
    EXPECT_TRUE(service_.is_being_caught_up(other_connection)) << "the other follower must go on being served";

    ASSERT_TRUE(service_.on_inter_thread_message(confirmed()));
    EXPECT_TRUE(service_.reading_has_stopped());
    EXPECT_EQ(events_.stopped, 1);
}

TEST_F(WalFollowerServiceTest, TheReadingThreadCountsAsStoppedOnlyOnceEveryStopAskedForIsConfirmed) {
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    service_.stop_serving(follower_connection);
    service_.stop_all();
    ASSERT_TRUE(service_.on_inter_thread_message(confirmed()));
    EXPECT_FALSE(service_.reading_has_stopped());
    EXPECT_EQ(events_.stopped, 0);
    ASSERT_TRUE(service_.on_inter_thread_message(stopped_all()));
    EXPECT_TRUE(service_.reading_has_stopped());
    EXPECT_EQ(events_.stopped, 1);
}

TEST_F(WalFollowerServiceTest, AFollowerServedPastADamagedRecordIsNeverHandedOverNorToldItCaughtUp) {
    service_.on_follow_request(follower_connection, request(5), Leading::Leads, FollowerLimit::LastCommitted, converter, no_application_value);
    ASSERT_TRUE(service_.on_inter_thread_message(damaged(follower_connection, 12, 6)));

    EXPECT_FALSE(service_.is_handed_over(follower_connection));
    EXPECT_FALSE(service_.is_being_caught_up(follower_connection));
    EXPECT_TRUE(links_.caught_up.empty()) << "a follower must never be told it caught up past a record that could not be read";
    EXPECT_TRUE(events_.handed_over.empty());
    ASSERT_EQ(events_.cannot_be_served.size(), 1U);
    EXPECT_EQ(events_.cannot_be_served[0], (std::pair<int, int64_t>{7, 12}));

    // A report about the serving that ended, still on its way, changes nothing.
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15)));
    EXPECT_FALSE(service_.is_handed_over(follower_connection));
    EXPECT_TRUE(links_.caught_up.empty());
}

TEST_F(WalFollowerServiceTest, AMessageThatIsNotAReportIsLeftForTheApplication) {
    const uint8_t bytes[4] = {1, 2, 3, 4};
    EXPECT_FALSE(service_.on_inter_thread_message(EventMessage::create_itc_message(ThreadID{9}, bytes, 4)));
}

TEST_F(WalFollowerServiceTest, AReportFromAnEarlierServingOfTheSameConnectionIsIgnored) {
    service_.serve(follower_connection, 5, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt);
    service_.serve(follower_connection, 8, FollowerLimit::LastCommitted, converter, CaughtUpMessage::DoNotSend, std::nullopt); // afresh, numbered 2
    ASSERT_GE(links_.commands.size(), 3U);
    EXPECT_EQ(links_.commands.back().serving, 2U);

    // The first serving's report, still on its way, must not hand the follower over.
    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 15, 1)));
    EXPECT_FALSE(service_.is_handed_over(follower_connection));

    ASSERT_TRUE(service_.on_inter_thread_message(reached(follower_connection, 20, 12, 2)));
    EXPECT_TRUE(service_.is_handed_over(follower_connection));
}

} // namespaces
