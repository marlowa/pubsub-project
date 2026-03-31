#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sys/epoll.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ReactorLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief This is the main class for the reactor framework, orchestrating the
 * event loop, managing handlers, and controlling application threads.
 */
class Reactor
{
public:
    /**
     * @brief Destroys the Reactor instance, closing the epoll file descriptor.
     *
     * This destructor expects all threads to have been joined by the shutdown function
     * before it is called.
     */
    ~Reactor();

    Reactor(const ReactorConfiguration& reactor_configuration, QuillLogger& logger);

    /**
     * @brief Starts the reactor's event loop.
     *
     * @returns int An integer status code: 0 for normal shutdown, non-zero otherwise.
     */
    int run();

    /**
     * @brief Initiates a graceful shutdown of the reactor and all threads.
     *
     * @param [in] reason The reason for the shutdown.
     */
    void shutdown(const std::string& reason);

    /**
     * @brief Handles a SIGTERM signal to trigger shutdown.
     */
    void handleSIGTERM();

    /**
     * @brief Registers an event handler with the reactor's event loop.
     * @param [in] handler A pointer to the handler to register.
     */
    void register_handler(std::unique_ptr<EventHandler> handler);

    /**
     * @brief Deregisters an event handler by its file descriptor.
     * @param [in] fd The file descriptor of the handler to deregister.
     */
    void deregister_handler(int fd);

    /**
     * @brief Registers an application thread with the reactor.
     *
     * PRECONDITION: Must only be called before Reactor::run().
     * Thread registration is not allowed once the reactor is running.
     *
     * All registry structures (threads_, threads_by_thread_id_, fast_path_threads_)
     * are updated atomically under thread_registry_mutex_.
     *
     * @param [in] thread A reference to the application thread.
     */
    void register_thread(std::shared_ptr<ApplicationThread> thread);

    /**
     * @brief Returns the name of a thread given its ID.
     *
     * @param [in] id The ID of the thread.
     * @returns std::string The name of the thread.
     * @throws PreconditionAssertion if the ThreadID is not found.
     */
    std::string get_thread_name_from_id(ThreadID id) const;

    // Note: finished here means either fully shutdown or shutdown in progress.
    bool is_finished() const {
        auto state = lifecycle_.load(std::memory_order_acquire);
        return state != ReactorLifecycleState::Running &&
               state != ReactorLifecycleState::NotStarted;
    }

    void route_message(ThreadID target_id, EventMessage message);

    bool is_initialized() const {
        return initialization_complete_.load(std::memory_order_acquire);
    }

    bool is_running() const {
        return lifecycle_.load(std::memory_order_acquire) == ReactorLifecycleState::Running;
    }

    void create_timer_fd(TimerID timer_id, const std::string& name, ThreadID owner_thread_id,
                         std::chrono::microseconds interval, TimerType type);

    /**
     * @brief Cancels and deregisters a previously created timerfd.
     * @param [in] id The TimerID to cancel.
     *
     * After cancellation, the Reactor will no longer watch the associated
     * timerfd and no further Timer events will be generated for this TimerID.
     * If the TimerID is unknown, this function is a no-op.
     */
    void cancel_timer_fd(ThreadID owner_thread_id, TimerID id);

    void cancel_all_timer_fds_for_thread(ThreadID owner_thread_id);

    void enqueue_control_command(const ReactorControlCommand& command);

    void on_housekeeping_tick();

    [[nodiscard]] TimerID allocate_timer_id();

    QuillLogger& get_logger() { return logger_; }

    ThreadLifecycleState::Tag get_thread_state(ThreadID id) const;

    /**
     * Fast-path lookup.
     * SAFE WITHOUT LOCKING under the reactor lifecycle model:
     *
     * - No writes to fast_path_threads_ occur while the reactor is Running.
     * - No reads occur during initialization or shutdown.
     * Violating this contract is a precondition failure.
     */
    ApplicationThread* get_fast_path_thread(ThreadID id) const;

    // Made public for unit test purposes only
    void finalize_threads_after_shutdown();
    void check_for_exited_threads();
    void check_for_stuck_threads();
    void dispatch_events(int nfds, epoll_event* events);

    /**
     * TEST SEAM: This method exists solely for unit tests to force lifecycle transitions without running
     * the full event loop. It must not be used by application code.
     */
    void set_lifecycle_state(ReactorLifecycleState::Tag state)
    {
        lifecycle_.store(state, std::memory_order_release);
    }

    /**
     * TEST SEAM: This method exists solely for unit tests to force lifecycle transitions without running
     * the full event loop. It must not be used by application code.
     */
    void set_initialization_complete(bool is_complete)
    {
        initialization_complete_.store(is_complete, std::memory_order_release);
    }

    /**
     * @brief Returns the human-readable reason why the reactor shut down.
     *
     * This value is set when shutdown is initiated and remains stable for the
     * remainder of the reactor's lifetime. It is intended for application-level
     * diagnostics and logging. The returned string is meaningful only after the
     * reactor has left the @ref ReactorLifecycleState::Running state.
     *
     * @return A copy of the shutdown reason string. May be empty if shutdown has not yet been requested.
     */
    std::string get_shutdown_reason() const;

protected:
    std::atomic<ReactorLifecycleState::Tag> lifecycle_{ReactorLifecycleState::NotStarted};

private:
    [[nodiscard]] bool initialize_threads();
    void event_loop();
    void check_for_inactive_sockets();
    bool wait_for_all_threads(std::function<bool(const ApplicationThread&)> predicate, const std::string& phase_name);
    void broadcast_reactor_event(EventType::EventTypeTag tag);
    void process_control_commands();

    // private data

    std::atomic<bool> initialization_complete_{false};

    // The core of the reactor
    int epoll_fd_{-1};
    int wake_fd_{-1};

    /**
     * @brief Mutex protecting all timer-related collections and next_timer_id_.
     */
    mutable std::mutex timer_registry_mutex_;

    // Monotonically increasing counter used to generate unique TimerIDs.
    // Note that the first timer we created is for the reactor backstop so will have a timer id of zero.
    TimerID next_timer_id_{0};

    // When the reactor is asked to create a named timer for a thread,
    // it is a precondition that the thread has not already created a timer with that name.
    std::map<ThreadID, std::map<std::string, TimerID>> thread_timer_names_;

    // Handler management, mapping file descriptors to their handlers
    std::map<int, std::unique_ptr<EventHandler>> handlers_;

    std::unordered_map<TimerID, int> timer_id_to_fd_;

    /**
     * Thread registry invariants:
     *
     * - threads_ owns the shared_ptr<ApplicationThread>.
     * - threads_by_thread_id_ provides ID-based lookup of the same shared_ptr.
     * - fast_path_threads_ provides a raw-pointer fast-path for routing.
     *
     * All three structures are mutated only during initialization and shutdown,
     * and always under thread_registry_mutex_. During steady-state running,
     * they are read-only. This guarantees that fast_path_threads_ never contains
     * dangling pointers.
     */
    mutable std::mutex thread_registry_mutex_;

    std::map<std::string, std::shared_ptr<ApplicationThread>> threads_;
    std::map<ThreadID, std::shared_ptr<ApplicationThread>> threads_by_thread_id_;

    /**
     * Fast-path routing: non-owning raw pointers
     * Written only during initialization and shutdown.
     * Read only during steady-state running.
     * Therefore safe without locking under the reactor lifecycle model.
     */
    std::unordered_map<ThreadID, ApplicationThread*> fast_path_threads_;

    /**
     * command_queue_ is backed by ExpandablePoolAllocator and cannot overflow.
     * It grows by allocating new pools as needed. No fixed capacity exists.
     */
    LockFreeMessageQueue<ReactorControlCommand> command_queue_;

    /**
     * @brief Internal storage for the human-readable shutdown reason.
     *
     * This is written exactly once when shutdown is initiated and then exposed
     * to applications via @ref get_shutdown_reason() for diagnostics and logging.
     * It is not intended for direct access outside the Reactor implementation.
     */
    std::string shutdown_reason_;

    // Configuration parameters
    ReactorConfiguration config_;
    QuillLogger& logger_;
};

} // namespace pubsub_itc_fw
