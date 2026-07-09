// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ios>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <pubsub_itc_fw/ConsoleCapture.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace pubsub_itc_fw {

namespace {

constexpr int fatal_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE};

/// Async-signal-safe write of an entire buffer (retries short writes and EINTR).
void write_all_raw(int fd, const char* data, size_t length) {
    size_t written = 0;
    while (written < length) {
        const ssize_t n = ::write(fd, data + written, length - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        written += static_cast<size_t>(n);
    }
}

/// Async-signal-safe: copy whatever is currently buffered in the pipe to dest_fd
/// without blocking. Used by both crash handlers so the last console output still
/// reaches disk without going through Quill (whose flush is async and unreliable
/// at crash time -- see QuillLogger.hpp).
void drain_pipe_to_fd(int pipe_read_fd, int dest_fd) {
    if (pipe_read_fd < 0 || dest_fd < 0) {
        return;
    }
    const int flags = ::fcntl(pipe_read_fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(pipe_read_fd, F_SETFL, flags | O_NONBLOCK);
    }
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(pipe_read_fd, buffer, sizeof(buffer))) > 0) {
        write_all_raw(dest_fd, buffer, static_cast<size_t>(n));
    }
}

} // namespace

// Internal state. Members are public so the process-global terminate/signal
// handlers (static members below) can reach them.
class ConsoleCapture::Impl {
  public:
    Impl(QuillLogger& logger_reference, const Options& options_value) : logger(logger_reference), options(options_value) {}
    ~Impl();

    QuillLogger& logger;
    Options options;

    int pipe_read_fd = -1;
    int saved_stdout_fd = -1;
    int saved_stderr_fd = -1;
    int crash_dump_fd = -1; //!< Raw crash-dump destination (may alias saved_stderr_fd).

    ThreadWithJoinTimeout reader_thread;

    std::terminate_handler previous_terminate_handler = nullptr;
    bool terminate_handler_installed = false;
    bool fatal_signal_handlers_installed = false;

    void run_reader();
    void log_line(const std::string& line);

    /// The single active instance; the handlers below have no parameter of their own.
    static std::atomic<Impl*> active_instance;

    static void terminate_handler();
    static void signal_handler(int signal_number);
};

std::atomic<ConsoleCapture::Impl*> ConsoleCapture::Impl::active_instance{nullptr};

void ConsoleCapture::Impl::log_line(const std::string& line) {
    const std::string message = options.line_prefix + line;
    PUBSUB_LOG_STR(logger, options.level, message);
}

void ConsoleCapture::Impl::run_reader() {
    std::string pending;
    std::vector<char> buffer(4096);

    while (true) {
        const ssize_t n = ::read(pipe_read_fd, buffer.data(), buffer.size());
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                const char ch = buffer[static_cast<size_t>(i)];
                if (ch == '\n') {
                    log_line(pending);
                    pending.clear();
                } else {
                    pending.push_back(ch);
                    if (pending.size() >= options.max_line_length) {
                        log_line(pending); // force-flush a newline-less spewer
                        pending.clear();
                    }
                }
            }
        } else if (n == 0) {
            break; // all write references to the pipe are gone (fds 1/2 restored)
        } else if (errno != EINTR) {
            break;
        }
    }

    if (!pending.empty()) {
        log_line(pending); // trailing partial line
    }
}

// The terminate handler runs in a normal (non-signal) context, but it still
// bypasses Quill and writes raw, because Quill's flush is async and unreliable at
// process-death time (QuillLogger.hpp, Concern 2).
void ConsoleCapture::Impl::terminate_handler() {
    Impl* self = active_instance.load();
    if (self != nullptr && self->crash_dump_fd >= 0) {
        static const char marker[] = "\nERROR [console-crash] std::terminate: ";
        write_all_raw(self->crash_dump_fd, marker, sizeof(marker) - 1);

        std::exception_ptr active = std::current_exception();
        if (active) {
            try {
                std::rethrow_exception(active);
            } catch (const std::exception& ex) {
                const char* what = ex.what();
                write_all_raw(self->crash_dump_fd, what, std::strlen(what));
            } catch (...) {
                static const char msg[] = "unhandled non-standard exception";
                write_all_raw(self->crash_dump_fd, msg, sizeof(msg) - 1);
            }
        } else {
            static const char msg[] = "no active exception";
            write_all_raw(self->crash_dump_fd, msg, sizeof(msg) - 1);
        }
        write_all_raw(self->crash_dump_fd, "\n", 1);

        drain_pipe_to_fd(self->pipe_read_fd, self->crash_dump_fd);
    }

    if (self != nullptr && self->previous_terminate_handler != nullptr) {
        self->previous_terminate_handler();
    }
    std::abort();
}

void ConsoleCapture::Impl::signal_handler(int signal_number) {
    Impl* self = active_instance.load();
    if (self != nullptr && self->crash_dump_fd >= 0) {
        static const char marker[] = "\nERROR [console-crash] fatal signal captured\n";
        write_all_raw(self->crash_dump_fd, marker, sizeof(marker) - 1);
        drain_pipe_to_fd(self->pipe_read_fd, self->crash_dump_fd);
    }
    // Re-raise with the default disposition so the real crash still happens.
    ::signal(signal_number, SIG_DFL);
    ::raise(signal_number);
}

ConsoleCapture::Impl::~Impl() {
    // Remove handlers first so nothing tries to use us during teardown.
    if (terminate_handler_installed) {
        std::set_terminate(previous_terminate_handler);
    }
    if (fatal_signal_handlers_installed) {
        for (const int signal_number : fatal_signals) {
            ::signal(signal_number, SIG_DFL);
        }
    }

    // Restoring fds 1 and 2 drops the last write references to the pipe, so the
    // reader thread's read() returns 0 (EOF) and it exits after a final drain.
    if (saved_stdout_fd >= 0) {
        std::fflush(stdout);
        ::dup2(saved_stdout_fd, STDOUT_FILENO);
    }
    if (saved_stderr_fd >= 0) {
        std::fflush(stderr);
        ::dup2(saved_stderr_fd, STDERR_FILENO);
    }

    if (reader_thread.joinable()) {
        (void)reader_thread.join_with_timeout(std::chrono::milliseconds(2000));
    }

    if (pipe_read_fd >= 0) {
        ::close(pipe_read_fd);
    }
    if (crash_dump_fd >= 0 && crash_dump_fd != saved_stderr_fd) {
        ::close(crash_dump_fd);
    }
    if (saved_stdout_fd >= 0) {
        ::close(saved_stdout_fd);
    }
    if (saved_stderr_fd >= 0) {
        ::close(saved_stderr_fd);
    }

    Impl* expected = this;
    active_instance.compare_exchange_strong(expected, nullptr);
}

std::unique_ptr<ConsoleCapture> ConsoleCapture::install(QuillLogger& logger) {
    return install(logger, Options{});
}

std::unique_ptr<ConsoleCapture> ConsoleCapture::install(QuillLogger& logger, const Options& options) {
    // Only one instance may own the console at a time.
    if (Impl::active_instance.load() != nullptr) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(logger, options);

    // Read end is close-on-exec (children must not inherit it); the write end is
    // deliberately NOT, so exec'd children keep writing to the captured fds 1/2.
    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return nullptr; // impl destructor is a no-op here (nothing set up yet)
    }
    impl->pipe_read_fd = pipe_fds[0];
    int pipe_write = pipe_fds[1];
    ::fcntl(pipe_write, F_SETFD, 0); // clear close-on-exec on the write end

    // Best-effort enlarge the pipe to make a blocking writer far less likely.
    ::fcntl(pipe_write, F_SETPIPE_SZ, static_cast<int>(options.pipe_capacity_bytes));

    // Save the originals for restore and for the bootstrap/crash destinations.
    impl->saved_stdout_fd = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    impl->saved_stderr_fd = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (impl->saved_stdout_fd < 0 || impl->saved_stderr_fd < 0) {
        ::close(pipe_write);
        return nullptr; // impl destructor closes the read end and any saved fd
    }

    // Force stdout unbuffered so it cannot lag behind unbuffered stderr in the
    // merged pipe -- keeps captured output in chronological order.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::ios_base::sync_with_stdio(true);
    std::fflush(stdout);
    std::fflush(stderr);

    if (::dup2(pipe_write, STDOUT_FILENO) < 0 || ::dup2(pipe_write, STDERR_FILENO) < 0) {
        ::close(pipe_write);
        return nullptr; // impl destructor restores fds (harmless no-op) and closes saved/read fds
    }
    ::close(pipe_write); // fds 1/2 now hold the only write references

    // Crash-dump destination, needed by either handler. Falls back to the
    // original stderr when no dedicated file is configured.
    if (options.install_terminate_handler || options.install_fatal_signal_handlers) {
        if (!options.crash_dump_file_path.empty()) {
            impl->crash_dump_fd = ::open(options.crash_dump_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        }
        if (impl->crash_dump_fd < 0) {
            impl->crash_dump_fd = impl->saved_stderr_fd;
        }
    }

    // Publish before installing handlers / starting the reader so they can find us.
    Impl::active_instance.store(impl.get());

    Impl* raw = impl.get();
    try {
        impl->reader_thread.start([raw]() { raw->run_reader(); });
    } catch (...) {
        return nullptr; // impl destructor restores fds, clears active_instance, closes fds
    }

    if (options.install_terminate_handler) {
        impl->previous_terminate_handler = std::set_terminate(&Impl::terminate_handler);
        impl->terminate_handler_installed = true;
    }

    if (options.install_fatal_signal_handlers) {
        struct sigaction action {};
        action.sa_handler = &Impl::signal_handler;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        for (const int signal_number : fatal_signals) {
            ::sigaction(signal_number, &action, nullptr);
        }
        impl->fatal_signal_handlers_installed = true;
    }

    return std::unique_ptr<ConsoleCapture>(new ConsoleCapture(std::move(impl)));
}

ConsoleCapture::ConsoleCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ConsoleCapture::~ConsoleCapture() = default; // Impl destructor performs the teardown

} // namespace pubsub_itc_fw
