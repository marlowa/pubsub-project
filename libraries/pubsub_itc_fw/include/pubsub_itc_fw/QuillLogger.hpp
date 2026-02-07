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

class QuillLogger {
public:
    ~QuillLogger() = default;

    explicit QuillLogger(const std::string& file_path,
                         FileOpenMode file_mode,
                         LogLevel file_level,
                         LogLevel syslog_level,
                         LogLevel console_level);

    explicit QuillLogger(); // for unit tests

    quill::Logger* quill_logger() const noexcept { return quill_logger_; }

    void set_log_level(LogLevel level);
    LogLevel log_level() const noexcept { return level_; }

    // Per-destination filtering
    bool should_log_to_file(LogLevel level) const;
    bool should_log_to_syslog(LogLevel level) const;
    bool should_log_to_console(LogLevel level) const;

private:
    quill::Logger* quill_logger_{nullptr};
    LogLevel level_{LogLevel::Info};

    std::shared_ptr<quill::Sink> file_sink_;
    std::shared_ptr<quill::Sink> console_sink_;
    std::shared_ptr<quill::Sink> syslog_sink_;

    LogLevel file_level_{LogLevel::Info};
    LogLevel console_level_{LogLevel::Info};
    LogLevel syslog_level_{LogLevel::Info};
};

} // namespace pubsub_itc_fw
