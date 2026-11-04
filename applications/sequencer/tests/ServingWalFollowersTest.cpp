// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include "ServingWalFollowers.hpp"

namespace sequencer::tests {

namespace {

using Kind = ServingWalFollowers::EngineKind;
using pubsub_itc_fw::ConnectionID;

constexpr ServingWalFollowers::Converters converters{10, 11, 12};
const ConnectionID engine{21};
const ConnectionID other_engine{22};
const ConnectionID publisher{31};
const ConnectionID peer{41};
constexpr int64_t records_in_wal = 20;

// What ServingWalFollowers asks of the thread, and the reading thread's commands it sends.
struct RecordingLinks : public ServingWalFollowersLinksInterface {
    void send_to_reading_thread(std::unique_ptr<pubsub_itc_fw::WalReadingCommand> command) override {
        commands.push_back(*command);
    }
    void send_declined(const ConnectionID& to, const pubsub_itc_fw::WalFollowDeclined&) override {
        declined.push_back(to.get_value());
    }
    void send_caught_up(const ConnectionID& to, const pubsub_itc_fw::WalFollowCaughtUp& message) override {
        caught_up.push_back(to.get_value());
        caught_up_request_numbers.push_back(message.request_number);
    }
    void send_leader_changed(const ConnectionID&, const pubsub_itc_fw::WalLeaderChanged&) override {}
    [[nodiscard]] ConnectionID peer_connection() const override {
        return peer;
    }
    [[nodiscard]] bool is_publisher_connection(const ConnectionID& connection) const override {
        return connection == publisher;
    }
    [[nodiscard]] int64_t first_seq_no_held() const override {
        return 1;
    }
    void publish_released_position() override {
        ++released_published;
    }
    void pause_for_hand_over() override {
        ++pauses;
    }
    void end_pause_for_hand_over() override {
        ++pause_ends;
    }
    void peer_caught_up(int64_t through) override {
        peer_caught_up_through.push_back(through);
    }
    void peer_catch_up_stopped() override {
        ++peer_stops;
    }
    void engine_catch_up_progress(const ConnectionID& connection, int64_t) override {
        progress.push_back(connection.get_value());
    }
    void engine_catch_up_ended(const ConnectionID& connection) override {
        catch_up_ended.push_back(connection.get_value());
    }
    void engine_handed_over(const ConnectionID& connection, int64_t, int64_t, bool answers_catch_up_request) override {
        handed_over.push_back(connection.get_value());
        handed_over_answered.push_back(answers_catch_up_request);
    }
    void engine_caught_up_to_lead(const ConnectionID& connection) override {
        routed_to.push_back(connection.get_value());
    }
    void set_damaged_record_gauge(int64_t seq_no) override {
        gauge = seq_no;
    }

    std::vector<pubsub_itc_fw::WalReadingCommand> commands;
    std::vector<int> declined;
    std::vector<int> caught_up;
    std::vector<int64_t> caught_up_request_numbers;
    int released_published{0};
    int pauses{0};
    int pause_ends{0};
    std::vector<int64_t> peer_caught_up_through;
    int peer_stops{0};
    std::vector<int> progress;
    std::vector<int> catch_up_ended;
    std::vector<int> handed_over;
    std::vector<bool> handed_over_answered;
    std::vector<int> routed_to;
    int64_t gauge{-1};
};

struct NoRecordsSent : public pubsub_itc_fw::WalHandedOverFollowersLinksInterface {
    void send_record(const ConnectionID&, int16_t, int64_t, const uint8_t*, size_t) override {}
};

// The reading thread's report that it has sent @p connection every record through @p reached_seq_no, for the
// service's serving number @p serving of it.
pubsub_itc_fw::EventMessage reached(const ConnectionID& connection, int64_t reached_seq_no, uint64_t serving = 1) {
    auto report = std::make_unique<pubsub_itc_fw::WalReadingReport>();
    report->serving = serving;
    report->kind = pubsub_itc_fw::WalReadingReport::Kind::Reached;
    report->connection = connection;
    report->reached_seq_no = reached_seq_no;
    report->records_sent = reached_seq_no;
    return pubsub_itc_fw::carry(pubsub_itc_fw::ThreadID{9}, std::move(report));
}

// The reading thread's report that record @p damaged_seq_no could not be read for @p connection.
pubsub_itc_fw::EventMessage damaged(const ConnectionID& connection, int64_t damaged_seq_no, uint64_t serving = 1) {
    auto report = std::make_unique<pubsub_itc_fw::WalReadingReport>();
    report->serving = serving;
    report->kind = pubsub_itc_fw::WalReadingReport::Kind::Damaged;
    report->connection = connection;
    report->reached_seq_no = damaged_seq_no;
    report->records_sent = 0;
    return pubsub_itc_fw::carry(pubsub_itc_fw::ThreadID{9}, std::move(report));
}

pubsub_itc_fw::WalFollowRequestView request(int64_t last_seq_no) {
    pubsub_itc_fw::WalFollowRequestView view{};
    view.follower_name = "follower";
    view.last_seq_no = last_seq_no;
    view.wants_new_records = true;
    view.request_number = 1;
    return view;
}

class ServingWalFollowersTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = pubsub_itc_fw::tests_common::make_scratch_directory("serving_wal_followers");
        wal_.open(directory_, 1 << 20);
        const uint8_t byte = 1;
        for (int64_t seq_no = 1; seq_no <= records_in_wal; ++seq_no) {
            wal_.append(seq_no, 1, &byte, 1, 0, 1);
            positions_.publish_written(seq_no, wal_.write_position());
        }
        positions_.publish_committed(records_in_wal);
        positions_.publish_released(records_in_wal);
        serving_ = std::make_unique<ServingWalFollowers>(converters, links_, logger_);
        serving_->start(wal_, directory_, positions_, record_links_, pubsub_itc_fw::WalKeptRecordsCapacity{});
    }
    void TearDown() override {
        serving_.reset();
        pubsub_itc_fw::tests_common::remove_scratch_directory(directory_);
    }

    static ServingWalFollowers::EngineServing serving(Kind kind, int64_t request_number = 0) {
        ServingWalFollowers::EngineServing result{kind, std::nullopt};
        if (request_number != 0) {
            pubsub_itc_fw::WalFollowStart started{};
            started.message.request_number = request_number;
            result.started = started;
        }
        return result;
    }

    int count_logged(const std::string& text) {
        logger_.flush();
        int count = 0;
        for (const std::string& line : log_) {
            count += line.find(text) != std::string::npos ? 1 : 0;
        }
        return count;
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    std::string directory_;
    pubsub_itc_fw::Wal wal_;
    pubsub_itc_fw::WalFollowingPositions positions_;
    RecordingLinks links_;
    NoRecordsSent record_links_;
    std::unique_ptr<ServingWalFollowers> serving_;
};

} // namespaces

TEST_F(ServingWalFollowersTest, AnEngineCaughtUpToLeadIsToldItHasCaughtUpAndHasOrdersRoutedToIt) {
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_lead, 4));
    EXPECT_EQ(links_.released_published, 1) << "the limit is published before the reading thread is asked";
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_EQ(links_.commands[0].converter, converters.engine);
    EXPECT_TRUE(serving_->being_caught_up(engine));
    EXPECT_TRUE(serving_->caught_up_on_its_request(engine));

    ASSERT_TRUE(serving_->on_inter_thread_message(reached(engine, records_in_wal)));
    EXPECT_EQ(links_.catch_up_ended, (std::vector<int>{21}));
    EXPECT_EQ(links_.handed_over, (std::vector<int>{21}));
    EXPECT_EQ(links_.handed_over_answered, (std::vector<bool>{true}));
    EXPECT_EQ(links_.caught_up, (std::vector<int>{21}));
    EXPECT_EQ(links_.caught_up_request_numbers, (std::vector<int64_t>{4})) << "the request number of the answer is echoed";
    EXPECT_EQ(links_.routed_to, (std::vector<int>{21}));
    EXPECT_FALSE(serving_->follows_the_wal(engine));
    EXPECT_FALSE(serving_->caught_up_on_its_request(engine)) << "no longer being served";
    EXPECT_EQ(links_.pause_ends, 1);
}

TEST_F(ServingWalFollowersTest, AnEngineCaughtUpToFollowIsSentEveryRecordAndOrdersAreNotRoutedToIt) {
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_follow, 4));
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_EQ(links_.commands[0].converter, converters.publisher) << "every record, reports included";
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(engine, records_in_wal)));
    EXPECT_EQ(links_.caught_up, (std::vector<int>{21}));
    EXPECT_TRUE(links_.routed_to.empty());
    EXPECT_TRUE(serving_->follows_the_wal(engine));
    serving_->engine_asks_afresh(engine);
    EXPECT_FALSE(serving_->follows_the_wal(engine));
}

TEST_F(ServingWalFollowersTest, AnEngineSentTheOrdersItLacksIsToldNothingMore) {
    serving_->serve_engine(engine, 5, serving(Kind::sent_the_orders_it_lacks));
    EXPECT_FALSE(serving_->caught_up_on_its_request(engine));
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(engine, records_in_wal)));
    EXPECT_EQ(links_.handed_over_answered, (std::vector<bool>{false}));
    EXPECT_TRUE(links_.caught_up.empty());
    EXPECT_TRUE(links_.routed_to.empty());
    EXPECT_FALSE(serving_->follows_the_wal(engine));
}

TEST_F(ServingWalFollowersTest, ProgressIsReportedOnlyForAnEngineThatAskedToBeCaughtUp) {
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_lead));
    serving_->serve_engine(other_engine, 5, serving(Kind::sent_the_orders_it_lacks));
    serving_->follower_progress(engine, 10);
    serving_->follower_progress(other_engine, 10);
    EXPECT_EQ(links_.progress, (std::vector<int>{21}));
    serving_->engine_connection_lost(engine);
    serving_->follower_progress(engine, 12);
    EXPECT_EQ(links_.progress, (std::vector<int>{21})) << "an engine whose connection is lost is no longer served";
}

TEST_F(ServingWalFollowersTest, StoppingLeadingStopsEveryFollower) {
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_follow));
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(engine, records_in_wal)));
    serving_->serve_engine(other_engine, 5, serving(Kind::caught_up_to_lead));
    const int pause_ends_before = links_.pause_ends;
    serving_->stopped_leading();
    EXPECT_FALSE(serving_->follows_the_wal(engine));
    EXPECT_FALSE(serving_->caught_up_on_its_request(other_engine));
    EXPECT_EQ(links_.pause_ends, pause_ends_before + 1);
}

TEST_F(ServingWalFollowersTest, TheFollowerSequencerIsCaughtUpOnThePeerConnectionAndHandedToTheReplicatedWal) {
    serving_->start_peer_catch_up(5);
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_EQ(links_.commands[0].converter, converters.peer);
    EXPECT_EQ(links_.commands[0].connection.get_value(), peer.get_value());
    EXPECT_EQ(links_.commands[0].limit, pubsub_itc_fw::FollowerLimit::LastWritten);
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(peer, records_in_wal)));
    EXPECT_EQ(links_.peer_caught_up_through, (std::vector<int64_t>{records_in_wal}));
    EXPECT_TRUE(links_.handed_over.empty()) << "it is not an engine";
    EXPECT_FALSE(serving_->being_caught_up(peer));
}

TEST_F(ServingWalFollowersTest, AFollowerThatAsksWhileThisInstanceDoesNotLeadIsDeclined) {
    serving_->engine_asks_while_not_leading(engine, request(5));
    EXPECT_EQ(links_.declined, (std::vector<int>{21}));
    EXPECT_TRUE(links_.commands.empty());
    serving_->publisher_asks(publisher, request(5), pubsub_itc_fw::Leading::DoesNotLead);
    EXPECT_EQ(links_.declined, (std::vector<int>{21, 31}));
}

TEST_F(ServingWalFollowersTest, APublisherIsServedEveryRecordUpToTheLastCommitted) {
    serving_->publisher_asks(publisher, request(5), pubsub_itc_fw::Leading::Leads);
    ASSERT_EQ(links_.commands.size(), 1U);
    EXPECT_EQ(links_.commands[0].converter, converters.publisher);
    EXPECT_EQ(links_.commands[0].limit, pubsub_itc_fw::FollowerLimit::LastCommitted);
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(publisher, records_in_wal)));
    EXPECT_TRUE(links_.handed_over.empty()) << "a publisher is not an engine";
    EXPECT_EQ(count_logged("matching engine publisher on connection 31 holds every record"), 1);
}

TEST_F(ServingWalFollowersTest, ADamagedRecordIsSaidAtErrorOnceForEachKindOfFollowerAndShownUntilAFollowerIsHandedOverPastIt) {
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_lead));
    ASSERT_TRUE(serving_->on_inter_thread_message(damaged(engine, 12)));
    EXPECT_EQ(links_.gauge, 12);
    EXPECT_FALSE(serving_->caught_up_on_its_request(engine)) << "it is no longer served";
    serving_->serve_engine(engine, 5, serving(Kind::caught_up_to_lead));
    ASSERT_TRUE(serving_->on_inter_thread_message(damaged(engine, 12, 2)));
    EXPECT_EQ(count_logged("(asked again, and refused again)"), 1);
    EXPECT_EQ(count_logged("ERROR"), 1) << "said at Error the first time only";
    EXPECT_EQ(links_.pause_ends, 2);

    serving_->publisher_asks(publisher, request(15), pubsub_itc_fw::Leading::Leads);
    // The service numbers its servings across every connection: this is its third.
    ASSERT_TRUE(serving_->on_inter_thread_message(reached(publisher, records_in_wal, 3)));
    EXPECT_EQ(links_.gauge, 0) << "a follower handed over past the record";
}

TEST_F(ServingWalFollowersTest, AHandOverFallingBehindPausesReading) {
    serving_->hand_over_falling_behind(engine);
    EXPECT_EQ(links_.pauses, 1);
}

} // namespaces
