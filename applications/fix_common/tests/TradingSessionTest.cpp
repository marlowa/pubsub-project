// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>

#include <gtest/gtest.h>

#include <leader_follower.hpp>

#include "TradingSession.hpp"

namespace fix_common::tests {

namespace {

using Phase = pubsub_itc_fw_app::TradingPhase;
using Reason = pubsub_itc_fw_app::CommandRefusedReason;

constexpr int32_t today = 20261116;

TradingSessionState in(Phase phase, bool halted = false) {
    return TradingSessionState{true, today, phase, halted};
}

} // namespaces

// The venue accepts members' orders and cancels in pre-open and open only; every other phase refuses them with its own
// reason, which both gateways and the sequencer give.
TEST(TradingSessionTest, OnlyPreOpenAndOpenAllowMembersCommands) {
    EXPECT_EQ(refusal_by_phase(false, Phase::StartOfDay), std::optional<Reason>(Reason::TradingDayNotStarted));
    EXPECT_EQ(refusal_by_phase(false, Phase::Open), std::optional<Reason>(Reason::TradingDayNotStarted)) << "the phase says nothing before the day starts";
    EXPECT_EQ(refusal_by_phase(true, Phase::StartOfDay), std::optional<Reason>(Reason::MarketNotOpen));
    EXPECT_EQ(refusal_by_phase(true, Phase::PreOpen), std::nullopt);
    EXPECT_EQ(refusal_by_phase(true, Phase::Open), std::nullopt);
    EXPECT_EQ(refusal_by_phase(true, Phase::Closed), std::optional<Reason>(Reason::MarketClosed));
    EXPECT_EQ(refusal_by_phase(true, Phase::EndOfDayBegin), std::optional<Reason>(Reason::TradingDayEnded));
    EXPECT_EQ(refusal_by_phase(true, Phase::EndOfDayEnd), std::optional<Reason>(Reason::TradingDayEnded)) << "a member sees no difference between the two";
}

// Members are told pre-open and open as themselves, or as halted while trading is halted; the venue's own preparation and
// its work after trading are reported as closed, with the FIX TradSesStatus (340) numbers.
TEST(TradingSessionTest, TheSessionStatusIsWhatAMemberCanDo) {
    EXPECT_EQ(session_status(TradingSessionState{}), SessionStatus::Closed);
    EXPECT_EQ(session_status(in(Phase::StartOfDay)), SessionStatus::Closed);
    EXPECT_EQ(session_status(in(Phase::PreOpen)), SessionStatus::PreOpen);
    EXPECT_EQ(session_status(in(Phase::Open)), SessionStatus::Open);
    EXPECT_EQ(session_status(in(Phase::Open, true)), SessionStatus::Halted);
    EXPECT_EQ(session_status(in(Phase::PreOpen, true)), SessionStatus::Halted);
    EXPECT_EQ(session_status(in(Phase::Closed, true)), SessionStatus::Closed) << "once closed, a halt changes nothing a member can do";
    EXPECT_EQ(session_status(in(Phase::EndOfDayBegin)), SessionStatus::Closed);
    EXPECT_EQ(session_status(in(Phase::EndOfDayEnd)), SessionStatus::Closed);
    EXPECT_EQ(static_cast<char>(SessionStatus::Halted), '1');
    EXPECT_EQ(static_cast<char>(SessionStatus::Open), '2');
    EXPECT_EQ(static_cast<char>(SessionStatus::Closed), '3');
    EXPECT_EQ(static_cast<char>(SessionStatus::PreOpen), '4');
}

TEST(TradingSessionTest, TheTextNamesTheDayThePhaseAndAHalt) {
    EXPECT_EQ(session_status_text(TradingSessionState{}), "the trading day has not started");
    EXPECT_EQ(session_status_text(in(Phase::Open)), "trading day 20261116: open");
    EXPECT_EQ(session_status_text(in(Phase::PreOpen, true)), "trading day 20261116: pre-open, trading halted");
    EXPECT_EQ(session_status_text(in(Phase::EndOfDayBegin)), "trading day 20261116: the end of day");
    EXPECT_EQ(session_status_text(in(Phase::EndOfDayEnd)), "trading day 20261116: the end of day");
}

// A change is noticed when the day, the phase or the halt changes; before the day starts the phase is not compared,
// because it says nothing.
TEST(TradingSessionTest, AChangeIsAChangeOfDayPhaseOrHalt) {
    EXPECT_EQ(in(Phase::Open), in(Phase::Open));
    EXPECT_FALSE(in(Phase::Open) == in(Phase::Closed));
    EXPECT_FALSE(in(Phase::Open) == in(Phase::Open, true));
    EXPECT_FALSE(in(Phase::Open) == (TradingSessionState{true, today + 1, Phase::Open, false}));
    EXPECT_EQ((TradingSessionState{false, 0, Phase::StartOfDay, false}), (TradingSessionState{false, 0, Phase::Open, false}));
}

} // namespaces
