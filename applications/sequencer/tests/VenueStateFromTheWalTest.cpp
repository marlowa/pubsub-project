// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "TradingDate.hpp"
#include "VenueStateFromTheWal.hpp"

namespace sequencer::tests {

namespace {

using How = VenueStateFromTheWal::How;
using Record = VenueStateFromTheWal::Record;

// A command encoded, kept so that a Record can point into it.
template <typename Command> std::vector<uint8_t> encoded(const Command& command) {
    size_t written = 0;
    size_t needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(command, nullptr, 0, written, needed));
    std::vector<uint8_t> bytes(needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(command, bytes.data(), bytes.size(), written, needed));
    bytes.resize(written);
    return bytes;
}

Record record_of(int16_t pdu_id, const std::vector<uint8_t>& bytes) {
    Record record{};
    record.pdu_id = pdu_id;
    record.payload = bytes.data();
    record.size = bytes.size();
    return record;
}

Record members_order() {
    Record record{};
    record.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
    return record;
}

Record report_answering(int64_t command) {
    Record record{};
    record.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    record.has_answers_seq_no = true;
    record.answers_seq_no = command;
    return record;
}

class VenueStateFromTheWalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        state_ = std::make_unique<VenueStateFromTheWal>(logger_);
        halt_ = encoded(pubsub_itc_fw_app::TechnicalEvent{pubsub_itc_fw_app::TradingPhase::Open, true, 20261103,
                                                          pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator});
        lift_ = encoded(pubsub_itc_fw_app::TechnicalEvent{pubsub_itc_fw_app::TradingPhase::Open, false, 20261103,
                                                          pubsub_itc_fw_app::TechnicalEventReason::LiftedByOperator});
        void_of_5_ = encoded(pubsub_itc_fw_app::VoidCommands{5, 5, false, pubsub_itc_fw_app::VoidReason::EngineDiedApplying});
    }

    [[nodiscard]] Record halt() const {
        return record_of(static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id), halt_);
    }
    [[nodiscard]] Record lift() const {
        return record_of(static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id), lift_);
    }
    [[nodiscard]] Record void_of_5() const {
        return record_of(static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id), void_of_5_);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    std::unique_ptr<VenueStateFromTheWal> state_;
    std::vector<uint8_t> halt_;
    std::vector<uint8_t> lift_;
    std::vector<uint8_t> void_of_5_;
};

} // namespaces

TEST_F(VenueStateFromTheWalTest, OnlyAFollowerLearnsTheTradingDateFromACommand) {
    Record dated = members_order();
    dated.has_trading_date = true;
    dated.trading_date = 20261103;
    static_cast<void>(state_->note_record(1, dated, How::read_at_open));
    static_cast<void>(state_->note_record(2, dated, How::written_by_this_leader));
    EXPECT_FALSE(state_->trading_date_known());
    static_cast<void>(state_->note_record(3, dated, How::written_as_follower));
    EXPECT_TRUE(state_->trading_date_known());
    EXPECT_EQ(state_->trading_date(std::nullopt, 0, 0), 20261103);
}

TEST_F(VenueStateFromTheWalTest, AFollowerFollowsItsLeadersTradingDate) {
    Record dated = members_order();
    dated.has_trading_date = true;
    dated.trading_date = 20261103;
    static_cast<void>(state_->note_record(1, dated, How::written_as_follower));
    dated.trading_date = 20261104;
    static_cast<void>(state_->note_record(2, dated, How::written_as_follower));
    EXPECT_EQ(state_->trading_date(std::nullopt, 0, 0), 20261104);
}

TEST_F(VenueStateFromTheWalTest, ALeaderTakesTheTradingDateOfTheFirstRecordOnce) {
    const int64_t first_record = 1793750400LL * 1000000000LL; // 2026-11-03T00:00:00Z
    const int64_t later = first_record + 3LL * 86400LL * 1000000000LL;
    EXPECT_EQ(state_->trading_date(first_record, 1, later), fix_common::trading_date_of(first_record));
    EXPECT_EQ(state_->trading_date(later, 1, later), fix_common::trading_date_of(first_record)) << "known, so not read again";
}

TEST_F(VenueStateFromTheWalTest, AnEmptyWalTakesTheDateOfTheCommandBeingWritten) {
    const int64_t now = 1793750400LL * 1000000000LL;
    EXPECT_EQ(state_->trading_date(std::nullopt, 0, now), fix_common::trading_date_of(now));
}

TEST_F(VenueStateFromTheWalTest, TheHighestCommandAnsweredRisesWithReportsAndNeverFalls) {
    static_cast<void>(state_->note_record(10, report_answering(7), How::written_by_this_leader));
    static_cast<void>(state_->note_record(11, report_answering(4), How::written_as_follower));
    static_cast<void>(state_->note_record(12, members_order(), How::read_at_open));
    EXPECT_EQ(state_->highest_command_answered(), 7);
    EXPECT_FALSE(state_->discard_after(5));
    EXPECT_EQ(state_->highest_command_answered(), 7) << "not lowered by records discarded";
}

TEST_F(VenueStateFromTheWalTest, AVoidIsNotedHoweverItIsHeldAndOnlyOnce) {
    EXPECT_TRUE(state_->note_record(20, void_of_5(), How::read_at_open).void_added);
    EXPECT_FALSE(state_->note_record(20, void_of_5(), How::written_as_follower).void_added) << "held already";
    ASSERT_EQ(state_->voids().size(), 1U);
    EXPECT_NE(state_->voids().voiding(5, fix_common::VoidedKind::cancel), nullptr);
}

TEST_F(VenueStateFromTheWalTest, AVoidWrittenAsLeaderIsSentToTheEnginesOnceCommitted) {
    EXPECT_TRUE(state_->note_record(20, void_of_5(), How::written_by_this_leader).void_added);
    EXPECT_FALSE(state_->take_voids_due(19));
    EXPECT_TRUE(state_->take_voids_due(20));
    EXPECT_FALSE(state_->take_voids_due(21)) << "owed once";
}

TEST_F(VenueStateFromTheWalTest, AVoidHeldAsFollowerIsNotOwedToTheEngines) {
    static_cast<void>(state_->note_record(20, void_of_5(), How::written_as_follower));
    EXPECT_FALSE(state_->take_voids_due(100));
}

TEST_F(VenueStateFromTheWalTest, AVoidThatDoesNotDecodeIsNotKept) {
    const std::vector<uint8_t> garbage{0xff};
    EXPECT_FALSE(
        state_->note_record(20, record_of(static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id), garbage), How::read_at_open).void_added);
    EXPECT_TRUE(state_->voids().empty());
}

TEST_F(VenueStateFromTheWalTest, ATechnicalEventHaltsTradingAfterTheLastMembersCommand) {
    static_cast<void>(state_->note_record(1, members_order(), How::written_by_this_leader));
    EXPECT_TRUE(state_->note_record(2, halt(), How::written_by_this_leader).halt_changed);
    EXPECT_TRUE(state_->trading_halt().halted());
    EXPECT_EQ(state_->trading_halt().latest()->last_members_command_before, 1);
    EXPECT_FALSE(state_->note_record(3, halt(), How::written_as_follower).halt_changed) << "halted already";
    EXPECT_TRUE(state_->note_record(4, lift(), How::written_as_follower).halt_changed);
    EXPECT_FALSE(state_->trading_halt().halted());
}

TEST_F(VenueStateFromTheWalTest, DiscardingRecordsDiscardsTheVoidsAndHaltsAfterThePoint) {
    static_cast<void>(state_->note_record(1, members_order(), How::written_as_follower));
    static_cast<void>(state_->note_record(2, void_of_5(), How::written_as_follower));
    static_cast<void>(state_->note_record(3, halt(), How::written_as_follower));
    EXPECT_TRUE(state_->discard_after(1));
    EXPECT_FALSE(state_->trading_halt().halted());
    EXPECT_TRUE(state_->voids().empty());
}

} // namespaces
