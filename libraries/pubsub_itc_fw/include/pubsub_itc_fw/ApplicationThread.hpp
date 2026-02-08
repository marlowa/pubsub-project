#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/TimerType.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

// Forward declarations
class Reactor;
class SocketHandler;

/**
 * This class abstracts the underlying std::thread and provides a consistent
 * interface for the Reactor to manage the lifecycle of the thread, including starting,
 * joining, and a mechanism for state-checking.
 *
 * An ApplicationThread holds a reference to the Reactor because it is needed
 * when the thread terminates with an exception.
 *
 * WATERMARK SEMANTICS
 * -------------------
 * Each ApplicationThread owns a LockFreeMessageQueue<EventMessage> with an optional
 * low and high watermark. These watermarks allow the Reactor (or the thread itself)
 * to detect overload conditions and recovery conditions.
 *
 * HIGH WATERMARK:
 *   - When the queue size grows to >= high_watermark, the queue fires the
 *     "gone_above_high_watermark" handler exactly once.
 *   - This handler is invoked by the *producer* thread performing the enqueue().
 *   - It fires only on the transition from below → above the high watermark.
 *
 * LOW WATERMARK:
 *   - When the queue size later drops to < low_watermark, the queue fires the
 *     "gone_below_low_watermark" handler exactly once.
 *   - This handler is invoked by the *consumer* thread performing the dequeue().
 *   - It fires only on the transition from above → below the low watermark.
 *
 * HYSTERESIS:
 *   - The pair (low_watermark, high_watermark) defines a hysteresis band.
 *   - While the queue remains above the high watermark, the high watermark handler
 *     will NOT fire again.
 *   - While the queue remains below the low watermark, the low watermark handler
 *     will NOT fire again.
 *   - This prevents handler storms when the queue size fluctuates near a boundary.
 *
 * THREADING MODEL:
 *   - enqueue() is wait-free for multiple producers.
 *   - dequeue() is lock-free and must be called only by the ApplicationThread itself.
 *   - Watermark handlers run in the context of the thread that caused the transition.
 *
 * SHUTDOWN BEHAVIOR:
 *   - During shutdown, the queue marks itself as "shutting down".
 *   - Any enqueue() after shutdown begins is ignored and the message is dropped.
 *   - This is safe because the Reactor stops routing messages to a thread before
 *     destroying the ApplicationThread and its queue.
 *
 * APPLICATION-LEVEL GUARANTEE:
 *   - Messages lost during shutdown are acceptable for the intended usage pattern
 *     (threads run for the duration of the day and terminate at end-of-day).
 *   - No enqueue() will occur after the queue is destroyed, because Reactor
 *     unregisters the thread before destruction.
 */
class ApplicationThread {
  public:
    virtual ~ApplicationThread();

    ApplicationThread(QuillLogger& logger,
                      Reactor& reactor,
                      const std::string& thread_name,
                      ThreadID thread_id,
                      const QueueConfig& queue_config,
                      const AllocatorConfig& allocator_config);

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
    [[nodiscard]] bool join_with_timeout(std::chrono::milliseconds timeout);

    /**
     * Returns the ID of the thread. This is chosen by the application developer
     * and enforced to be unique by the Reactor.
     *
     * @return ThreadID The thread ID.
     */
    ThreadID get_thread_id() const {
        return thread_id_;
    }

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
     * @param[in] target_thread_id The id of the thread to post to.
     * @param[in] message The message to enqueue, a discriminated union.
     *
     * For now, this posts to this thread's own queue; routing to other
     * threads is handled by the Reactor.
     */
    void post_message(ThreadID target_thread_id, EventMessage message);

    /**
     * @brief Returns a reference to the thread's message queue.
     * @return LockFreeMessageQueue<EventMessage>& The message queue.
     */
    LockFreeMessageQueue<EventMessage>& get_queue() {
        return message_queue_;
    }

    /**
     * @brief Returns a reference to the logger.
     * @return QuillLogger& The logger instance.
     */
    QuillLogger& get_logger() const {
        return logger_;
    }

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

    auto get_time_event_started() const {
        return time_event_started_;
    }

    auto get_time_event_finished() const {
        return time_event_finished_;
    }

    void set_time_event_started(std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_started) {
        time_event_started_ = time_event_started;
    }

    void set_time_event_finished(std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_finished) {
        time_event_finished_ = time_event_finished;
    }

    bool get_has_processed_initial_event() {
        return has_processed_initial_event_.load();
    }

    void set_has_processed_initial_event() {
        has_processed_initial_event_.store(true);
    }

    void shutdown(const std::string& reason);

    bool is_running() const noexcept {
        return is_running_.load();
    }

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
    QuillLogger& logger_;
    Reactor& reactor_;

    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_started_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> time_event_finished_;

    std::string thread_name_;
    ThreadID thread_id_;

    std::atomic<bool> is_paused_{false};
    std::atomic<bool> is_running_{false};
    std::atomic<bool> has_processed_initial_event_{false};

    LockFreeMessageQueue<EventMessage> message_queue_;
    std::unique_ptr<ThreadWithJoinTimeout> thread_;
};

} // namespace pubsub_itc_fw
