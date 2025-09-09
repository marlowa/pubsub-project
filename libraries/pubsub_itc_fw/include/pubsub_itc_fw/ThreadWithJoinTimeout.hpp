#pragma once

#include <chrono>
#include <future>
#include <thread>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief This class provides a thread abstraction that can be joined with a timeout.
 *
 * It encapsulates a std::thread and provides a `joinWithTimeout` method that safely
 * waits for the thread to complete, or detaches it if a timeout occurs. This prevents
 * the main thread from getting stuck indefinitely.
 */
class ThreadWithJoinTimeout {
public:
    ~ThreadWithJoinTimeout() {
        if (thread_.joinable()) {
            // Avoid undefined behavior by detaching if the thread is still running
            // when this object is destroyed.
            thread_.detach();
        }
    }

    /**
     * @brief Starts the thread with a given function and arguments.
     * @tparam Callable The type of the function to be called.
     * @tparam Args The types of the arguments to be passed to the function.
     * @param [in] func The function to run in the new thread.
     * @param [in] args The arguments to pass to the function.
     */
    template <typename Callable, typename... Args>
    void start(Callable&& func, Args&&... args) {
        if (thread_.joinable()) {
            throw std::runtime_error("Thread already running. Call join or detach first.");
        }
        thread_ = std::thread(std::forward<Callable>(func), std::forward<Args>(args)...);
    }

    /**
     * @brief Waits for the thread to finish.
     */
    void join() { thread_.join(); }

    /**
     * @brief Checks if the thread is joinable.
     * @returns bool True if the thread is joinable, false otherwise.
     */
    bool joinable() const { return thread_.joinable(); }

    /**
     * @brief Waits for the thread to finish with a specified timeout.
     * @param [in] timeout The maximum time to wait for the thread to finish.
     * @returns bool True if the thread joined successfully, false if a timeout occurred.
     */
    [[nodiscard]] bool joinWithTimeout(std::chrono::milliseconds timeout) {
        if (!thread_.joinable()) {
            return false;
        }

        auto task = std::packaged_task<void()>([this]() { thread_.join(); });
        auto future = task.get_future();

        std::thread joiner(std::move(task));

        if (future.wait_for(timeout) == std::future_status::timeout) {
            joiner.detach();
            return false;
        }

        joiner.join();
        return true;
    }

private:
    std::thread thread_;
};

} // namespace pubsub_itc_fw
