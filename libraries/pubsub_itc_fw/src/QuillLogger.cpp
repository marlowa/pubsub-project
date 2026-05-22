// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/backend/PatternFormatter.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/RotatingFileSink.h>
#include <quill/sinks/Sink.h>
#include <quill/sinks/SyslogSink.h>

#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggerUtils.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/utils/FileSystemUtils.hpp>

/*
TODO this quill logger area needs serious attention, we need a way to get the thread id of the logger backend thread.

The backend thread consumes too much CPU

By default, the backend thread runs in a tight loop polling the queues for new messages, which can consume CPU cycles even when idle.
We need something like the below:

void pin_logger_backend() {
    // Get the kernel thread ID (LWP) from Quill
    uint32_t tid = quill::Backend::get_thread_id();

    if (tid != 0) {
        cpu_set_set cpuset;
        CPU_ZERO(&cpuset);

        // Example: Pin to Core 1, strictly excluding Core 0
        CPU_SET(1, &cpuset);

        if (sched_setaffinity(static_cast<pid_t>(tid), sizeof(cpu_set_t), &cpuset) == -1) {
            // Handle error (e.g., log to stderr as the logger might not be ready)
            perror("sched_setaffinity");
        }
    }
}

since the backend thread MUST be core pinned we should add the core number to the QuillLogger constructor.
*/

namespace {

constexpr char const* log_pattern = "%(time) [%(thread_id)] %(thread_name:<24) %(short_source_location:<28) %(log_level:<10) %(logger:<16) %(message)";
constexpr char const* time_format = "%Y-%m-%d %H:%M:%S.%Qns";

std::once_flag backend_started;

void ensure_backend_started() {
    std::call_once(backend_started, []() { quill::Backend::start(quill::BackendOptions{}); });
}

quill::PatternFormatterOptions make_pattern_formatter_options() {
    quill::PatternFormatterOptions options;
    options.format_pattern = log_pattern;
    options.timestamp_pattern = time_format;
    options.timestamp_timezone = quill::Timezone::LocalTime;
    return options;
}

/*
 * CallbackSink — a Quill sink that forwards each fully formatted log record
 * to a user-supplied callback.  Used in unit-test mode so that tests can
 * assert on the contents of logged records without touching the filesystem
 * or syslog.
 */
class CallbackSink : public quill::Sink {
  public:
    explicit CallbackSink(pubsub_itc_fw::QuillLogger::LogCallback callback) : callback_(std::move(callback)) {}

    void write_log(quill::MacroMetadata const* /*metadata*/, uint64_t /*log_timestamp*/, std::string_view /*thread_id*/, std::string_view /*thread_name*/,
                   std::string const& /*process_id*/, std::string_view /*logger_name*/, quill::LogLevel /*log_level*/,
                   std::string_view /*log_level_description*/, std::string_view /*log_level_short_code*/,
                   std::vector<std::pair<std::string, std::string>> const* /*named_args*/, std::string_view /*log_message*/,
                   std::string_view log_statement) override {
        if (callback_ != nullptr) {
            callback_(std::string{log_statement});
        }
    }

    void flush_sink() override {}

  private:
    pubsub_itc_fw::QuillLogger::LogCallback callback_;
};

} // namespace

namespace pubsub_itc_fw {

std::atomic<uint64_t> QuillLogger::instance_counter_{0};

std::string QuillLogger::generate_unique_logger_name(const std::string& prefix) {
    const uint64_t id = instance_counter_.fetch_add(1, std::memory_order_relaxed);
    return prefix + "_logger_" + std::to_string(id);
}

std::string QuillLogger::generate_unique_sink_name(const std::string& prefix) {
    const uint64_t id = instance_counter_.fetch_add(1, std::memory_order_relaxed);
    return prefix + "_sink_" + std::to_string(id);
}

void QuillLogger::block_signals_before_construction() {
    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGTERM);
    ::sigaddset(&mask, SIGINT);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

std::string QuillLogger::ensure_log_file_writable(const std::string& file_path) {
    // Extract the parent directory from the file path.
    const size_t last_slash = file_path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        const std::string parent_dir = file_path.substr(0, last_slash);
        const std::string dir_error = FileSystemUtils::make_directories(parent_dir);
        if (!dir_error.empty()) {
            return "QuillLogger::ensure_log_file_writable: " + dir_error;
        }
    }

    // Attempt to open the file for writing to verify it is accessible.
    std::ofstream test_file(file_path, std::ios::app);
    if (!test_file.is_open()) {
        return "QuillLogger::ensure_log_file_writable: cannot open '" + file_path + "' for writing";
    }

    return "";
}

QuillLogger::~QuillLogger() = default;

QuillLogger::QuillLogger(const std::string& file_path, FileOpenMode file_mode, FwLogLevel applog_level, FwLogLevel syslog_level,
                         const RollingLogfileConfiguration& rollingLogfileConfiguration)
    : applog_level_{applog_level}, syslog_level_{syslog_level}, m_rollingLogfileConfiguration(rollingLogfileConfiguration) {
    ensure_backend_started();

    quill::FileSinkConfig file_config;
    file_config.set_open_mode(file_mode == FileOpenMode(FileOpenMode::Append) ? 'a' : 'w');
    if (m_rollingLogfileConfiguration.mode == RollingLogfileConfiguration::Mode::Size) {
        applog_sink_ = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(file_path, [file_mode, this]() {
            quill::RotatingFileSinkConfig cfg;
            cfg.set_open_mode(file_mode == FileOpenMode(FileOpenMode::Append) ? 'a' : 'w');
            cfg.set_rotation_max_file_size(m_rollingLogfileConfiguration.max_file_size);
            cfg.set_max_backup_files(m_rollingLogfileConfiguration.max_backup_files);
            return cfg;
        }());
    } else if (m_rollingLogfileConfiguration.mode == RollingLogfileConfiguration::Mode::Daily) {
        applog_sink_ = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(file_path, [file_mode, this]() {
            quill::RotatingFileSinkConfig cfg;
            cfg.set_open_mode(file_mode == FileOpenMode(FileOpenMode::Append) ? 'a' : 'w');
            cfg.set_rotation_time_daily(m_rollingLogfileConfiguration.rotation_time);
            cfg.set_max_backup_files(m_rollingLogfileConfiguration.max_backup_files);
            return cfg;
        }());
    } else {
        applog_sink_ = quill::Frontend::create_or_get_sink<quill::FileSink>(file_path, file_config, quill::FileEventNotifier{});
    }

    quill::PatternFormatterOptions formatter_options = make_pattern_formatter_options();

    quill::SyslogSinkConfig syslog_config;
    syslog_config.set_identifier("pubsub_app");
    syslog_config.set_facility(LOG_USER);
    syslog_config.set_options(LOG_PID);
    syslog_config.set_format_message(false);
    syslog_sink_ = quill::Frontend::create_or_get_sink<quill::SyslogSink>(generate_unique_sink_name("syslog"), syslog_config);

    applog_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(applog_level));
    syslog_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(syslog_level));

    const quill::LogLevel gate = applog_level <= syslog_level ? LoggerUtils::to_quill_log_level(applog_level) : LoggerUtils::to_quill_log_level(syslog_level);

    quill_logger_ = quill::Frontend::create_or_get_logger(generate_unique_logger_name("quillLog"), {applog_sink_, syslog_sink_}, formatter_options);
    quill_logger_->set_log_level(gate);
}

QuillLogger::QuillLogger(FwLogLevel applog_level, LogCallback callback) : applog_level_{applog_level}, syslog_level_{applog_level} {
    ensure_backend_started();

    applog_sink_ = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(generate_unique_sink_name("console"));
    applog_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(applog_level));

    std::vector<std::shared_ptr<quill::Sink>> sinks{applog_sink_};

    if (callback != nullptr) {
        auto cb_sink = quill::Frontend::create_or_get_sink<CallbackSink>(generate_unique_sink_name("callback"), std::move(callback));
        cb_sink->set_log_level_filter(LoggerUtils::to_quill_log_level(applog_level));
        callback_sink_ = cb_sink;
        sinks.push_back(std::move(cb_sink));
    }

    quill_logger_ = quill::Frontend::create_or_get_logger(generate_unique_logger_name("pubsub_test"), sinks, make_pattern_formatter_options());
    quill_logger_->set_log_level(LoggerUtils::to_quill_log_level(applog_level));
    quill_logger_->set_immediate_flush();
}

void QuillLogger::set_log_level(FwLogLevel level) {
    applog_level_ = level;
    if (applog_sink_ != nullptr) {
        applog_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(level));
    }
    if (callback_sink_ != nullptr) {
        callback_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(level));
    }
    if (quill_logger_ != nullptr) {
        // Recompute the gate as the minimum of applog and syslog thresholds.
        const quill::LogLevel gate =
            applog_level_ <= syslog_level_ ? LoggerUtils::to_quill_log_level(applog_level_) : LoggerUtils::to_quill_log_level(syslog_level_);
        quill_logger_->set_log_level(gate);
    }
}

void QuillLogger::set_syslog_level(FwLogLevel level) {
    syslog_level_ = level;
    if (syslog_sink_ != nullptr) {
        syslog_sink_->set_log_level_filter(LoggerUtils::to_quill_log_level(level));
    }
    if (quill_logger_ != nullptr) {
        // Recompute the gate as the minimum of applog and syslog thresholds.
        const quill::LogLevel gate =
            applog_level_ <= syslog_level_ ? LoggerUtils::to_quill_log_level(applog_level_) : LoggerUtils::to_quill_log_level(syslog_level_);
        quill_logger_->set_log_level(gate);
    }
}

} // namespace pubsub_itc_fw
