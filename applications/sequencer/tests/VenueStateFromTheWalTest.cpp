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

#include "GatewayIds.hpp"
#include "SessionIdentity.hpp"
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
        halt_ = encoded(pubsub_itc_fw_app::TechnicalEvent{pubsub_itc_fw_app::TradingPhase::Open, true, 20'261'103,
                                                          pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator});
        lift_ = encoded(pubsub_itc_fw_app::TechnicalEvent{pubsub_itc_fw_app::TradingPhase::Open, false, 20'261'103,
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

namespace {

Record phase_change(const std::vector<uint8_t>& bytes) {
    return record_of(static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id), bytes);
}

std::vector<uint8_t> entering(pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day, bool halted = false) {
    return encoded(pubsub_itc_fw_app::TechnicalEvent{phase, halted, trading_day, pubsub_itc_fw_app::TechnicalEventReason::RequestedByOperator});
}

} // namespaces

TEST_F(VenueStateFromTheWalTest, TheTradingDayHasNotStartedUntilTheWalHoldsAStartOfDay) {
    EXPECT_FALSE(state_->trading_day_started());
    EXPECT_EQ(state_->trading_day(), 0);
    Record dated = members_order();
    dated.has_trading_date = true;
    dated.trading_date = 20'261'103;
    static_cast<void>(state_->note_record(1, dated, How::written_as_follower));
    EXPECT_FALSE(state_->trading_day_started()) << "the date written on a command is not a start of day";
}

TEST_F(VenueStateFromTheWalTest, TheStartOfDayFixesTheTradingDayHoweverTheRecordIsHeld) {
    const std::vector<uint8_t> start = entering(pubsub_itc_fw_app::TradingPhase::StartOfDay, 20'261'116);
    for (const How how : {How::read_at_open, How::written_as_follower, How::written_by_this_leader}) {
        VenueStateFromTheWal state(logger_);
        EXPECT_TRUE(state.note_record(1, phase_change(start), how).phase_changed);
        EXPECT_TRUE(state.trading_day_started());
        EXPECT_EQ(state.trading_day(), 20'261'116);
        EXPECT_EQ(state.trading_phase().latest()->phase, pubsub_itc_fw_app::TradingPhase::StartOfDay);
    }
}

TEST_F(VenueStateFromTheWalTest, EachTechnicalEventGivesThePhaseAndDiscardingOneGoesBack) {
    const std::vector<uint8_t> start = entering(pubsub_itc_fw_app::TradingPhase::StartOfDay, 20'261'116);
    const std::vector<uint8_t> pre_open = entering(pubsub_itc_fw_app::TradingPhase::PreOpen, 20'261'116);
    const std::vector<uint8_t> open = entering(pubsub_itc_fw_app::TradingPhase::Open, 20'261'116);
    static_cast<void>(state_->note_record(1, phase_change(start), How::written_as_follower));
    static_cast<void>(state_->note_record(2, phase_change(pre_open), How::written_as_follower));
    static_cast<void>(state_->note_record(3, phase_change(open), How::written_as_follower));
    EXPECT_EQ(state_->trading_phase().latest()->phase, pubsub_itc_fw_app::TradingPhase::Open);
    EXPECT_EQ(state_->trading_phase().entered_at(pubsub_itc_fw_app::TradingPhase::PreOpen), 2);
    static_cast<void>(state_->discard_after(2));
    EXPECT_EQ(state_->trading_phase().latest()->phase, pubsub_itc_fw_app::TradingPhase::PreOpen);
    static_cast<void>(state_->discard_after(0));
    EXPECT_FALSE(state_->trading_day_started()) << "a discarded start of day leaves no trading day";
}

TEST_F(VenueStateFromTheWalTest, AChangeOfPhaseWhileHaltedCarriesTheHaltOn) {
    const std::vector<uint8_t> start = entering(pubsub_itc_fw_app::TradingPhase::StartOfDay, 20'261'116);
    const std::vector<uint8_t> halt_in_start = encoded(pubsub_itc_fw_app::TechnicalEvent{pubsub_itc_fw_app::TradingPhase::StartOfDay, true, 20'261'116,
                                                                                         pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator});
    const std::vector<uint8_t> pre_open_halted = entering(pubsub_itc_fw_app::TradingPhase::PreOpen, 20'261'116, true);
    static_cast<void>(state_->note_record(1, phase_change(start), How::written_by_this_leader));
    static_cast<void>(state_->note_record(2, phase_change(halt_in_start), How::written_by_this_leader));
    const VenueStateFromTheWal::Noted noted = state_->note_record(3, phase_change(pre_open_halted), How::written_by_this_leader);
    EXPECT_TRUE(noted.phase_changed);
    EXPECT_FALSE(noted.halt_changed);
    EXPECT_TRUE(state_->trading_halt().halted());
    EXPECT_EQ(state_->trading_halt().latest()->reason, pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator)
        << "the halt keeps the reason it was declared with";
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

// The cancels on disconnect waiting to be carried out are kept from the WAL by every instance, however it holds the
// records: a SessionEnded starts a wait from the record's own time, a SessionEndWithdrawn ends it, and so does a
// CancelSessionOrders; discarded records take the waits they started with them.
TEST_F(VenueStateFromTheWalTest, CancelsOnDisconnectAreKeptFromTheWal) {
    constexpr int64_t second = 1'000'000'000;
    const fix_common::SessionIdentity member = fix_common::SessionIdentity::make("MEMBER", gateway_ids::fix_order_gateway);
    const fix_common::SessionIdentity other = fix_common::SessionIdentity::make("OTHER", gateway_ids::fix_order_gateway);
    const std::vector<uint8_t> member_ended = encoded(pubsub_itc_fw_app::SessionEnded{"MEMBER", gateway_ids::fix_order_gateway, 8, false});
    const std::vector<uint8_t> other_ended = encoded(pubsub_itc_fw_app::SessionEnded{"OTHER", gateway_ids::fix_order_gateway, 8, false});
    const std::vector<uint8_t> member_withdrawn = encoded(pubsub_itc_fw_app::SessionEndWithdrawn{"MEMBER", gateway_ids::fix_order_gateway});
    const std::vector<uint8_t> other_cancelled = encoded(pubsub_itc_fw_app::CancelSessionOrders{"OTHER", gateway_ids::fix_order_gateway});

    Record ended = record_of(static_cast<int16_t>(pubsub_itc_fw_app::SessionEnded::message_pdu_id), member_ended);
    ended.wall_time_ns = 100 * second;
    static_cast<void>(state_->note_record(10, ended, How::written_as_follower));
    Record ended_other = record_of(static_cast<int16_t>(pubsub_itc_fw_app::SessionEnded::message_pdu_id), other_ended);
    ended_other.wall_time_ns = 101 * second;
    static_cast<void>(state_->note_record(11, ended_other, How::read_at_open));
    EXPECT_TRUE(state_->session_cancels().pending(member));
    EXPECT_TRUE(state_->session_cancels().pending(other));

    int due = 0;
    state_->session_cancels().for_each_due(108 * second, [&due](const fix_common::SessionIdentity&) { ++due; });
    EXPECT_EQ(due, 1) << "the member's wait runs from its record's time, 100 s, and is due at 108 s; the other's is due at 109 s";

    static_cast<void>(state_->note_record(12, record_of(static_cast<int16_t>(pubsub_itc_fw_app::SessionEndWithdrawn::message_pdu_id), member_withdrawn),
                                          How::written_by_this_leader));
    EXPECT_FALSE(state_->session_cancels().pending(member));
    static_cast<void>(state_->note_record(13, record_of(static_cast<int16_t>(pubsub_itc_fw_app::CancelSessionOrders::message_pdu_id), other_cancelled),
                                          How::written_by_this_leader));
    EXPECT_FALSE(state_->session_cancels().pending(other));

    static_cast<void>(state_->note_record(14, ended, How::written_as_follower));
    static_cast<void>(state_->discard_after(13));
    EXPECT_FALSE(state_->session_cancels().pending(member));
}

TEST_F(VenueStateFromTheWalTest, TheLatestTradingScheduleIsTheScheduleAndDiscardingOneGoesBack) {
    const std::vector<uint8_t> start = entering(pubsub_itc_fw_app::TradingPhase::StartOfDay, 20'261'116);
    static_cast<void>(state_->note_record(1, phase_change(start), How::read_at_open));
    std::vector<pubsub_itc_fw_app::TradingScheduleEntry> first_entries{{pubsub_itc_fw_app::TradingPhase::PreOpen, 1000},
                                                                       {pubsub_itc_fw_app::TradingPhase::Open, 2000}};
    pubsub_itc_fw_app::TradingSchedule first{};
    first.trading_day = 20'261'116;
    first.entries = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::TradingScheduleEntry>{first_entries.data(), first_entries.size()};
    first.operator_name = "alice";
    const std::vector<uint8_t> first_bytes = encoded(first);
    std::vector<pubsub_itc_fw_app::TradingScheduleEntry> second_entries{{pubsub_itc_fw_app::TradingPhase::PreOpen, 500}};
    pubsub_itc_fw_app::TradingSchedule second{};
    second.trading_day = 20'261'116;
    second.entries = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::TradingScheduleEntry>{second_entries.data(), second_entries.size()};
    second.operator_name = "bob";
    const std::vector<uint8_t> second_bytes = encoded(second);
    const auto pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::TradingSchedule::message_pdu_id);

    EXPECT_FALSE(state_->trading_schedule().recorded());
    EXPECT_TRUE(state_->note_record(2, record_of(pdu_id, first_bytes), How::written_as_follower).schedule_changed);
    EXPECT_TRUE(state_->note_record(7, record_of(pdu_id, second_bytes), How::written_by_this_leader).schedule_changed);
    ASSERT_TRUE(state_->trading_schedule().recorded());
    EXPECT_EQ(state_->trading_schedule().latest()->seq_no, 7);
    EXPECT_EQ(state_->trading_schedule().latest()->operator_name, "bob");
    EXPECT_EQ(state_->trading_schedule().entry_for(pubsub_itc_fw_app::TradingPhase::PreOpen)->earliest_utc_ns, 500);
    EXPECT_FALSE(state_->trading_schedule().entry_for(pubsub_itc_fw_app::TradingPhase::Open).has_value()) << "the later schedule replaces the earlier";

    static_cast<void>(state_->discard_after(6));
    EXPECT_EQ(state_->trading_schedule().latest()->seq_no, 2);
    EXPECT_EQ(state_->trading_schedule().entry_for(pubsub_itc_fw_app::TradingPhase::Open)->earliest_utc_ns, 2000);
}

} // namespaces
