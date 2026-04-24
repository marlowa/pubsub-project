#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

/*
 * LoggerWithSink — a QuillLogger wired to an in-memory record store for use
 * in unit and integration tests.
 *
 * Construct with a desired applog threshold.  The logger is in unit-test mode:
 * output goes to the console and every record that passes the threshold is also
 * delivered to the callback, which appends the fully formatted string to
 * records.
 *
 * Test code can inspect records directly or use the helper methods below.
 */
class LoggerWithSink {
  public:
    explicit LoggerWithSink(FwLogLevel level = FwLogLevel::Debug) : logger(level, [this](const std::string& record) { records.push_back(record); }) {}

    LoggerWithSink(const LoggerWithSink&) = delete;
    LoggerWithSink& operator=(const LoggerWithSink&) = delete;
    LoggerWithSink(LoggerWithSink&&) = delete;
    LoggerWithSink& operator=(LoggerWithSink&&) = delete;

    void clear() {
        records.clear();
    }

    bool contains_message(const std::string& text) const {
        for (const auto& record : records) {
            if (record.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    size_t count() const {
        return records.size();
    }

    QuillLogger logger;
    std::vector<std::string> records;
};

} // namespace pubsub_itc_fw
