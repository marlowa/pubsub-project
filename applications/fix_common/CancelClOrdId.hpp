#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string_view>

#include "FixOrderLimits.hpp"

namespace cancel_cl_ord_id {

// Long enough for any prefix a gateway uses plus two decimal integers and the separators,
// and bounded by the shared ClOrdID maximum so a generated id can never exceed what the
// matching engine's book key holds.
inline constexpr size_t max_length = fix_order_limits::max_cl_ord_id_length;

/**
 * @brief Formats a gateway-generated cancel's ClOrdID into a caller-owned buffer.
 *
 * @param[out] buffer      Storage for the id; the returned view points into it.
 * @param[in]  prefix      Gateway-specific literal, e.g. "GW-CXL-" or "BGW-CXL-".
 * @param[in]  session_id  The dead session's connection id.
 * @param[in]  counter     Per-session counter making the id unique.
 * @return A view of the formatted id, or an empty view if it would not fit.
 *
 * Exists because the obvious spelling allocates three times per cancel:
 *
 *     const std::string id = prefix + std::to_string(session) + "-" + std::to_string(n);
 *
 * two from std::to_string and one from the concatenation. That is on the cancel-drain hot
 * path, where a disconnecting client can generate thousands of cancels in a burst, and it
 * showed up as malloc traffic in the gateway profile. std::to_chars writes decimal digits
 * straight into the buffer with no allocation and no locale.
 *
 * The buffer is the caller's so this can be a stack array reused across a drain loop.
 */
[[nodiscard]] inline std::string_view format(std::array<char, max_length>& buffer, std::string_view prefix, int32_t session_id, int counter) {
    char* const begin = buffer.data();
    char* const end = begin + buffer.size();
    char* cursor = begin;

    if (prefix.size() >= buffer.size()) {
        return {};
    }
    std::memcpy(cursor, prefix.data(), prefix.size());
    cursor += prefix.size();

    std::to_chars_result written = std::to_chars(cursor, end, session_id);
    if (written.ec != std::errc{}) {
        return {};
    }
    cursor = written.ptr;

    if (cursor >= end) {
        return {};
    }
    *cursor++ = '-';

    written = std::to_chars(cursor, end, counter);
    if (written.ec != std::errc{}) {
        return {};
    }
    cursor = written.ptr;

    return {begin, static_cast<size_t>(cursor - begin)};
}

} // namespaces
