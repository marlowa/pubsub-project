#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/Timer.hpp>

namespace pubsub_itc_fw {

class Reactor;
class ApplicationThread;

/**
 * @brief An `EventHandler` for a timer event.
 *
 * This class inherits from `EventHandler` and correctly implements the
 * `handle_event` method to process timer events. This class would be
 * registered with the Reactor to handle a specific timer file descriptor.
 */
class TimerHandler : public EventHandler {
  public:
    ~TimerHandler() override;

    TimerHandler(const Timer& timer, Reactor& reactor);

    /**
     * @brief Handles a timer event detected by the Reactor.
     * @param [in] events The event types that occurred (e.g., EPOLLIN).
     * @return True if the handler successfully processed the event, false otherwise.
     */
    [[nodiscard]] bool handle_event(uint32_t events) override;

    /**
     * @brief Returns the file descriptor associated with this handler.
     * @return The file descriptor.
     */
    int get_fd() const override {
        return fd_;
    }

    const Timer& get_timer() const {
        return timer_;
    }

  private:
    int fd_{-1};
    Timer timer_;
    Reactor& reactor_;
    ApplicationThread* owner_thread_; // lock-free pointer to owner thread
};

} // namespace pubsub_itc_fw
