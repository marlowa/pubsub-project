// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <string>
#include <tuple>
#include <utility> // for std::move

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/InboundConnection.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

InboundConnection::InboundConnection(const ConnectionID& id, std::unique_ptr<TcpSocket> socket, ThreadID target_thread_id,
                                     std::unique_ptr<ProtocolHandlerInterface> handler, std::string peer_description, IdleTimeoutFlag idle_timeout)
    : id_(id)
    , peer_description_(std::move(peer_description))
    , target_thread_id_(target_thread_id)
    , socket_(std::move(socket))
    , handler_(std::move(handler))
    , last_activity_time_(std::chrono::steady_clock::now())
    , idle_timeout_(idle_timeout) {
    if (socket_ == nullptr) {
        throw PreconditionAssertion("InboundConnection: socket must not be null", __FILE__, __LINE__);
    }
    if (handler_ == nullptr) {
        throw PreconditionAssertion("InboundConnection: handler must not be null", __FILE__, __LINE__);
    }
}

int InboundConnection::get_fd() const {
    return socket_ ? socket_->get_file_descriptor() : -1;
}

std::tuple<bool, std::string, bool> InboundConnection::handle_read() {
    last_activity_time_ = std::chrono::steady_clock::now();
    return handler_->on_data_ready();
}

} // namespaces
