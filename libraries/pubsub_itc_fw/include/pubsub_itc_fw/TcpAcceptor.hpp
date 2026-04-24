#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>

namespace pubsub_itc_fw {

class TcpAcceptorImpl;

/**
 * @brief Manages inbound TCP connections using a non-blocking listening socket.
 *
 * Construction is performed via the static create() method, which reports all
 * errors explicitly rather than throwing exceptions.
 */
class TcpAcceptor {
  public:
    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    TcpAcceptor(TcpAcceptor&& other);
    TcpAcceptor& operator=(TcpAcceptor&& other);

    /**
     * @brief Creates a TcpAcceptor bound to the given local address and backlog.
     *
     * @returns A tuple:
     * - unique_ptr<TcpAcceptor>: valid on success, nullptr on failure.
     * - string: error message on failure, empty on success.
     */
    [[nodiscard]]
    static std::tuple<std::unique_ptr<TcpAcceptor>, std::string> create(const InetAddress& local_address, int backlog);

    /**
     * @brief Accepts a new incoming connection in non-blocking mode.
     *
     * @returns A tuple:
     * - unique_ptr<TcpSocket>: the accepted client socket, or nullptr.
     * - unique_ptr<InetAddress>: the client's remote address, or nullptr.
     * - string: error message on failure; empty on success or EAGAIN/EWOULDBLOCK.
     */
    [[nodiscard]]
    std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> accept_connection();

    /**
     * @brief Returns the listening socket's file descriptor.
     */
    [[nodiscard]]
    int get_listening_file_descriptor() const;

  private:
    explicit TcpAcceptor(std::unique_ptr<TcpAcceptorImpl> p_impl);

    std::unique_ptr<TcpAcceptorImpl> p_impl_;
};

} // namespace pubsub_itc_fw
