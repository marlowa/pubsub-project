#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

namespace order_gateway {

/**
 * @brief Captures inbound and outbound FIX wire bytes to a binary file.
 *
 * The gateway thread calls capture() with no file I/O: the record is placed
 * on an internal queue.  A background writer thread drains the queue and
 * writes to disk.
 *
 * Binary record format (little-endian):
 *   uint32_t  payload_size    -- byte count of the raw FIX data
 *   int64_t   timestamp_ns    -- nanoseconds since Unix epoch (wall clock)
 *   uint8_t   direction       -- 0 = inbound, 1 = outbound
 *   uint8_t   data[...]       -- raw FIX wire bytes
 *
 * The file is opened for writing on construction (existing content is
 * truncated) and flushed and closed on destruction.  All queued records
 * are drained before the destructor returns.
 */
class FixCapture {
  public:
    /**
     * @brief Direction of a captured FIX message.
     */
    enum class Direction : uint8_t {
        Inbound  = 0,
        Outbound = 1
    };

    /**
     * @param[in] file_path    Path to the capture output file.
     * @param[in] logger       Logger for error messages. Must outlive this object.
     * @param[in] queue_depth  Maximum number of records the queue may hold before
     *                         new records are dropped with a warning.
     */
    FixCapture(const std::string& file_path, pubsub_itc_fw::QuillLogger& logger, size_t queue_depth);

    ~FixCapture();

    FixCapture(const FixCapture&) = delete;
    FixCapture& operator=(const FixCapture&) = delete;

    /**
     * @brief Enqueue a FIX wire record for capture.
     *
     * Called from the gateway thread.  Never performs file I/O.
     * If the internal queue exceeds the drop threshold, the record is
     * silently dropped and a warning is logged.
     *
     * @param[in] direction    Inbound (from FIX client) or Outbound (to FIX client).
     * @param[in] data         Pointer to raw FIX wire bytes.
     * @param[in] size         Number of bytes.
     * @param[in] timestamp_ns Nanoseconds since Unix epoch at time of capture.
     */
    void capture(Direction direction, const uint8_t* data, size_t size, int64_t timestamp_ns);

  private:
    struct Record {
        int64_t timestamp_ns;
        Direction direction;
        std::vector<uint8_t> bytes;
    };

    void writer_loop();

    std::string file_path_;
    pubsub_itc_fw::QuillLogger& logger_;
    size_t queue_depth_;
    std::vector<Record> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
    std::thread writer_thread_;
};

} // namespace order_gateway
