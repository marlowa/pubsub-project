#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>


// TODO we also want users to create an application thread using make_shared.
// we might need a factory function to enforce this. The factory function might use CRTP.

namespace pubsub_itc_fw {

// Forward declarations
class Reactor;
class SocketHandler;

/** @ingroup threading_subsystem */

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
 * BACKPRESSURE AND MEMORY EXHAUSTION
 * ------------------------------------
 * The message queue is backed by an ExpandablePoolAllocator, which means the
 * queue is never "full" in the traditional sense. When the current fixed-size
 * memory pool is exhausted, the allocator transparently chains in a new pool
 * and allocation continues. There is therefore no producer-side blocking or
 * message dropping due to queue capacity.
 *
 * Two independent notification mechanisms exist:
 *
 * WATERMARK CALLBACKS (queue-depth layer):
 *   The high- and low-watermark callbacks on LockFreeMessageQueue fire based
 *   on the number of messages currently in the queue. They are the intended
 *   backpressure mechanism: the high-watermark callback signals that the
 *   consumer is falling behind and producers should slow down or shed load;
 *   the low-watermark callback signals recovery. See WATERMARK SEMANTICS above.
 *
 * POOL EXHAUSTION HANDLER (memory layer):
 *   The handler in AllocatorConfig fires when a FixedSizeMemoryPool slab
 *   within the ExpandablePoolAllocator is exhausted and a new slab must be
 *   allocated from the heap. This is a memory-layer event entirely independent
 *   of queue depth. It signals that the queue has grown beyond its
 *   pre-allocated capacity and that heap allocation is occurring. It may be
 *   used to log or raise an alert, but it is not a backpressure mechanism —
 *   by the time it fires, the message has already been enqueued successfully.
 *
 * Note that because the allocator can grow without bound, the only true
 * protection against unbounded memory growth is the high-watermark handler
 * combined with appropriate producer-side flow control.
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

    ApplicationThread(const ApplicationThread&) = delete;
    ApplicationThread& operator=(const ApplicationThread&) = delete;
    ApplicationThread(ApplicationThread&&) = delete;
    ApplicationThread& operator=(ApplicationThread&&) = delete;

    /**
     * Return the thread name, as a const ref to avoid copying
     */
    const std::string& get_thread_name() const;

    /**
     * @brief The main loop for the application thread.
     * TODO we need to remember why we had this as pure virtual at one point.
     */
    void run();

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
        return *message_queue_;
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

    /**
     * @brief Schedules a high-resolution timer.
     * @param [in] name The name of the timer.
     * @param [in] interval The delay or interval before the timer rings.
     * @param [in] type Whether the timer is single-shot or recurring.
     * @return The TimerID for the timer created.
     *
     * Timers are scheduled by ApplicationThread but managed by Reactor. Reactor owns
     * the underlying timerfd, waits for it in epoll, reads it when it becomes
     * readable, and enqueues one TimerRings event per ring into this thread's
     * message queue. ApplicationThread never touches timerfds directly.
     */
    TimerID schedule_timer(const std::string& name,
                           std::chrono::microseconds interval,
                           TimerType type);

    auto get_time_event_started() const {
        return time_event_started_;
    }

    auto get_time_event_finished() const {
        return time_event_finished_;
    }

    void set_time_event_started(HighResolutionClock::time_point time_event_started) {
        time_event_started_ = time_event_started;
    }

    void set_time_event_finished(HighResolutionClock::time_point time_event_finished) {
        time_event_finished_ = time_event_finished;
    }

    void shutdown(const std::string& reason);

    bool is_running() const {
        auto tag = get_lifecycle_state().as_tag();
        return tag >= ThreadLifecycleState::Started && tag < ThreadLifecycleState::ShuttingDown;
    }

    bool is_operational() const {
        auto tag = get_lifecycle_state().as_tag();
        return tag == ThreadLifecycleState::Operational;
    }

    bool has_processed_initial() const {
        return get_lifecycle_state().as_tag() >= ThreadLifecycleState::InitialProcessed;
    }

    ThreadLifecycleState get_lifecycle_state() const {
        return ThreadLifecycleState(lifecycle_state_.load(std::memory_order_acquire));
    }

    void set_lifecycle_state(ThreadLifecycleState::Tag new_tag);

protected:
    void run_internal();

    void process_message(EventMessage& message);

    virtual void on_initial_event() {}

    virtual void on_app_ready_event() {}

    virtual void on_termination_event([[maybe_unused]] const std::string& reason) {}

    virtual void on_itc_message(const EventMessage& msg) = 0;

    void on_timer_id_event(TimerID id);

    virtual void on_timer_event([[maybe_unused]] const std::string& name) {}

    virtual void on_pubsub_message([[maybe_unused]] const EventMessage& msg) {}

    virtual void on_raw_socket_message([[maybe_unused]] const EventMessage& msg) {}

    void assert_called_from_owner() const {
        pthread_t owner = thread_->get_pthread_id();
        if (!pthread_equal(owner, pthread_self())) {
            throw PreconditionAssertion(
                "Timer APIs must be called from within the owning ApplicationThread's callbacks",
                __FILE__, __LINE__);
        }
    }

private:
    QuillLogger& logger_;
    Reactor& reactor_;

    HighResolutionClock::time_point time_event_started_;
    HighResolutionClock::time_point time_event_finished_;

    std::string thread_name_;
    ThreadID thread_id_;

    std::atomic<bool> is_paused_{false};
    // TODO check if we need to do something similar for EventType atomic.
    std::atomic<ThreadLifecycleState::Tag> lifecycle_state_{ThreadLifecycleState::NotCreated};

    // Note: We heap allocate the lock free queue so that if the application thread
    // misbehaves and does not join properly in the dtor, we can still destroy the
    // application thread, but we leave the queue alone, in case the rogue thread
    // still tries to use it.
    std::unique_ptr<LockFreeMessageQueue<EventMessage>> message_queue_;
    std::unique_ptr<ThreadWithJoinTimeout> thread_;

    std::unordered_map<std::string, TimerID> name_to_id_;
    std::unordered_map<TimerID, std::string> id_to_name_;
};

} // namespace pubsub_itc_fw
