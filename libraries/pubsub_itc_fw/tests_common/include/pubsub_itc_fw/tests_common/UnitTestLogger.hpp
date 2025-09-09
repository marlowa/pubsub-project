#pragma once

#include <string>  // For std::string
#include <vector>  // For std::vector
#include <atomic>  // For std::atomic
#include <mutex>   // For std::mutex, std::lock_guard

// Project headers
#include <pubsub_itc_fw/LoggerInterface.hpp> // Base interface
#include <pubsub_itc_fw/LogLevel.hpp>       // For LogLevel enum
#include <quill/bundled/fmt/format.h>       // For fmt::format in PUBSUB_LOG macro

namespace pubsub_itc_fw::tests_common {

/**
 * @brief A simple in-memory logger for unit testing purposes.
 *
 * This logger captures log messages into a vector of strings, allowing unit
 * tests to easily inspect generated log output. It implements the
 * `LoggerInterface` and is designed to be self-contained for testing,
 * without relying on external logging frameworks like Quill for its operation.
 *
 * It uses a mutex to protect access to its internal log buffer, as it might
 * be used concurrently in multi-threaded tests, although its primary purpose
 * is for verifying single-threaded logging behavior or final aggregated logs.
 */
class UnitTestLogger final : public LoggerInterface {
public:
    /**
     * @brief Constructs a `UnitTestLogger` with a specified log level.
     * @param[in] log_level The maximum log level that this logger will process.
     */
    explicit UnitTestLogger(LogLevel log_level);

    /**
     * @brief Destructor for `UnitTestLogger`.
     */
    ~UnitTestLogger() override = default;

    // Deleted copy and move constructors/assignment operators to ensure
    // unique ownership of resources (e.g., the log buffer).
    UnitTestLogger(const UnitTestLogger&) = delete;
    UnitTestLogger& operator=(const UnitTestLogger&) = delete;
    UnitTestLogger(UnitTestLogger&&) = delete;
    UnitTestLogger& operator=(UnitTestLogger&&) = delete;

    /**
     * @brief Checks if a given log level should be processed by this logger.
     * @param[in] log_level The log level to check.
     * @return `true` if `log_level` is less than or equal to the logger's configured level, `false` otherwise.
     */
    [[nodiscard]] bool should_log(LogLevel log_level) const override;

    /**
     * @brief Logs a formatted message.
     *
     * This method captures the formatted message into an internal buffer.
     *
     * @param[in] log_level The level of the log message.
     * @param[in] filename The name of the source file where the log originated.
     * @param[in] line_number The line number in the source file.
     * @param[in] function_name The name of the function where the log originated.
     */
    void log(LogLevel log_level, const char* filename, int line_number, const char* function_name) const override;

    /**
     * @brief Returns a thread-local buffer (not used by `UnitTestLogger`).
     *
     * This method is part of `LoggerInterface` but is not actively used by
     * `UnitTestLogger` for formatting, as it captures the final string.
     * A dummy vector is returned.
     * @return A reference to a dummy `std::vector<char>`.
     */
    [[nodiscard]] std::vector<char>& get_tls_buffer() const override;

    /**
     * @brief Flushes any buffered messages (no-op for `UnitTestLogger`).
     */
    void flush() const override;

    /**
     * @brief Sets the new minimum log level for this logger.
     * @param[in] log_level The new log level to set.
     */
    void set_log_level(LogLevel log_level) override;

    /**
     * @brief Sets the logger to immediate flush mode (no-op for `UnitTestLogger`).
     */
    void set_immediate_flush() override;

    /**
     * @brief Retrieves all captured log messages.
     * @return A `std::vector<std::string>` containing all log messages in order.
     */
    [[nodiscard]] std::vector<std::string> get_logged_messages() const;

    /**
     * @brief Clears all captured log messages from the internal buffer.
     */
    void clear_logged_messages();

private:
    LogLevel log_level_{LogLevel::Debug};            /**< @brief The current log level for this logger. */
    mutable std::vector<std::string> logged_messages_; /**< @brief The in-memory buffer for captured log messages. */
    mutable std::mutex log_mutex_;                   /**< @brief Mutex to protect `logged_messages_` during concurrent access. */

    // Dummy buffer for get_tls_buffer, as it's required by interface but not used by this logger.
    static thread_local std::vector<char> tls_dummy_buffer_;
};

} // namespaces
