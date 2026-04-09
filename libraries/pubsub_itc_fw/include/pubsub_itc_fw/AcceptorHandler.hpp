#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/InboundListener.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

class Reactor;

/**
 * @brief EventHandler for a TCP listening socket.
 *
 * @ingroup reactor_subsystem
 *
 * Registered with the reactor's epoll instance for `EPOLLIN` on the listening
 * socket fd of an `InboundListener`. When epoll signals a new incoming connection,
 * `handle_event()` calls `Reactor::on_accept()` which enforces the one-connection
 * rule, creates an `InboundConnection`, and delivers `ConnectionEstablished` to
 * the target ApplicationThread.
 *
 * Lifetime: owned by `Reactor::handlers_` map, keyed by listening socket fd.
 * The associated `InboundListener` is owned by `Reactor::inbound_listeners_`.
 */
class AcceptorHandler : public EventHandler {
public:
    ~AcceptorHandler() override = default;

    /**
     * @brief Constructs an AcceptorHandler.
     *
     * @param[in] fd       The listening socket file descriptor.
     * @param[in] listener The InboundListener this handler serves. Must outlive
     *                     this object (owned by Reactor::inbound_listeners_).
     * @param[in] reactor  The owning Reactor. Must outlive this object.
     * @param[in] logger   Logger instance. Must outlive this object.
     */
    AcceptorHandler(int fd, InboundListener& listener, Reactor& reactor, QuillLogger& logger);

    /**
     * @brief Handles an EPOLLIN event on the listening socket.
     *
     * Calls `Reactor::on_accept()` which accepts the connection and enforces
     * the one-connection-per-listener rule.
     *
     * @param[in] events The epoll event mask.
     * @return true always — errors are logged but do not stop the listener.
     */
    [[nodiscard]] bool handle_event(uint32_t events) override;

    /**
     * @brief Returns the listening socket file descriptor.
     */
    [[nodiscard]] int get_fd() const override { return fd_; }

private:
    int fd_;
    InboundListener& listener_;
    Reactor& reactor_;
    QuillLogger& logger_;
};

} // namespace pubsub_itc_fw
