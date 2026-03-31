#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <memory>
#include <sys/epoll.h>

namespace pubsub_itc_fw {

/**
 * @brief An `EventHandler` for a network socket.
 *
 * This class inherits from `EventHandler` and correctly implements the
 * `handle_event` method to process I/O events on a specific file descriptor.
 * It encapsulates the socket file descriptor and a reference to the thread
 * that should process the data.
 */
class SocketHandler  : public EventHandler {
public:
    ~SocketHandler() override;

    /**
     * @brief Constructs a SocketHandler.
     * @param [in] fd The file descriptor of the socket.
     * @param [in] processing_thread The thread to which data will be dispatched.
     */
    SocketHandler(int fd, std::shared_ptr<ApplicationThread> processing_thread)
        : fd_(fd), processing_thread_(std::move(processing_thread)) {}

    /**
     * @brief Handles a socket event detected by the Reactor.
     * @param [in] events The event types that occurred (e.g., EPOLLIN).
     * @return True if the handler successfully processed the event, false otherwise.
     */
    bool handle_event(uint32_t events) override;

    /**
     * @brief Returns the file descriptor associated with this handler.
     * @return The file descriptor.
     */
    int get_fd() const override {
        return fd_;
    }

private:
    int fd_;
    std::shared_ptr<ApplicationThread> processing_thread_;
};

} // namespace pubsub_itc_fw
