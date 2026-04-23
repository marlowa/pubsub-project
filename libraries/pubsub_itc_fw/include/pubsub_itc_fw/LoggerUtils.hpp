#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string_view>

#include <quill/core/LogLevel.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace pubsub_itc_fw {

/** @ingroup logging_subsystem */

class LoggerUtils {
public:
    /**
     * @brief Returns a pointer to the filename component of a full path.
     * @param filename Full file path, e.g. from __FILE__.
     * @return Pointer into filename at the start of the leaf name.
     */
    static const char* leafname(const char* filename) {
        auto slen = std::strlen(filename);
        const char* ptr = filename + slen;
        while (ptr != filename && *(ptr - 1) != '/') {
            --ptr;
        }
        return ptr;
    }

    /**
     * @brief Extracts the bare function name from a compiler-provided signature.
     * @param function_signature Value of __PRETTY_FUNCTION__ or similar.
     * @return View of just the function name, without return type or parameters.
     */
    static std::string_view function_name(const char* function_signature) {
        std::string_view result = function_signature;
        auto pos = result.find('(');
        if (pos != std::string_view::npos) {
            result = result.substr(0, pos);
            pos = result.rfind(' ');
            if (pos != std::string_view::npos) {
                result = result.substr(pos + 1);
            }
        }
        return result;
    }

    /**
     * @brief Converts an FwLogLevel to the nearest quill::LogLevel.
     * @param level [in] Framework log level.
     * @return Corresponding quill::LogLevel.
     */
    static quill::LogLevel to_quill_log_level(FwLogLevel level) {
        switch (level.log_level_) {
            case FwLogLevel::Trace:
                return quill::LogLevel::TraceL3;
            case FwLogLevel::Debug:
                return quill::LogLevel::Debug;
            case FwLogLevel::Info:
                return quill::LogLevel::Info;
            case FwLogLevel::Notice:
                return quill::LogLevel::Info;   // Quill has no Notice; map to Info
            case FwLogLevel::Warning:
                return quill::LogLevel::Warning;
            case FwLogLevel::Error:
                return quill::LogLevel::Error;
            case FwLogLevel::Critical:
            case FwLogLevel::Alert:
                return quill::LogLevel::Critical;
            default:
                return quill::LogLevel::Info;
        }
    }

    /**
     * @brief Converts a quill::LogLevel to the nearest FwLogLevel.
     * @param level [in] Quill log level.
     * @return Corresponding FwLogLevel.
     */
    static FwLogLevel from_quill_log_level(quill::LogLevel level) {
        switch (level) {
            case quill::LogLevel::TraceL1:
            case quill::LogLevel::TraceL2:
            case quill::LogLevel::TraceL3:
                return FwLogLevel::Trace;
            case quill::LogLevel::Debug:
                return FwLogLevel::Debug;
            case quill::LogLevel::Info:
                return FwLogLevel::Info;
            case quill::LogLevel::Warning:
                return FwLogLevel::Warning;
            case quill::LogLevel::Error:
                return FwLogLevel::Error;
            case quill::LogLevel::Critical:
            case quill::LogLevel::Backtrace:
                return FwLogLevel::Critical;
            default:
                return FwLogLevel::Info;
        }
    }
};

} // namespace pubsub_itc_fw
