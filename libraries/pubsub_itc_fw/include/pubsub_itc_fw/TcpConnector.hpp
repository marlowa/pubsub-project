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

class TcpConnectorImpl;

/**
 * @brief Initiates and finalizes non-blocking outbound TCP connections.
 *
 * The connector is stateless with respect to remote endpoints. Each call to
 * connect() begins a new non-blocking connection attempt to the specified
 * InetAddress. The caller must later invoke finish_connect() when the socket
 * becomes writable. Ownership of the connected TcpSocket is transferred via
 * get_connected_socket() once the connection is fully established.
 */
class TcpConnector {
public:
    ~TcpConnector();
    TcpConnector();

    TcpConnector(const TcpConnector&) = delete;
    TcpConnector& operator=(const TcpConnector&) = delete;

    TcpConnector(TcpConnector&& other);
    TcpConnector& operator=(TcpConnector&& other);

    /**
     * @brief Begins a non-blocking connection attempt to the given remote address.
     *
     * @returns A tuple:
     * - bool: true if connected immediately, false if the connection is in progress.
     * - string: error message if initiation failed; empty on success.
     */
    [[nodiscard]] std::tuple<bool, std::string>
    connect(const InetAddress& remote_address);

    /**
     * @brief Attempts to complete a non-blocking connection.
     *
     * Should be called when the underlying socket becomes writable.
     *
     * @returns A tuple:
     * - bool: true if the connection is now fully established.
     * - string: error message if the connection failed; empty if still in progress.
     */
    [[nodiscard]] std::tuple<bool, std::string>
    finish_connect();

    /**
     * @brief Indicates whether a connection attempt is currently active.
     *
     * @returns true if a connection is in progress; false otherwise.
     */
    [[nodiscard]] bool is_connecting() const;

    /**
     * @brief Transfers ownership of the connected socket.
     *
     * Only returns a non-null pointer if the connection is fully established.
     */
    [[nodiscard]] std::unique_ptr<TcpSocket>
    get_connected_socket();

    /**
     * @brief Cancels any ongoing connection attempt.
     *
     * Closes the internal socket and resets the connector to the idle state.
     */
    void cancel();

private:
    std::unique_ptr<TcpConnectorImpl> p_impl_;
};

} // namespace pubsub_itc_fw
