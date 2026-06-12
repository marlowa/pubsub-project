// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <array>
#include <string>
#include <string_view>

#include <cstring> // for strerror_r

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

bool StringUtils::starts_with(const std::string& str, const std::string& prefix) {
    // Check if the string is at least as long as the prefix.
    // Then, use std::string::compare to check the prefix.
    // compare(pos, len, str) compares a substring of length 'len' starting at 'pos'
    // with 'str'. If they are equal, it returns 0.
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool StringUtils::starts_with(const std::string& str, std::string_view prefix) {
    // Similar logic, but uses string_view's size and data for the compare method.
    // string_view is very efficient as it avoids unnecessary memory allocations.
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0;
}

bool StringUtils::starts_with(const std::string& str, const char* prefix) {
    // Convert the C-style string to a string_view for efficient comparison.
    // This avoids creating a temporary std::string object.
    return StringUtils::starts_with(str, std::string_view(prefix));
}

std::string StringUtils::get_error_string(int errnum) {
    // Define a reasonable buffer size for strerror_r messages.
    // This is typically sufficient for most system error strings.
    constexpr size_t ErrorStringBufferLength = 256;
    std::array<char, ErrorStringBufferLength> error_buffer{};

    // Use GNU-specific strerror_r (returns char*).
    // It attempts to write into error_buffer, but might return a pointer
    // to an internal static buffer if error_buffer is too small or for certain errnums.
    // The pointer is guaranteed to never be null.
    char* strerror_result_ptr = strerror_r(errnum, error_buffer.data(), error_buffer.size());

    // GNU strerror_r returns a pointer to the string.
    std::string error_message = strerror_result_ptr;
    return error_message;
}

std::string StringUtils::get_errno_string() {
    // Capture errno immediately -- it is thread-local but can change
    // between calls if any library function runs.
    const int err = errno;
    return get_error_string(err);
}

std::string StringUtils::leafname(const std::string& filename) {
    // Find the last slash or backslash to get the leaf name.
    const auto last_slash = filename.find_last_of("/\\");
    if (std::string::npos != last_slash) {
        return filename.substr(last_slash + 1);
    }
    return filename;
}

std::string StringUtils::hex_dump(const void* data, size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);

    constexpr size_t bytes_per_line = 16;

    std::string out;
    out.reserve(len * 4); // rough heuristic

    // Header
    out += std::to_string(len);
    out += " bytes\n";

    for (size_t i = 0; i < len; i += bytes_per_line) {
        std::array<char, 32> buf{};

        // Offset
        (void)std::snprintf(buf.data(), buf.size(), "%04zx: ", i);
        out += buf.data();

        // Hex column
        for (size_t j = 0; j < bytes_per_line; ++j) {
            if (i + j < len) {
                (void)std::snprintf(buf.data(), buf.size(), "%02X ", bytes[i + j]);
                out += buf.data();
            } else {
                out += "   ";
            }
        }

        out += " |";

        // ASCII column (strict)
        for (size_t j = 0; j < bytes_per_line; ++j) {
            if (i + j < len) {
                const uint8_t c = bytes[i + j];
                if (c >= 32 && c <= 126) {
                    out += static_cast<char>(c);
                } else {
                    out += '.';
                }
            } else {
                out += ' ';
            }
        }

        out += "|\n";
    }

    return out;
}

} // namespaces
