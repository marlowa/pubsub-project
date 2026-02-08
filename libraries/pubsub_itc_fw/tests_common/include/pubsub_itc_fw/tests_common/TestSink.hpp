#pragma once

#include <vector>
#include <string>
#include <mutex>

#include <iostream> // HACK

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

#if 0
    TestSink() = default;
#else
    TestSink() { std::fprintf(stderr, "TestSink::CTOR this=%p\n", static_cast<void*>(this)); }
#endif

    /**
     * @brief Called by Quill backend to write a log record
     */
    void write_log(quill::MacroMetadata const* log_metadata,
                   uint64_t log_timestamp,
                   std::string_view thread_id,
                   std::string_view thread_name,
                   std::string const& process_id,
                   std::string_view logger_name,
                   quill::LogLevel log_level,
                   std::string_view log_level_description,
                   std::string_view log_level_short_code,
                   std::vector<std::pair<std::string, std::string>> const* named_args,
                   std::string_view log_message,
                   std::string_view log_statement) override
    {
#if 1
    std::fprintf(stderr, "TestSink::write_log CALLED: %s\n", std::string{log_message}.c_str());
    std::lock_guard<std::mutex> lock(mutex_);

    records_.emplace_back(
        log_level,
        std::string{logger_name},
        std::string{log_message},
        std::string{thread_id},
        log_timestamp
    );
    std::fprintf(stderr, "TestSink::write_log this=%p message=%.*s size=%zu\n", static_cast<void*>(this), static_cast<int>(log_message.size()), log_message.data(), records_.size());
#else
        std::lock_guard<std::mutex> lock(mutex_);

        records_.emplace_back(
            log_level,
            std::string{logger_name},
            std::string{log_message},
            std::string{thread_id},
            log_timestamp
        );
#endif
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
#if 0
        return records_.size();
#else
    std::fprintf(stderr, "TestSink::count this=%p size=%zu\n", static_cast<const void*>(this), records_.size());
    return records_.size();
#endif
    }

    /**
     * @brief Clear all captured records
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
 std::fprintf(stderr, "TestSink::clear this=%p (before size=%zu)\n", static_cast<void*>(this), records_.size());
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
