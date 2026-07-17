#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace fix_codec {

/**
 * @brief Computes the FIX checksum of a byte range.
 *
 * The FIX checksum is the sum of every byte value modulo 256. The range passed
 * in must cover every byte from the start of the message (tag 8) up to and
 * including the SOH that terminates the last body field, i.e. everything before
 * the "10=" checksum field itself.
 *
 * This performs no allocation.
 */
[[nodiscard]] inline unsigned int compute_checksum(std::string_view message_bytes) {
    unsigned int sum = 0;
    for (const unsigned char byte : message_bytes) {
        sum += byte;
    }
    return sum % 256U;
}

/**
 * @brief Validates a received FIX checksum field against a computed checksum.
 *
 * @param[in] message_bytes     Bytes from tag 8 up to (not including) the "10=" field.
 * @param[in] received_checksum The value of the checksum field (tag 10), which must be
 *                              exactly three ASCII digits.
 * @return True if the received checksum is well formed and equals the computed value.
 *
 * Unlike a formatting-based comparison, this parses the three received digits into
 * an integer and compares numerically, so it performs no allocation on any path.
 */
[[nodiscard]] inline bool checksum_matches(std::string_view message_bytes, std::string_view received_checksum) {
    if (received_checksum.size() != 3) {
        return false;
    }
    unsigned int received = 0;
    const char* const first = received_checksum.data();
    const char* const last = received_checksum.data() + received_checksum.size();
    const std::from_chars_result result = std::from_chars(first, last, received);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    return compute_checksum(message_bytes) == received;
}

} // namespaces
