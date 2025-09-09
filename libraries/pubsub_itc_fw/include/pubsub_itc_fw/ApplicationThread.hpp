#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

// Forward declarations
class Reactor;
class SocketHandler;

/**
 * @brief Abstract base class for threads managed by the Reactor.
 *
 * This class abstracts the underlying std::thread and provides a consistent
 * interface for the Reactor to manage its lifecycle, including starting,
 * joining, and a mechanism for state-checking. It provides the core
 * infrastructure for event-driven processing and thread management within the framework.
 */
class ApplicationThread : public EventHandler {
public:
    /**
     * @brief Destructor. Joins with the managed thread if it is still running.
     */
    virtual ~ApplicationThread();

    /**
     * @brief Constructs an ApplicationThread.
     * @param [in] logger A reference to the logger instance.
     * @param [in] thread_name The name of the thread.
     * @param [in] low_watermark The low watermark for the message queue.
     * @param [in] high_watermark The high watermark for the message queue.
     * @param [in] for_client_use A user-defined pointer for client-specific data.
     * @param [in] gone_below_low_watermark_handler A handler for when the queue size drops below the low watermark.
     * @param [in] gone_above_high_watermark_handler A handler for when the queue size exceeds the high watermark.
     */
    ApplicationThread(const LoggerInterface& logger,
                      const std::string& thread_name,
                      int low_watermark,
                      int high_watermark,
                      void* for_client_use,
                      std::function<void(void* for_client_use)> gone_below_low_watermark_handler,
                      std::function<void(void* for_client_use)> gone_above_high_watermark_handler);

    /**
     * @brief Pushes an EventMessage onto the thread's message queue.
     * @param [in] event_message The message to be pushed.
     * @return True if the push was successful, false otherwise.
     */
    bool push(EventMessage&& event_message);

    /**
     * @brief Starts the thread by launching its internal run loop.
     */
    void start();

    /**
     * @brief Requests a graceful shutdown and joins the thread with a timeout.
     * @param [in] timeout The duration to wait for the thread to join.
     */
    void join_with_timeout(std::chrono::milliseconds timeout);

    /**
     * @brief Returns the name of the thread.
     * @return The thread name.
     */
    [[nodiscard]] const std::string& get_thread_name() const;

    /**
     * @brief Returns the unique ID of the application thread.
     * @return The thread's ID.
     */
    [[nodiscard]] ThreadID get_thread_id() const;

    /**
     * @brief Handles an event.
     * @param [in] event_data A pointer to event-specific data.
     * @param [in] originating_thread_id The ID of the thread that generated the event.
     */
    void handle_event(void* event_data, ThreadID originating_thread_id) override;

    /**
     * @brief Registers a timer handler.
     * @param [in] timer_type The type of timer to register.
     * @param [in] handler The function to call when the timer expires.
     */
    void register_handler(TimerType timer_type, std::function<void()> handler);

    /**
     * @brief Retrieves the Reactor instance associated with this thread.
     * @return A pointer to the Reactor instance.
     */
    [[nodiscard]] Reactor* get_reactor() const;

    /**
     * @brief Sets the Reactor instance for this thread.
     * @param [in] reactor The Reactor instance.
     */
    void set_reactor(Reactor* reactor);

    // Getters and setters for event timing
    auto get_time_event_started() const { return time_event_started_; }
    auto get_time_event_finished() const { return time_event_finished_; }

    void set_time_event_started(std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_started) {
        time_event_started_ = time_event_started;
    }

    void set_time_event_finished(std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_finished) {
        time_event_finished_ = time_event_finished;
    }

    bool get_has_processed_initial_event() { return has_processed_initial_event_.load(); }
    void set_has_processed_initial_event() { has_processed_initial_event_.store(true); }

    void shutdown(const std::string& reason);

protected:
    /**
     * @brief The main loop for the application thread.
     *
     * This is the entry point for the thread's execution.
     */
    virtual void run() = 0;

    /**
     * @brief Processes a message received via the internal queue.
     * @param [in] message The message to process.
     */
    virtual void process_message(EventMessage& message) = 0;

    /**
     * @brief Handles the deallocation of a message popped from the queue.
     * @param [in] message The message to deallocate.
     */
    virtual void do_pop_message_deallocate(EventMessage& message) = 0;

    // The primary internal run loop for the thread.
    void run_internal();

    // Internal handler for data messages.
    void on_data_message(const EventMessage& event_message);

    LoggerInterface& get_logger() { return logger_; }

    // Private members
private:
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_started_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_finished_;
    LoggerInterface& logger_;
    std::string thread_name_;
    ThreadID thread_id_{ 0 };
    std::atomic<bool> is_running_{ false };
    std::atomic<bool> is_paused_{ false };
    std::atomic<bool> has_processed_initial_event_{ false };
    LockFreeMessageQueue<EventMessage> queue_;
    std::unique_ptr<ThreadWithJoinTimeout> thread_with_join_timeout_;
    Reactor* reactor_;
};

} // namespace pubsub_itc_fw
