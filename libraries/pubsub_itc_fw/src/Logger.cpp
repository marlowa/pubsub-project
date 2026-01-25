/*
NOTE: This logger design needs to be rewritten.
We should not use a TLS buffer into which to assemble the logging record.
That causes a lot of work to be done on the wrong thread.
Instead, we should take advantage of the fact that quill sends the variable length parameter list
to the logging thread, which causes all the work to be done on the logging thread.
 */

#include <cstring>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/Logger.hpp>
#include <pubsub_itc_fw/LoggerUtils.hpp>

namespace pubsub_itc_fw {

// We use TLS (Thread Local Storage) to avoid repeated heap allocation for the formatted log record.

thread_local std::vector<char> Logger::tls_buffer_;

Logger::Logger(LogLevel log_level, const std::string& log_directory, const std::string& filename, FilenameAppendMode append_mode, int rolling_size)
    : log_level_(log_level), log_directory_(log_directory) {
    // Ensure the log directory exists, creating it if necessary.
    try {
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (!boost::filesystem::exists(log_directory_)) {
            // NOLINTNEXTLINE(misc-include-cleaner)
            boost::filesystem::create_directories(log_directory_);
        }
    // NOLINTNEXTLINE(misc-include-cleaner)
    } catch (const boost::filesystem::filesystem_error& ex) {
        throw std::runtime_error(fmt::format("Error creating log directory [{}], exception [{}]'", log_directory_, ex.what()));
    }

    // Start the quill backend thread
    // NOLINTNEXTLINE(misc-include-cleaner)
    const quill::BackendOptions backend_options;
    quill::Backend::start(backend_options);

    const boost::filesystem::path full_logfile_path = boost::filesystem::path(log_directory) / filename;
    const std::string& full_logfile_name = full_logfile_path.string();

    quill::FilenameAppendOption quill_append_option{quill::FilenameAppendOption::None};
    if (append_mode == FilenameAppendMode::StartDateTime) {
        quill_append_option = quill::FilenameAppendOption::StartDateTime;
    }

    if (rolling_size == 0) {
        // No rolling → plain FileSink (avoids the zero-size rotation gotcha)
        sink_ = quill::Frontend::create_or_get_sink<quill::FileSink>(full_logfile_name, [quill_append_option] {
            quill::FileSinkConfig cfg;
            cfg.set_open_mode('w');
            cfg.set_filename_append_option(quill_append_option);
            return cfg;
        }());
    } else {
        // Rolling → RotatingFileSink
        sink_ = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(full_logfile_name, [rolling_size, quill_append_option] {
            // NOLINTNEXTLINE(misc-include-cleaner)
            quill::RotatingFileSinkConfig cfg;
            cfg.set_open_mode('w');
            cfg.set_filename_append_option(quill_append_option);
            cfg.set_rotation_time_daily("00:00");         // keep if you want daily rollover too
            cfg.set_rotation_max_file_size(rolling_size); // only when > 0
            return cfg;
        }());
    }

    const std::string logger_name = "logger_" + filename;
    quill_logger_ = quill::Frontend::create_or_get_logger(
        logger_name, std::move(sink_),
        // NOLINTNEXTLINE(misc-include-cleaner)
        quill::PatternFormatterOptions{"%(time) [%(thread_id)] LOG_%(log_level:<6) %(logger:<1) %(message)", "%F %H:%M:%S.%Qns", quill::Timezone::GmtTime});

    // set the log level of the logger just once to log everything. We filter out at the higher level.
    // This avoids possible race conditions between our log level and the quill level.
    // NOLINTNEXTLINE(misc-include-cleaner)
    quill_logger_->set_log_level(quill::LogLevel::Debug);
}

void Logger::log(LogLevel log_level, const char* filename, int line_number, const char* function_name) const {
    // Your existing log level handling code...
    if (log_level == LogLevel::Alert || log_level == LogLevel::Critical) {
        LOG_CRITICAL(quill_logger_, "{}:{} {} {}", LoggerUtils::leafname(filename), line_number, LoggerUtils::function_name(function_name),
                     std::string_view(tls_buffer_.data(), strlen(tls_buffer_.data())));
        quill_logger_->flush_log();
    } else if (log_level == LogLevel::Error) {
        LOG_ERROR(quill_logger_, "{}:{} {} {}", LoggerUtils::leafname(filename), line_number, LoggerUtils::function_name(function_name),
                  std::string_view(tls_buffer_.data(), strlen(tls_buffer_.data())));
    } else if (log_level == LogLevel::Warning) {
        LOG_WARNING(quill_logger_, "{}:{} {} {}", LoggerUtils::leafname(filename), line_number, LoggerUtils::function_name(function_name),
                    std::string_view(tls_buffer_.data(), strlen(tls_buffer_.data())));
    } else if (log_level == LogLevel::Notice || log_level == LogLevel::Info) {
        LOG_INFO(quill_logger_, "{}:{} {} {}", LoggerUtils::leafname(filename), line_number, LoggerUtils::function_name(function_name),
                 std::string_view(tls_buffer_.data(), strlen(tls_buffer_.data())));
    } else if (log_level == LogLevel::Debug) {
        LOG_DEBUG(quill_logger_, "{}:{} {} {}", LoggerUtils::leafname(filename), line_number, LoggerUtils::function_name(function_name),
                  std::string_view(tls_buffer_.data(), strlen(tls_buffer_.data())));
    }
}

void Logger::flush() const {
    quill_logger_->flush_log();
}

void Logger::set_log_level(LogLevel log_level) {
    log_level_ = log_level;
}

void Logger::set_immediate_flush() {
    quill_logger_->set_immediate_flush();
}

} // namespace pubsub_itc_fw
