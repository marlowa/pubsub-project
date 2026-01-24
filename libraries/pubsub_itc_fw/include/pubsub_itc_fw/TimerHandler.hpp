#if 0
#pragma once

#include <memory>
#include <sys/epoll.h>

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/Timer.hpp>

namespace pubsub_itc_fw {

/**
 * @brief An `EventHandler` for a timer event.
 *
 * This class inherits from `EventHandler` and correctly implements the
 * `handle_event` method to process timer events. This class would be
 * registered with the Reactor to handle a specific timer file descriptor.
 */
class TimerHandler final : public EventHandler {
public:
    /**
     * @brief Constructs a TimerHandler.
     * @param [in] fd The file descriptor of the timer.
     * @param [in] timer A unique pointer to the Timer object.
     * @param [in] processing_thread The thread to which the timer event will be dispatched.
     */
    TimerHandler(int fd, std::unique_ptr<Timer> timer, std::shared_ptr<ApplicationThread> processing_thread);

    /**
     * @brief Destructor for TimerHandler.
     */
    ~TimerHandler() override = default;

    /**
     * @brief Handles a timer event detected by the Reactor.
     * @param [in] events The event types that occurred (e.g., EPOLLIN).
     * @return True if the handler successfully processed the event, false otherwise.
     */
    bool handle_event(uint32_t events) noexcept override;

    /**
     * @brief Returns the file descriptor associated with this handler.
     * @return The file descriptor.
     */
    int get_fd() const noexcept override {
        return fd_;
    }

private:
    int fd_;
    std::unique_ptr<Timer> timer_;
    std::shared_ptr<ApplicationThread> processing_thread_;
};

} // namespace pubsub_itc_fw

#endif
