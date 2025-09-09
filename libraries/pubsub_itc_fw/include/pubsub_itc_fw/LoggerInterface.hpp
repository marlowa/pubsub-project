#pragma once

#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/LogLevel.hpp>

namespace pubsub_itc_fw {

/**
 * Abstract interface for logging functionality.
 * This allows for easy mocking and testing of components that depend on logging.
 */
class LoggerInterface {
  public:
    /**
     * @brief Controls how the logger names its output files.
     */
    enum class FilenameAppendMode {
        None,         /**< Use the filename exactly as provided. */
        StartDateTime /**< Append the logger start datetime to the filename. */
    };

    virtual ~LoggerInterface() = default;

    /**
     * Check if a message at the given log level should be logged.
     * @param log_level The log level to check
     * @return true if the message should be logged, false otherwise
     */
    virtual bool should_log(LogLevel log_level) const = 0;

    /**
     * Log a message that has already been formatted into the TLS buffer.
     * @param[in] log_level The log level of the message
     * @param[in] filename The source file where the log call originated
     * @param]in] line_number The line number where the log call originated
     * @param[in] function_name The function name where the log call originated
     */
    virtual void log(LogLevel log_level, const char* filename, int line_number, const char* function_name) const = 0;

    /**
     * Get access to the thread-local buffer for formatting log messages.
     * @return Reference to the TLS buffer
     */
    virtual std::vector<char>& get_tls_buffer() const = 0;

    virtual void set_log_level(LogLevel log_level) = 0;

    /**
     * Flushes any buffered log messages to their final destination.
     * This ensures that all pending log entries are written, which is crucial
     * for critical alerts or before application shutdown.
     */
    virtual void flush() const = 0;

    virtual void set_immediate_flush() = 0;
};

#define PUBSUB_LOG_FMT(logger, format_string, ...)                                                                                                             \
    do {                                                                                                                                                       \
        std::vector<char>& tls_buffer = logger.get_tls_buffer();                                                                                               \
        constexpr size_t initial_size = 1024;                                                                                                                  \
        if (tls_buffer.capacity() == 0) {                                                                                                                      \
            tls_buffer.reserve(initial_size);                                                                                                                  \
        }                                                                                                                                                      \
        auto fmt_result = fmt::format_to_n(tls_buffer.data(), tls_buffer.capacity(), format_string, ##__VA_ARGS__);                                       \
        auto fmt_count = fmt_result.size;                                                                                                                      \
        if (fmt_count < tls_buffer.capacity() && fmt_count > 0) {                                                                                              \
            tls_buffer[fmt_count] = '\0';                                                                                                                      \
        } else if (fmt_count >= tls_buffer.capacity()) {                                                                                                       \
            tls_buffer.reserve(fmt_count * 2);                                                                                                                 \
            auto fmt_result = fmt::format_to_n(tls_buffer.data(), tls_buffer.capacity(), format_string, ##__VA_ARGS__);                                   \
            fmt_count = fmt_result.size;                                                                                                                       \
            if (fmt_count > 0) {                                                                                                                               \
                tls_buffer[fmt_count] = '\0';                                                                                                                  \
            } else {                                                                                                                                           \
                tls_buffer.clear();                                                                                                                            \
                tls_buffer.push_back('\0');                                                                                                                    \
            }                                                                                                                                                  \
        } else {                                                                                                                                               \
            tls_buffer.clear();                                                                                                                                \
            tls_buffer.push_back('\0');                                                                                                                        \
        }                                                                                                                                                      \
    } while (false)

#define PUBSUB_LOG(logger_expr, log_level, format_string, ...)                                         \
    do {                                                                                               \
        /* Introduce a local reference to ensure correct type resolution and consistent access */      \
        auto& logger_local_ref = (logger_expr); /* Corrected variable name as requested */             \
        if (logger_local_ref.should_log(log_level)) {                                                  \
            logger_local_ref.get_tls_buffer().clear();                                                 \
            try {                                                                                      \
                /* Use fmt::format_to with back_inserter for efficient formatting into the buffer */   \
                fmt::format_to(std::back_inserter(logger_local_ref.get_tls_buffer()), format_string, ##__VA_ARGS__); \
                /* Call the virtual log method on the interface */                                     \
                logger_local_ref.log(log_level, __FILE__, __LINE__, __PRETTY_FUNCTION__);              \
            } catch (const fmt::format_error& ex) {                                                    \
                /* Error handling for formatting failure within the macro */                           \
                std::ostringstream ostr_err;                                                           \
                ostr_err << "format error [" << ex.what() << "], string is [" << format_string << "]"; \
                auto error_str = ostr_err.str();                                                       \
                logger_local_ref.get_tls_buffer().assign(error_str.begin(), error_str.end());          \
                logger_local_ref.log(log_level, __FILE__, __LINE__, __PRETTY_FUNCTION__);              \
            }                                                                                          \
        }                                                                                              \
    } while (false)

#define PUBSUB_LOG_STR(logger, log_level, log_string) PUBSUB_LOG(logger, log_level, "{}", log_string);

} // namespace pubsub_itc_fw
