#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pthread.h>

#include <optional>
#include <string>

#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/BackgroundRecorder.hpp>
#include <pubsub_itc_fw/LeasePromiseRecord.hpp>
#include <pubsub_itc_fw/LeasePromiseRecorderInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Writes lease promise records on a thread of its own, so that the thread handling leases never waits for the disk.
 *
 * A promise record is written, and synced, before the vote it records is given. Writing and syncing can take seconds on a
 * slow disk. The sequencer and the matching engine handle leases on the same thread that handles orders, and that thread
 * must never wait for the disk or take a lock (principles F2 and F4 in docs/design_principles.md). So this recorder writes
 * on its own thread, with BackgroundRecorder, and the thread handling leases only starts a write and later reads its
 * outcome. Neither ever waits.
 *
 * Use: one thread, the one that handles leases, calls every member function. The writer thread is started when this is
 * constructed and stopped when it is destroyed.
 */
class BackgroundPromiseRecorder : public BackgroundPromiseRecorderInterface {
  public:
    /**
     * @brief Stops the writer thread, waiting up to a second for a write in progress to finish (BackgroundRecorder).
     */
    ~BackgroundPromiseRecorder() override = default;

    /**
     * @brief Starts the writer thread.
     * @param[in] store The recorder that writes the record to disk. It must outlive this object, and the writer thread if a
     *            write is stuck when this object is destroyed.
     * @param[in] thread_name Linux name to assign to the writer thread. It must contain at most 15 bytes.
     */
    explicit BackgroundPromiseRecorder(LeasePromiseRecorderInterface& store, const std::string& thread_name = "PromiseWriter")
        : recorder_([&store](const LeasePromiseRecord& record) { return store.record(record); }, thread_name) {}

    BackgroundPromiseRecorder(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder& operator=(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder(BackgroundPromiseRecorder&&) = delete;
    BackgroundPromiseRecorder& operator=(BackgroundPromiseRecorder&&) = delete;

    /**
     * @brief Returns the writer thread handle for background-tier registration.
     */
    [[nodiscard]] pthread_t writer_pthread_id() const {
        return recorder_.writer_pthread_id();
    }

    /**
     * @brief Start a write on the writer thread; false, and nothing asked for, if one is already in progress.
     *
     * The outcome of an earlier write that has not been read with background_result() is discarded.
     */
    [[nodiscard]] bool record_in_background(const LeasePromiseRecord& record) override {
        return recorder_.start(record);
    }

    /** @brief The outcome of the last background write, once, after it has finished. */
    [[nodiscard]] std::optional<bool> background_result() override {
        return recorder_.result();
    }

  private:
    BackgroundRecorder<LeasePromiseRecord> recorder_;
};

} // namespaces
