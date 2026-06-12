#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno> // for errno
#include <fmt/core.h>
#include <stdexcept>
#include <string>

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Custom exception class for all framework errors.
 *
 * This class is designed to be thread-safe when dealing with system errors
 * by using the StringUtils helper function.
 */
class PubSubItcException : public std::runtime_error {
  public:
    explicit PubSubItcException(const std::string& error_text) : std::runtime_error(error_text) {}

    /**
     * @brief Throws an exception with details about the last system error (errno).
     * @param error_text A user-provided description of the error.
     * @param filename The name of the file where the error occurred.
     * @param line_number The line number where the error occurred.
     */
    static void throw_errno(const std::string& error_text, const char* filename, int line_number) {
        // Use the thread-safe StringUtils helper function to get the errno string.
        throw PubSubItcException(fmt::format("{}. Error(errno) = {} ({}). Thrown from {}:{}.", error_text, errno,
                                             StringUtils::get_errno_string(), // Uses strerror_r internally
                                             StringUtils::leafname(filename), line_number));
    }
};

} // namespaces
