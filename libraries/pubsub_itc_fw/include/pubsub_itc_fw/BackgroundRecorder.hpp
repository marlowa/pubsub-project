#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <pthread.h>

#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Writes a record on a thread of its own, so that the thread that asks for the write never waits for the disk.
 *
 * Some records must be written, and synced, before what they record is acted on, and writing and syncing can take
 * seconds on a slow disk. The threads that ask for such writes handle leases, and must not wait for the disk (principles
 * F2 and F4 in docs/design_principles.md). So the write happens on this class's own thread: the asking thread only starts
 * a write and later reads its outcome, and neither thread ever waits for the other. Lease promise records are written this
 * way (BackgroundPromiseRecorder), and so are the arbiter's records of leaders' statements
 * (docs/availability/recording_statements_in_the_background.md).
 *
 * How the two threads pass a record and its outcome without a lock: the record goes in one slot, and two counters say how
 * many writes have been asked for and how many have finished. Each counter is changed by one thread only. The asking
 * thread fills the slot only when the counters are equal, which means no write is in progress and the writer thread is not
 * reading the slot, and then increases the count of writes asked for. The writer thread sees that count ahead of the count
 * finished, copies the slot, writes the record, stores whether it succeeded, and then increases the count finished. The
 * asking thread learns the outcome by reading the count finished.
 *
 * The writer thread looks for a write to do every 10 milliseconds and sleeps in between. It handles no orders, so sleeping
 * costs nothing. C++17 has no way for one thread to wait for an atomic variable to change without a lock.
 *
 * A record should be cheap to copy and its copy should allocate nothing, since the asking thread copies it into the slot:
 * a fixed-size structure, not a container that grows.
 *
 * Use: one thread, the asking one, calls every member function. The writer thread is started when this is constructed and
 * stopped when it is destroyed.
 *
 * @tparam Record The record written.
 */
template <typename Record> class BackgroundRecorder {
  public:
    /// Writes a record, and says whether it reached the disk. Called on the writer thread only.
    using Write = std::function<bool(const Record&)>;

    /**
     * @brief Stops the writer thread, waiting up to a second for a write in progress to finish.
     *
     * A write stuck on a disk that has stopped answering does not keep the process from shutting down: after the wait the
     * thread is left to finish or be ended with the process. The counters, the slot and the write function it uses are
     * shared with it, so they outlive this object for as long as the thread needs them, and so must anything the write
     * function refers to.
     */
    ~BackgroundRecorder() {
        shared_->stopping.store(true, std::memory_order_release);
        static_cast<void>(writer_.join_with_timeout(std::chrono::milliseconds{1000}));
    }

    /**
     * @brief Starts the writer thread.
     * @param[in] write       Writes a record to disk on the writer thread. What it refers to must outlive this object, and
     *                        the writer thread if a write is stuck when this object is destroyed.
     * @param[in] thread_name Linux name to assign to the writer thread. It must contain at most 15 bytes.
     */
    BackgroundRecorder(Write write, const std::string& thread_name) : shared_(std::make_shared<Shared>()) {
        shared_->write = std::move(write);
        writer_.start([shared = shared_] { write_requests(*shared); });
        const int name_result = pthread_setname_np(writer_.get_pthread_id(), thread_name.c_str());
        if (name_result != 0) {
            shared_->stopping.store(true, std::memory_order_release);
            writer_.join();
            throw PubSubItcException("BackgroundRecorder: failed to set the writer thread name");
        }
    }

    BackgroundRecorder(const BackgroundRecorder&) = delete;
    BackgroundRecorder& operator=(const BackgroundRecorder&) = delete;
    BackgroundRecorder(BackgroundRecorder&&) = delete;
    BackgroundRecorder& operator=(BackgroundRecorder&&) = delete;

    /// The writer thread's handle, for registering it with the background tier.
    [[nodiscard]] pthread_t writer_pthread_id() const {
        return writer_.get_pthread_id();
    }

    /**
     * @brief Start writing @p record on the writer thread; false, and nothing asked for, if a write is already in progress.
     *
     * The outcome of an earlier write that has not been read with result() is discarded.
     */
    [[nodiscard]] bool start(const Record& record) {
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

    /// Whether a write is in progress: started and not yet finished.
    [[nodiscard]] bool in_progress() const {
        return shared_->requests_started.load(std::memory_order_relaxed) != shared_->requests_finished.load(std::memory_order_acquire);
    }

    /** @brief The outcome of the last write, once, after it has finished: whether it reached the disk. */
    [[nodiscard]] std::optional<bool> result() {
        const uint64_t finished = shared_->requests_finished.load(std::memory_order_acquire);
        if (finished == outcomes_read_) {
            return std::nullopt;
        }
        outcomes_read_ = finished;
        return shared_->last_write_succeeded.load(std::memory_order_relaxed);
    }

  private:
    // What the two threads share. It is held by both, so that it outlives this object while a write that is stuck on the
    // disk still needs it.
    struct Shared {
        // How many writes the asking thread has asked for. Changed only by that thread.
        std::atomic<uint64_t> requests_started{0};
        // How many writes the writer thread has finished. Changed only by the writer thread, after it has stored
        // last_write_succeeded.
        std::atomic<uint64_t> requests_finished{0};
        std::atomic<bool> last_write_succeeded{false};
        std::atomic<bool> stopping{false};
        // The record to write. The asking thread fills it only while the two counts are equal, and the writer thread reads
        // it only while they are not, so the two never use it at the same time.
        Record slot{};
        // Set before the writer thread starts, and only read after.
        Write write;
    };

    // How long the writer thread sleeps between looks for a write to do.
    static constexpr std::chrono::milliseconds look_for_work_interval{10};

    static void write_requests(Shared& shared) {
        uint64_t finished = 0;
        while (!shared.stopping.load(std::memory_order_acquire)) {
            if (shared.requests_started.load(std::memory_order_acquire) == finished) {
                std::this_thread::sleep_for(look_for_work_interval);
                continue;
            }
            const Record request = shared.slot;
            const bool written = shared.write(request);
            shared.last_write_succeeded.store(written, std::memory_order_relaxed);
            ++finished;
            shared.requests_finished.store(finished, std::memory_order_release);
        }
    }

    std::shared_ptr<Shared> shared_;

    // How many outcomes the asking thread has read, or discarded by starting another write.
    uint64_t outcomes_read_{0};

    ThreadWithJoinTimeout writer_; ///< Last, so it starts after every member it uses exists and stops first.
};

} // namespaces
