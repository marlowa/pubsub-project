#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <sys/epoll.h>

namespace pubsub_itc_fw {

/**
 * @class Reactor
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

    /**
     * @brief Constructs a Reactor.
     * @param [in] config The reactor's configuration.
     * @param [in] logger The logger instance.
     */
    explicit Reactor(const ReactorConfiguration& config, LoggerInterface& logger);

    /**
     * @brief Starts the reactor's event loop.
     * @returns int An integer status code: 0 for normal shutdown, non-zero otherwise.
     */
    int run();

    /**
     * @brief Initiates a graceful shutdown of the reactor and all threads.
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
    void registerThread(ApplicationThread& thread);

    /**
     * @brief Returns the name of a thread given its ID.
     * @param [in] id The ID of the thread.
     * @returns std::string The name of the thread.
     * @throws PreconditionAssertion if the ThreadID is not found.
     */
    std::string getThreadNameFromID(ThreadID id) const;

private:
    void checkForInactiveThreads();
    void checkForInactiveSockets();
    void dispatchEvents(int nfds, epoll_event* events);

    // The core of the reactor
    int epoll_fd_;
    std::atomic<bool> is_finished_{false};

    // Handler management, mapping file descriptors to their handlers
    std::map<int, std::unique_ptr<EventHandler>> handlers_;

    // Thread management
    std::map<std::string, ApplicationThread*> threads_;
    std::map<ThreadID, ApplicationThread*> threads_by_thread_id_;

    // Configuration parameters
    ReactorConfiguration config_;

    // The LoggerInterface dependency
    LoggerInterface& logger_;
};

} // namespace pubsub_itc_fw
