#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <quill/core/LogLevel.h>

namespace pubsub_itc_fw {

// Note: The numerical value of these enumerations is significant.
// The higher values imply use of the lower values.
// This is why we start with critical, then error, etc.
// The higher the number, the more stuff we log.

class FwLogLevel {
  public:
    enum LogLevelTag { Alert = 0, Critical = 1, Error = 2, Warning = 3, Notice = 4, Info = 5, Debug = 6, Trace =7 };

    FwLogLevel(LogLevelTag LogLevel) : log_level_(LogLevel) {}

    std::string as_string() const {
        return log_level_ == Alert      ? "ALERT   "
               : log_level_ == Critical ? "CRITICAL"
               : log_level_ == Error    ? "ERROR   "
               : log_level_ == Warning  ? "WARNING "
               : log_level_ == Notice   ? "NOTICE  "
               : log_level_ == Info     ? "INFO    "
               : log_level_ == Debug    ? "DEBUG   "
                                        : "UNKNOWN";
    }

    bool isEqual(const FwLogLevel& rhs) const {
        return log_level_ == rhs.log_level_;
    }

    bool isLessThan(const FwLogLevel& rhs) const {
        return log_level_ < rhs.log_level_;
    }

    LogLevelTag log_level_;
};

inline bool operator<=(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return lhs.isLessThan(rhs) || lhs.isEqual(rhs);
}

inline bool operator==(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return lhs.isEqual(rhs);
}

inline bool operator!=(const FwLogLevel& lhs, const FwLogLevel& rhs) {
    return !lhs.isEqual(rhs);
}

} // namespace pubsub_itc_fw
