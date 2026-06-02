// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <tuple>

#include <pubsub_itc_fw/InboundConnection.hpp>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

InboundConnection::InboundConnection(ConnectionID id, std::unique_ptr<TcpSocket> socket, ThreadID target_thread_id,
                                     std::unique_ptr<ProtocolHandlerInterface> handler, std::string peer_description,
                                     bool idle_timeout_exempt)
    : id_(id)
    , peer_description_(std::move(peer_description))
    , target_thread_id_(target_thread_id)
    , socket_(std::move(socket))
    , handler_(std::move(handler))
    , last_activity_time_(std::chrono::steady_clock::now())
    , idle_timeout_exempt_(idle_timeout_exempt) {
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

} // namespace pubsub_itc_fw
