#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/InboundConnectionManager.hpp>
#include <pubsub_itc_fw/InboundListener.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/OutboundConnectionManager.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ReactorLifecycleState.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadLookupInterface.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief The main class for the reactor framework, orchestrating the event loop,
 * managing handlers, and controlling application threads.
 *
 * The Reactor owns all ApplicationThread instances, manages epoll-registered
 * EventHandlers, drives the timer lifecycle, and processes ReactorControlCommands
 * from application threads. All socket I/O is performed exclusively on the
 * reactor thread.
 *
 * Inbound and outbound connection management is fully delegated to
 * InboundConnectionManager and OutboundConnectionManager respectively. The
 * Reactor inherits ThreadLookupInterface so that both managers can deliver
 * EventMessages to ApplicationThreads without depending on the Reactor class
 * directly.
 *
 * The ServiceRegistry supplied at construction is used by the
 * OutboundConnectionManager to resolve logical service names to their primary
 * and secondary network endpoints when processing Connect commands.
 */
class Reactor : public ThreadLookupInterface {
public:
    /**
     * @brief Destroys the Reactor instance, closing the epoll file descriptor.
     *
     * Expects all threads to have been joined by shutdown() before it is called.
     */
    ~Reactor();

    /**
     * @brief Constructs a Reactor.
     *
     * @param[in] reactor_configuration Configuration controlling event loop behaviour,
     *                                  timeouts, and HA topology.
     * @param[in] service_registry      Registry mapping logical service names to their
     *                                  primary and secondary network endpoints. Used to
     *                                  resolve Connect commands. Must outlive this Reactor.
     * @param[in] logger                Logger instance. Must outlive this Reactor.
     */
    Reactor(const ReactorConfiguration& reactor_configuration,
            const ServiceRegistry& service_registry,
            QuillLogger& logger);

    /**
     * @brief Starts the reactor's event loop.
     *
     * @return An integer status code: 0 for normal shutdown, non-zero otherwise.
     */
    int run();

    /**
     * @brief Initiates a graceful shutdown of the reactor and all threads.
     *
     * @param[in] reason The reason for the shutdown.
     */
    void shutdown(const std::string& reason);

    /**
     * @brief Handles a SIGTERM signal, broadcasting Termination to all threads
     *        and initiating a graceful reactor shutdown.
     *
     * Called exclusively from the reactor thread when the signalfd registered
     * in the constructor becomes readable. SIGTERM is blocked on all threads
     * via pthread_sigmask in the constructor so it is never delivered directly
     * to any thread -- it arrives only through the signalfd epoll path.
     *
     * May also be called programmatically (e.g. from tests) to simulate a
     * SIGTERM without sending an actual signal.
     */
    void handleSIGTERM();

    /**
     * @brief Registers an event handler with the reactor's event loop.
     * @param[in] handler A pointer to the handler to register.
     */
    void register_handler(std::unique_ptr<EventHandler> handler);

    /**
     * @brief Deregisters an event handler by its file descriptor.
     * @param[in] fd The file descriptor of the handler to deregister.
     */
    void deregister_handler(int fd);

    /**
     * @brief Registers an application thread with the reactor.
     *
     * Must only be called before Reactor::run(). Thread registration is not
     * allowed once the reactor is running.
     *
     * All registry structures (threads_, threads_by_thread_id_, fast_path_threads_)
     * are updated atomically under thread_registry_mutex_.
     *
     * @param[in] thread A shared pointer to the application thread.
     */
    void register_thread(std::shared_ptr<ApplicationThread> thread);

    /**
     * @brief Registers a TCP listener that accepts inbound connections.
     *
     * Must be called before Reactor::run(). The reactor binds and listens on the
     * given address during initialisation, and routes all inbound data from
     * accepted connections to the specified ApplicationThread.
     *
     * One-connection contract:
     *   Each registered listener accepts exactly one peer connection at a time.
     *   If a second peer attempts to connect while a connection is already
     *   established, the reactor accepts and immediately closes the socket, logs
     *   a Warning, and does NOT notify the application thread.
     *   When the established connection is lost, the listener resumes accepting.
     *
     * Protocol type:
     *   Pass ProtocolType::FrameworkPdu (the default) for connections carrying
     *   framework PDUs. Pass ProtocolType::RawBytes for connections carrying
     *   foreign or alien byte streams; in that case raw_buffer_capacity sets the
     *   minimum size of the MirroredBuffer allocated per accepted connection.
     *
     * @param[in] address             The address and port to listen on.
     * @param[in] target_thread_id    The ThreadID to receive ConnectionEstablished,
     *                                data, and ConnectionLost events.
     * @param[in] protocol_type       Handler strategy for accepted connections.
     *                                Defaults to FrameworkPdu.
     * @param[in] raw_buffer_capacity Minimum MirroredBuffer size in bytes for
     *                                RawBytes listeners. Ignored for FrameworkPdu.
     * @throws PreconditionAssertion if called after run().
     */
    void register_inbound_listener(NetworkEndpointConfig address,
                                   ThreadID target_thread_id,
                                   ProtocolType protocol_type = ProtocolType{ProtocolType::FrameworkPdu},
                                   int64_t raw_buffer_capacity = 0);

    /**
     * @brief Returns the name of a thread given its ID.
     *
     * @param[in] id The ID of the thread.
     * @return The name of the thread.
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

    /**
     * @brief Called by dispatch_events() when EPOLLIN fires on a listening socket fd.
     *
     * The Reactor's epoll dispatch loop uses InboundConnectionManager::find_listener_by_fd()
     * to identify the listener, then calls this method directly. Allocates a ConnectionID
     * and delegates to InboundConnectionManager::on_accept().
     *
     * @param[in] listener The InboundListener whose listening socket became readable.
     */
    void on_accept(InboundListener& listener);

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
     *
     * After cancellation, the Reactor will no longer watch the associated timerfd
     * and no further Timer events will be generated for this TimerID.
     * If the TimerID is unknown, this function is a no-op.
     *
     * @param[in] owner_thread_id The thread that owns the timer.
     * @param[in] id              The TimerID to cancel.
     */
    void cancel_timer_fd(ThreadID owner_thread_id, TimerID id);

    void cancel_all_timer_fds_for_thread(ThreadID owner_thread_id);

    void enqueue_control_command(const ReactorControlCommand& command);

    void on_housekeeping_tick();

    [[nodiscard]] TimerID allocate_timer_id();

    QuillLogger& get_logger() { return logger_; }

    /**
     * @brief Returns the inbound slab allocator.
     *
     * ApplicationThreads use this to deallocate inbound PDU payload chunks
     * after processing FrameworkPdu messages.
     */
    ExpandableSlabAllocator& inbound_slab_allocator() { return inbound_slab_allocator_; }

    ThreadLifecycleState::Tag get_thread_state(ThreadID id) const;

    /**
     * @brief Returns a non-owning pointer to the ApplicationThread with the given
     *        ThreadID, or nullptr if no such thread is registered.
     *
     * Implements ThreadLookupInterface. Safe without locking under the reactor
     * lifecycle model: fast_path_threads_ is written only during initialisation
     * and shutdown, and read only while running.
     *
     * @param[in] id The ThreadID to look up.
     * @return A non-owning pointer to the thread, or nullptr if not found.
     */
    [[nodiscard]] ApplicationThread* get_fast_path_thread(ThreadID id) const override;

    // Made public for unit test purposes only.
    void finalize_threads_after_shutdown();
    void check_for_exited_threads();
    void check_for_stuck_threads();
    void dispatch_events(int nfds, epoll_event* events);

    /**
     * TEST SEAM: exists solely for unit tests to force lifecycle transitions
     * without running the full event loop. Must not be used by application code.
     */
    void set_lifecycle_state(ReactorLifecycleState::Tag state) {
        lifecycle_.store(state, std::memory_order_release);
    }

    /**
     * TEST SEAM: exists solely for unit tests to force lifecycle transitions
     * without running the full event loop. Must not be used by application code.
     */
    void set_initialization_complete(bool is_complete) {
        initialization_complete_.store(is_complete, std::memory_order_release);
    }

    /**
     * TEST SEAM: returns the port number that the first registered inbound
     * listener was assigned by the OS (when registered with port=0).
     * Valid only after the reactor has been initialized (is_initialized() == true).
     * Returns 0 if no listeners are registered or the port cannot be determined.
     */
    [[nodiscard]] uint16_t get_first_inbound_listener_port() const;

    /**
     * @brief Returns the human-readable reason why the reactor shut down.
     *
     * Meaningful only after the reactor has left ReactorLifecycleState::Running.
     * May be empty if shutdown has not yet been requested.
     *
     * @return A copy of the shutdown reason string.
     */
    std::string get_shutdown_reason() const;

protected:
    std::atomic<ReactorLifecycleState::Tag> lifecycle_{ReactorLifecycleState::NotStarted};

private:
    [[nodiscard]] bool initialize_threads();
    void event_loop();
    bool wait_for_all_threads(std::function<bool(const ApplicationThread&)> predicate,
                              const std::string& phase_name);
    void broadcast_reactor_event(EventType::EventTypeTag tag);
    void process_control_commands();

    /**
     * @brief Allocates the next ConnectionID from the shared monotonic counter.
     *
     * Called by on_accept() and process_connect_command() before delegating to
     * the respective manager. Keeping the counter here ensures the ID space is
     * unified across inbound and outbound connections without coupling the
     * managers to each other.
     */
    [[nodiscard]] ConnectionID allocate_connection_id();

    /**
     * @brief Creates an epoll file descriptor and returns it.
     *
     * Called from the constructor initialiser list so that epoll_fd_ holds
     * the real descriptor before InboundConnectionManager and
     * OutboundConnectionManager are constructed. Both managers store epoll_fd_
     * by value, so they must receive the final descriptor, not the -1 sentinel.
     *
     * @throws PubSubItcException if epoll_create1 fails.
     */
    [[nodiscard]] static int create_epoll_fd();

    /**
     * @brief Blocks SIGTERM on the calling thread and creates a signalfd for it.
     *
     * Must be called on the main thread before any ApplicationThreads are
     * spawned so that all threads inherit the blocked signal mask. The returned
     * fd is registered with epoll by the constructor so that SIGTERM is
     * delivered to the reactor thread as a normal readable event, with no C
     * signal handler or global state required.
     *
     * @throws PubSubItcException if pthread_sigmask or signalfd fails.
     */
    [[nodiscard]] static int create_signal_fd();

    std::atomic<bool> initialization_complete_{false};

    /**
     * @brief Mutex protecting all timer-related collections and next_timer_id_.
     */
    mutable std::mutex timer_registry_mutex_;

    // Monotonically increasing counter used to generate unique TimerIDs.
    // The first timer created is for the reactor backstop and has timer id zero.
    TimerID next_timer_id_{0};

    // Per-thread map of timer names to TimerIDs. Enforces uniqueness of timer
    // names within a thread.
    std::map<ThreadID, std::map<std::string, TimerID>> thread_timer_names_;

    // Handler management: maps file descriptors to their EventHandlers.
    std::map<int, std::unique_ptr<EventHandler>> handlers_;

    std::unordered_map<TimerID, int> timer_id_to_fd_;

    /**
     * Thread registry invariants:
     *
     * - threads_ owns the shared_ptr<ApplicationThread>.
     * - threads_by_thread_id_ provides ID-based lookup of the same shared_ptr.
     * - fast_path_threads_ provides a raw-pointer fast-path for routing.
     *
     * All three structures are mutated only during initialisation and shutdown,
     * always under thread_registry_mutex_. During steady-state running they are
     * read-only, which guarantees fast_path_threads_ never contains dangling pointers.
     */
    mutable std::mutex thread_registry_mutex_;

    std::map<std::string, std::shared_ptr<ApplicationThread>> threads_;
    std::map<ThreadID, std::shared_ptr<ApplicationThread>> threads_by_thread_id_;

    /**
     * Fast-path routing: non-owning raw pointers.
     * Written only during initialisation and shutdown.
     * Read only during steady-state running.
     * Safe without locking under the reactor lifecycle model.
     */
    std::unordered_map<ThreadID, ApplicationThread*> fast_path_threads_;

    /**
     * command_queue_ is backed by ExpandablePoolAllocator and cannot overflow.
     * It grows by allocating new pools as needed. No fixed capacity exists.
     */
    LockFreeMessageQueue<ReactorControlCommand> command_queue_;

    std::string shutdown_reason_;

    /**
     * Monotonically increasing counter for ConnectionID assignment.
     * ConnectionID(0) is reserved as invalid; first real ID is 1.
     * Shared between inbound and outbound: the Reactor allocates the ID and
     * passes it into the appropriate manager as a parameter.
     */
    ConnectionID next_connection_id_{1};

    // Configuration and dependencies — must precede slab allocators and managers
    // in declaration order so the constructor initialiser list is valid.
    ReactorConfiguration config_;
    const ServiceRegistry& service_registry_;
    QuillLogger& logger_;

    // epoll_fd_ is initialised via create_epoll_fd() in the constructor initialiser
    // list. It must be declared after config_ and logger_ (which it does not depend
    // on directly) but -- critically -- before inbound_manager_ and outbound_manager_,
    // which receive it by value at construction time. Declaration order governs
    // initialisation order, so these ints sit here, just above the managers.
    int epoll_fd_{-1};
    int wake_fd_{-1};

    // signal_fd_ receives SIGTERM as a readable epoll event, avoiding any need
    // for a C signal handler or global state. SIGTERM is blocked on all threads
    // via pthread_sigmask in the constructor so it can only be consumed here.
    // Declared after epoll_fd_ (which it is registered with) and before the
    // managers so the initialiser list order is respected.
    int signal_fd_{-1};

    /**
     * Inbound slab allocator: used by PduParser to allocate receive buffers.
     * Payload bytes from the socket are read directly into slab chunks.
     * Slab size is configured via ReactorConfiguration::inbound_slab_size.
     * Declared after config_ so it can be initialised from config_ in the
     * constructor initialiser list.
     */
    ExpandableSlabAllocator inbound_slab_allocator_;

    /**
     * Manages all inbound TCP connections: listener registry, accept, read,
     * write, idle-timeout, and teardown. Declared after epoll_fd_,
     * inbound_slab_allocator_, config_, and logger_ so that all four are
     * fully constructed before this manager's constructor runs.
     */
    InboundConnectionManager inbound_manager_;

    /**
     * Manages all outbound TCP connections: connect, read, write, connect-timeout,
     * and teardown. Declared after epoll_fd_, inbound_slab_allocator_, config_,
     * service_registry_, and logger_ so that all five are fully constructed before
     * this manager's constructor runs.
     */
    OutboundConnectionManager outbound_manager_;
};

} // namespace pubsub_itc_fw
