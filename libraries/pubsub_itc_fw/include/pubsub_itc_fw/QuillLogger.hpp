#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <memory>

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/Sink.h>
#include <quill/core/LogLevel.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggerUtils.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>

namespace pubsub_itc_fw {

/** @ingroup logging_subsystem */

class QuillLogger {
public:
    /**
    * @brief Blocks SIGINT and SIGTERM on the calling thread before any threads
    *        are spawned, including the quill backend thread.
    *
    * Must be called once on the main thread before constructing any QuillLogger
    * instance. All threads spawned after this call inherit the blocked signal
    * mask. The Reactor consumes these signals via signalfd.
    *
    * Failure to call this before constructing a QuillLogger will result in the
    * quill backend thread having an unblocked signal mask, causing SIGINT or
    * SIGTERM to terminate the process immediately via the default handler rather
    * than being handled gracefully by the Reactor.
    */
    static void block_signals_before_construction();

    ~QuillLogger();

    QuillLogger(const std::string& file_path,
                         FileOpenMode file_mode,
                         FwLogLevel file_level,
                         FwLogLevel syslog_level,
                         FwLogLevel console_level);

    explicit QuillLogger(); // for unit tests

    QuillLogger(const std::string& logger_name, std::shared_ptr<quill::Sink> test_sink,
                         FwLogLevel log_level = FwLogLevel::Debug);

    quill::Logger* quill_logger() const { return quill_logger_; }

    void set_log_level(FwLogLevel level);
    FwLogLevel log_level() const { return level_; }

    // Per-destination filtering
    bool should_log_to_file(FwLogLevel level) const;
    bool should_log_to_syslog(FwLogLevel level) const;
    bool should_log_to_console(FwLogLevel level) const;

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
    FwLogLevel level_{FwLogLevel::Info};

    FwLogLevel file_level_{FwLogLevel::Info};
    FwLogLevel console_level_{FwLogLevel::Info};
    FwLogLevel syslog_level_{FwLogLevel::Info};

    quill::Logger* quill_logger_{nullptr};
};

} // namespace pubsub_itc_fw
