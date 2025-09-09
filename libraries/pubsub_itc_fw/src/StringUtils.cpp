#include <string>
#include <string_view>
#include <array>

#include <fmt/format.h>

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
    std::string error_message;

    // Use GNU-specific strerror_r (returns char*).
    // It attempts to write into error_buffer, but might return a pointer
    // to an internal static buffer if error_buffer is too small or for certain errnums.
    char* strerror_result_ptr = strerror_r(errnum, error_buffer.data(), error_buffer.size());

    if (strerror_result_ptr != nullptr) {
        // GNU strerror_r returns a pointer to the string.
        // We check if it points to our buffer, or an internal one.
        // If it points to our buffer, it's null-terminated by strerror_r.
        // If it points to an internal one, we use that.
        error_message = strerror_result_ptr;

        // Optionally, you might check for ERANGE if you strictly want to know
        // if your provided buffer was too small, but the result ptr will still be valid.
        // if (errno == ERANGE) {
        //     error_message = fmt::format("Error (buffer too small, truncated): {}", error_message);
        // }
    } else {
        // If strerror_r_ptr is null, it indicates a failure within strerror_r itself (rare).
        // Fallback to a generic message.
        error_message = fmt::format("Failed to get strerror_r message for error {}. strerror_r returned nullptr.", errnum);
    }

    return error_message;
}

std::string StringUtils::leafname(const std::string& filename) {
    // Find the last slash or backslash to get the leaf name.
    const auto last_slash = filename.find_last_of("/\\");
    if (std::string::npos != last_slash) {
        return filename.substr(last_slash + 1);
    }
    return filename;
}

} // namespace pubsub_itc_fw
