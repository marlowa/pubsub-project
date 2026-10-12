#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseRecord.hpp>
#include <pubsub_itc_fw/LeasePromiseRecorderInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Writes lease promise records on a thread of its own, so that the thread handling leases never waits for the disk.
 *
 * A promise record is written, and synced, before the vote it records is given. Writing and syncing can
 * take seconds on a slow disk. The sequencer and the matching engine handle leases on the same thread
 * that handles orders, and that thread must never wait for the disk or take a lock (principles F2 and
 * F4 in docs/design_principles.md). So this recorder writes on its own thread, and the thread handling
 * leases only starts a write and later reads its outcome. Neither ever waits.
 *
 * How the two threads pass a record and its outcome without a lock: the record goes in one slot, and two
 * counters say how many writes have been asked for and how many have finished. Each counter is changed
 * by one thread only. The thread handling leases fills the slot only when the counters are equal, which
 * means no write is in progress and the writer thread is not reading the slot, and then increases the
 * count of writes asked for. The writer thread sees that count ahead of the count finished, copies the
 * slot, writes and syncs the record, stores whether it succeeded, and then increases the count finished.
 * The thread handling leases learns the outcome by reading the count finished.
 *
 * The writer thread looks for a write to do every 10 milliseconds and sleeps in between. It handles no
 * orders, so sleeping costs nothing. C++17 has no way for one thread to wait for an atomic variable to
 * change without a lock.
 *
 * Use: one thread, the one that handles leases, calls every member function. The writer thread is
 * started when this is constructed and stopped when it is destroyed.
 */
class BackgroundPromiseRecorder : public BackgroundPromiseRecorderInterface {
  public:
    /**
     * @brief Stops the writer thread, waiting up to a second for a write in progress to finish.
     *
     * A write stuck on a disk that has stopped answering does not keep the process from shutting down:
     * after the wait the thread is left to finish or be ended with the process. The counters and the slot
     * it uses are shared with it, so they outlive this object for as long as the thread needs them. The
     * store passed to the constructor must too.
     */
    ~BackgroundPromiseRecorder() override {
        shared_->stopping.store(true, std::memory_order_release);
        static_cast<void>(writer_.join_with_timeout(std::chrono::milliseconds{1000}));
    }

    /**
     * @brief Starts the writer thread.
     * @param[in] store The recorder that writes the record to disk. It must outlive this object, and the
     *            writer thread if a write is stuck when this object is destroyed.
     */
    explicit BackgroundPromiseRecorder(LeasePromiseRecorderInterface& store) : shared_(std::make_shared<Shared>()) {
        writer_.start([shared = shared_, &store] { write_requests(*shared, store); });
    }

    BackgroundPromiseRecorder(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder& operator=(const BackgroundPromiseRecorder&) = delete;
    BackgroundPromiseRecorder(BackgroundPromiseRecorder&&) = delete;
    BackgroundPromiseRecorder& operator=(BackgroundPromiseRecorder&&) = delete;

    /**
     * @brief Start a write on the writer thread; false, and nothing asked for, if one is already in progress.
     *
     * The outcome of an earlier write that has not been read with background_result() is discarded.
     */
    [[nodiscard]] bool record_in_background(const LeasePromiseRecord& record) override {
        const uint64_t started = shared_->requests_started.load(std::memory_order_relaxed);
        const uint64_t finished = shared_->requests_finished.load(std::memory_order_acquire);
        if (started != finished) {
            return false;
        }
        shared_->slot = record;
        outcomes_read_ = finished;
        shared_->requests_started.store(started + 1, std::memory_order_release);
        return true;
    }

    /** @brief The outcome of the last background write, once, after it has finished. */
    [[nodiscard]] std::optional<bool> background_result() override {
        const uint64_t finished = shared_->requests_finished.load(std::memory_order_acquire);
        if (finished == outcomes_read_) {
            return std::nullopt;
        }
        outcomes_read_ = finished;
        return shared_->last_write_succeeded.load(std::memory_order_relaxed);
    }

  private:
    // What the two threads share. It is held by both, so that it outlives this object while a write that
    // is stuck on the disk still needs it.
    struct Shared {
        // How many writes the thread handling leases has asked for. Changed only by that thread.
        std::atomic<uint64_t> requests_started{0};
        // How many writes the writer thread has finished. Changed only by the writer thread, after it has
        // stored last_write_succeeded.
        std::atomic<uint64_t> requests_finished{0};
        std::atomic<bool> last_write_succeeded{false};
        std::atomic<bool> stopping{false};
        // The record to write. The thread handling leases fills it only while the two counts are equal, and
        // the writer thread reads it only while they are not, so the two never use it at the same time.
        LeasePromiseRecord slot{};
    };

    // How long the writer thread sleeps between looks for a write to do.
    static constexpr std::chrono::milliseconds look_for_work_interval{10};

    static void write_requests(Shared& shared, LeasePromiseRecorderInterface& store) {
        uint64_t finished = 0;
        while (!shared.stopping.load(std::memory_order_acquire)) {
            if (shared.requests_started.load(std::memory_order_acquire) == finished) {
                std::this_thread::sleep_for(look_for_work_interval);
                continue;
            }
            const LeasePromiseRecord request = shared.slot;
            const bool written = store.record(request);
            shared.last_write_succeeded.store(written, std::memory_order_relaxed);
            ++finished;
            shared.requests_finished.store(finished, std::memory_order_release);
        }
    }

    std::shared_ptr<Shared> shared_;

    // How many outcomes the thread handling leases has read, or discarded by starting another write.
    uint64_t outcomes_read_{0};

    pubsub_itc_fw::ThreadWithJoinTimeout writer_; ///< Last, so it starts after every member it uses exists and stops first.
};

} // namespaces
