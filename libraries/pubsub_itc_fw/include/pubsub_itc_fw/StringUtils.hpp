#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/core.h>

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief Provides a collection of static utility functions for string
 * manipulation and conversions.
 *
 * This class includes thread-safe methods for getting system error strings
 * and for converting various types to string representations for logging
 * and debugging.
 */
class StringUtils {
  public:
    /**
     * @brief Checks if a string starts with a given prefix.
     * @param [in] str The string to check.
     * @param [in] prefix The prefix to search for.
     * @returns bool True if the string starts with the prefix, false otherwise.
     */
    static bool starts_with(const std::string& str, const std::string& prefix);

    /**
     * @brief Checks if a string starts with a given string_view prefix.
     * @param [in] str The string to check.
     * @param [in] prefix The string_view prefix to search for.
     * @returns bool True if the string starts with the prefix, false otherwise.
     */
    static bool starts_with(const std::string& str, std::string_view prefix);

    /**
     * @brief Checks if a string starts with a given C-style string prefix.
     * @param [in] str The string to check.
     * @param [in] prefix The C-style string prefix to search for.
     * @returns bool True if the string starts with the prefix, false otherwise.
     */
    static bool starts_with(const std::string& str, const char* prefix);

    /**
     * @brief Returns a thread-safe string representation of a system error.
     * @param [in] errnum The system error number (errno).
     * @returns std::string A string containing the error message.
     */
    static std::string get_error_string(int errnum);

    /**
     * @brief Thread-safe equivalent of strerror(errno).
     *
     * Captures the current thread's errno value and returns the corresponding
     * error string using the thread-safe get_error_string(int) implementation.
     */
    static std::string get_errno_string();

    /**
     * @brief Extracts the file name (leaf name) from a full file path.
     * @param [in] filename The full path to the file.
     * @returns std::string The leaf name of the file.
     */
    static std::string leafname(const std::string& filename);

    /**
     * @brief Converts a std::chrono::duration to a string representation.
     * @param [in] duration The duration to convert.
     * @returns std::string A string representation of the duration.
     */
    template <typename Rep, typename Period> static std::string chronoDurationAsString(const std::chrono::duration<Rep, Period>& duration) {
        return fmt::format("{}", duration);
    }

    /**
     * @brief Converts a std::chrono::time_point to a string representation.
     * @param [in] timepoint The time_point to convert.
     * @returns std::string A string representation of the time_point.
     */
    template <typename Clock, typename Duration> static std::string chronoTimepointAsString(const std::chrono::time_point<Clock, Duration>& timepoint) {
        return fmt::format("{}", timepoint);
    }

    static std::string nanoseconds_since_epoch_as_datetime(int64_t total_ns) {

    // Break into seconds and nanosecond fraction
    const auto seconds = std::chrono::seconds(total_ns / 1'000'000'000LL);
    const auto nanoseconds = total_ns % 1'000'000'000LL;

    // Convert seconds to std::time_t for calendar conversion
    const std::time_t epoch_seconds = seconds.count();
    std::tm tm_buf{};

    // Use thread-safe gmtime_r
    gmtime_r(&epoch_seconds, &tm_buf);

    // Format: YYYY-MM-DDTHH:MM:SS.NNNNNNNNN
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:09}",
                       tm_buf.tm_year + 1900,
                       tm_buf.tm_mon + 1,
                       tm_buf.tm_mday,
                       tm_buf.tm_hour,
                       tm_buf.tm_min,
                       tm_buf.tm_sec,
                       nanoseconds);
    }

    /**
     * @brief Returns a hex-dump string of the given memory region.
     * @param[in] data Pointer to the start of the region to dump.
     * @param[in] len  Number of bytes to dump.
     * @returns std::string Space-separated two-digit hex values, e.g. "de ad be ef".
     */
    static std::string hex_dump(const void* data, size_t len);
};

} // namespace pubsub_itc_fw
