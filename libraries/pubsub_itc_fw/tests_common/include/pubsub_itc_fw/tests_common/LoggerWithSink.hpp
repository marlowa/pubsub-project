#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw::tests_common {

/*
 * LoggerWithSink -- a QuillLogger wired to an in-memory record store for use
 * in unit and integration tests.
 *
 * Construct with a desired applog threshold.  The logger is in unit-test mode:
 * output goes to the console and every record that passes the threshold is also
 * delivered to the callback, which stores the fully formatted string.
 *
 * The backend thread delivers records after the log macro returns. The helper
 * methods below flush first, so they see everything logged before they were called.
 *
 * Records are written by Quill's backend thread and read by the test's thread, often
 * while other threads are still logging. So the store takes no lock and is never
 * resized: it has room for a fixed number of records, set aside at construction. The
 * backend thread, the only writer, fills the next slot and then publishes the new count
 * with a release store; a reader loads the count with an acquire load and reads only
 * the slots below it, which the writer never touches again. clear() moves the point
 * the readers start from, rather than erasing anything. A test that logs more than the
 * store holds is told so by the helper methods, rather than having records go missing.
 */
class LoggerWithSink {
  public:
    /// How many records the store holds, which no test comes near.
    static constexpr size_t capacity = 1U << 17U;

    explicit LoggerWithSink(FwLogLevel level = FwLogLevel::Debug) : slots_(capacity), logger(level, [this](const std::string& record) { store(record); }) {}

    LoggerWithSink(const LoggerWithSink&) = delete;
    LoggerWithSink& operator=(const LoggerWithSink&) = delete;
    LoggerWithSink(LoggerWithSink&&) = delete;
    LoggerWithSink& operator=(LoggerWithSink&&) = delete;

    /// Forgets every record delivered so far; later helper calls see only records delivered after this.
    void clear() {
        logger.flush();
        read_from_ = written_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool contains_message(const std::string& text) {
        for (const auto& record : records()) {
            if (record.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] size_t count() {
        return records().size();
    }

    /// A copy of every record delivered since construction or the last clear(), after a flush.
    [[nodiscard]] std::vector<std::string> records() {
        logger.flush();
        if (dropped_.load(std::memory_order_acquire) != 0) {
            throw PubSubItcException("LoggerWithSink: the test logged more records than the store holds, so some are missing");
        }
        const size_t written = written_.load(std::memory_order_acquire);
        return std::vector<std::string>(slots_.begin() + static_cast<std::ptrdiff_t>(read_from_), slots_.begin() + static_cast<std::ptrdiff_t>(written));
    }

  private:
    // Called on Quill's backend thread only.
    void store(const std::string& record) {
        const size_t index = written_.load(std::memory_order_relaxed);
        if (index >= capacity) {
            dropped_.fetch_add(1, std::memory_order_release);
            return;
        }
        slots_[index] = record;
        written_.store(index + 1, std::memory_order_release);
    }

    // Declared before the logger, so that they are destroyed after it: the logger's destructor
    // flushes, delivering any record still pending into this store.
    std::vector<std::string> slots_;
    std::atomic<size_t> written_{0};
    std::atomic<size_t> dropped_{0};
    // Read and written only by the test's thread.
    size_t read_from_{0};

  public:
    QuillLogger logger;
};

} // namespaces
