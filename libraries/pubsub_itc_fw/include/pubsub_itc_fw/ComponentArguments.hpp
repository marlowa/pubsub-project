#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace pubsub_itc_fw {

/**
 * @brief A trading day given as text in the form yyyymmdd, for example "20261025", as the number yyyymmdd.
 *
 * The trading day is given to each component by the procedure that starts it, and no component computes it from its
 * clock (R-0164; docs/venue/start_and_end_of_day.md).
 *
 * @param[in] text Eight digits: the year, the month and the day.
 * @return The day, or nothing if @p text is not eight digits naming a day of the Gregorian calendar.
 */
inline std::optional<int32_t> parse_trading_day(std::string_view text) {
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

/// What every component of the venue is given on its command line.
struct ComponentArguments {
    std::string log_file;    ///< the Quill log to write
    std::string config_file; ///< the component's TOML configuration
    int32_t trading_day{0};  ///< the trading day it is started for, as the number yyyymmdd
};

/**
 * @brief Reads a component's command line: `<program> <logfile> <config.toml> --trading-day YYYYMMDD`.
 *
 * Every component is given the trading day, and holds every file it writes during the day to it
 * (docs/venue/start_and_end_of_day.md, decision 1).
 *
 * @param[in] argc The number of arguments, as main() is given it.
 * @param[in] argv The arguments, as main() is given them.
 * @param[in] program The component's name, for the usage message.
 * @param[out] errors Where to write what is wrong, with the usage, when the command line is not as above.
 * @return The arguments, or nothing when the command line is not as above.
 */
inline std::optional<ComponentArguments> parse_component_arguments(int argc, const char* const* argv, std::string_view program, std::ostream& errors) {
    const std::string usage = std::string("Usage: ") + std::string(program) + " <logfile> <config.toml> --trading-day YYYYMMDD\n";
    if (argc != 5 || std::string_view(argv[3]) != "--trading-day") {
        errors << usage;
        return std::nullopt;
    }
    const std::optional<int32_t> trading_day = parse_trading_day(argv[4]);
    if (!trading_day.has_value()) {
        errors << program << ": --trading-day " << argv[4] << " is not a date written as YYYYMMDD\n" << usage;
        return std::nullopt;
    }
    return ComponentArguments{argv[1], argv[2], *trading_day};
}

} // namespaces
