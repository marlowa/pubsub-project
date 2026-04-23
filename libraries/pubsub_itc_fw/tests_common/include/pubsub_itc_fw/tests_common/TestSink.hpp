#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <quill/core/LogLevel.h>
#include <quill/sinks/Sink.h>

namespace pubsub_itc_fw {

/**
 * A Quill sink that captures log records for unit testing.
 *
 * TestSink accumulates all log records into an in-memory vector that tests
 * can inspect to verify logging behaviour.
 */
class TestSink : public quill::Sink {
public:
    struct LogRecord {
        quill::LogLevel level;
        std::string logger_name;
        std::string message;
        std::string thread_id;
        uint64_t timestamp;

        LogRecord(quill::LogLevel lvl, std::string logger, std::string msg,
                  std::string tid, uint64_t ts)
            : level(lvl)
            , logger_name(std::move(logger))
            , message(std::move(msg))
            , thread_id(std::move(tid))
            , timestamp(ts) {}
    };

    TestSink() = default;

    static std::string format_timestamp_iso8601(uint64_t ns_since_epoch) {
        using namespace std::chrono;
        auto tp = time_point<system_clock, nanoseconds>(nanoseconds{ns_since_epoch});
        auto secs = time_point_cast<seconds>(tp);
        auto ns = duration_cast<nanoseconds>(tp - secs).count();
        std::time_t tt = system_clock::to_time_t(secs);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        std::ostringstream oss;
        oss << buf << "." << std::setw(9) << std::setfill('0') << ns << "Z";
        return oss.str();
    }

    void write_log(const quill::MacroMetadata* log_metadata,
                   uint64_t log_timestamp,
                   std::string_view thread_id,
                   [[maybe_unused]] std::string_view thread_name,
                   [[maybe_unused]] const std::string& process_id,
                   std::string_view logger_name,
                   quill::LogLevel log_level,
                   std::string_view log_level_description,
                   [[maybe_unused]] std::string_view log_level_short_code,
                   [[maybe_unused]] const std::vector<std::pair<std::string, std::string>>* named_args,
                   std::string_view log_message,
                   [[maybe_unused]] std::string_view log_statement) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.emplace_back(log_level, std::string{logger_name}, std::string{log_message},
                              std::string{thread_id}, log_timestamp);

        const std::string ts = format_timestamp_iso8601(log_timestamp);
        std::cout << ts << " [" << log_level_description << "] "
                  << "(" << thread_id << ") "
                  << logger_name << " "
                  << log_metadata->source_location() << ": "
                  << log_message << "\n";
        std::cout.flush();
    }

    void flush_sink() override {}

    /**
     * @brief Returns all captured log records.
     */
    const std::vector<LogRecord>& records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    /**
     * @brief Returns the number of captured records.
     */
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    /**
     * @brief Clears all captured records.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
    }

    /**
     * @brief Returns true if any captured record's message contains the given text.
     */
    bool contains_message(const std::string& text) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& record : records_) {
            if (record.message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Returns the number of captured records at the given Quill log level.
     */
    size_t count_at_level(quill::LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t result = 0;
        for (const auto& record : records_) {
            if (record.level == level) {
                ++result;
            }
        }
        return result;
    }

    /**
     * @brief Returns the most recently captured record.
     * @throws std::runtime_error if no records have been captured.
     */
    LogRecord last_record() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (records_.empty()) {
            throw std::runtime_error("No log records captured");
        }
        return records_.back();
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

} // namespace pubsub_itc_fw
