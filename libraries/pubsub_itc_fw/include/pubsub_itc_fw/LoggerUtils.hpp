#pragma once

#include <cstring>
#include <string_view>

#include <quill/core/LogLevel.h>

#include <pubsub_itc_fw/LogLevel.hpp>

namespace pubsub_itc_fw {

class LoggerUtils {
public:
    static const char* leafname(const char* filename)
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
     * @brief Convert pubsub_itc_fw::LogLevel to quill::LogLevel
     */
    static quill::LogLevel to_quill_log_level(LogLevel level) noexcept
    {
        switch (level.log_level_) {
            case LogLevel::Trace:
                return quill::LogLevel::TraceL3;
            case LogLevel::Debug:
                return quill::LogLevel::Debug;
            case LogLevel::Info:
                return quill::LogLevel::Info;
            case LogLevel::Notice:
                return quill::LogLevel::Info;  // Quill doesn't have Notice, map to Info
            case LogLevel::Warning:
                return quill::LogLevel::Warning;
            case LogLevel::Error:
                return quill::LogLevel::Error;
            case LogLevel::Critical:
            case LogLevel::Alert:
                return quill::LogLevel::Critical;
            default:
                return quill::LogLevel::Info;
        }
    }

    /**
     * @brief Convert quill::LogLevel to pubsub_itc_fw::LogLevel
     */
    static LogLevel from_quill_log_level(quill::LogLevel level) noexcept
    {
        switch (level) {
            case quill::LogLevel::TraceL1:
            case quill::LogLevel::TraceL2:
            case quill::LogLevel::TraceL3:
                return LogLevel::Trace;
            case quill::LogLevel::Debug:
                return LogLevel::Debug;
            case quill::LogLevel::Info:
                return LogLevel::Info;
            case quill::LogLevel::Warning:
                return LogLevel::Warning;
            case quill::LogLevel::Error:
                return LogLevel::Error;
            case quill::LogLevel::Critical:
            case quill::LogLevel::Backtrace:
                return LogLevel::Critical;
            default:
                return LogLevel::Info;
        }
    }

};

} // namespace pubsub_itc_fw
