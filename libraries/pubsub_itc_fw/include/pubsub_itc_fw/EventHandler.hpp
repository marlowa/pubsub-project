#pragma once

#include <memory>
#include <unistd.h>
#include <sys/epoll.h>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief This is an abstract base class that provides a common interface
 * for all event sources handled by the Reactor.
 *
 * Concrete classes will derive from this to handle specific events, such as
 * incoming data on a socket, a timer firing, or a message from an ITC queue.
 */
class EventHandler {
public:
    /**
     * @brief The pure virtual destructor ensures that derived classes are
     * properly destroyed through a base class pointer.
     */
    virtual ~EventHandler() = default;

    EventHandler() = default;

    /**
     * @brief Handles an event detected by the Reactor.
     *
     * This is the core method that the Reactor's event loop will call
     * when an event is ready for processing on this handler's file descriptor.
     *
     * @param events The event types that occurred (e.g., EPOLLIN).
     * @return True if the handler successfully processed the event, false otherwise.
     */
    virtual bool handle_event(uint32_t events) noexcept = 0;

    /**
     * @brief Returns the file descriptor associated with this handler.
     * @return The file descriptor.
     */
    virtual int get_fd() const noexcept = 0;

private:
    // A deleted copy constructor and assignment operator to prevent copying.
    EventHandler(const EventHandler&) = delete;
    EventHandler& operator=(const EventHandler&) = delete;
};

} // namespace pubsub_itc_fw
