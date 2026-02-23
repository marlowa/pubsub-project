#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <sys/epoll.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
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
     * The function is virtual so that it can be mocked.
     *
     * This destructor expects all threads to have been joined by the shutdown function
     * before it is called.
     */
    virtual ~Reactor() = default;

    /**
     * @brief Constructs a Reactor.
     * @param [in] config The reactor's configuration.
     * @param [in] logger The logger instance.
     */
    explicit Reactor(const ReactorConfiguration& config, QuillLogger& logger);

    /**
     * @brief Starts the reactor's event loop.
     * The function is virtual so that it can overridden in a mock.
     *
     * @returns int An integer status code: 0 for normal shutdown, non-zero otherwise.
     */
    virtual int run();

    /**
     * @brief Initiates a graceful shutdown of the reactor and all threads.
     *
     * It is impure virtual so it can be overridden by a mock.
     *
     * @param [in] reason The reason for the shutdown.
     */
    virtual void shutdown(const std::string& reason);

    /**
     * @brief Handles a SIGTERM signal to trigger shutdown.
     */
    void handleSIGTERM();

    /**
     * @brief Registers an event handler with the reactor's event loop.
     * @param [in] handler A pointer to the handler to register.
     */
    void registerHandler(std::unique_ptr<EventHandler> handler);

    /**
     * @brief Deregisters an event handler by its file descriptor.
     * @param [in] fd The file descriptor of the handler to deregister.
     */
    void deregisterHandler(int fd);

    /**
     * @brief Registers an application thread with the reactor.
     * @param [in] thread A reference to the application thread.
     */
    void register_thread(std::shared_ptr<ApplicationThread> thread);

    /**
     * @brief Returns the name of a thread given its ID.
     *
     * It is impure virtual so it can be overridden by a mock.
     *
     * @param [in] id The ID of the thread.
     * @returns std::string The name of the thread.
     * @throws PreconditionAssertion if the ThreadID is not found.
     */
    virtual std::string get_thread_name_from_id(ThreadID id) const;

    bool is_finished() const {
        return is_finished_.load();
    }

    void route_message(ThreadID target_id, EventMessage message);

    bool is_initialized() const noexcept {
        return initialization_complete_.load(std::memory_order_acquire);
    }

    /**
     * @brief Creates and registers a timerfd for an ApplicationThread.
     * @param [in] owner_thread_id The ThreadID of the owning ApplicationThread.
     * @param [in] interval The delay or interval before the timer rings.
     * @param [in] type Whether the timer is single-shot or recurring.
     * @return The TimerID for the timer created.
     *
     * The Reactor owns the underlying timerfd and will deliver Timer events to
     * the specified ApplicationThread when the timer rings. One event is
     * generated per ring; no coalescing is performed.
     */
    TimerID create_timer_fd(ThreadID owner_thread_id, std::chrono::microseconds interval, TimerType type);

    /**
     * @brief Cancels and deregisters a previously created timerfd.
     * @param [in] id The TimerID to cancel.
     *
     * After cancellation, the Reactor will no longer watch the associated
     * timerfd and no further Timer events will be generated for this TimerID.
     * If the TimerID is unknown, this function is a no-op.
     */
    void cancel_timer_fd(TimerID id);

protected:
    std::atomic<bool> is_finished_{false};
    std::string shutdown_reason_;

private:
    void initialize_threads();
    void event_loop();
    void check_for_inactive_threads();
    void check_for_inactive_sockets();
    void dispatchEvents(int nfds, epoll_event* events);
    void finalize_threads_after_shutdown();

    void handle_timer_fd_ready(int fd);

    std::atomic<bool> initialization_complete_{false};

    // The core of the reactor
    int epoll_fd_{-1};

    // Monotonically increasing counter used to generate unique TimerIDs.
    TimerID next_timer_id_{0};

    // Timer bookkeeping: Reactor owns timerfds and maps them back to threads and TimerIDs.
    std::unordered_map<int, ThreadID> timer_fd_to_thread_;
    std::unordered_map<int, TimerID> timer_fd_to_timer_id_;

    // Handler management, mapping file descriptors to their handlers
    std::map<int, std::unique_ptr<EventHandler>> handlers_;

    // Thread management
    std::map<std::string, std::shared_ptr<ApplicationThread>> threads_;
    std::map<ThreadID, std::shared_ptr<ApplicationThread>> threads_by_thread_id_;

    // Configuration parameters
    ReactorConfiguration config_;

    QuillLogger& logger_;
};

} // namespace pubsub_itc_fw
