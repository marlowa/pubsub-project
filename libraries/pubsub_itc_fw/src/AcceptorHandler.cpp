// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <sys/epoll.h>

#include <pubsub_itc_fw/AcceptorHandler.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

namespace pubsub_itc_fw {

AcceptorHandler::AcceptorHandler(int fd, InboundListener& listener,
                                 Reactor& reactor, QuillLogger& logger)
    : fd_(fd)
    , listener_(listener)
    , reactor_(reactor)
    , logger_(logger)
{
}

bool AcceptorHandler::handle_event(uint32_t events)
{
    if (!(events & EPOLLIN)) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "AcceptorHandler::handle_event: unexpected event mask 0x{:x} on listening fd {}",
            events, fd_);
        return true;
    }

    reactor_.on_accept(listener_);
    return true;
}

} // namespace pubsub_itc_fw
