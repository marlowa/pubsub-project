#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Abstract base for console capture: assembles a raw byte stream into
 *        newline-delimited log records.
 *
 * @ingroup logging_subsystem
 *
 * The line-assembly logic lives here, separated from the file-descriptor, pipe,
 * reader-thread and signal-handler machinery of ConsoleCapture, so it can be
 * exercised in isolation. A subclass implements log_line() to decide what happens
 * to each completed record: the production ConsoleCapture logs it via Quill; a
 * unit test overrides log_line() to intercept records and drives the assembly by
 * calling feed_bytes()/flush_pending() directly, with no real console involved.
 */
class ConsoleCaptureInterface {
  public:
    virtual ~ConsoleCaptureInterface() = default;

  protected:
    /**
     * @param[in] max_line_length Force-flush a newline-less run as one record once
     *        the pending line reaches this length, so a spewer with no newlines
     *        cannot grow the buffer without bound.
     */
    explicit ConsoleCaptureInterface(size_t max_line_length);

    /**
     * @brief Consumes a run of raw bytes, emitting one record per newline.
     *
     * Partial-line state is retained across calls, so a line split over several
     * feed_bytes() calls is assembled and emitted once, when its newline arrives.
     *
     * @param[in] data   Bytes to consume; not required to be newline-terminated.
     * @param[in] length Number of bytes at @p data.
     */
    void feed_bytes(const char* data, size_t length);

    /// Emits any buffered partial (newline-less) line as a final record. Call at
    /// end of stream.
    void flush_pending();

    /**
     * @brief Handles one completed log record.
     *
     * @param[in] line The record, without its trailing newline.
     */
    virtual void log_line(const std::string& line) = 0;

  private:
    std::string pending_line_{};
    size_t max_line_length_;
};

} // namespaces
