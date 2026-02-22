#pragma once

#include <chrono>
#include <future>
#include <thread>
#include <stdexcept>

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief A C++17-safe wrapper around std::thread that supports join-with-timeout.
 *
 * This class provides deterministic ownership semantics:
 *
 *  - It is move-only.
 *  - It never implicitly joins (no hidden blocking).
 *  - It never implicitly detaches except in the destructor as a last resort.
 *  - joinWithTimeout() transfers ownership of the underlying std::thread
 *    into a helper thread so that the wrapper can be safely destroyed even
 *    if the worker thread is stuck.
 *
 * After joinWithTimeout() returns (true or false), this wrapper no longer owns a joinable thread.
 */
class ThreadWithJoinTimeout {
public:
    ThreadWithJoinTimeout() = default;

    template <typename Callable, typename... Args>
    explicit ThreadWithJoinTimeout(Callable&& func, Args&&... args) {
        start(std::forward<Callable>(func), std::forward<Args>(args)...);
    }

    // Non-copyable
    ThreadWithJoinTimeout(const ThreadWithJoinTimeout&) = delete;
    ThreadWithJoinTimeout& operator=(const ThreadWithJoinTimeout&) = delete;

    // Move constructor
    ThreadWithJoinTimeout(ThreadWithJoinTimeout&& other) noexcept
        : thread_(std::move(other.thread_)) {}

    // Move assignment
    ThreadWithJoinTimeout& operator=(ThreadWithJoinTimeout&& other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) {
                // Policy choice: detach as last resort
                thread_.detach();
            }
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    /**
     * @brief Destructor detaches only if the thread is still joinable.
     *
     * In a framework, you may want to assert/log here instead of detaching.
     */
    ~ThreadWithJoinTimeout() {
        if (thread_.joinable()) {
            thread_.detach();
        }
    }

    /**
     * @brief Starts the thread with the given function and arguments.
     */
    template <typename Callable, typename... Args>
    void start(Callable&& func, Args&&... args) {
        if (thread_.joinable()) {
            throw std::runtime_error("Thread already running. Call join or joinWithTimeout first.");
        }
        thread_ = std::thread(std::forward<Callable>(func), std::forward<Args>(args)...);
    }

    /**
     * @brief Returns true if the thread is joinable.
     */
    bool joinable() const noexcept {
        return thread_.joinable();
    }

    void detach() {
        thread_.detach();
    }

#if 1
    /**
     * @brief Joins the thread if joinable.
     *
     * TODO need to think about this, is it dangerous for this function to exist,
     * since it could block forever? The unit test currently uses it.
     *
     */
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
#endif

    /**
     * @brief Attempts to join the thread within the given timeout.
     *
     * After this call returns (true or false), this wrapper no longer owns
     * a joinable std::thread. Ownership is transferred to the joiner thread.
     *
     * @param timeout Maximum time to wait.
     * @return true if the thread finished and was joined within the timeout.
     *         false if the timeout expired.
     */
    [[nodiscard]] bool join_with_timeout(std::chrono::milliseconds timeout) {
        if (!thread_.joinable()) {
            return true; // nothing to do
        }

        // packaged_task that takes ownership of the std::thread
        std::packaged_task<void(std::thread)> task(
            [](std::thread t) mutable {
                if (t.joinable()) {
                    t.join();
                }
            }
        );

        auto future = task.get_future();

        // Move the worker thread into the joiner thread
        std::thread joiner(std::move(task), std::move(thread_));

        // thread_ is now empty (not joinable)
        // joiner owns the real worker thread

        if (future.wait_for(timeout) == std::future_status::timeout) {
            // Timed out — joiner continues running in background
            joiner.detach();
            return false;
        }

        // Join completed — join the joiner thread itself
        joiner.join();
        return true;
    }

private:
    std::thread thread_;
};

} // namespace pubsub_itc_fw
