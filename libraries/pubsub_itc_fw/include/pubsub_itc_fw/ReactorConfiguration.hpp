#pragma once

#include <cstdint> // For uint16_t

#include <chrono>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief Configuration data for the Reactor.
 *
 * This struct holds various settings that control the behavior of the Reactor,
 * such as the maximum number of events it should process in a single loop and
 * the port it should listen on.
 */
struct ReactorConfiguration {
    size_t max_events_per_loop = 64; /**< The maximum number of events to handle in a single `epoll_wait` call. */
    uint16_t port = 8080; /**< The port number the application's TCP acceptor should listen on. */

    /**
     * @brief The interval for checking for inactive threads and sockets.
     * This interval is also used to check if the reactor has been told to shutdown.
     */
    std::chrono::milliseconds inactivity_check_interval_{std::chrono::seconds{1}};

    /**
     * @brief The maximum allowed inactivity interval for inter-thread communication.
     */
    std::chrono::milliseconds itc_maximum_inactivity_interval_{std::chrono::seconds{60}};

    /**
     * @brief The maximum allowed inactivity interval for sockets.
     */
    std::chrono::milliseconds socket_maximum_inactivity_interval_{std::chrono::seconds{60}};

    // TODO need to revise what this is for this is how long we wait for INIT to complete
    // We probably need to add more smarts to the backstop check for this
    std::chrono::milliseconds init_phase_timeout_{std::chrono::seconds{10}};

    std::chrono::milliseconds shutdown_timeout_{std::chrono::seconds{1}};
};

} // namespace pubsub_itc_fw
