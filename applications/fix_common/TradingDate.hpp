#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace fix_common {

/**
 * @brief The trading date a wall time falls on: its calendar date in UTC, as the number yyyymmdd.
 *
 * The leading sequencer writes the trading date on every command it puts in the WAL. It is the date
 * of the first record in the day's WAL, so it stays the same for the whole day even if the day runs
 * past midnight. The matching engine makes its identifiers with a key derived from it
 * (docs/availability/deterministic_matching_engine.md, section 3.3).
 *
 * @param[in] wall_time_ns Nanoseconds since 1970-01-01T00:00:00 UTC; may be negative.
 * @return The date, for example 20261025 for 25 October 2026.
 */
inline int32_t trading_date_of(int64_t wall_time_ns) {
    constexpr int64_t nanoseconds_per_day = 86'400'000'000'000;
    int64_t days = wall_time_ns / nanoseconds_per_day;
    if (wall_time_ns % nanoseconds_per_day < 0) {
        --days;
    }
    // The civil date of a count of days since 1970-01-01, by Howard Hinnant's civil_from_days, which counts in eras of
    // 400 years starting on 1 March so that the leap day falls at the end of each year it counts.
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t day_of_era = days - era * 146097;
    const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_from_march = (5 * day_of_year + 2) / 153;
    const int64_t day = day_of_year - (153 * month_from_march + 2) / 5 + 1;
    const int64_t month = month_from_march < 10 ? month_from_march + 3 : month_from_march - 9;
    const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    return static_cast<int32_t>(year * 10000 + month * 100 + day);
}

/**
 * @brief A trading date written as text in the form yyyy-mm-dd, for example "2026-10-25".
 * @param[in] trading_date The date as the number yyyymmdd.
 */
inline std::string trading_date_text(int32_t trading_date) {
    return fmt::format("{:04}-{:02}-{:02}", trading_date / 10000, (trading_date / 100) % 100, trading_date % 100);
}

} // namespaces
