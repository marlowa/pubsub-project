#include <pubsub_itc_fw/QuillLogger.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/LogMacros.h>

#include <quill/sinks/StreamSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/ConsoleSink.h>
// Note: SyslogSink may not be available on all platforms in Quill 11
// #include <quill/sinks/SyslogSink.h>

#include <pubsub_itc_fw/FileOpenMode.hpp>

namespace pubsub_itc_fw {

QuillLogger::QuillLogger(const std::string& file_path,
                         FileOpenMode file_mode,
                         LogLevel file_level,
                         LogLevel syslog_level,
                         LogLevel console_level)
    : file_level_{file_level},
      console_level_{console_level},
      syslog_level_{syslog_level}
{
    // 1. Start Quill backend thread (Quill 11.x API)
    quill::BackendOptions backend_options{};
    quill::Backend::start(backend_options);

    // 2. Configure file sink (Quill 11.x API)
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

    // 3. Console sink (Quill 11.x API)
    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console_sink");
    console_sink_ = console_sink;

    // 4. Syslog sink (if available on your platform)
    // For now, we'll use console for syslog as well
    // If you need real syslog, you'll need to check if SyslogSink is available in Quill 11
    // TODO this needs attention
    syslog_sink_ = console_sink_;

    // 5. Create logger with all sinks (Quill 11.x API)
    quill_logger_ = quill::Frontend::create_or_get_logger("pubsub_logger", {file_sink, console_sink} );

    // 6. Set initial log level
    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(file_level));
}

/**
 * This ctor is for unit tests were we do not want to write to a file or syslog, just the console.
 * It also writes at debug level and above.
 */
QuillLogger::QuillLogger()
    : file_level_{LogLevel::Debug},
      console_level_{LogLevel::Debug},
      syslog_level_{LogLevel::Debug}  {

    quill::BackendOptions backend_options{};
    quill::Backend::start(backend_options);

    console_sink_ = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console_sink");
    quill_logger_ = quill::Frontend::create_or_get_logger("pubsub_logger", {console_sink_} );

    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(LogLevel::Debug));
}

/**
 * Constructor for unit tests with custom sink (e.g., TestSink)
 */
QuillLogger::QuillLogger(const std::string& logger_name, std::shared_ptr<quill::Sink> test_sink, LogLevel log_level)
    :
      console_sink_{test_sink},
      level_{log_level},
      file_level_{log_level},
      console_level_{log_level},
      syslog_level_{log_level}
{
    // If the quill backend is already running from a previous test, then stop it.
    quill::Backend::stop();

    // Start backend
    quill::BackendOptions backend_options{};
    backend_options.sleep_duration = std::chrono::nanoseconds{0};
    backend_options.sink_min_flush_interval = std::chrono::milliseconds{0};
    quill::Backend::start(backend_options);

    // Create logger with the test sink
    quill_logger_ = quill::Frontend::create_or_get_logger(logger_name, {test_sink} );

    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(log_level));

    quill_logger_->set_immediate_flush();
}

// -----------------------------------------------------------------------------
// Filtering
// -----------------------------------------------------------------------------
bool QuillLogger::should_log_to_file(LogLevel level) const
{
    return level <= file_level_;
}

bool QuillLogger::should_log_to_syslog(LogLevel level) const
{
    return level <= syslog_level_;
}

bool QuillLogger::should_log_to_console(LogLevel level) const
{
    return level <= console_level_;
}

// -----------------------------------------------------------------------------
// Unified log level control
// -----------------------------------------------------------------------------
void QuillLogger::set_log_level(LogLevel level)
{
    level_ = level;
    file_level_ = syslog_level_ = console_level_ = level;
    if (quill_logger_) {
        quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(level));
    }
}

} // namespace pubsub_itc_fw
