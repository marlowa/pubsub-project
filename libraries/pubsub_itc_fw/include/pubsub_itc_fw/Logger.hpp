#pragma once

#include <memory>
#include <string>

#include <quill/Logger.h>
#include <quill/sinks/Sink.h>

#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/LogLevel.hpp>

namespace pubsub_itc_fw {

class Logger : public LoggerInterface {
public:
    Logger(LogLevel log_level,
           const std::string& log_directory,
           const std::string& filename,
           FilenameAppendMode append_mode,
           int rolling_size);

    ~Logger() override = default;

    bool should_log(LogLevel log_level) const override {
        return log_level <= log_level_;
    }

    void set_log_level(LogLevel log_level) override {
        log_level_ = log_level;
    }

    void flush() const override {
        quill_logger_->flush_log();
    }

    void set_immediate_flush() override {
        quill_logger_->set_immediate_flush();
    }

    quill::Logger* quill_logger() const noexcept {
        return quill_logger_;
    }

private:
    LogLevel log_level_{LogLevel::Info};
    std::string log_directory_;
    quill::Logger* quill_logger_{nullptr};
    std::shared_ptr<quill::Sink> sink_;
};

} // namespace pubsub_itc_fw
