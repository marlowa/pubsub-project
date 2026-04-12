#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfig.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>


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
 *   - It fires only on the transition from below -> above the high watermark.
 *
 * LOW WATERMARK:
 *   - When the queue size later drops to < low_watermark, the queue fires the
 *     "gone_below_low_watermark" handler exactly once.
 *   - This handler is invoked by the *consumer* thread performing the dequeue().
 *   - It fires only on the transition from above -> below the low watermark.
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
 *   used to log or raise an alert, but it is not a backpressure mechanism -
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
                      const AllocatorConfig& allocator_config,
                      const ApplicationThreadConfig& thread_config);

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
     * @name Timer API Contract
     *
     * The timer APIs (`start_one_off_timer`, `start_recurring_timer`,
     * `cancel_timer`, and all user-overridable timer callbacks) have strict
     * lifecycle and threading requirements. Violating any of these is a precondition failure.
     *
     * **Threading Requirements**
     * --------------------------
     * - Timer APIs must be called **only from within the owning
     *   ApplicationThread's callback context** (e.g., `on_initial_event`,
     *   `on_app_ready_event`, `on_itc_message`, `on_timer_event`, etc.).
     * - They must never be called from other threads, including the Reactor
     *   thread or application threads posting messages to each other.
     * - Enforcement is performed by `assert_called_from_owner()`, which
     *   compares the current pthread ID with the owning thread's ID.
     *
     * **Lifecycle Requirements**
     * --------------------------
     * - Timer APIs must not be called **before `start()` has been invoked**.
     *   Prior to `start()`, the underlying thread object does not exist and
     *   no timer operations are valid.
     * - Timer APIs must not be called **after the thread has left the
     *   Running band** (i.e., after entering `ShuttingDown` or `Terminated`).
     * - Timers are created and owned by the Reactor; ApplicationThread only
     *   schedules and cancels them.
     *
     * **Safety Guarantees**
     * ---------------------
     * - If a timer API is called before the thread has started, or from the
     *   wrong thread, a `PreconditionAssertion` is thrown.
     * - These checks prevent undefined behaviour such as dereferencing a
     *   null `thread_` pointer or manipulating timer state from the wrong
     *   execution context.
     *
     * These constraints ensure that timer operations are deterministic,
     * thread-safe, and consistent with the Reactor's ownership model.
     */

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
     * @name Connection API Contract
     *
     * connect_to_service() follows the same threading and lifecycle rules as
     * the timer APIs. It must only be called from within the owning
     * ApplicationThread's callback context (e.g. on_initial_event,
     * on_app_ready_event, on_itc_message, etc.) and never from another thread.
     *
     * The reactor resolves the service name via the ServiceRegistry, initiates
     * a non-blocking TCP connect, and delivers one of the following events to
     * this thread's queue when the attempt completes:
     *
     *   ConnectionEstablished - carrying the assigned ConnectionID.
     *   ConnectionFailed      - carrying a human-readable reason string.
     *
     * If the connection is later lost, a ConnectionLost event is delivered
     * carrying the ConnectionID and a reason string.
     */

    /**
     * @brief Requests the reactor to establish an outbound TCP connection to a
     *        named service.
     *
     * The service name is resolved via the ServiceRegistry supplied to the
     * Reactor at construction. The reactor tries the primary endpoint first
     * and falls back to the secondary if the primary is unreachable.
     *
     * The result is delivered asynchronously via on_connection_established()
     * or on_connection_failed().
     *
     * @param[in] service_name Logical name of the service to connect to.
     */
    void connect_to_service(const std::string& service_name);

    /**
     * @brief Notifies the reactor that the application has finished processing
     *        a contiguous region of bytes from a RawBytesProtocolHandler connection.
     *
     * Must be called from within the owning ApplicationThread's callback context
     * (typically on_raw_socket_message()) after the application has read and
     * processed bytes_consumed bytes starting from EventMessage::payload().
     * The reactor advances the MirroredBuffer tail by this amount, freeing space
     * for future reads.
     *
     * Failing to call this will prevent the buffer from draining and will
     * eventually trigger the buffer-full backpressure policy, which closes
     * the connection.
     *
     * @param[in] conn_id        The ConnectionID of the raw-bytes connection.
     * @param[in] bytes_consumed The number of bytes the application has finished
     *                           processing. Must be > 0 and <= the bytes_available
     *                           count delivered in the most recent RawSocketCommunication
     *                           event for this connection.
     */
    void commit_raw_bytes(ConnectionID conn_id, int64_t bytes_consumed);

    /**
     * @brief Sends raw bytes on a RawBytesProtocolHandler connection.
     *
     * Allocates a slab chunk, copies the supplied bytes into it, and enqueues a
     * SendRaw command to the reactor. The reactor transmits the bytes as-is — no
     * header is prepended. The slab chunk is deallocated by the reactor once the
     * send completes.
     *
     * Must be called from within the owning ApplicationThread's callback context
     * and never from another thread.
     *
     * @param[in] conn_id The ConnectionID of the raw-bytes connection.
     * @param[in] data    Pointer to the bytes to send. Must not be nullptr.
     * @param[in] size    Number of bytes to send. Must be > 0.
     */
    void send_raw(ConnectionID conn_id, const void* data, uint32_t size);

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

    /**
     * Requests shutdown of this ApplicationThread.
     *
     * This does NOT join the underlying thread. Threads are joined exclusively
     * by Reactor::finalize_threads_after_shutdown(). Calling shutdown() is
     * idempotent once the thread has entered ShuttingDown or a later state.
     */
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

    void assert_called_from_owner() const {
        if (thread_ == nullptr) {
            throw PreconditionAssertion("Timer APIs must not be called before the ApplicationThread has been started", __FILE__, __LINE__);
        }

        pthread_t owner = thread_->get_pthread_id();
        if (!pthread_equal(owner, pthread_self())) {
            throw PreconditionAssertion(
                "Timer APIs must be called from within the owning ApplicationThread's callbacks",
                __FILE__, __LINE__);
        }
    }

protected:
    /**
     * @brief Returns a reference to the owning Reactor.
     *
     * Available to subclasses so they can enqueue ReactorControlCommands
     * (e.g. Connect, SendPdu, Disconnect) and access slab allocators
     * from within their callback implementations.
     */
    Reactor& get_reactor() { return reactor_; }

    /**
     * @brief Returns a reference to this thread's outbound PDU slab allocator.
     *
     * Each ApplicationThread owns its own ExpandableSlabAllocator for outbound
     * PDUs. Subclasses call this from their callback implementations to allocate
     * a chunk, encode a PDU into it, and enqueue a SendPdu command to the reactor.
     * The reactor deallocates the chunk once the send is complete, using the
     * allocator pointer carried in the SendPdu command.
     *
     * This allocator is not shared with the reactor or any other thread. Allocation
     * happens entirely on this application thread. Deallocation is performed by the
     * reactor thread, which is safe because ExpandableSlabAllocator::deallocate()
     * is thread-safe.
     */
    ExpandableSlabAllocator& outbound_slab_allocator() { return outbound_allocator_; }

    /**
     * @brief Returns a reference to this thread's inbound PDU decode arena buffer.
     *
     * This buffer provides the backing store for a BumpAllocator used when decoding
     * variable-length inbound PDUs (those containing string, optional, or list fields).
     * It is reserved once at construction to ApplicationThreadConfig::inbound_decode_arena_size
     * bytes and reused for every inbound PDU callback - no heap allocation occurs on
     * the message handling path.
     *
     * Usage in on_framework_pdu_message:
     * @code
     *     BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
     *     size_t consumed = 0;
     *     size_t arena_bytes_needed = 0;
     *     if (decode(view, payload, size, consumed, arena, arena_bytes_needed)) { ... }
     * @endcode
     *
     * The decoded view's string_view and ListView fields point into this buffer.
     * They are valid only for the duration of the current callback - the buffer
     * is reused on the next call.
     *
     * Must only be called from within the owning ApplicationThread's callback
     * context and never from another thread.
     */
    std::vector<uint8_t>& decode_arena_buffer() { return decode_arena_buffer_; }

    /**
     * @brief Sends a DSL-encoded PDU on an established connection.
     *
     * Handles all framing details: measures the encoded payload size, allocates
     * a slab chunk, writes the PduHeader in network byte order, encodes the
     * message payload, and enqueues a SendPdu command to the reactor. Both
     * fixed-size and variable-length messages are supported.
     *
     * Must only be called from within the owning ApplicationThread's callback
     * context (e.g. on_connection_established, on_framework_pdu_message, etc.)
     * and never from another thread.
     *
     * The reactor deallocates the slab chunk once the send is complete.
     *
     * @param[in] conn_id  The ConnectionID of the established connection to send on.
     * @param[in] pdu_id   The DSL message ID as defined in the .dsl file.
     * @param[in] msg      The DSL message struct to encode and send.
     */
    template <typename MsgT>
    void send_pdu(ConnectionID conn_id, int16_t pdu_id, const MsgT& msg) {
        // Pass 1: measure payload size. encode() with out_size=0 sets bytes_needed
        // without writing anything. The call cannot fail on the measuring pass
        // (out_size=0 guarantees the buffer-too-small branch is not reached for
        // variable-length messages), but we check anyway for safety.
        std::size_t bytes_written = 0;
        std::size_t bytes_needed  = 0;
        [[maybe_unused]] bool ok = encode(msg, nullptr, 0, bytes_written, bytes_needed);

        const std::size_t frame_size = sizeof(PduHeader) + bytes_needed;
        auto [slab_id, chunk] = outbound_allocator_.allocate(frame_size);

        // Write PduHeader in network byte order.
        auto* hdr        = reinterpret_cast<PduHeader*>(chunk);
        hdr->byte_count  = htonl(static_cast<uint32_t>(bytes_needed));
        hdr->pdu_id      = htons(static_cast<uint16_t>(pdu_id));
        hdr->version     = 1;
        hdr->filler_a    = 0;
        hdr->canary      = htonl(pdu_canary_value);
        hdr->filler_b    = 0;

        // Pass 2: encode payload directly into the slab chunk after the header.
        // The buffer is sized exactly to bytes_needed so this must succeed.
        auto* payload = static_cast<uint8_t*>(chunk) + sizeof(PduHeader);
        if (!encode(msg, payload, bytes_needed, bytes_written, bytes_needed)) {
            outbound_allocator_.deallocate(slab_id, chunk);
            throw PubSubItcException("ApplicationThread::send_pdu: encode failed on second pass - this should not happen");
        }

        // Delegate the reactor call to a non-template method defined in the .cpp,
        // where Reactor is fully defined. This avoids an incomplete-type error when
        // send_pdu is instantiated in translation units that only have a forward
        // declaration of Reactor.
        enqueue_send_pdu_command(conn_id, slab_id, chunk, static_cast<uint32_t>(bytes_written));
    }

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

    /**
     * @brief Handler for fully packetized framework PDUs.
     *
     * The Reactor's framing layer has already assembled a complete PDU and
     * stripped any transport header. The payload in the EventMessage is the
     * exact PDU byte sequence expected by the DSL-generated decode functions.
     *
     * Override this in ApplicationThread subclasses that consume framework
     * PDUs (e.g., leader-follower, replication, or component-to-component
     * protocols). Threads that only care about foreign protocols (e.g. FIX)
     * may ignore this and rely solely on on_raw_socket_message().
     */
    virtual void on_framework_pdu_message([[maybe_unused]] const EventMessage& msg) {}

    /**
     * @brief Called when an outbound TCP connection requested via
     *        connect_to_service() has been successfully established.
     *
     * @param[in] id The ConnectionID assigned by the reactor. Use this in
     *               subsequent send_pdu() calls to identify the connection.
     */
    virtual void on_connection_established([[maybe_unused]] ConnectionID id) {}

    /**
     * @brief Called when an outbound TCP connection attempt has failed.
     *
     * @param[in] reason Human-readable description of the failure.
     */
    virtual void on_connection_failed([[maybe_unused]] const std::string& reason) {}

    /**
     * @brief Called when an established TCP connection has been lost unexpectedly.
     *
     * The application should re-issue connect_to_service() if it wishes to
     * reconnect.
     *
     * @param[in] id     The ConnectionID of the lost connection.
     * @param[in] reason Human-readable description of why the connection was lost.
     */
    virtual void on_connection_lost([[maybe_unused]] ConnectionID id,
                                    [[maybe_unused]] const std::string& reason) {}

private:
    QuillLogger& logger_;
    Reactor& reactor_;
    ExpandableSlabAllocator outbound_allocator_;

    // Backing store for BumpAllocator when decoding variable-length inbound PDUs.
    // Reserved once at construction to inbound_decode_arena_size bytes and reused
    // for every inbound PDU. No heap allocation occurs on the message handling path
    // because reserve() is called in the constructor and the buffer is never allowed
    // to grow beyond its initial capacity.
    std::vector<uint8_t> decode_arena_buffer_;

    HighResolutionClock::time_point time_event_started_;
    HighResolutionClock::time_point time_event_finished_;

    // Non-template helper for send_pdu. Defined in ApplicationThread.cpp where
    // Reactor is fully defined, avoiding incomplete-type errors in translation
    // units that include ApplicationThread.hpp with only a forward-declared Reactor.
    void enqueue_send_pdu_command(ConnectionID conn_id, int slab_id, void* chunk, uint32_t payload_bytes);

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
