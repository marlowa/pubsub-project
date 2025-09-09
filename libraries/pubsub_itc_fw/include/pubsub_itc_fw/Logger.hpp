#pragma once

#include <sstream>
#include <string>
#include <vector>

#include <quill/Logger.h>
// TODO #include <quill/bundled/fmt/format.h>

#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>

namespace pubsub_itc_fw {

// Note: Although this class is not a blatent singleton, it does assume that we will only have
// one logger object per thread. This is due to use of thread local storage to hold a buffer
// into which the record to be logged is formatted.

class Logger : public LoggerInterface {
  public:
    ~Logger() = default;

    Logger(LogLevel log_level, const std::string& log_directory, const std::string& filename, FilenameAppendMode append_mode, int rolling_size);

    bool should_log(LogLevel log_level) const override {
        return log_level <= log_level_;
    }

    void log(LogLevel log_level, const char* filename, int line_number, const char* function_name) const override;

    std::vector<char>& get_tls_buffer() const override {
        return tls_buffer_;
    }

    void flush() const override;

    void set_log_level(LogLevel log_level) override;

    void set_immediate_flush() override;

  private:
    LogLevel log_level_{LogLevel::Info};
    std::string log_directory_;
    static thread_local std::vector<char> tls_buffer_;
    quill::Logger* quill_logger_{nullptr};
    std::shared_ptr<quill::Sink> sink_;
};

} // namespace pubsub_itc_fw
