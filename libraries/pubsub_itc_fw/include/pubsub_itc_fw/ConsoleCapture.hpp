#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <memory>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace pubsub_itc_fw {

class QuillLogger;

/**
 * @brief Captures everything written to stdout/stderr and logs it via Quill.
 *
 * @ingroup logging_subsystem
 *
 * Redirects file descriptors 1 and 2 into a single pipe (fd level, so it catches
 * C `printf`, libc, the runtime's `std::terminate` message, and libraries that
 * write directly to the console -- none of which a C++ streambuf redirect would
 * see). A reader thread splits the pipe into newline-delimited records and logs
 * each one, at ERROR by default, through the supplied QuillLogger. See
 * docs/design/console_capture.md for the full design and rationale.
 *
 * Ordering: stdout is forced unbuffered at install time so it cannot lag behind
 * unbuffered stderr in the merged pipe -- captured output stays in chronological
 * order, so the context that preceded an error is always visible before it.
 *
 * Activation is opt-in and belongs in an application's main(), AFTER the
 * QuillLogger is constructed and BEFORE other threads start. It must not be
 * installed in unit tests, whose QuillLogger writes to the console on purpose.
 *
 * The QuillLogger passed in must NOT have a console sink active, or captured
 * output would loop back into the pipe. The production QuillLogger is file-only,
 * which satisfies this.
 *
 * Single-instance: owns process-global state (fds 1/2, the terminate handler and,
 * if enabled, fatal-signal handlers), so at most one may exist at a time.
 */
class ConsoleCapture {
  public:
    struct Options {
        size_t pipe_capacity_bytes = 1U << 20;  //!< Requested pipe size (best effort).
        size_t max_line_length = 8U << 10;      //!< Force-flush a newline-less line at this length.
        FwLogLevel level = FwLogLevel::Error;   //!< Any console write is a defect, so ERROR.
        std::string line_prefix = "[console] "; //!< Tag prepended to each captured line in the log.

        //! Install a std::terminate handler that drains the pipe and flushes the
        //! log before the process ends. Not a signal handler; safe to log from.
        bool install_terminate_handler = true;

        //! Install SIGSEGV/SIGABRT/SIGBUS/SIGFPE handlers that drain the pipe and
        //! write its raw bytes (async-signal-safe, never touching Quill) to the
        //! crash-dump file. Off by default: QuillLogger.hpp records that an earlier
        //! Quill-flushing signal handler destabilised the process; this raw-write
        //! approach avoids that, but stays opt-in.
        bool install_fatal_signal_handlers = false;

        //! Destination for the fatal-signal raw dump (only used when the above is
        //! true). Opened append-only at install time and held for the handler.
        std::string crash_dump_file_path;
    };

    /**
     * @brief Installs console capture. Call once, early in main().
     *
     * @param[in] logger  The application logger. Must be file-only (no console sink).
     * @param[in] options Tuning; defaults are the recommended production settings.
     * @return An owning handle on success (capture is active until it is
     *         destroyed), or nullptr if the pipe/dup setup failed -- in which case
     *         the real console is untouched and the caller may report the failure
     *         there.
     */
    [[nodiscard]] static std::unique_ptr<ConsoleCapture> install(QuillLogger& logger, const Options& options);

    /// Overload using default Options (block, ERROR level, terminate handler on).
    [[nodiscard]] static std::unique_ptr<ConsoleCapture> install(QuillLogger& logger);

    /// Restores fds 1 and 2, stops the reader thread after a final drain, and
    /// removes any handlers this instance installed.
    ~ConsoleCapture();

    ConsoleCapture(const ConsoleCapture&) = delete;
    ConsoleCapture& operator=(const ConsoleCapture&) = delete;
    ConsoleCapture(ConsoleCapture&&) = delete;
    ConsoleCapture& operator=(ConsoleCapture&&) = delete;

  private:
    class Impl;
    explicit ConsoleCapture(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace pubsub_itc_fw
