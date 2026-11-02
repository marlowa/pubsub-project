// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include "TradingHaltState.hpp"

namespace {

using sequencer::TradingHaltState;
using Reason = pubsub_itc_fw_app::TechnicalEventReason;

// A WAL with no TechnicalEvent: trading is not halted, and there is no latest event.
TEST(TradingHaltStateTest, AWalWithNoTechnicalEventIsNotHalted) {
    TradingHaltState state;
    state.note_members_command(1);
    EXPECT_FALSE(state.halted());
    EXPECT_FALSE(state.latest().has_value());
    EXPECT_EQ(state.last_members_command(), 1);
}

// The latest TechnicalEvent says whether trading is halted, and records the last member's command before it.
TEST(TradingHaltStateTest, TheLatestTechnicalEventSaysWhetherTradingIsHalted) {
    TradingHaltState state;
    state.note_members_command(3);
    state.note_members_command(5);
    EXPECT_TRUE(state.note_technical_event(8, true, Reason::RecoveryTooLong)) << "halting changes the state";
    EXPECT_TRUE(state.halted());
    ASSERT_TRUE(state.latest().has_value());
    EXPECT_EQ(state.latest()->seq_no, 8);
    EXPECT_EQ(state.latest()->reason, Reason::RecoveryTooLong);
    EXPECT_EQ(state.latest()->last_members_command_before, 5);

    EXPECT_FALSE(state.note_technical_event(9, true, Reason::DeclaredByOperator)) << "a second halt changes nothing";
    state.note_members_command(10);
    EXPECT_TRUE(state.note_technical_event(12, false, Reason::LiftedByOperator)) << "lifting changes the state";
    EXPECT_FALSE(state.halted());
    EXPECT_EQ(state.latest()->last_members_command_before, 10);
}

// A discard that removes a lift leaves trading halted again; one that removes the halt leaves it not halted.
TEST(TradingHaltStateTest, ADiscardRemovesTheTechnicalEventsAfterIt) {
    TradingHaltState state;
    state.note_members_command(2);
    static_cast<void>(state.note_technical_event(4, true, Reason::DeclaredByOperator));
    static_cast<void>(state.note_technical_event(6, false, Reason::LiftedByOperator));
    EXPECT_TRUE(state.discard_after(5)) << "the lift is gone, so trading is halted";
    EXPECT_TRUE(state.halted());
    EXPECT_FALSE(state.discard_after(4)) << "the halt at 4 is kept";
    EXPECT_TRUE(state.discard_after(3)) << "the halt is gone too";
    EXPECT_FALSE(state.halted());
}

// After a discard, the last member's command is the last one left, so a halt written next records it exactly.
TEST(TradingHaltStateTest, ADiscardFindsTheLastMembersCommandLeft) {
    TradingHaltState state;
    state.note_members_command(10);
    state.note_members_command(20);
    state.note_members_command(30);
    static_cast<void>(state.discard_after(25));
    EXPECT_EQ(state.last_members_command(), 20);
    static_cast<void>(state.note_technical_event(26, true, Reason::DeclaredByOperator));
    EXPECT_EQ(state.latest()->last_members_command_before, 20);
}

// More members' commands than are kept: the oldest are forgotten, and the latest of those is still known exactly.
TEST(TradingHaltStateTest, TheLatestForgottenCommandIsKnownExactly) {
    TradingHaltState state;
    const auto kept = static_cast<int64_t>(TradingHaltState::recent_members_commands_kept);
    // Commands at 1, 3, 5 and so on: twice as many as are kept, so the first half are forgotten.
    for (int64_t index = 0; index < 2 * kept; ++index) {
        state.note_members_command(2 * index + 1);
    }
    EXPECT_EQ(state.last_members_command(), 2 * (2 * kept - 1) + 1);
    const int64_t latest_forgotten = 2 * (kept - 1) + 1;
    // Cut back to just after the latest forgotten command: every kept command is discarded.
    static_cast<void>(state.discard_after(latest_forgotten + 1));
    EXPECT_EQ(state.last_members_command(), latest_forgotten) << "the latest forgotten command is the last one left";
}

// A discard of more commands than are kept, past the latest forgotten one, falls back to the position cut back to.
TEST(TradingHaltStateTest, ADiscardOfMoreThanIsKeptTakesThePositionCutBackTo) {
    TradingHaltState state;
    const auto kept = static_cast<int64_t>(TradingHaltState::recent_members_commands_kept);
    for (int64_t index = 0; index < 3 * kept; ++index) {
        state.note_members_command(2 * index + 1);
    }
    static_cast<void>(state.discard_after(100));
    EXPECT_EQ(state.last_members_command(), 100) << "no command left is later than 100, which is never early";
}

// The halt may be lifted only while trading is halted, an engine leads, and the last member's command before the halt
// is answered.
TEST(TradingHaltStateTest, TheHaltIsLiftedOnlyWithAnEngineLeadingAndEveryCommandBeforeItAnswered) {
    using Lift = TradingHaltState::Lift;
    TradingHaltState state;
    EXPECT_EQ(state.may_lift(true, 0), Lift::NotHalted);
    state.note_members_command(7);
    static_cast<void>(state.note_technical_event(10, true, Reason::RecoveryTooLong));
    // A command after the halt is not needed for the lift.
    state.note_members_command(11);
    EXPECT_EQ(state.may_lift(false, 100), Lift::NoEngineLeads) << "no engine leads, however much is answered";
    EXPECT_EQ(state.may_lift(true, 6), Lift::ReportsOutstanding) << "command 7, before the halt, is not answered yet";
    EXPECT_EQ(state.may_lift(true, 7), Lift::Allowed);
    static_cast<void>(state.note_technical_event(12, false, Reason::LiftedByOperator));
    EXPECT_EQ(state.may_lift(true, 7), Lift::NotHalted);
}

} // namespaces
