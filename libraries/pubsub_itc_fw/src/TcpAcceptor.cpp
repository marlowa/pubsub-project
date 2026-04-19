// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <sys/socket.h>
#include <cerrno>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

class TcpAcceptorImpl {
public:
    ~TcpAcceptorImpl() {
        close_listening_socket();
    }

    TcpAcceptorImpl(const InetAddress& local_address,
                    std::unique_ptr<TcpSocket> listening_socket)
        : local_address_(local_address),
          listening_socket_(std::move(listening_socket)) {}

    TcpAcceptorImpl(const TcpAcceptorImpl&) = delete;
    TcpAcceptorImpl& operator=(const TcpAcceptorImpl&) = delete;

    TcpAcceptorImpl(TcpAcceptorImpl&& other)
        : local_address_(std::move(other.local_address_)),
          listening_socket_(std::move(other.listening_socket_)) {}

    TcpAcceptorImpl& operator=(TcpAcceptorImpl&& other) {
        if (this != &other) {
            close_listening_socket();
            local_address_ = std::move(other.local_address_);
            listening_socket_ = std::move(other.listening_socket_);
        }
        return *this;
    }

    std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string>
    accept_connection() {
        if (!listening_socket_) {
            return {nullptr, nullptr, "Acceptor is not initialized or listening socket is closed."};
        }

        auto [client_socket_ptr, client_address_ptr, accept_error] =
            listening_socket_->accept();

        return {std::move(client_socket_ptr),
                std::move(client_address_ptr),
                accept_error};
    }

    [[nodiscard]] int get_listening_file_descriptor() const {
        return listening_socket_ ? listening_socket_->get_file_descriptor() : -1;
    }

private:
    void close_listening_socket() {
        if (listening_socket_) {
            listening_socket_->close();
            listening_socket_.reset();
        }
    }

    InetAddress local_address_;
    std::unique_ptr<TcpSocket> listening_socket_;
};

// --- TcpAcceptor public API ---

TcpAcceptor::TcpAcceptor(std::unique_ptr<TcpAcceptorImpl> p_impl)
    : p_impl_(std::move(p_impl)) {}

TcpAcceptor::~TcpAcceptor() = default;

TcpAcceptor::TcpAcceptor(TcpAcceptor&& other) = default;
TcpAcceptor& TcpAcceptor::operator=(TcpAcceptor&& other) = default;

std::tuple<std::unique_ptr<TcpAcceptor>, std::string>
TcpAcceptor::create(const InetAddress& local_address, int backlog) {
    // 1. Create socket
    auto [socket_ptr, create_error] =
        TcpSocket::create(local_address.get_sockaddr()->sa_family);
    if (!socket_ptr) {
        return {nullptr,
                fmt::format("Failed to create listening TcpSocket: {}", create_error)};
    }

    // 2. Bind
    auto [bind_success, bind_error] = socket_ptr->bind(local_address);
    if (!bind_success) {
        return {nullptr,
                fmt::format("Failed to bind listening socket to {}:{}: {}",
                            local_address.get_ip_address_string(),
                            local_address.get_port(),
                            bind_error)};
    }

    // 3. Listen
    auto [listen_success, listen_error] = socket_ptr->listen(backlog);
    if (!listen_success) {
        return {nullptr,
                fmt::format("Failed to set listening socket to listen mode: {}",
                            listen_error)};
    }

    // 4. Construct acceptor
    auto impl = std::make_unique<TcpAcceptorImpl>(local_address, std::move(socket_ptr));
    // TODO clang-tidy says the below leaks, maybe used make_unique?
    return {std::unique_ptr<TcpAcceptor>(new TcpAcceptor(std::move(impl))), ""};
}

std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string>
TcpAcceptor::accept_connection() {
    if (!p_impl_) {
        return {nullptr, nullptr, "TcpAcceptor is not initialized (p_impl_ is null)."};
    }
    return p_impl_->accept_connection();
}

int TcpAcceptor::get_listening_file_descriptor() const {
    return p_impl_ ? p_impl_->get_listening_file_descriptor() : -1;
}

} // namespace pubsub_itc_fw
