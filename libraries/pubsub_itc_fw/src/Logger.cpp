#include <stdexcept>
#include <string>
#include <utility>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <pubsub_itc_fw/Logger.hpp>

namespace pubsub_itc_fw {

Logger::Logger(LogLevel log_level,
               const std::string& log_directory,
               const std::string& filename,
               FilenameAppendMode append_mode,
               int rolling_size)
    : log_level_(log_level),
      log_directory_(log_directory)
{
    if (!boost::filesystem::exists(log_directory_)) {
        boost::filesystem::create_directories(log_directory_);
    }

    quill::Backend::start();

    const auto full_path = (boost::filesystem::path(log_directory_) / filename).string();

    quill::FilenameAppendOption append_option = quill::FilenameAppendOption::None;
    if (append_mode == FilenameAppendMode::StartDateTime) {
        append_option = quill::FilenameAppendOption::StartDateTime;
    }

    if (rolling_size == 0) {
        sink_ = quill::Frontend::create_or_get_sink<quill::FileSink>(
            full_path,
            [append_option] {
                quill::FileSinkConfig cfg;
                cfg.set_open_mode('w');
                cfg.set_filename_append_option(append_option);
                return cfg;
            }()
        );
    } else {
        sink_ = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
            full_path,
            [rolling_size, append_option] {
                quill::RotatingFileSinkConfig cfg;
                cfg.set_open_mode('w');
                cfg.set_filename_append_option(append_option);
                cfg.set_rotation_time_daily("00:00");
                cfg.set_rotation_max_file_size(rolling_size);
                return cfg;
            }()
        );
    }

    const std::string logger_name = "logger_" + filename;

    quill_logger_ = quill::Frontend::create_or_get_logger(
        logger_name,
        sink_,
        quill::PatternFormatterOptions{
            "%(time) [%(thread_id)] LOG_%(log_level:<6) %(logger:<1) %(message)",
            "%F %H:%M:%S.%Qns",
            quill::Timezone::GmtTime
        }
    );

    quill_logger_->set_log_level(quill::LogLevel::Debug);
}

} // namespace pubsub_itc_fw
