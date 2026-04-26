#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <quill/core/LogLevel.h>

namespace pubsub_itc_fw {

/*
 * FwLogLevel — framework log level abstraction.
 *
 * Numerical values are significant: lower value means more severe.
 * The lower the number, the less is logged. Alert(7) logs everything
 * down to and including itself; Trace(0) logs only Trace records.
 * This matches Quill's own convention.
 */

class FwLogLevel {
  public:
    enum LogLevelTag { Trace = 0, Debug = 1, Info = 2, Notice = 3, Warning = 4, Error = 5, Critical = 6, Alert = 7 };

    FwLogLevel(LogLevelTag log_level) : log_level_(log_level) {}

    /**
     * @brief Returns a fixed-width string representation of this log level.
     * @return Eight-character string, space-padded.
     */
    std::string as_string() const {
        return log_level_ == Trace      ? "TRACE   "
               : log_level_ == Debug    ? "DEBUG   "
               : log_level_ == Info     ? "INFO    "
               : log_level_ == Notice   ? "NOTICE  "
               : log_level_ == Warning  ? "WARNING "
               : log_level_ == Error    ? "ERROR   "
               : log_level_ == Critical ? "CRITICAL"
               : log_level_ == Alert    ? "ALERT   "
                                        : "UNKNOWN ";
    }

    /**
     * @brief Parses a log level from a case-insensitive string.
     *
     * Accepted values: "trace", "debug", "info", "notice", "warning",
     * "error", "critical", "alert".
     *
     * @param[in]  str    The string to parse.
     * @param[out] level  Populated on success.
     * @return true on success, false if the string is not a recognised level.
     */
    static bool from_string(const std::string& str, FwLogLevel& level) {
        if (str == "trace"    || str == "TRACE")    { level = FwLogLevel{Trace};    return true; }
        if (str == "debug"    || str == "DEBUG")    { level = FwLogLevel{Debug};    return true; }
        if (str == "info"     || str == "INFO")     { level = FwLogLevel{Info};     return true; }
        if (str == "notice"   || str == "NOTICE")   { level = FwLogLevel{Notice};   return true; }
        if (str == "warning"  || str == "WARNING")  { level = FwLogLevel{Warning};  return true; }
        if (str == "error"    || str == "ERROR")    { level = FwLogLevel{Error};    return true; }
        if (str == "critical" || str == "CRITICAL") { level = FwLogLevel{Critical}; return true; }
        if (str == "alert"    || str == "ALERT")    { level = FwLogLevel{Alert};    return true; }
        return false;
    }

    bool is_equal(const FwLogLevel& rhs) const {
        return log_level_ == rhs.log_level_;
    }

    bool is_less_than(const FwLogLevel& rhs) const {
        return log_level_ < rhs.log_level_;
    }

    LogLevelTag log_level_;
};

inline bool operator<(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return lhs.is_less_than(rhs);
}

inline bool operator<=(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return lhs.is_less_than(rhs) || lhs.is_equal(rhs);
}

inline bool operator>(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return rhs.is_less_than(lhs);
}

inline bool operator>=(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return rhs.is_less_than(lhs) || lhs.is_equal(rhs);
}

inline bool operator==(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator!=(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return !lhs.is_equal(rhs);
}

} // namespace pubsub_itc_fw
