#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw::tests_common {

/*
 * LoggerWithSink -- a QuillLogger wired to an in-memory record store for use
 * in unit and integration tests.
 *
 * Construct with a desired applog threshold.  The logger is in unit-test mode:
 * output goes to the console and every record that passes the threshold is also
 * delivered to the callback, which appends the fully formatted string to
 * records.
 *
 * The backend thread delivers records after the log macro returns. The helper
 * methods below flush first, so they see everything logged before they were called;
 * test code that reads records directly calls logger.flush() first.
 */
class LoggerWithSink {
  public:
    explicit LoggerWithSink(FwLogLevel level = FwLogLevel::Debug) : logger(level, [this](const std::string& record) { records.push_back(record); }) {}

    LoggerWithSink(const LoggerWithSink&) = delete;
    LoggerWithSink& operator=(const LoggerWithSink&) = delete;
    LoggerWithSink(LoggerWithSink&&) = delete;
    LoggerWithSink& operator=(LoggerWithSink&&) = delete;

    void clear() {
        logger.flush();
        records.clear();
    }

    [[nodiscard]] bool contains_message(const std::string& text) {
        logger.flush();
        for (const auto& record : records) {
            if (record.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] size_t count() {
        logger.flush();
        return records.size();
    }

    // Declared before the logger, so that it is destroyed after it: the logger's destructor
    // flushes, delivering any record still pending into this vector.
    std::vector<std::string> records;
    QuillLogger logger;
};

} // namespaces
