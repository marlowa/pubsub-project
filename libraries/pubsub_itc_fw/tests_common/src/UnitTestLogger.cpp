// C++ headers whose names start with ‘c’
#include <cstddef>   // For std::size_t

#include <mutex>     // For std::lock_guard

#include <pubsub_itc_fw/tests_common/UnitTestLogger.hpp> // Header for this class
#include <pubsub_itc_fw/StringUtils.hpp>                // For utility functions like leafname

namespace pubsub_itc_fw::tests_common { // Corrected namespace opening

// Initialize thread_local dummy buffer
thread_local std::vector<char> UnitTestLogger::tls_dummy_buffer_;

UnitTestLogger::UnitTestLogger(LogLevel log_level)
    : log_level_(log_level) {
    // Constructor initializes the log level.
}

bool UnitTestLogger::should_log(LogLevel log_level) const {
    return log_level <= log_level_;
}

void UnitTestLogger::log(LogLevel log_level, const char* filename, int line_number, const char* function_name) const {
    if (!should_log(log_level)) {
        return;
    }

    // We must acquire the lock before accessing logged_messages_
    std::lock_guard<std::mutex> lock(log_mutex_);

    // The PUBSUB_LOG macro (from Logger.hpp) will format into the buffer returned by get_tls_buffer().
    // We then capture that content.
    logged_messages_.emplace_back(tls_dummy_buffer_.data(), tls_dummy_buffer_.size());
}

std::vector<char>& UnitTestLogger::get_tls_buffer() const {
    // For UnitTestLogger, this buffer is just a temporary scratchpad for fmt::format.
    // The actual logging happens when `log()` is called, which then copies the content.
    // We resize it to a reasonable default or ensure it's large enough.
    tls_dummy_buffer_.resize(4096); // Reasonable size for log messages
    return tls_dummy_buffer_;
}

void UnitTestLogger::flush() const {
    // No-op for in-memory logger as messages are immediately stored.
}

void UnitTestLogger::set_log_level(LogLevel log_level) {
    log_level_ = log_level;
}

void UnitTestLogger::set_immediate_flush() {
    // No-op for in-memory logger.
}

std::vector<std::string> UnitTestLogger::get_logged_messages() const {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return logged_messages_;
}

void UnitTestLogger::clear_logged_messages() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    logged_messages_.clear();
}

} // namespace pubsub_itc_fw::tests_common
