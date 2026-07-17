#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace fix_codec {

namespace detail {

/**
 * @brief Parses text as a number of type NumberType, returning fallback on failure.
 *
 * Uses std::from_chars: no allocation, no locale, no exceptions. The whole view
 * must be consumed for the parse to be considered successful.
 */
template <typename NumberType> inline NumberType parse_number(std::string_view text, NumberType fallback) {
    NumberType value{};
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return fallback;
    }
    return value;
}

/**
 * @brief Days from the Unix epoch (1970-01-01) to the given proleptic Gregorian date.
 *
 * Howard Hinnant's civil-from-days algorithm, valid for a very wide date range.
 * Avoids timegm() so the conversion is portable and free of global state.
 */
inline int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int>(day_of_era) - 719468;
}

} // namespaces

/**
 * @brief One parsed FIX field: a tag number and a zero-copy view of its value.
 *
 * The value view points directly into the byte buffer that FixMessageReader was
 * constructed over. It is valid only while that buffer is alive and unmodified.
 * The typed accessors convert on demand (hffix's field_value idea) and allocate
 * nothing.
 */
struct FixField {
    int tag{0};
    std::string_view value{};

    [[nodiscard]] bool empty() const {
        return value.empty();
    }

    [[nodiscard]] std::string_view as_string_view() const {
        return value;
    }

    [[nodiscard]] int as_int(int fallback = 0) const {
        return detail::parse_number<int>(value, fallback);
    }

    [[nodiscard]] int64_t as_int64(int64_t fallback = 0) const {
        return detail::parse_number<int64_t>(value, fallback);
    }

    [[nodiscard]] uint32_t as_uint(uint32_t fallback = 0) const {
        return detail::parse_number<uint32_t>(value, fallback);
    }

    [[nodiscard]] char as_char(char fallback = '\0') const {
        return value.size() == 1 ? value.front() : fallback;
    }

    /** @brief FIX boolean: a single 'Y' is true, anything else false. */
    [[nodiscard]] bool as_bool() const {
        return value == "Y";
    }

    /**
     * @brief Parses a fixed-point decimal (e.g. "-123.45") as mantissa and exponent.
     *
     * On success sets @p mantissa and @p exponent such that the value equals
     * mantissa * 10^exponent (exponent <= 0) and returns true. "-123.45" yields
     * mantissa -12345, exponent -2. Scientific notation is rejected. No float is
     * produced, so no precision is lost.
     */
    [[nodiscard]] bool as_decimal(int64_t& mantissa, int& exponent) const {
        if (value.empty()) {
            return false;
        }
        size_t index = 0;
        bool negative = false;
        if (value[index] == '+' || value[index] == '-') {
            negative = value[index] == '-';
            ++index;
        }
        int64_t digits = 0;
        int fractional_digits = 0;
        bool seen_dot = false;
        bool seen_digit = false;
        for (; index < value.size(); ++index) {
            const char character = value[index];
            if (character == '.') {
                if (seen_dot) {
                    return false;
                }
                seen_dot = true;
                continue;
            }
            if (character < '0' || character > '9') {
                return false;
            }
            digits = digits * 10 + (character - '0');
            fractional_digits += seen_dot ? 1 : 0;
            seen_digit = true;
        }
        if (!seen_digit) {
            return false;
        }
        mantissa = negative ? -digits : digits;
        exponent = -fractional_digits;
        return true;
    }

    /**
     * @brief Parses a FIX UTCTimestamp to nanoseconds since the Unix epoch.
     *
     * Accepts "YYYYMMDD-HH:MM:SS" with an optional fractional part of up to nine
     * digits ("YYYYMMDD-HH:MM:SS.sss..."). Returns @p fallback (0 by default) if
     * the text is too short or malformed.
     */
    [[nodiscard]] int64_t as_utc_timestamp_ns(int64_t fallback = 0) const {
        if (value.size() < 17 || value[8] != '-' || value[11] != ':' || value[14] != ':') {
            return fallback;
        }
        const int year = two_or_four_digits(0, 4);
        const int month = two_or_four_digits(4, 2);
        const int day = two_or_four_digits(6, 2);
        const int hour = two_or_four_digits(9, 2);
        const int minute = two_or_four_digits(12, 2);
        const int second = two_or_four_digits(15, 2);
        if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
            return fallback;
        }
        const int64_t days = detail::days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
        const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
        return seconds * 1000000000LL + fractional_nanoseconds();
    }

  private:
    /** @brief Parses @p count digits starting at @p offset, or -1 on any non-digit. */
    [[nodiscard]] int two_or_four_digits(size_t offset, size_t count) const {
        int result = 0;
        for (size_t i = 0; i < count; ++i) {
            const char character = value[offset + i];
            if (character < '0' || character > '9') {
                return -1;
            }
            result = result * 10 + (character - '0');
        }
        return result;
    }

    /** @brief Reads the optional ".fffffffff" fractional part and scales it to nanoseconds. */
    [[nodiscard]] int64_t fractional_nanoseconds() const {
        if (value.size() <= 17 || value[17] != '.') {
            return 0;
        }
        int64_t nanoseconds = 0;
        int scaled_digits = 0;
        for (size_t index = 18; index < value.size() && scaled_digits < 9; ++index) {
            const char character = value[index];
            if (character < '0' || character > '9') {
                break;
            }
            nanoseconds = nanoseconds * 10 + (character - '0');
            ++scaled_digits;
        }
        for (; scaled_digits < 9; ++scaled_digits) {
            nanoseconds *= 10;
        }
        return nanoseconds;
    }
};

} // namespaces
