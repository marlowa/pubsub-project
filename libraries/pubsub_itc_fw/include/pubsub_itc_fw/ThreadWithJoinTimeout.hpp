#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <stdexcept>
#include <pthread.h>
#include <time.h>

#include <pubsub_itc_fw/PubSubItcException.hpp>

/** @ingroup threading_subsystem */

namespace pubsub_itc_fw {

/**
 * @brief A thread wrapper providing deterministic join-with-timeout semantics.
 *
 * This class offers strict and predictable ownership rules for a worker thread.
 * It is designed for low-latency Linux environments where pthreads are the
 * native threading API and where valgrind-clean execution is required.
 *
 * Key properties:
 *
 *   - Move-only: thread ownership cannot be copied.
 *
 *   - No implicit joins: the class never blocks unless join() or
 *     join_with_timeout() is explicitly invoked.
 *
 *   - No hidden helper threads, no futures, and no additional heap allocations:
 *     the implementation uses pthreads directly for maximum control and
 *     minimal overhead.
 *
 *   - join_with_timeout() uses pthread_timedjoin_np() to attempt a timed join.
 *     If the timeout expires, the worker thread is detached so that the wrapper
 *     can be safely destroyed even if the worker thread is stuck forever.
 *
 *   - After join_with_timeout() returns (true or false), the wrapper no longer
 *     owns a joinable thread. The destructor is guaranteed not to block.
 *
 *   - This design ensures that all thread lifecycle transitions are explicit,
 *     deterministic, and valgrind-clean.
 */
class ThreadWithJoinTimeout {
public:
    /**
     * @brief Destructor detaches the thread if still joinable.
     *
     * This guarantees that destruction is always non-blocking, even if the
     * worker thread is stuck forever.
     */
    ~ThreadWithJoinTimeout() {
        if (has_thread_) {
            pthread_detach(thread_);
        }
    }

    ThreadWithJoinTimeout() = default;

    template <typename Callable, typename... Args>
    explicit ThreadWithJoinTimeout(Callable&& func, Args&&... args) {
        start(std::forward<Callable>(func), std::forward<Args>(args)...);
    }

    ThreadWithJoinTimeout(const ThreadWithJoinTimeout&) = delete;
    ThreadWithJoinTimeout& operator=(const ThreadWithJoinTimeout&) = delete;

    ThreadWithJoinTimeout(ThreadWithJoinTimeout&& other)
        : thread_(other.thread_)
        , has_thread_(other.has_thread_) {
        other.has_thread_ = false;
    }

    ThreadWithJoinTimeout& operator=(ThreadWithJoinTimeout&& other) {
        if (this != &other) {
            if (has_thread_) {
                pthread_detach(thread_);
            }
            thread_ = other.thread_;
            has_thread_ = other.has_thread_;
            other.has_thread_ = false;
        }
        return *this;
    }

    /**
     * @brief Starts a new worker thread.
     *
     * The callable is heap-allocated and freed inside the thread entry point.
     * This avoids capturing by reference and ensures safe lifetime semantics.
     */
    template <typename Callable, typename... Args>
    void start(Callable&& func, Args&&... args) {
        if (has_thread_) {
            throw PubSubItcException("Thread already running. Call join or join_with_timeout first.");
        }

        using Functor = std::decay_t<Callable>;
        auto* heap_func = new Functor(std::forward<Callable>(func));

        int rc = pthread_create(
            &thread_,
            nullptr,
            [](void* arg) -> void* {
                std::unique_ptr<Functor> f(static_cast<Functor*>(arg));
                (*f)();
                return nullptr;
            },
            heap_func);

        if (rc != 0) {
            delete heap_func;
            throw PubSubItcException("pthread_create failed");
        }

        has_thread_ = true;
    }

    /**
     * @brief Returns true if the wrapper still owns a joinable thread.
     */
    bool joinable() const {
        return has_thread_;
    }

    /**
     * @brief Detaches the worker thread, relinquishing ownership.
     */
    void detach() {
        if (has_thread_) {
            pthread_detach(thread_);
            has_thread_ = false;
        }
    }

    /**
     * @brief Joins the worker thread without a timeout.
     *
     * Safe only if the worker thread is known to finish.
     */
    void join() {
        if (has_thread_) {
            pthread_join(thread_, nullptr);
            has_thread_ = false;
        }
    }

    /**
     * @brief Attempts to join the worker thread within the given timeout.
     *
     * Uses pthread_timedjoin_np() for a true timed join. If the timeout
     * expires, the thread is detached so that the wrapper can be safely
     * destroyed without blocking.
     *
     * @param timeout Maximum time to wait.
     * @return true if the thread finished and was joined within the timeout.
     *         false if the timeout expired.
     */
    [[nodiscard]] bool join_with_timeout(std::chrono::milliseconds timeout) {
        if (!has_thread_) {
            return true;
        }

        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);

        auto ns = static_cast<long>(timeout.count()) * 1000000L;
        ts.tv_sec += ns / 1000000000L;
        ts.tv_nsec += ns % 1000000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_timedjoin_np(thread_, nullptr, &ts);
        if (rc == 0) {
            has_thread_ = false;
            return true;
        }

        // Timed out: detach so destructor is non-blocking
        pthread_detach(thread_);
        has_thread_ = false;
        return false;
    }

    pthread_t get_pthread_id() const {
        if (!has_thread_) {
            throw PubSubItcException("ThreadWithJoinTimeout: get_pthread_id called with no running thread");
        }
        return thread_;
    }

private:
    pthread_t thread_{};
    bool has_thread_{false};
};

} // namespace pubsub_itc_fw
