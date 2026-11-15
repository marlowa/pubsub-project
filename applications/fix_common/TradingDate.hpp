#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace fix_common {

/**
 * @brief A trading date given as text in the form yyyymmdd, for example "20261025", as the number yyyymmdd.
 *
 * The trading date is given to each run by the procedure that starts it, and no component computes it from its clock
 * (R-0164; docs/venue/start_and_end_of_day.md). The leading sequencer writes it on every record of the day's WAL, taken
 * from the start of day; the matching engine makes its identifiers with a key derived from it
 * (docs/availability/deterministic_matching_engine.md, section 3.3).
 *
 * @param[in] text Eight digits: the year, the month and the day.
 * @return The date, or nothing if @p text is not eight digits naming a day of the Gregorian calendar.
 */
inline std::optional<int32_t> parse_trading_date(std::string_view text) {
    if (text.size() != 8) {
        return std::nullopt;
    }
    int32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10 + (digit - '0');
    }
    const int32_t year = value / 10000;
    const int32_t month = (value / 100) % 100;
    const int32_t day = value % 100;
    if (year < 1 || month < 1 || month > 12 || day < 1) {
        return std::nullopt;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    constexpr int32_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int32_t last_day = month == 2 && leap ? 29 : days_in_month[month - 1];
    if (day > last_day) {
        return std::nullopt;
    }
    return value;
}

/**
 * @brief A trading date written as text in the form yyyy-mm-dd, for example "2026-10-25".
 * @param[in] trading_date The date as the number yyyymmdd.
 */
inline std::string trading_date_text(int32_t trading_date) {
    return fmt::format("{:04}-{:02}-{:02}", trading_date / 10000, (trading_date / 100) % 100, trading_date % 100);
}

} // namespaces
