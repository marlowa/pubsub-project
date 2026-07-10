#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <memory>
#include <string>

#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>
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
 *
 * The crash-dump file: when the terminate handler or the optional fatal-signal
 * handlers are enabled, the last bytes still buffered in the capture pipe are
 * written, as raw async-signal-safe output, to a dedicated crash-dump file the
 * moment the process is dying. That path never goes through Quill, whose flush is
 * asynchronous and unreliable at crash time (see QuillLogger.hpp), so the console
 * output that immediately preceded a crash still reaches disk. The file is named
 * by Options::crash_dump_file_path; when that is empty the handlers fall back to
 * writing the dump to the process's original stderr.
 */
class ConsoleCapture : public ConsoleCaptureInterface {
    class Impl; // implementation detail; defined in ConsoleCapture.cpp

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

        //! Path of the crash-dump file (see the class overview): the plain file the
        //! terminate and fatal-signal handlers write the pipe's final buffered bytes
        //! to, as raw output, while the process is dying. Only consulted when a
        //! terminate or fatal-signal handler is installed. Opened append-only at
        //! install time and held open for the handlers. When empty, the handlers fall
        //! back to the process's original stderr.
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

    /**
     * @brief RAII handle for a capture engine installed via install_engine().
     *
     * Restores the console (fds 1 and 2, handlers) when destroyed, exactly as the
     * ConsoleCapture destructor does. Returned by value and consumed in place, so
     * it is neither copyable nor movable.
     */
    class EngineHandle {
      public:
        ~EngineHandle();

        EngineHandle(const EngineHandle&) = delete;
        EngineHandle& operator=(const EngineHandle&) = delete;

        /// True if the capture engine was installed (setup succeeded).
        [[nodiscard]] bool installed() const {
            return impl_ != nullptr;
        }

      private:
        friend class ConsoleCapture;
        explicit EngineHandle(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };

    /**
     * @brief Installs the capture engine feeding a custom sink instead of a Quill
     *        logger.
     *
     * Uses the same file-descriptor, pipe, reader-thread and handler machinery as
     * install(); only the record destination differs -- completed records go to
     * @p sink.log_line() rather than through Quill.
     *
     * @param[in] sink    Receives each completed record; must outlive the handle.
     * @param[in] options Tuning. `level` and `line_prefix` are ignored -- the sink
     *                    decides what to do with each record.
     * @return A handle whose installed() is true on success. The console is
     *         restored when it is destroyed.
     */
    [[nodiscard]] static EngineHandle install_engine(ConsoleCaptureInterface& sink, const Options& options);

    /// Restores fds 1 and 2, stops the reader thread after a final drain, and
    /// removes any handlers this instance installed.
    ~ConsoleCapture();

    ConsoleCapture(const ConsoleCapture&) = delete;
    ConsoleCapture& operator=(const ConsoleCapture&) = delete;
    ConsoleCapture(ConsoleCapture&&) = delete;
    ConsoleCapture& operator=(ConsoleCapture&&) = delete;

  private:
    ConsoleCapture(QuillLogger& logger, const Options& options);

    void log_line(const std::string& line) override;

    static std::unique_ptr<Impl> create_engine(ConsoleCaptureInterface& sink, const Options& options);

    QuillLogger& logger_;
    FwLogLevel level_;
    std::string line_prefix_;
    std::unique_ptr<Impl> impl_{};
};

} // namespaces
