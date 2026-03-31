// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <mutex> // for std::once_flag
#include <memory>
#include <string>

#include <cstdint>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/LogMacros.h>

#include <quill/sinks/StreamSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/SyslogSink.h>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggerUtils.hpp>

namespace {
std::once_flag backend_started;

// Helper to start backend exactly once
void ensure_backend_started() {
    std::call_once(backend_started, []() {
        const quill::BackendOptions backend_options{};
        quill::Backend::start(backend_options);
    });
}

}

namespace pubsub_itc_fw {

// Initialize static counter
std::atomic<uint64_t> QuillLogger::instance_counter_{0};

// Helper function to generate unique logger names
std::string QuillLogger::generate_unique_logger_name(const std::string& prefix) {
    const uint64_t id = instance_counter_.fetch_add(1, std::memory_order_relaxed);
    return prefix + "_" + std::to_string(id);
}

// Helper function to generate unique sink names
std::string QuillLogger::generate_unique_sink_name(const std::string& prefix) {
    const uint64_t id = instance_counter_.fetch_add(1, std::memory_order_relaxed);
    return prefix + "_" + std::to_string(id);
}

QuillLogger::QuillLogger(const std::string& file_path,
                         FileOpenMode file_mode,
                         FwLogLevel file_level,
                         FwLogLevel syslog_level,
                         FwLogLevel console_level)
    : file_level_{file_level},
      console_level_{console_level},
      syslog_level_{syslog_level}
{
    ensure_backend_started();

    auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(file_path,
        [file_mode]() {
            quill::FileSinkConfig config;
            if (file_mode == FileOpenMode::Append) {
                config.set_open_mode('a');
            } else {
                config.set_open_mode('w');
            }
            return config;
        }(),
        quill::FileEventNotifier{}
    );
    file_sink_ = file_sink;

    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
        generate_unique_sink_name("console_sink"));
    console_sink_ = console_sink;

    auto syslog_sink = quill::Frontend::create_or_get_sink<quill::SyslogSink>(
        generate_unique_sink_name("syslog_sink"),
        []() {
            quill::SyslogSinkConfig config;
            config.set_identifier("pubsub_app");  // Shows as "pubsub_app" in syslog
            config.set_facility(LOG_USER);        // Standard user-level facility
            config.set_options(LOG_PID);          // Include PID in each message
            config.set_format_message(false);     // Use raw message, not formatted
            return config;
        }()
    );
    syslog_sink_ = syslog_sink;

    quill_logger_ = quill::Frontend::create_or_get_logger(
        generate_unique_logger_name("pubsub_logger"),
        {file_sink, console_sink, syslog_sink} );

    // 6. Set initial log level
    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(file_level));
}

/**
 * This ctor is for unit tests were we do not want to write to a file or syslog, just the console.
 * It also writes at debug level and above.
 */
QuillLogger::QuillLogger()
    : file_level_{FwLogLevel::Debug},
      console_level_{FwLogLevel::Debug},
      syslog_level_{FwLogLevel::Debug}  {

    ensure_backend_started();

    console_sink_ = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(generate_unique_sink_name("console_sink"));
    quill_logger_ = quill::Frontend::create_or_get_logger(generate_unique_logger_name("pubsub_logger"), {console_sink_} );

    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(FwLogLevel::Debug));
}

/**
 * Constructor for unit tests with custom sink (e.g., TestSink)
 */
QuillLogger::QuillLogger(const std::string& logger_name, std::shared_ptr<quill::Sink> test_sink, FwLogLevel log_level)
    :
      console_sink_{test_sink},
      level_{log_level},
      file_level_{log_level},
      console_level_{log_level},
      syslog_level_{log_level}
{
    ensure_backend_started();

    // Create logger with the test sink
    quill_logger_ = quill::Frontend::create_or_get_logger(logger_name, {test_sink} );

    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(log_level));

    quill_logger_->set_immediate_flush();
}

// -----------------------------------------------------------------------------
// Filtering
// -----------------------------------------------------------------------------
bool QuillLogger::should_log_to_file(FwLogLevel level) const
{
    return level <= file_level_;
}

bool QuillLogger::should_log_to_syslog(FwLogLevel level) const
{
    return level <= syslog_level_;
}

bool QuillLogger::should_log_to_console(FwLogLevel level) const
{
    return level <= console_level_;
}

// -----------------------------------------------------------------------------
// Unified log level control
// -----------------------------------------------------------------------------
void QuillLogger::set_log_level(FwLogLevel level)
{
    level_ = level;
    file_level_ = syslog_level_ = console_level_ = level;
    if (quill_logger_ != nullptr) {
        quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(level));
    }
}

} // namespace pubsub_itc_fw
