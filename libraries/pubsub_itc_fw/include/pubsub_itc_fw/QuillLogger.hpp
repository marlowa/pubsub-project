#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <quill/Logger.h>
#include <quill/core/LogLevel.h>
#include <quill/sinks/Sink.h>

#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace pubsub_itc_fw {

/*
 * QuillLogger — framework wrapper around a quill::Logger.
 *
 * Two operating modes:
 *
 *   Production:  one file sink (applog) + one syslog sink, each with its own
 *                severity threshold.  File and syslog are never both absent in
 *                production; console is never active in production.
 *
 *   Unit test:   one console sink (applog replacement) with its own severity
 *                threshold.  Syslog is suppressed entirely.  An optional
 *                callback receives each fully formatted log record that passes
 *                the applog threshold, allowing tests to assert on log output.
 *
 * The Quill logger gate is set to min(applog_level, syslog_level) so that
 * neither sink is starved by the gate.  Per-destination filtering is owned
 * by each sink.
 *
 * Any class that needs to log receives a QuillLogger& in its constructor and
 * stores it as a member.
 *
 * Format string safety:
 *   In C++17, format string mismatches between the fmt argument and the
 *   variadic argument list are not caught at compile time -- this requires
 *   C++20's consteval. At runtime, Quill's backend catches format errors
 *   inside _populate_formatted_log_message and emits them as a normal log
 *   record containing "[Could not format log statement. ...]". This record
 *   is visible in the applog file and console sink. Note that the
 *   error_notifier in BackendOptions is NOT called for format errors in
 *   Quill 11.0.2 -- it is only used for queue failures and backend exceptions.
 *   See LoggingMacros.hpp for the full explanation and coding rules.
 *
 * Flush behaviour and known limitations:
 *   Quill 11.x removed per-severity and per-statement flush. All log records
 *   are processed asynchronously by the backend thread and flushed according
 *   to the sink's flush interval. There is no way to guarantee that an
 *   ERROR or CRITICAL record has been flushed to disk at the point the
 *   calling thread returns from the log macro.
 *
 *   Concern 1 -- visibility latency: In practice the Quill backend wakes up
 *   frequently enough that error records appear in the log file within
 *   milliseconds. For most operational purposes this is acceptable. The
 *   immediate-flush API (logger->set_immediate_flush()) exists but blocks
 *   the frontend thread and is not recommended on reactor or application
 *   threads in a low-latency system.
 *
 *   Concern 2 -- unexpected termination (SIGSEGV, SIGABRT etc.): If the
 *   process receives a fatal signal, the Quill backend thread may not have
 *   processed and flushed the final error records before the process dies.
 *   The natural fix would be to install a fatal signal handler that calls
 *   quill::Backend::stop() before re-raising the signal, but this has caused
 *   serious stability problems in practice and is not done here. As a result,
 *   the last few log records before a crash may be lost. This is a known
 *   limitation of using Quill 11.x in a process that can die unexpectedly.
 */

/** @ingroup logging_subsystem */
class QuillLogger {
  public:
    /// Callback type used in unit-test mode.  Receives a fully formatted log
    /// record string for each record that passes the applog threshold.
    using LogCallback = std::function<void(const std::string&)>;

    ~QuillLogger();

    QuillLogger(const QuillLogger&) = delete;
    QuillLogger& operator=(const QuillLogger&) = delete;
    QuillLogger(QuillLogger&&) = delete;
    QuillLogger& operator=(QuillLogger&&) = delete;

    /**
     * @brief Production constructor.  Creates a file sink and a syslog sink.
     * @param file_path     [in] Path to the applog file.
     * @param file_mode     [in] Truncate or append.
     * @param applog_level  [in] Minimum severity written to the applog file.
     * @param syslog_level  [in] Minimum severity written to syslog.
     */
    QuillLogger(const std::string& file_path, FileOpenMode file_mode, FwLogLevel applog_level, FwLogLevel syslog_level);

    /**
     * @brief Unit-test constructor.  Creates a console sink; syslog is suppressed.
     * @param applog_level  [in] Minimum severity written to the console.
     * @param callback      [in] Optional.  Called with the fully formatted string
     *                          for every record that passes applog_level.  Pass
     *                          nullptr to receive no callbacks.
     */
    explicit QuillLogger(FwLogLevel applog_level, LogCallback callback = nullptr);

    /**
     * @brief Blocks SIGINT and SIGTERM on the calling thread.
     *
     * Must be called once on the main thread before constructing any
     * QuillLogger.  All threads spawned after this call inherit the blocked
     * signal mask, so the Quill backend thread will not receive SIGINT or
     * SIGTERM directly.  The Reactor consumes those signals via signalfd.
     */
    static void block_signals_before_construction();

    /**
     * @brief Ensures the given log file path is writable before constructing
     *        a QuillLogger.
     *
     * Creates any missing parent directories using FileSystemUtils::make_directories,
     * then attempts to open the file for writing to verify it is accessible.
     * This must be called before constructing a QuillLogger so that any failure
     * can be reported to the console -- once the logger is constructed there is
     * no console fallback.
     *
     * @param[in] file_path Path to the log file.
     * @return An empty string on success, or a human-readable error description
     *         on failure. The caller should print the error to stderr and exit.
     */
    [[nodiscard]] static std::string ensure_log_file_writable(const std::string& file_path);

    /// @brief Returns the underlying quill::Logger pointer (used by the macros).
    [[nodiscard]] quill::Logger* quill_logger() const {
        return quill_logger_;
    }

    /**
     * @brief Sets the applog severity threshold at runtime.
     *
     * Typically called once after the config file has been read to apply the
     * configured log level. Safe to call from any thread.
     *
     * @param[in] level New minimum severity for the applog sink.
     */
    void set_log_level(FwLogLevel level);

    /**
     * @brief Sets the syslog severity threshold at runtime.
     *
     * Typically called once after the config file has been read. If the config
     * does not specify a syslog level, this method should not be called and
     * the default (Info) remains in effect. Safe to call from any thread.
     *
     * @param[in] level New minimum severity for the syslog sink.
     */
    void set_syslog_level(FwLogLevel level);

    /**
     * @brief Returns the current applog severity threshold.
     */
    [[nodiscard]] FwLogLevel log_level() const {
        return applog_level_;
    }

    /**
     * @brief Returns the current syslog severity threshold.
     */
    [[nodiscard]] FwLogLevel syslog_level() const {
        return syslog_level_;
    }

  private:
    static std::atomic<uint64_t> instance_counter_;

    static std::string generate_unique_logger_name(const std::string& prefix);
    static std::string generate_unique_sink_name(const std::string& prefix);

    std::shared_ptr<quill::Sink> applog_sink_;
    std::shared_ptr<quill::Sink> syslog_sink_;
    std::shared_ptr<quill::Sink> callback_sink_;

    FwLogLevel applog_level_;
    FwLogLevel syslog_level_;

    quill::Logger* quill_logger_{nullptr};
};

} // namespace pubsub_itc_fw
