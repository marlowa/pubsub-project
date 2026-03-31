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
    const static char* leafname(const char* filename)
    {
        auto slen = std::strlen(filename);
        const char* ptr = filename + slen;
        while (ptr != filename && *(ptr - 1) != '/') {
            --ptr;
        }
        return ptr;
    }

    static std::string_view function_name(const char* function_signature)
    {
        std::string_view retval = function_signature;
        auto pos = retval.find("(");
        if (pos != std::string::npos) {
            retval = retval.substr(0, pos);
            pos = retval.rfind(" ");
            if (pos != std::string::npos) {
                retval = retval.substr(pos + 1);
            }
        }
        return retval;
    }

    /**
     * @brief Convert pubsub_itc_fw::FwLogLevel to quill::LogLevel
     */
    static quill::LogLevel to_quill_log_level(FwLogLevel level)
    {
        switch (level.log_level_) {
            case FwLogLevel::Trace:
                return quill::LogLevel::TraceL3;
            case FwLogLevel::Debug:
                return quill::LogLevel::Debug;
            case FwLogLevel::Info:
                return quill::LogLevel::Info;
            case FwLogLevel::Notice:
                return quill::LogLevel::Info;  // Quill doesn't have Notice, map to Info
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
     * @brief Convert quill::LogLevel to pubsub_itc_fw::LogLevel
     */
    static FwLogLevel from_quill_log_level(quill::LogLevel level)
    {
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
