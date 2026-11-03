// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "EngineCatchUp.hpp"

namespace matching_engine::tests {

namespace {

// What EngineCatchUp asks the thread to do, copied, because a request is valid only during the call.
struct RecordingLinks : public EngineCatchUpLinksInterface {
    struct Request {
        int connection{0};
        int64_t number{0};
        int64_t last_seq_no{0};
        bool to_lead{false};
        bool wants_new_records{false};
        std::string follower_name;
    };
    void send_follow_request(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequest& request) override {
        requests.push_back(Request{connection.get_value(), request.request_number, request.last_seq_no, request.to_lead, request.wants_new_records,
                                   std::string(request.follower_name)});
    }
    void start_retry_timer() override {
        ++retry_timer_starts;
        retry_timer_running = true;
    }
    void stop_retry_timer() override {
        retry_timer_running = false;
    }
    void request_halt() override {
        ++halt_requests;
    }
    void start_halt_timer() override {
        ++halt_timer_starts;
    }
    void stop_halt_timer() override {
        ++halt_timer_stops;
    }
    void region_rebuilt(int64_t through) override {
        rebuilt.push_back(through);
    }
    std::vector<Request> requests;
    int retry_timer_starts{0};
    bool retry_timer_running{false};
    int halt_requests{0};
    int halt_timer_starts{0};
    int halt_timer_stops{0};
    std::vector<int64_t> rebuilt;
};

class EngineCatchUpTest : public ::testing::Test {
  protected:
    void SetUp() override {
        catch_up_ = std::make_unique<EngineCatchUp>(1, true, connections_, links_, logger_);
    }

    [[nodiscard]] static EngineCatchUp::RequestContext context(bool to_lead = false, int64_t highest_applied = 0) {
        EngineCatchUp::RequestContext context{};
        context.to_lead = to_lead;
        context.highest_applied = highest_applied;
        return context;
    }

    static pubsub_itc_fw::WalFollowStartedView started(int64_t number, int64_t after, int64_t reports_in_wal_through = 0) {
        pubsub_itc_fw::WalFollowStartedView view{};
        view.request_number = number;
        view.after_seq_no = after;
        view.serving_through_seq_no = after + 100;
        view.application_value = reports_in_wal_through;
        return view;
    }

    static pubsub_itc_fw::WalFollowCaughtUpView caught_up(int64_t number, int64_t records_sent, int64_t last, int64_t first_held = 1) {
        pubsub_itc_fw::WalFollowCaughtUpView view{};
        view.request_number = number;
        view.records_sent = records_sent;
        view.last_seq_no = last;
        view.first_seq_no_held = first_held;
        return view;
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unordered_set<pubsub_itc_fw::ConnectionID> connections_;
    std::unique_ptr<EngineCatchUp> catch_up_;
    const pubsub_itc_fw::ConnectionID first_{21};
    const pubsub_itc_fw::ConnectionID second_{22};
};

} // namespaces

TEST_F(EngineCatchUpTest, WithNoSequencerConnectedNothingIsAskedButTheRetryTimerStarts) {
    EXPECT_FALSE(catch_up_->begin(context()));
    EXPECT_TRUE(links_.requests.empty());
    EXPECT_FALSE(catch_up_->request_sent());
    EXPECT_TRUE(links_.retry_timer_running);
}

TEST_F(EngineCatchUpTest, TheRequestGoesOnEveryConnectionWithANewNumber) {
    connections_ = {first_, second_};
    EXPECT_TRUE(catch_up_->begin(context()));
    ASSERT_EQ(links_.requests.size(), 2U);
    EXPECT_EQ(links_.requests[0].number, 1);
    EXPECT_EQ(links_.requests[1].number, 1);
    EXPECT_EQ(links_.requests[0].follower_name, "matching_engine_1");
    EXPECT_TRUE(links_.requests[0].wants_new_records);
    EXPECT_TRUE(catch_up_->request_sent());
    EXPECT_TRUE(catch_up_->send_request(context()));
    EXPECT_EQ(links_.requests.back().number, 2);
}

TEST_F(EngineCatchUpTest, WithNoRegionTheEngineAsksToBePlacedAtTheHead) {
    connections_ = {first_};
    catch_up_->no_region_found();
    ASSERT_TRUE(catch_up_->begin(context(false, 0)));
    EXPECT_EQ(links_.requests[0].last_seq_no, -1);
}

TEST_F(EngineCatchUpTest, TheRequestNamesTheLaterOfThePositionFoundAndTheLastCommandApplied) {
    connections_ = {first_};
    catch_up_->region_found_current_to(50);
    ASSERT_TRUE(catch_up_->begin(context(false, 40)));
    EXPECT_EQ(links_.requests[0].last_seq_no, 50);
    ASSERT_TRUE(catch_up_->send_request(context(false, 60)));
    EXPECT_EQ(links_.requests[1].last_seq_no, 60);
}

TEST_F(EngineCatchUpTest, AnEngineAsksToLeadWhenToldToAndAlwaysWithHighAvailabilityOff) {
    connections_ = {first_};
    ASSERT_TRUE(catch_up_->begin(context(true)));
    EXPECT_TRUE(links_.requests[0].to_lead);
    ASSERT_TRUE(catch_up_->send_request(context(false)));
    EXPECT_FALSE(links_.requests[1].to_lead);
    catch_up_ = std::make_unique<EngineCatchUp>(1, false, connections_, links_, logger_);
    ASSERT_TRUE(catch_up_->begin(context(false)));
    EXPECT_TRUE(links_.requests[2].to_lead);
}

TEST_F(EngineCatchUpTest, NothingIsAskedWhileTheDiedApplyingQuestionIsOpen) {
    connections_ = {first_};
    EngineCatchUp::RequestContext waiting = context();
    waiting.may_ask = false;
    waiting.died_applying = 7;
    EXPECT_FALSE(catch_up_->begin(waiting));
    EXPECT_TRUE(links_.requests.empty());
    EXPECT_FALSE(catch_up_->request_sent());
    catch_up_->on_retry_tick(true, waiting);
    EXPECT_TRUE(links_.requests.empty()) << "the retry timer does not ask either";
}

TEST_F(EngineCatchUpTest, ANewConnectionIsAskedAloneWithTheSameNumber) {
    connections_ = {first_};
    ASSERT_TRUE(catch_up_->begin(context()));
    catch_up_->send_request_on(second_, context());
    ASSERT_EQ(links_.requests.size(), 2U);
    EXPECT_EQ(links_.requests[1].connection, 22);
    EXPECT_EQ(links_.requests[1].number, 1);
}

TEST_F(EngineCatchUpTest, OnlyTheAnswerToTheLatestRequestIsApplied) {
    connections_ = {first_, second_};
    ASSERT_TRUE(catch_up_->begin(context()));
    ASSERT_TRUE(catch_up_->send_request(context()));
    EXPECT_TRUE(catch_up_->from_an_earlier_answer(first_)) << "no answer has started";
    EXPECT_FALSE(catch_up_->started(first_, started(1, 0), true)) << "an earlier request's answer";
    EXPECT_FALSE(catch_up_->started(first_, started(2, 0), false)) << "not catching up";
    EXPECT_TRUE(catch_up_->started(second_, started(2, 0, 9), true));
    EXPECT_FALSE(catch_up_->from_an_earlier_answer(second_));
    EXPECT_TRUE(catch_up_->from_an_earlier_answer(first_));
    EXPECT_EQ(catch_up_->reports_in_wal_through(), 9);
}

TEST_F(EngineCatchUpTest, ACatchUpIsCompleteOnlyWhenTheTallyAccountsForEveryRecordSent) {
    connections_ = {first_};
    ASSERT_TRUE(catch_up_->begin(context()));
    ASSERT_TRUE(catch_up_->started(first_, started(1, 10), true));
    catch_up_->record_offered(11);
    catch_up_->record_offered(12);
    EXPECT_EQ(catch_up_->caught_up(first_, caught_up(2, 2, 12), 0), EngineCatchUp::Answer::other_request);
    EXPECT_EQ(catch_up_->caught_up(first_, caught_up(1, 3, 13), 0), EngineCatchUp::Answer::incomplete);
    EXPECT_TRUE(links_.retry_timer_running) << "left running, to ask again";
    EXPECT_EQ(catch_up_->caught_up(first_, caught_up(1, 2, 12), 0), EngineCatchUp::Answer::complete);
    EXPECT_FALSE(links_.retry_timer_running);
    EXPECT_EQ(catch_up_->position(), 12);
    EXPECT_TRUE(catch_up_->has_position());
    EXPECT_EQ(catch_up_->last_records_sent(), 2);
}

TEST_F(EngineCatchUpTest, TheRetryTimerAsksAgainOnlyWhenNothingHasArrived) {
    connections_ = {first_};
    ASSERT_TRUE(catch_up_->begin(context()));
    catch_up_->record_offered(1);
    catch_up_->on_retry_tick(true, context());
    EXPECT_EQ(links_.requests.size(), 1U) << "records are arriving";
    catch_up_->on_retry_tick(true, context());
    ASSERT_EQ(links_.requests.size(), 2U) << "nothing arrived since the last tick";
    EXPECT_EQ(links_.requests[1].number, 2);
    catch_up_->on_retry_tick(false, context());
    EXPECT_FALSE(links_.retry_timer_running) << "stopped once the engine is no longer catching up";
}

TEST_F(EngineCatchUpTest, BeginningACatchUpSetsTheCountsToZero) {
    connections_ = {first_};
    ASSERT_TRUE(catch_up_->begin(context()));
    catch_up_->record_offered(1);
    catch_up_->report_sent();
    catch_up_->report_left_out();
    ASSERT_TRUE(catch_up_->started(first_, started(1, 0, 5), true));
    ASSERT_TRUE(catch_up_->begin(context()));
    EXPECT_EQ(catch_up_->records_seen(), 0);
    EXPECT_EQ(catch_up_->reports_sent(), 0);
    EXPECT_EQ(catch_up_->reports_left_out(), 0);
    EXPECT_EQ(catch_up_->reports_in_wal_through(), 0);
    EXPECT_EQ(links_.retry_timer_starts, 2);
}

TEST_F(EngineCatchUpTest, WhileFollowingEachRecordMustBeTheNext) {
    EXPECT_TRUE(catch_up_->next_followed_record(99)) << "before the catch-up has ended, every record is its answer";
    catch_up_->follow_from(5);
    EXPECT_TRUE(catch_up_->next_followed_record(5));
    EXPECT_TRUE(catch_up_->next_followed_record(6));
    EXPECT_FALSE(catch_up_->next_followed_record(8));
    EXPECT_EQ(catch_up_->next_followed(), 7);
    catch_up_->stop_following();
    EXPECT_TRUE(catch_up_->next_followed_record(8));
}

TEST_F(EngineCatchUpTest, ANewRequestStopsExpectingAParticularRecord) {
    connections_ = {first_};
    catch_up_->follow_from(5);
    ASSERT_TRUE(catch_up_->send_request(context()));
    EXPECT_EQ(catch_up_->next_followed(), 0);
}

TEST_F(EngineCatchUpTest, AnUnusableRegionAsksForEveryRecordFromTheFirst) {
    connections_ = {first_};
    catch_up_->region_found_unusable();
    EXPECT_TRUE(catch_up_->region_was_unusable());
    ASSERT_TRUE(catch_up_->begin(context(false, 0)));
    EXPECT_EQ(links_.requests[0].last_seq_no, 0);
}

TEST_F(EngineCatchUpTest, ARebuiltBookIsTheVenuesWhenTheWalReachesTheFirstCommandOfTheDay) {
    catch_up_->region_found_unusable();
    catch_up_->account_for_unusable_region(caught_up(1, 10, 10, 1), true, 4);
    EXPECT_FALSE(catch_up_->region_was_unusable());
    EXPECT_EQ(links_.rebuilt, (std::vector<int64_t>{10}));
    EXPECT_EQ(links_.halt_requests, 0);
}

TEST_F(EngineCatchUpTest, AFollowerThatCannotAccountForTheBookDoesNotAskForAHalt) {
    catch_up_->region_found_unusable();
    catch_up_->account_for_unusable_region(caught_up(1, 10, 110, 100), false, 4);
    EXPECT_TRUE(catch_up_->region_was_unusable());
    EXPECT_EQ(links_.halt_requests, 0);
    EXPECT_FALSE(catch_up_->halt_awaited());
}

TEST_F(EngineCatchUpTest, AnEngineToLeadThatCannotAccountForTheBookAsksForAHaltUntilOneComes) {
    catch_up_->region_found_unusable();
    catch_up_->account_for_unusable_region(caught_up(1, 10, 110, 100), true, 4);
    EXPECT_TRUE(catch_up_->halt_awaited());
    EXPECT_EQ(links_.halt_requests, 1);
    EXPECT_EQ(links_.halt_timer_starts, 1);
    catch_up_->account_for_unusable_region(caught_up(2, 10, 110, 100), true, 4);
    EXPECT_EQ(links_.halt_timer_starts, 1) << "the timer is already running";
    catch_up_->on_halt_tick(false);
    EXPECT_EQ(links_.halt_requests, 3);
    catch_up_->on_halt_tick(true);
    EXPECT_EQ(links_.halt_requests, 3) << "trading is halted";
    catch_up_->stop_awaiting_halt();
    catch_up_->stop_awaiting_halt();
    EXPECT_EQ(links_.halt_timer_stops, 1);
    EXPECT_FALSE(catch_up_->halt_awaited());
    catch_up_->on_halt_tick(false);
    EXPECT_EQ(links_.halt_requests, 3);
}

} // namespaces
