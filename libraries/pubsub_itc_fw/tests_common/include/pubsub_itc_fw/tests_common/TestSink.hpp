#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <vector>
#include <string>
#include <mutex>

#include <iostream> // HACK TODO

#include <quill/sinks/Sink.h>
#include <quill/core/LogLevel.h>

namespace pubsub_itc_fw {

/**
 * @brief A Quill sink that captures log records for unit testing
 *
 * TestSink captures all log messages into a vector that can be inspected
 * in unit tests to verify logging behavior. This allows tests to use the
 * real QuillLogger with a test-friendly sink.
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
            , timestamp(ts)
        {}
    };

    TestSink() = default;

    static std::string format_timestamp_iso8601(uint64_t ns_since_epoch) {
        using namespace std::chrono;
        auto tp = time_point<system_clock, nanoseconds>(nanoseconds{ns_since_epoch});
        auto secs = time_point_cast<seconds>(tp);
        auto ns = duration_cast<nanoseconds>(tp - secs).count();
        std::time_t tt = system_clock::to_time_t(secs); std::tm tm{};
        gmtime_r(&tt, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        std::ostringstream oss; oss << buf << "." << std::setw(9) << std::setfill('0')
                                    << ns << "Z"; return oss.str();
    }

    static void print_console_log(const std::string& timestamp,
                                  std::string_view level_desc,
                                  std::string_view thread_id,
                                  std::string_view logger_name,
                                  std::string_view message,
                                  std::string_view source_location) {
        std::cout << timestamp << " [" << level_desc << "] " << "(" << thread_id << ") "
                  << logger_name << " " << source_location << " — " << ": " << message << std::endl;
        std::cout.flush();
    }

    /**
     * @brief Called by Quill backend to write a log record
     *
     * Several of the args here are not needed since this is test code.
     */
    void write_log(const quill::MacroMetadata* log_metadata,
                   uint64_t log_timestamp,
                   [[maybe_unused]] std::string_view thread_id,
                   [[maybe_unused]] std::string_view thread_name,
                   [[maybe_unused]] const std::string& process_id, // we dont need the pid on every record
                   std::string_view logger_name,
                   quill::LogLevel log_level,
                   std::string_view log_level_description,
                   [[maybe_unused]] std::string_view log_level_short_code, // we dont need this
                   [[maybe_unused]] const std::vector<std::pair<std::string, std::string>>* named_args,
                   std::string_view log_message,
                   [[maybe_unused]] std::string_view log_statement) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.emplace_back(log_level, std::string{logger_name}, std::string{log_message},
                              std::string{thread_id}, log_timestamp);
        std::string ts = format_timestamp_iso8601(log_timestamp);

        std::string_view source_location = log_metadata->source_location();
        print_console_log(ts, log_level_description, thread_id, logger_name, log_message, source_location);
    }

    /**
     * @brief Flush any buffered data (no-op for test sink)
     */
    void flush_sink() override {
        // No buffering in test sink
    }

    // Test interface methods

    /**
     * @brief Get all captured log records
     */
    const std::vector<LogRecord>& records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    /**
     * @brief Get number of captured records
     */
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    /**
     * @brief Clear all captured records
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
    }

    /**
     * @brief Check if a specific message was logged
     */
    bool contains_message(const std::string& msg) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& record : records_) {
            if (record.message.find(msg) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Count records at a specific log level
     */
    size_t count_at_level(quill::LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& record : records_) {
            if (record.level == level) {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief Get the most recent log record (throws if empty)
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
