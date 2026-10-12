#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

#include <LeasePromiseRecord.hpp>
#include <LeasePromiseRecorderInterface.hpp>

namespace fix_common {

/**
 * @brief Writes lease promise records on a thread of its own, so that the thread handling leases need not wait for the disk.
 *
 * A promise record is written, and synced, before the vote it records is given. Written on the thread
 * that handles leases, a sync that takes seconds stops that instance answering lease requests and
 * renewing for as long as it takes, and the leases of every pair it votes in can run out (BUG-0107).
 * This recorder lets PairLeaseAgent refresh a record well before the record it holds stops covering
 * the promise it is making: the refresh is written here, in the background, and the lease thread only
 * waits for the disk when a record it needs at once has not been written yet.
 *
 * It wraps the recorder that actually writes, normally a LeasePromiseStore, and calls it from at most
 * one thread at a time: record() waits for any background write to finish before it writes.
 *
 * Use: one thread, the one that handles leases, calls every member function. The writer thread is
 * started when this is constructed and stopped when it is destroyed.
 */
class BackgroundPromiseRecorder : public LeasePromiseRecorderInterface {
  public:
    /**
     * @brief Stops the writer thread, waiting up to a second for a write in progress to finish.
     *
     * A write stuck on a disk that has stopped answering does not keep the process from shutting
     * down: after the wait the thread is left to finish or be ended with the process.
     */
    ~BackgroundPromiseRecorder() override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        static_cast<void>(writer_.join_with_timeout(std::chrono::milliseconds{1000}));
    }

    /**
     * @brief Starts the writer thread.
     * @param[in] store The recorder that writes the record to disk. It must outlive this object.
     */
    explicit BackgroundPromiseRecorder(LeasePromiseRecorderInterface& store) : store_(store) {
        writer_.start([this] { write_requests(); });
    }

    BackgroundPromiseRecorder(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder& operator=(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder(BackgroundPromiseRecorder&&) = delete;
    BackgroundPromiseRecorder& operator=(BackgroundPromiseRecorder&&) = delete;

    /**
     * @brief Write a record now, on the calling thread, after any background write has finished.
     *
     * The outcome of a background write this waited for is discarded: the record written here
     * replaces it.
     */
    [[nodiscard]] bool record(const LeasePromiseRecord& record) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_.wait(lock, [this] { return !request_pending_ && !writing_; });
            result_.reset();
        }
        return store_.record(record);
    }

    /** @brief Start a write on the writer thread; false, and nothing asked for, if one is already in progress. */
    [[nodiscard]] bool record_in_background(const LeasePromiseRecord& record) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (request_pending_ || writing_ || stopping_) {
                return false;
            }
            requested_ = record;
            request_pending_ = true;
            result_.reset();
        }
        wake_.notify_all();
        return true;
    }

    /** @brief The outcome of the last background write, once, after it has finished. */
    [[nodiscard]] std::optional<bool> background_result() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::optional<bool> outcome = result_;
        result_.reset();
        return outcome;
    }

  private:
    void write_requests() {
        for (;;) {
            LeasePromiseRecord request{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || request_pending_; });
                if (stopping_) {
                    return;
                }
                request = requested_;
                request_pending_ = false;
                writing_ = true;
            }
            const bool written = store_.record(request);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                writing_ = false;
                result_ = written;
            }
            idle_.notify_all();
        }
    }

    LeasePromiseRecorderInterface& store_;
    std::mutex mutex_;
    std::condition_variable wake_; ///< The writer waits on this for a request or for the order to stop.
    std::condition_variable idle_; ///< record() waits on this for a background write to finish.
    bool stopping_{false};
    bool request_pending_{false};
    bool writing_{false};
    LeasePromiseRecord requested_{};
    std::optional<bool> result_;
    pubsub_itc_fw::ThreadWithJoinTimeout writer_; ///< Last, so it starts after every member it uses exists and stops first.
};

} // namespaces
