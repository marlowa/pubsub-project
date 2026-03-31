#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <cerrno>   // for errno
#include <stdexcept>
#include <string>
#include <fmt/core.h>

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Custom exception class for all framework errors.
 *
 * This class is designed to be thread-safe when dealing with system errors
 * by using the StringUtils helper function.
 */
class PubSubItcException : public std::runtime_error
{
public:
    explicit PubSubItcException(const std::string& errorText)
        : std::runtime_error(errorText)
    {
    }

    /**
     * @brief Throws an exception with details about the last system error (errno).
     * @param errorText A user-provided description of the error.
     * @param filename The name of the file where the error occurred.
     * @param lineNumber The line number where the error occurred.
     */
    static void throwErrno(const std::string& errorText, const char* filename, int lineNumber)
    {
        // Use the thread-safe StringUtils helper function to get the errno string.
        throw PubSubItcException(
            fmt::format("{}. Error(errno) = {} ({}). Thrown from {}:{}.",
                errorText,
                errno,
                StringUtils::get_errno_string(), // Uses strerror_r internally
                StringUtils::leafname(filename),
                lineNumber
            )
        );
    }
};

} // namespace pubsub_itc_fw
