#pragma once

#include <string>
#include <vector>
#include <mutex>

#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/LogLevel.hpp>

namespace pubsub_itc_fw::tests_common {

class UnitTestLogger final : public LoggerInterface {
public:
    explicit UnitTestLogger(LogLevel log_level)
        : log_level_(log_level) {}

    ~UnitTestLogger() override = default;

    UnitTestLogger(const UnitTestLogger&) = delete;
    UnitTestLogger& operator=(const UnitTestLogger&) = delete;
    UnitTestLogger(UnitTestLogger&&) = delete;
    UnitTestLogger& operator=(UnitTestLogger&&) = delete;

    [[nodiscard]] bool should_log(LogLevel level) const override {
        return level <= log_level_;
    }

    void set_log_level(LogLevel level) override {
        log_level_ = level;
    }

    void flush() const override {
        // no-op
    }

    void set_immediate_flush() override {
        // no-op
    }

    // --- Test helpers --------------------------------------------------------

    void record(const std::string& msg) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        logged_messages_.push_back(msg);
    }

    [[nodiscard]] std::vector<std::string> get_logged_messages() const {
        std::lock_guard<std::mutex> lock(log_mutex_);
        return logged_messages_;
    }

    void clear_logged_messages() {
        std::lock_guard<std::mutex> lock(log_mutex_);
        logged_messages_.clear();
    }

private:
    LogLevel log_level_{LogLevel::Debug};
    mutable std::mutex log_mutex_;
    mutable std::vector<std::string> logged_messages_;
};

} // namespace pubsub_itc_fw::tests_common
