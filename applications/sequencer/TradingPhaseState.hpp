#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <vector>

#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief The trading day and the phase the venue is in, as the TechnicalEvent records in the sequencer's WAL say
 *        (docs/venue/trading_day_plan.md).
 *
 * The first record of a trading day's WAL is the TechnicalEvent that enters the start of day, and it fixes the trading
 * day for every later record. Until the WAL holds it, the trading day has not started: the sequencer accepts no member's
 * command and writes nothing of its own except that record.
 *
 * Every TechnicalEvent states the phase the venue is in once it is applied, so the latest one gives the phase. The
 * sequencer keeps this from the WAL in the same three ways as it keeps whether trading is halted: as the replicated WAL
 * reads the whole WAL when it opens, as this instance writes records as leader, and as it writes its leader's records as
 * follower. When records after a position are discarded, the TechnicalEvents among them are forgotten, and the phase is
 * what the latest one left says.
 *
 * One thread, the sequencer's, calls every member function.
 */
class TradingPhaseState {
  public:
    /// One TechnicalEvent record in the WAL.
    struct PhaseRecord {
        int64_t seq_no{0};                                                                  ///< its position
        pubsub_itc_fw_app::TradingPhase phase{pubsub_itc_fw_app::TradingPhase::StartOfDay}; ///< the phase the venue is in once it is applied
        int32_t trading_day{0};                                                             ///< the trading day it states, as the number yyyymmdd
    };

    /**
     * @brief A TechnicalEvent at @p seq_no, which is later than every record noted before it.
     * @return Whether it changes the phase or the trading day.
     */
    bool note_technical_event(int64_t seq_no, pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day) {
        const std::optional<PhaseRecord> before = latest();
        events_.push_back(PhaseRecord{seq_no, phase, trading_day});
        return !before.has_value() || before->phase != phase || before->trading_day != trading_day;
    }

    /**
     * @brief Forgets every TechnicalEvent after @p seq_no, which the WAL no longer holds.
     * @return Whether that changes the phase or the trading day.
     */
    bool discard_after(int64_t seq_no) {
        const std::optional<PhaseRecord> before = latest();
        while (!events_.empty() && events_.back().seq_no > seq_no) {
            events_.pop_back();
        }
        const std::optional<PhaseRecord> after = latest();
        if (before.has_value() != after.has_value()) {
            return true;
        }
        return before.has_value() && (before->phase != after->phase || before->trading_day != after->trading_day);
    }

    /// Whether the WAL holds a start of day: the trading day has started.
    [[nodiscard]] bool started() const {
        return !events_.empty();
    }

    /// The latest TechnicalEvent in the WAL, if it holds one.
    [[nodiscard]] std::optional<PhaseRecord> latest() const {
        if (events_.empty()) {
            return std::nullopt;
        }
        return events_.back();
    }

    /// The trading day, as the number yyyymmdd; zero while the day has not started.
    [[nodiscard]] int32_t trading_day() const {
        return events_.empty() ? 0 : events_.back().trading_day;
    }

    /// The position of the TechnicalEvent that entered @p phase, if the WAL holds one: the earliest, since a phase is
    /// entered once.
    [[nodiscard]] std::optional<int64_t> entered_at(pubsub_itc_fw_app::TradingPhase phase) const {
        for (const PhaseRecord& event : events_) {
            if (event.phase == phase) {
                return event.seq_no;
            }
        }
        return std::nullopt;
    }

  private:
    // Every TechnicalEvent in the WAL, oldest first. A day has few.
    std::vector<PhaseRecord> events_;
};

} // namespaces
