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

#include "DiedApplyingCheck.hpp"

namespace matching_engine::tests {

namespace {

using Question = DiedApplyingCheck::Question;
using Outcome = pubsub_itc_fw_app::DiedApplyingOutcome;

// What DiedApplyingCheck asks the thread to do.
struct RecordingLinks : public DiedApplyingCheckLinksInterface {
    pubsub_itc_fw::ConnectionID ask_peer_which_commands_it_applied() override {
        if (peer_connected) {
            ++peer_asks;
            return pubsub_itc_fw::ConnectionID{31};
        }
        return pubsub_itc_fw::ConnectionID{};
    }
    void start_peer_answer_timer() override {
        peer_timer_running = true;
    }
    void stop_peer_answer_timer() override {
        peer_timer_running = false;
    }
    void send_void_request(int64_t command) override {
        void_requests.push_back(command);
    }
    void start_void_request_timer() override {
        void_timer_running = true;
    }
    void stop_void_request_timer() override {
        void_timer_running = false;
    }
    void settled(int64_t command, Outcome outcome) override {
        settled_command = command;
        outcomes.push_back(outcome);
    }
    bool peer_connected{false};
    int peer_asks{0};
    bool peer_timer_running{false};
    std::vector<int64_t> void_requests;
    bool void_timer_running{false};
    int64_t settled_command{0};
    std::vector<Outcome> outcomes;
};

class DiedApplyingCheckTest : public ::testing::Test {
  protected:
    void SetUp() override {
        check_ = std::make_unique<DiedApplyingCheck>(links_, logger_);
    }

    // The previous process died applying command 7, and high availability is on.
    void begin_asking_the_peer() {
        check_->died_applying(7);
        check_->begin(true);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<DiedApplyingCheck> check_;
};

} // namespaces

TEST_F(DiedApplyingCheckTest, WithNoCommandThereIsNoQuestion) {
    check_->begin(true);
    EXPECT_EQ(check_->question(), Question::none);
    EXPECT_FALSE(check_->open());
    EXPECT_FALSE(links_.peer_timer_running);
}

TEST_F(DiedApplyingCheckTest, WithHighAvailabilityOffTheSequencersAreAskedAtOnce) {
    check_->died_applying(7);
    check_->begin(false);
    EXPECT_EQ(check_->question(), Question::asking_sequencers);
    EXPECT_EQ(links_.void_requests, (std::vector<int64_t>{7}));
    EXPECT_TRUE(links_.void_timer_running);
}

TEST_F(DiedApplyingCheckTest, ThePeerIsAskedOnceWhenAConnectionToItIsUp) {
    begin_asking_the_peer();
    EXPECT_EQ(check_->question(), Question::asking_peer);
    EXPECT_TRUE(check_->open());
    EXPECT_TRUE(links_.peer_timer_running);
    EXPECT_EQ(links_.peer_asks, 0) << "no connection to the peer yet";
    links_.peer_connected = true;
    check_->peer_connected();
    check_->peer_connected();
    EXPECT_EQ(links_.peer_asks, 1);
}

TEST_F(DiedApplyingCheckTest, APeerThatAppliedTheCommandSettlesIt) {
    links_.peer_connected = true;
    begin_asking_the_peer();
    check_->peer_answered(7);
    EXPECT_FALSE(check_->open());
    EXPECT_EQ(links_.outcomes, (std::vector<Outcome>{Outcome::PeerAppliedIt}));
    EXPECT_EQ(links_.settled_command, 7);
    EXPECT_FALSE(links_.peer_timer_running);
    ASSERT_TRUE(check_->outcome().has_value());
    EXPECT_EQ(*check_->outcome(), Outcome::PeerAppliedIt);
}

TEST_F(DiedApplyingCheckTest, APeerThatDidNotApplyItSendsTheQuestionToTheSequencers) {
    links_.peer_connected = true;
    begin_asking_the_peer();
    check_->peer_answered(6);
    EXPECT_EQ(check_->question(), Question::asking_sequencers);
    EXPECT_FALSE(links_.peer_timer_running);
    EXPECT_EQ(links_.void_requests, (std::vector<int64_t>{7}));
    EXPECT_TRUE(links_.void_timer_running);
    EXPECT_TRUE(links_.outcomes.empty());
}

TEST_F(DiedApplyingCheckTest, APeerSilentForALeasePeriodSendsTheQuestionToTheSequencers) {
    begin_asking_the_peer();
    check_->peer_answer_timer_fired();
    EXPECT_EQ(check_->question(), Question::asking_sequencers);
    check_->peer_answered(9);
    EXPECT_EQ(check_->question(), Question::asking_sequencers) << "an answer after the lease period is ignored";
}

TEST_F(DiedApplyingCheckTest, TheSequencersAreAskedAgainOnEachTickAndEachNewConnection) {
    begin_asking_the_peer();
    check_->sequencer_connected();
    check_->void_request_timer_fired();
    EXPECT_TRUE(links_.void_requests.empty()) << "not while the peer is being asked";
    check_->peer_answer_timer_fired();
    check_->sequencer_connected();
    check_->void_request_timer_fired();
    EXPECT_EQ(links_.void_requests.size(), 3U);
}

TEST_F(DiedApplyingCheckTest, AVoidNamingTheCommandSettlesIt) {
    begin_asking_the_peer();
    check_->peer_answer_timer_fired();
    check_->voids_learned(false);
    EXPECT_TRUE(check_->open());
    check_->voids_learned(true);
    EXPECT_FALSE(check_->open());
    EXPECT_EQ(links_.outcomes, (std::vector<Outcome>{Outcome::Voided}));
    EXPECT_FALSE(links_.void_timer_running);
}

TEST_F(DiedApplyingCheckTest, ADeclineOfThisCommandSettlesIt) {
    begin_asking_the_peer();
    check_->peer_answer_timer_fired();
    check_->declined(8, pubsub_itc_fw_app::VoidDeclinedReason::AnsweredAlready);
    EXPECT_TRUE(check_->open()) << "a decline of another command";
    check_->declined(7, pubsub_itc_fw_app::VoidDeclinedReason::AnsweredAlready);
    EXPECT_EQ(links_.outcomes, (std::vector<Outcome>{Outcome::Declined}));
    EXPECT_FALSE(links_.void_timer_running);
}

TEST_F(DiedApplyingCheckTest, AVoidOrADeclineWhileThePeerIsAskedIsIgnored) {
    begin_asking_the_peer();
    check_->voids_learned(true);
    check_->declined(7, pubsub_itc_fw_app::VoidDeclinedReason::AnsweredAlready);
    EXPECT_EQ(check_->question(), Question::asking_peer);
    EXPECT_TRUE(links_.outcomes.empty());
}

} // namespaces
