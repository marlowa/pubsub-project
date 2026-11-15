#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

namespace pubsub_itc_fw {

/**
 * @brief The trading day a small text file records, as its first field, and the check that a component makes of it on
 *        starting (docs/venue/start_and_end_of_day.md, sections 2.1 and 5; BUG-0137).
 *
 * Every file a component writes during a trading day belongs to that day, and the next day starts with none of them.
 * A file left from an earlier day must never be taken for a restart of the present one, so each file records the
 * trading day it was written on, and a component given another day refuses to start, naming the file and the day.
 *
 * The small text files the library keeps (EpochStore, LeasePromiseStore, ComponentStatementStore) begin with the field
 * `trading_day=YYYYMMDD`, followed by a space or a newline and then what the file has always held, so that the day can
 * be read with cat. A file written without the field records no trading day, and is refused as a file from an unknown
 * day.
 */
inline constexpr std::string_view trading_day_field = "trading_day=";

/// The field that begins a file written on @p trading_day, with the space that follows it: "trading_day=20261116 ".
inline std::string trading_day_field_text(int32_t trading_day) {
    return fmt::format("{}{} ", trading_day_field, trading_day);
}

/**
 * @brief Reads the trading day field at the start of @p text, and removes it and the space or newline after it.
 * @return The trading day, or zero, with @p text unchanged, when @p text does not begin with the field.
 */
inline int32_t take_trading_day(std::string_view& text) {
    if (text.substr(0, trading_day_field.size()) != trading_day_field) {
        return 0;
    }
    std::string_view rest = text.substr(trading_day_field.size());
    int32_t day = 0;
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9' && digits < 8) {
        day = day * 10 + (rest[digits] - '0');
        ++digits;
    }
    if (digits != 8) {
        return 0;
    }
    rest = rest.substr(digits);
    if (!rest.empty() && (rest.front() == ' ' || rest.front() == '\n')) {
        rest = rest.substr(1);
    }
    text = rest;
    return day;
}

/**
 * @brief The trading day the text file at @p path records in its first field.
 * @return Nothing when the file is absent or empty, which is how a component starting a new day finds it; zero when it
 *         exists but records no trading day; otherwise the day, as the number yyyymmdd.
 */
inline std::optional<int32_t> recorded_trading_day(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    char buffer[64] = {};
    const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (bytes_read <= 0) {
        return std::nullopt;
    }
    std::string_view text(buffer, static_cast<size_t>(bytes_read));
    return take_trading_day(text);
}

/**
 * @brief Why the file at @p path, which records @p recorded, cannot be used by a component started for @p expected, in
 *        words; or an empty string when it can.
 * @param[in] path The file, as it is to be named in the words.
 * @param[in] recorded What the file records: nothing when it is absent, zero when it records no trading day.
 * @param[in] expected The trading day the component was started for.
 */
inline std::string another_days_file(const std::string& path, std::optional<int32_t> recorded, int32_t expected) {
    if (!recorded.has_value() || *recorded == expected) {
        return {};
    }
    if (*recorded == 0) {
        return fmt::format("{} records no trading day, so it cannot be known to belong to trading day {}", path, expected);
    }
    return fmt::format("{} belongs to trading day {}, not {}", path, *recorded, expected);
}

/// A file a component checks on starting, and the trading day it records: nothing when it is absent, zero when it records
/// none (recorded_trading_day()).
struct FileAndItsDay {
    std::string path;
    std::optional<int32_t> recorded;
};

/**
 * @brief Every file among @p files that a component started for @p trading_day cannot use, in words, separated by
 *        semicolons; an empty string when it can use them all.
 */
inline std::string files_of_another_day(int32_t trading_day, const std::vector<FileAndItsDay>& files) {
    std::string problems;
    for (const FileAndItsDay& file : files) {
        const std::string problem = another_days_file(file.path, file.recorded, trading_day);
        if (!problem.empty()) {
            problems += problems.empty() ? problem : "; " + problem;
        }
    }
    return problems;
}

/// What a component says, after the files it found, when it refuses to start because of them.
inline constexpr std::string_view another_days_files_remedy =
    "every file the venue writes during a trading day belongs to that day; run the end-of-day procedure for the day they belong to, which "
    "archives them, before starting this one (docs/venue/start_and_end_of_day.md)";

} // namespaces
