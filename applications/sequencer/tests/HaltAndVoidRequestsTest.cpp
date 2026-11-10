// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "HaltAndVoidRequests.hpp"

namespace sequencer::tests {

namespace {

using Outcome = pubsub_itc_fw_app::OperatorHaltOutcome;
using EventReason = pubsub_itc_fw_app::TechnicalEventReason;
using Declined = pubsub_itc_fw_app::VoidDeclinedReason;

// The facts HaltAndVoidRequests decides from, and every command written and answer sent, in order.
struct RecordingLinks : public HaltAndVoidRequestsLinksInterface {
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] bool orders_routed_to(const pubsub_itc_fw::ConnectionID& connection) const override {
        return connection == routed;
    }
    [[nodiscard]] bool an_engine_takes_orders() const override {
        return engine_takes_orders;
    }
    [[nodiscard]] bool another_engine_leads(int64_t instance_id) const override {
        return leading_instance != 0 && leading_instance != instance_id;
    }
    [[nodiscard]] const TradingHaltState& trading_halt() const override {
        return halt;
    }
    [[nodiscard]] int64_t highest_command_answered() const override {
        return answered_through;
    }
    [[nodiscard]] int64_t released_through() const override {
        return released;
    }
    [[nodiscard]] int64_t last_seq_no() const override {
        return next - 1;
    }
    [[nodiscard]] int64_t next_seq_no() const override {
        return next;
    }
    [[nodiscard]] int32_t trading_day(int64_t) override {
        return 20261104;
    }
    [[nodiscard]] bool voided_already(int64_t seq_no) const override {
        return seq_no == voided;
    }
    [[nodiscard]] CommandInRecord command_in_wal_at(int64_t seq_no, bool& found) override {
        found = seq_no < next;
        return in_record;
    }
    bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) override {
        if (technical_event_cannot_be_encoded) {
            return false;
        }
        written.push_back(std::string("TechnicalEvent ") + (event.halted ? "halts " : "lifts ") + std::string(pubsub_itc_fw_app::to_string(event.reason)));
        halt.note_technical_event(next, event.halted, event.reason);
        ++next;
        return true;
    }
    bool write_void_commands(const pubsub_itc_fw_app::VoidCommands& command) override {
        written.push_back("VoidCommands " + std::to_string(command.first_seq_no) + "-" + std::to_string(command.last_seq_no) +
                          (command.new_orders_only ? " new orders" : " any"));
        ++next;
        return true;
    }
    bool write_cancel_book(const pubsub_itc_fw_app::CancelBook& command) override {
        written.push_back(std::string("CancelBook ") + std::string(pubsub_itc_fw_app::to_string(command.reason)) +
                          (command.orders_placed_through == std::numeric_limits<int64_t>::max() ? " every order" : " some orders"));
        ++next;
        return true;
    }
    void send_voids(const pubsub_itc_fw::ConnectionID& connection) override {
        voids_sent_to.push_back(connection.get_value());
    }
    void decline_void(const pubsub_itc_fw::ConnectionID&, int64_t, pubsub_itc_fw_app::VoidDeclinedReason reason) override {
        declined.push_back(reason);
    }
    void answer_operator(const pubsub_itc_fw::ConnectionID&, pubsub_itc_fw_app::OperatorHaltOutcome outcome, int64_t seq_no, const std::string&) override {
        answers.push_back(outcome);
        answer_seq_nos.push_back(seq_no);
    }

    bool is_leading{true};
    pubsub_itc_fw::ConnectionID routed{31};
    bool engine_takes_orders{true};
    int64_t leading_instance{0};
    TradingHaltState halt;
    int64_t answered_through{0};
    bool technical_event_cannot_be_encoded{false};
    int64_t released{0};
    int64_t next{11};
    int64_t voided{0};
    CommandInRecord in_record{CommandInRecord::MembersCommand};
    std::vector<std::string> written;
    std::vector<int> voids_sent_to;
    std::vector<Declined> declined;
    std::vector<Outcome> answers;
    std::vector<int64_t> answer_seq_nos;
};

class HaltAndVoidRequestsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        requests_ = std::make_unique<HaltAndVoidRequests>(links_, logger_);
    }

    void operator_asks(bool halt) {
        requests_->operator_request(operator_connection_, "alice", halt, 0);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<HaltAndVoidRequests> requests_;
    const pubsub_itc_fw::ConnectionID operator_connection_{61};
    const pubsub_itc_fw::ConnectionID engine_connection_{31};
};

} // namespaces

// ---- An operator's request ------------------------------------------------------------------------------------------

TEST_F(HaltAndVoidRequestsTest, AnOperatorsHaltWritesATechnicalEventAndCancelsNothing) {
    operator_asks(true);
    EXPECT_EQ(links_.written, (std::vector<std::string>{"TechnicalEvent halts DeclaredByOperator"}));
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Halted}));
    EXPECT_EQ(links_.answer_seq_nos, (std::vector<int64_t>{11})) << "the answer names the record written";
}

TEST_F(HaltAndVoidRequestsTest, AFollowerWritesNothingForAnOperator) {
    links_.is_leading = false;
    operator_asks(true);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotLeader}));
}

TEST_F(HaltAndVoidRequestsTest, AHaltWhileHaltedWritesNothing) {
    operator_asks(true);
    operator_asks(true);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Halted, Outcome::AlreadyHalted}));
}

TEST_F(HaltAndVoidRequestsTest, AHaltThatCannotBeEncodedIsAnsweredAsNotWritten) {
    links_.technical_event_cannot_be_encoded = true;
    operator_asks(true);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotWritten}));
}

TEST_F(HaltAndVoidRequestsTest, LiftingWhenNotHaltedWritesNothing) {
    operator_asks(false);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotHalted}));
}

TEST_F(HaltAndVoidRequestsTest, TheHaltIsLiftedOnlyOnceAnEngineLeadsAndHasAnsweredEveryCommandBeforeIt) {
    links_.halt.note_members_command(9);
    operator_asks(true);
    links_.engine_takes_orders = false;
    operator_asks(false);
    links_.engine_takes_orders = true;
    links_.answered_through = 8;
    operator_asks(false);
    links_.answered_through = 9;
    operator_asks(false);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Halted, Outcome::NoEngineLeads, Outcome::ReportsOutstanding, Outcome::Lifted}));
    EXPECT_EQ(links_.written, (std::vector<std::string>{"TechnicalEvent halts DeclaredByOperator", "TechnicalEvent lifts LiftedByOperator"}));
    EXPECT_FALSE(links_.halt.halted());
}

// ---- A matching engine's request to halt ----------------------------------------------------------------------------

TEST_F(HaltAndVoidRequestsTest, AnEngineOrdersAreRoutedToHaltsTradingWithItsReason) {
    requests_->engine_halt_request(engine_connection_, 2, 3, EventReason::CannotAccountForBook, 0);
    EXPECT_EQ(links_.written, (std::vector<std::string>{"TechnicalEvent halts CannotAccountForBook"}));
}

TEST_F(HaltAndVoidRequestsTest, AnEngineOrdersAreNotRoutedToCannotHaltTrading) {
    requests_->engine_halt_request(pubsub_itc_fw::ConnectionID{32}, 2, 3, EventReason::CannotAccountForBook, 0);
    links_.is_leading = false;
    requests_->engine_halt_request(engine_connection_, 2, 3, EventReason::CannotAccountForBook, 0);
    EXPECT_TRUE(links_.written.empty());
}

// ---- A matching engine's request to void a command ------------------------------------------------------------------

TEST_F(HaltAndVoidRequestsTest, ACommandAnEngineDiedApplyingIsVoided) {
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_EQ(links_.written, (std::vector<std::string>{"VoidCommands 7-7 any"}));
    EXPECT_TRUE(links_.declined.empty());
}

TEST_F(HaltAndVoidRequestsTest, ACommandVoidedAlreadyIsAnsweredWithTheVoids) {
    links_.voided = 7;
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.voids_sent_to, (std::vector<int>{31}));
}

TEST_F(HaltAndVoidRequestsTest, AVoidIsDeclinedWhenTheOtherEngineLeads) {
    links_.leading_instance = 1;
    requests_->void_request(engine_connection_, 2, 7);
    links_.leading_instance = 2;
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_EQ(links_.declined, (std::vector<Declined>{Declined::AnotherEngineLeads})) << "the engine asking is the one that leads, so it is voided";
    EXPECT_EQ(links_.written, (std::vector<std::string>{"VoidCommands 7-7 any"}));
}

TEST_F(HaltAndVoidRequestsTest, AVoidIsDeclinedForACommandAReportAnswers) {
    links_.answered_through = 7;
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_EQ(links_.declined, (std::vector<Declined>{Declined::AnsweredAlready}));
    EXPECT_TRUE(links_.written.empty());
}

TEST_F(HaltAndVoidRequestsTest, AVoidIsDeclinedForARecordTheWalDoesNotHoldOrThatIsNotAMembersCommand) {
    requests_->void_request(engine_connection_, 2, 11);
    links_.in_record = CommandInRecord::OtherRecord;
    requests_->void_request(engine_connection_, 2, 7);
    links_.in_record = CommandInRecord::NotDecodable;
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_EQ(links_.declined, (std::vector<Declined>{Declined::NotInWal, Declined::NotAMembersCommand, Declined::NotAMembersCommand}));
    EXPECT_TRUE(links_.written.empty());
}

TEST_F(HaltAndVoidRequestsTest, AFollowerDoesNotActOnAVoidRequest) {
    links_.is_leading = false;
    requests_->void_request(engine_connection_, 2, 7);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_TRUE(links_.declined.empty());
}

// ---- The halt when the venue cannot recover by itself ---------------------------------------------------------------

TEST_F(HaltAndVoidRequestsTest, TheRecoveryHaltVoidsTheNewOrdersHeldCancelsTheBookAndHaltsInThatOrder) {
    links_.released = 6;
    links_.answered_through = 4;
    requests_->recovery_halt(0);
    EXPECT_EQ(links_.written,
              (std::vector<std::string>{"VoidCommands 7-10 new orders", "CancelBook RecoveryTooLong every order", "TechnicalEvent halts RecoveryTooLong"}));
}

TEST_F(HaltAndVoidRequestsTest, TheRecoveryHaltVoidsNothingAReportAnswers) {
    links_.released = 6;
    links_.answered_through = 8;
    requests_->recovery_halt(0);
    ASSERT_FALSE(links_.written.empty());
    EXPECT_EQ(links_.written[0], "VoidCommands 9-10 new orders");
}

TEST_F(HaltAndVoidRequestsTest, WithNothingHeldTheRecoveryHaltVoidsNothing) {
    links_.released = 10;
    requests_->recovery_halt(0);
    EXPECT_EQ(links_.written, (std::vector<std::string>{"CancelBook RecoveryTooLong every order", "TechnicalEvent halts RecoveryTooLong"}));
}

} // namespaces
