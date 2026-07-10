#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace pubsub_itc_fw {
namespace console_capture_detail {

// Internal helpers behind ConsoleCapture's crash-dump path. They are declared here
// (rather than kept file-local) so they can be exercised directly by unit tests:
// the terminate/signal handlers that use them end the process, so driving them
// through a real crash records no coverage.

/// Async-signal-safe write of an entire buffer (retries short writes and EINTR).
void write_all_raw(int fd, const char* data, size_t length);

/// Async-signal-safe: copy whatever is currently buffered in the pipe to dest_fd
/// without blocking. Used by both crash handlers so the last console output still
/// reaches disk without going through Quill (whose flush is async and unreliable
/// at crash time -- see QuillLogger.hpp).
void drain_pipe_to_fd(int pipe_read_fd, int dest_fd);

/// Writes the std::terminate crash record to crash_dump_fd: a marker, the active
/// exception's text (or a placeholder), then drains the capture pipe. Not
/// async-signal-safe (uses the exception machinery); the terminate handler runs in
/// a normal context.
void write_terminate_crash_dump(int crash_dump_fd, int pipe_read_fd);

/// Writes the fatal-signal crash record to crash_dump_fd: a marker, then drains the
/// capture pipe. Async-signal-safe.
void write_signal_crash_dump(int crash_dump_fd, int pipe_read_fd);

} // namespaces
} // namespaces
