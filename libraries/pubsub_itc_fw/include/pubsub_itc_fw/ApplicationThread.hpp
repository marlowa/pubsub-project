#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

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
 * interface for the Reactor to manage the lifecycle of the thread, including starting,
 * joining, and a mechanism for state-checking.
 */
class ApplicationThread {
public:

    ~ApplicationThread();

    /**
     * TODO need to document how watermarks work
     *
     * Note that the thread id is allocated by the Reactor when the thread is registered.
     * The ApplicationThread stores a reference to the Reactor during construction.
     * The Reactor constructor is simple so it is an easy thing to create the Reactor,
     * not do anything with it yet, then create the ApplicationThreads. The creation of the thread
     * registers it with the Reactor but nothing happens until the Reactor run function is invoked.
     * This means that when this constructor completes the ApplicationThread can obtain its ThreadID.
     *
     * @brief Constructs an ApplicationThread.
     * @param[in] logger A reference to the logger instance.
     * @param[in] reactor a reference to the reactor which is needed when the thread terminates with an exception
     * @param[in] thread_name The name of the thread.
     * @param[in] low_watermark The low watermark for the message queue.
     * @param[in] high_watermark The high watermark for the message queue.
     */
    ApplicationThread(const LoggerInterface& logger,
                      Reactor& reactor,
                      const std::string& thread_name,
                      uint32_t low_watermark, uint32_t high_watermark);

    /**
     * Return the thread name, as a const ref to avoid copying
     */
    const std::string& get_thread_name() const {
        return thread_name_;
    }

    /**
     * @brief The main loop for the application thread.
     */
    virtual void run() = 0;

    /**
     * Starts the underlying thread.
     */
    void start();

    /**
     * @brief Joins the underlying thread with a timeout for hung threads.
     */
    bool join_with_timeout(std::chrono::milliseconds timeout);

    /**
     * Returns the ID of the thread. This is allocated by the Reactor during the
     * execution of the ApplicationThread constructor.
     *
     * @return ThreadID The thread ID.
     */
    ThreadID get_thread_id() const;

    /**
     * @brief Sets the ID of the thread. This is called by the Reactor during registration.
     * @param [in] id The ID to set.
     */
    void set_thread_id(ThreadID id);

    /**
     * @brief Pauses the thread's execution loop.
     */
    void pause();

    /**
     * @brief Resumes the thread's execution loop.
     */
    void resume();

    /**
     * @brief Enqueues an event message to the thread's queue.
     *
     *
     * @param[in] target_thread_name The name of the thread to post to
     * @param[in] message The message to enqueue, a discriminated union
     */
    void post_message(const std::string& target_thread_name, const EventMessage& message);

    /**
     * @brief Returns a reference to the thread's message queue.
     * @return LockFreeMessageQueue<EventMessage>& The message queue.
     */
    LockFreeMessageQueue<EventMessage>& get_queue();

    /**
     * @brief Returns a reference to the logger.
     * @return const LoggerInterface& The logger instance.
     */
    const LoggerInterface& get_logger() const;

    /**
     * @brief Starts a one-off timer.
     * @param [in] name The name of the timer.
     * @param [in] interval The timer interval.
     * @return the timer ID for the timer created
     */
    TimerID start_one_off_timer(const std::string& name, std::chrono::microseconds interval);

    /**
     * @brief Starts a recurring timer.
     * @param [in] name The name of the timer.
     * @param [in] interval The timer interval.
     * @return the timer ID for the timer created
     */
    TimerID start_recurring_timer(const std::string& name, std::chrono::microseconds interval);

    /**
     * @brief Cancels a timer.
     * @param [in] name The name of the timer to cancel.
     */
    void cancel_timer(const std::string& name);

    //void post_socket_read_message(const int length, void* socket_handler); TODO in flux

    auto get_time_event_started() const { return time_event_started_; }
    auto get_time_event_finished() const { return time_event_finished_; }

    void set_time_event_started(std::chrono::time_point<std::chrono::system_clock,
                                std::chrono::nanoseconds> time_event_started) {
        time_event_started_ = time_event_started;
    }

    void set_time_event_finished(std::chrono::time_point<std::chrono::system_clock,
                                 std::chrono::nanoseconds> time_event_finished) {
        time_event_finished_ = time_event_finished;
    }

    bool get_has_processed_initial_event() { return has_processed_initial_event_.load(); }
    void set_has_processed_initial_event() { has_processed_initial_event_.store(true); }

    void shutdown(const std::string& reason);

protected:

    /**
     * This function is called by the wrapper 'run'. The work of 'run' is done here.
     * The wrapper catches any exception and handles it by marking the thread as finished,
     * cancelling any timers created by the thread, and then invoking the Reactor's shutdown function.
     * This is because if any application encounters an unhandled exception it is fatal to the Reactor.
     * In such cases the Reactor shuts down and the application should halt.
     */
    void run_internal();

    void on_data_message(const EventMessage& event_message);
    virtual void process_message(EventMessage& message) = 0;

private:

    const LoggerInterface& logger_;

    /**
     * When the Reactor posts events to this thread, it notes the time the event started
     * (i.e., when it posted) and when the event finished (i.e. when the handler returned).
     * This enables the Reactor to detect in its backstop thread if a thread has hung in its
     * event processing.
     */
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_started_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_finished_;

    std::string thread_name_;

    /**
     * Each thread has a thread_id which is basically a wrapped integer.
     * It is used when events are posted as inter-thread messages. The receiver needs to know who
     * sent the message. Hence the EventMessage contains the thread_id. It is more efficient then
     * storing a string, the thread name. Any logging will probably want to use the thread name, so
     * a function is provided by the Reactor to give the name from the id, get_thread_name_from_id.
     */
    ThreadID thread_id_;

    std::atomic<bool> is_paused_{ false };
    std::atomic<bool> has_processed_initial_event_{ false };
    LockFreeMessageQueue<EventMessage> message_queue_;
    std::unique_ptr<ThreadWithJoinTimeout> thread_;
};
} // namespace pubsub_itc_fw
