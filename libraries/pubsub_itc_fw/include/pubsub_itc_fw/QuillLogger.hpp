#pragma once

#include <string>
#include <memory>

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/Sink.h>
#include <quill/core/LogLevel.h>

#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/LoggerUtils.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>

namespace pubsub_itc_fw {

/** @ingroup logging_subsystem */

class QuillLogger {
public:
    ~QuillLogger() = default;

    QuillLogger(const std::string& file_path,
                         FileOpenMode file_mode,
                         LogLevel file_level,
                         LogLevel syslog_level,
                         LogLevel console_level);

    explicit QuillLogger(); // for unit tests

    QuillLogger(const std::string& logger_name, std::shared_ptr<quill::Sink> test_sink,
                         LogLevel log_level = LogLevel::Debug);

    quill::Logger* quill_logger() const noexcept { return quill_logger_; }

    void set_log_level(LogLevel level);
    LogLevel log_level() const noexcept { return level_; }

    // Per-destination filtering
    bool should_log_to_file(LogLevel level) const;
    bool should_log_to_syslog(LogLevel level) const;
    bool should_log_to_console(LogLevel level) const;

private:

    // Static counter for generating unique logger names
    static std::atomic<uint64_t> instance_counter_;

    // Helper to generate unique names
    static std::string generate_unique_logger_name(const std::string& prefix);
    static std::string generate_unique_sink_name(const std::string& prefix);

    std::shared_ptr<quill::Sink> file_sink_;
    std::shared_ptr<quill::Sink> console_sink_;
    std::shared_ptr<quill::Sink> syslog_sink_;

    // TODO APM not sure about this, having a single level here.
    LogLevel level_{LogLevel::Info};

    LogLevel file_level_{LogLevel::Info};
    LogLevel console_level_{LogLevel::Info};
    LogLevel syslog_level_{LogLevel::Info};

    quill::Logger* quill_logger_{nullptr};
};

} // namespace pubsub_itc_fw
