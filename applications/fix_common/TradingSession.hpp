#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief What the venue is doing, as the leading sequencer last told a gateway in OrderAcceptance: whether the trading
 *        day has started, the trading day, the phase, and whether trading is halted (docs/venue/trading_day_plan.md,
 *        section 4).
 */
struct TradingSessionState {
    bool day_started{false};                                                            ///< whether the WAL holds a start of day
    int32_t trading_day{0};                                                             ///< the trading day, as the number yyyymmdd; 0 before it starts
    pubsub_itc_fw_app::TradingPhase phase{pubsub_itc_fw_app::TradingPhase::StartOfDay}; ///< the phase; says nothing before the day starts
    bool halted{false};                                                                 ///< whether trading is halted

    [[nodiscard]] bool operator==(const TradingSessionState& other) const {
        return day_started == other.day_started && trading_day == other.trading_day && (!day_started || phase == other.phase) && halted == other.halted;
    }
};

/**
 * @brief Why the venue refuses every member's order and cancel in the phase it is in, or nothing when it accepts them.
 *
 * The venue accepts members' orders and cancels in pre-open and open only. The sequencer refuses by this rule whatever a
 * gateway believed, and both gateways refuse by it before a command reaches the sequencer, so that all three refuse the
 * same commands for the same reasons (R-0170, R-0171).
 */
inline std::optional<pubsub_itc_fw_app::CommandRefusedReason> refusal_by_phase(bool day_started, pubsub_itc_fw_app::TradingPhase phase) {
    if (!day_started) {
        return pubsub_itc_fw_app::CommandRefusedReason::TradingDayNotStarted;
    }
    switch (phase) {
        case pubsub_itc_fw_app::TradingPhase::StartOfDay:
            return pubsub_itc_fw_app::CommandRefusedReason::MarketNotOpen;
        case pubsub_itc_fw_app::TradingPhase::Closed:
            return pubsub_itc_fw_app::CommandRefusedReason::MarketClosed;
        case pubsub_itc_fw_app::TradingPhase::EndOfDayBegin:
        case pubsub_itc_fw_app::TradingPhase::EndOfDayEnd:
            return pubsub_itc_fw_app::CommandRefusedReason::TradingDayEnded;
        case pubsub_itc_fw_app::TradingPhase::PreOpen:
        case pubsub_itc_fw_app::TradingPhase::Open:
            break;
    }
    return std::nullopt;
}

/// The state of the trading session as FIX states it in TradSesStatus (340), and as the binary gateway's
/// TradingSessionStatus states it, with the same numbers.
enum class SessionStatus : char {
    Halted = '1',
    Open = '2',
    Closed = '3',
    PreOpen = '4',
};

/**
 * @brief The state of the trading session for a member, from what the venue is doing.
 *
 * Pre-open and open are reported as themselves, or as halted while trading is halted. Every other phase, and the time
 * before the trading day starts, is reported as closed, as real venues report their own preparation before trading and
 * their work after it: the start of day and the end of day are the venue's, not the members' (trading_day_plan.md, section
 * 4.2).
 */
inline SessionStatus session_status(const TradingSessionState& state) {
    if (!state.day_started) {
        return SessionStatus::Closed;
    }
    switch (state.phase) {
        case pubsub_itc_fw_app::TradingPhase::PreOpen:
            return state.halted ? SessionStatus::Halted : SessionStatus::PreOpen;
        case pubsub_itc_fw_app::TradingPhase::Open:
            return state.halted ? SessionStatus::Halted : SessionStatus::Open;
        case pubsub_itc_fw_app::TradingPhase::StartOfDay:
        case pubsub_itc_fw_app::TradingPhase::Closed:
        case pubsub_itc_fw_app::TradingPhase::EndOfDayBegin:
        case pubsub_itc_fw_app::TradingPhase::EndOfDayEnd:
            break;
    }
    return SessionStatus::Closed;
}

/// The phase in words, for a member and for a log reader.
inline const char* phase_in_words(pubsub_itc_fw_app::TradingPhase phase) {
    switch (phase) {
        case pubsub_itc_fw_app::TradingPhase::StartOfDay:
            return "the start of day";
        case pubsub_itc_fw_app::TradingPhase::PreOpen:
            return "pre-open";
        case pubsub_itc_fw_app::TradingPhase::Open:
            return "open";
        case pubsub_itc_fw_app::TradingPhase::Closed:
            return "closed";
        case pubsub_itc_fw_app::TradingPhase::EndOfDayBegin:
        case pubsub_itc_fw_app::TradingPhase::EndOfDayEnd:
            break;
    }
    return "the end of day";
}

/**
 * @brief What the venue is doing, in words, as both gateways tell a member in the Text of a trading session status:
 *        for example "trading day 20261116: open" or "trading day 20261116: pre-open, trading halted".
 */
inline std::string session_status_text(const TradingSessionState& state) {
    if (!state.day_started) {
        return "the trading day has not started";
    }
    return fmt::format("trading day {}: {}{}", state.trading_day, phase_in_words(state.phase), state.halted ? ", trading halted" : "");
}

} // namespaces
