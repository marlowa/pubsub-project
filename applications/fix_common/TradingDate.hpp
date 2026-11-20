#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <pubsub_itc_fw/ComponentArguments.hpp>

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
    return pubsub_itc_fw::parse_trading_day(text);
}

/**
 * @brief A trading date written as text in the form yyyy-mm-dd, for example "2026-10-25".
 * @param[in] trading_date The date as the number yyyymmdd.
 */
inline std::string trading_date_text(int32_t trading_date) {
    return fmt::format("{:04}-{:02}-{:02}", trading_date / 10'000, (trading_date / 100) % 100, trading_date % 100);
}

} // namespaces
