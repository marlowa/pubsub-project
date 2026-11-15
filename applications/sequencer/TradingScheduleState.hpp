#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <leader_follower.hpp>

namespace sequencer {

/// One phase of the day's schedule, and the earliest moment the venue may enter it.
struct ScheduledPhase {
    pubsub_itc_fw_app::TradingPhase phase{pubsub_itc_fw_app::TradingPhase::PreOpen}; ///< the phase to enter
    int64_t earliest_utc_ns{0}; ///< the earliest moment to enter it, in nanoseconds since 1970 UTC; 0 means as soon as its conditions hold

    [[nodiscard]] bool operator==(const ScheduledPhase& other) const {
        return phase == other.phase && earliest_utc_ns == other.earliest_utc_ns;
    }
};

/// A moment as a schedule gives it, nanoseconds since 1970 UTC, in words to the second: for example "2026-11-16 07:50:00
/// UTC".
inline std::string utc_time_text(int64_t utc_ns) {
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> moment{
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::nanoseconds{utc_ns})};
    return fmt::format("{:%Y-%m-%d %H:%M:%S} UTC", moment);
}

/**
 * @brief Why @p entries is not a schedule the venue can follow from @p current, the phase it is in, or an empty string when
 *        it is one (docs/venue/trading_day_plan.md, section 3.2).
 *
 * A schedule names consecutive phases in order, starting from the phase after @p current, so that it never names a phase
 * the venue has entered or passed. It may stop at any phase: the phase after its last entry, and every one after that, is
 * then entered only when an operator asks. A time is never negative, and the times that are not zero never decrease from
 * one entry to the next. Zero means "as soon as the phase's conditions hold", and may follow any time.
 */
inline std::string schedule_problem(pubsub_itc_fw_app::TradingPhase current, const std::vector<ScheduledPhase>& entries) {
    int expected = static_cast<int>(current) + 1;
    int64_t latest_time = 0;
    for (size_t index = 0; index < entries.size(); ++index) {
        const ScheduledPhase& entry = entries[index];
        if (static_cast<int>(entry.phase) != expected) {
            if (index == 0) {
                return fmt::format("its first entry is phase {}, but the venue is in phase {}, and a schedule starts at the phase after that one; a schedule "
                                   "never names a phase the venue has entered or passed",
                                   pubsub_itc_fw_app::to_string(entry.phase), pubsub_itc_fw_app::to_string(current));
            }
            return fmt::format("entry {} is phase {}, where the phase after {} was expected; the entries name consecutive phases in order", index + 1,
                               pubsub_itc_fw_app::to_string(entry.phase), pubsub_itc_fw_app::to_string(entries[index - 1].phase));
        }
        if (entry.earliest_utc_ns < 0) {
            return fmt::format("entry {}, phase {}, has a negative time", index + 1, pubsub_itc_fw_app::to_string(entry.phase));
        }
        if (entry.earliest_utc_ns != 0) {
            if (entry.earliest_utc_ns < latest_time) {
                return fmt::format("entry {}, phase {}, is at {}, earlier than the {} given to a phase before it", index + 1,
                                   pubsub_itc_fw_app::to_string(entry.phase), utc_time_text(entry.earliest_utc_ns), utc_time_text(latest_time));
            }
            latest_time = entry.earliest_utc_ns;
        }
        ++expected;
    }
    return {};
}

/// The schedule in words, for the Quill log and an operator: for example "PreOpen at 2026-11-16 07:50:00 UTC, Open as
/// soon as its conditions hold".
inline std::string schedule_in_words(const std::vector<ScheduledPhase>& entries) {
    if (entries.empty()) {
        return "no scheduled phase: every phase is entered when an operator asks";
    }
    std::string words;
    for (const ScheduledPhase& entry : entries) {
        if (!words.empty()) {
            words += ", ";
        }
        words += entry.earliest_utc_ns == 0 ? fmt::format("{} as soon as its conditions hold", pubsub_itc_fw_app::to_string(entry.phase))
                                            : fmt::format("{} at {}", pubsub_itc_fw_app::to_string(entry.phase), utc_time_text(entry.earliest_utc_ns));
    }
    return words;
}

/**
 * @brief The day's schedule, as the TradingSchedule records in the sequencer's WAL say (docs/venue/trading_day_plan.md,
 *        section 3).
 *
 * The leading sequencer writes a TradingSchedule straight after the start of day, and again whenever an operator replaces
 * the times that remain. The latest one is the schedule. Every sequencer keeps it from the WAL in the same three ways as it
 * keeps the phase: as the replicated WAL reads the whole WAL when it opens, as this instance writes records as leader, and
 * as it writes its leader's records as follower. A newly promoted leader therefore knows when the next phase is due.
 *
 * One thread, the sequencer's, calls every member function.
 */
class TradingScheduleState {
  public:
    /// One TradingSchedule record in the WAL.
    struct ScheduleRecord {
        int64_t seq_no{0};                   ///< its position
        int32_t trading_day{0};              ///< the trading day it states, as the number yyyymmdd
        std::vector<ScheduledPhase> entries; ///< the phases it schedules, in order
        std::string operator_name;           ///< who supplied it
    };

    /// A TradingSchedule at @p record.seq_no, which is later than every record noted before it.
    void note_schedule(ScheduleRecord record) {
        schedules_.push_back(std::move(record));
    }

    /**
     * @brief Forgets every TradingSchedule after @p seq_no, which the WAL no longer holds.
     * @return Whether that changes the schedule.
     */
    bool discard_after(int64_t seq_no) {
        bool changed = false;
        while (!schedules_.empty() && schedules_.back().seq_no > seq_no) {
            schedules_.pop_back();
            changed = true;
        }
        return changed;
    }

    /// Whether the WAL holds a schedule.
    [[nodiscard]] bool recorded() const {
        return !schedules_.empty();
    }

    /// The latest TradingSchedule in the WAL, or nullptr when it holds none.
    [[nodiscard]] const ScheduleRecord* latest() const {
        return schedules_.empty() ? nullptr : &schedules_.back();
    }

    /// The entry of the latest schedule for @p phase, if it schedules that phase.
    [[nodiscard]] std::optional<ScheduledPhase> entry_for(pubsub_itc_fw_app::TradingPhase phase) const {
        if (schedules_.empty()) {
            return std::nullopt;
        }
        for (const ScheduledPhase& entry : schedules_.back().entries) {
            if (entry.phase == phase) {
                return entry;
            }
        }
        return std::nullopt;
    }

  private:
    // Every TradingSchedule in the WAL, oldest first. A day has few.
    std::vector<ScheduleRecord> schedules_;
};

} // namespaces
