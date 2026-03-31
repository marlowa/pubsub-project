#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// C headers
#include <sys/socket.h> // For socket(), bind(), listen(), accept(), connect(), shutdown(), setsockopt(), getsockopt(), socklen_t
#include <unistd.h>     // For close()

// C++ headers whose names start with ‘c’
#include <cstdint> // For int, uint16_t for port numbers

// System C++ headers
#include <memory>  // For std::unique_ptr, std::make_unique
#include <string>  // For std::string in error messages
#include <tuple>   // For std::tuple return types

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/ByteStreamInterface.hpp>    // Base interface
#include <pubsub_itc_fw/InetAddress.hpp>            // For address types
#include <pubsub_itc_fw/utils/SimpleSpan.hpp>      // For SimpleSpan

namespace pubsub_itc_fw {

// Forward declaration for the Pimpl implementation class
class TcpSocketImpl;

/**
 * @brief Concrete implementation of `ByteStreamInterface` for TCP/IP sockets.
 *
 * This class encapsulates a raw Berkeley TCP socket file descriptor and provides
 * non-blocking operations for socket lifecycle management (creation, connection,
 * binding, listening, accepting, closing) and data transfer (sending, receiving).
 * It adheres to a modern C++17 design, using smart pointers, value-based error
 * reporting via `std::tuple`, and the Pimpl idiom to hide implementation details
 * and reduce compile-time dependencies.
 *
 * All I/O operations are designed to be non-blocking. Error conditions like
 * `EWOULDBLOCK` or `EAGAIN` are returned as negative `int` values (converted from
 * `errno`) to allow higher-level components (e.g., event loop) to handle them
 * asynchronously without blocking the calling thread.
 */
class TcpSocket  : public ByteStreamInterface {
  public:
    /**
     * @brief Destructor for `TcpSocket`.
     * Ensures proper closure of the underlying socket file descriptor and
     * cleanup of resources managed by the Pimpl implementation.
     */
    ~TcpSocket() override;

    /**
     * @brief Constructs a new `TcpSocket` instance.
     *
     * This constructor creates a new socket file descriptor in non-blocking mode.
     * It does not establish a connection or bind to an address; it merely prepares
     * the socket for subsequent operations like `connect()`, `bind()`, or `listen()`.
     *
     * @param[in] ip_version The IP version to use for the socket (AF_INET for IPv4, AF_INET6 for IPv6, or AF_UNSPEC).
     * @return A `std::tuple` containing:
     * - A `std::unique_ptr<TcpSocket>` to the newly created socket on success.
     * This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if socket creation failed.
     * It will be empty on success.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<TcpSocket>, std::string> create(int ip_version);

    /**
     * @brief Constructs a `TcpSocket` by adopting an existing socket file descriptor.
     *
     * This factory method is typically used after an `accept()` call on a listening socket,
     * where an existing, connected file descriptor needs to be managed by a `TcpSocket` object.
     * The adopted socket is immediately set to non-blocking mode.
     *
     * @param[in] socket_fd The existing socket file descriptor to adopt.
     * @return A `std::tuple` containing:
     * - A `std::unique_ptr<TcpSocket>` to the new `TcpSocket` instance on success.
     * This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if adoption or non-blocking setup failed.
     * It will be empty on success.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<TcpSocket>, std::string> adopt(int socket_fd);

    // ByteStreamInterface implementations
    /**
     * @brief Sends a specified amount of binary data over the TCP socket.
     *
     * This is a non-blocking operation. It attempts to send as much data as
     * possible without blocking. If the socket's send buffer is full, it will
     * return a negative value representing `EWOULDBLOCK` or `EAGAIN`.
     *
     * @param[in] data A `utils::SimpleSpan<const uint8_t>` representing the binary
     * data to be sent.
     * @return A `std::tuple` containing:
     * - An `int` indicating the number of bytes sent (>= 0) on success,
     * - A negative value (e.g., -EWOULDBLOCK, -ECONNRESET) on error or if the operation would block.
     * - A `std::string` containing an error message if the operation failed.
     */
    [[nodiscard]] std::tuple<int, std::string> send(utils::SimpleSpan<const uint8_t> data) override;

    /**
     * @brief Receives binary data from the TCP socket into the provided buffer.
     *
     * This is a non-blocking operation. It attempts to read as much data as
     * possible into the buffer. If no data is immediately available, it will
     * return 0 or a negative value representing `EWOULDBLOCK` or `EAGAIN`.
     * A return value of 0 bytes with no error indicates the peer has gracefully
     * closed its writing half of the connection.
     *
     * @param[out] buffer A `utils::SimpleSpan<uint8_t>` representing the buffer
     * where received data will be stored.
     * @return A `std::tuple` containing:
     * - An `int` indicating the number of bytes received (>= 0) on success.
     * - A negative value (e.g., -EWOULDBLOCK, -ECONNRESET) on error or if the operation would block.
     * - A `std::string` containing an error message if the operation failed.
     */
    [[nodiscard]] std::tuple<int, std::string> receive(utils::SimpleSpan<uint8_t> buffer) override;

    /**
     * @brief Closes the TCP socket connection.
     *
     * This releases the associated socket file descriptor. After this call,
     * the socket is no longer usable. This is distinct from `shutdown()`,
     * which only closes one direction of the connection.
     */
    void close() override;

    /**
     * @brief Retrieves the remote (peer) network address associated with this socket.
     *
     * This is typically used for connected sockets to determine who the peer is.
     *
     * @returns A `std::tuple` containing:
     * - A `std::unique_ptr<InetAddress>` to the peer's network address on success.
     * This pointer will be `nullptr` on failure (e.g., socket not connected).
     * - A `std::string` containing an error message if retrieving the
     * address failed. It will be empty on success.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> get_peer_address() const override;

    /**
     * @brief Retrieves the local network address associated with this socket.
     *
     * This is typically used for bound or connected sockets to determine the
     * local endpoint being used.
     *
     * @returns A `std::tuple` containing:
     * - A `std::unique_ptr<InetAddress>` to the local network address on success.
     * This pointer will be `nullptr` on failure (e.g., socket not bound).
     * - A `std::string` containing an error message if retrieving the
     * address failed. It will be empty on success.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> get_local_address() const;

    /**
     * @brief Connects the socket to a remote `InetAddress`.
     *
     * This is a non-blocking operation. For non-blocking sockets, `connect()`
     * typically returns -EINPROGRESS immediately. Completion of the connection
     * needs to be checked later, usually via `epoll` or by calling `getsockopt`
     * with `SO_ERROR`.
     *
     * @param[in] remote_address The `InetAddress` of the remote endpoint to connect to.
     * @return A `std::tuple` containing:
     * - A `bool` indicating if the connection was established immediately (`true`)
     * or if it's in progress (`false`).
     * - A `std::string` containing an error message if the connection failed
     * immediately. It will be empty if successful or in progress.
     */
    [[nodiscard]] std::tuple<bool, std::string> connect(const InetAddress& remote_address);

    /**
     * @brief Binds the socket to a local `InetAddress`.
     *
     * This is typically used by server sockets to listen on a specific address
     * and port. This operation must happen before `listen()`.
     *
     * @param[in] local_address The `InetAddress` to bind the socket to.
     * @return A `std::tuple` containing:
     * - A `bool` indicating success (`true`) or failure (`false`).
     * - A `std::string` containing an error message if binding failed.
     * It will be empty on success.
     */
    [[nodiscard]] std::tuple<bool, std::string> bind(const InetAddress& local_address);

    /**
     * @brief Puts the socket into a listening state for incoming connections.
     *
     * This is typically used by server sockets after `bind()`.
     *
     * @param[in] backlog The maximum length of the queue of pending connections.
     * @return A `std::tuple` containing:
     * - A `bool` indicating success (`true`) or failure (`false`).
     * - A `std::string` containing an error message if listening failed.
     * It will be empty on success.
     */
    [[nodiscard]] std::tuple<bool, std::string> listen(int backlog);

    /**
     * @brief Accepts a new incoming connection on a listening socket.
     *
     * This is a non-blocking operation. If no pending connections are available,
     * it will return `nullptr` for the new socket and a negative value representing
     * `EWOULDBLOCK` or `EAGAIN`. The returned `TcpSocket` will already be
     * set to non-blocking mode.
     *
     * @returns A `std::tuple` containing:
     * - A `std::unique_ptr<TcpSocket>` to the newly accepted client socket on success.
     * This pointer will be `nullptr` if no connection is available or on error.
     * - A `std::unique_ptr<InetAddress>` to the remote address of the accepted client.
     * This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if accepting failed.
     * It will be empty on success.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> accept();

    /**
     * @brief Retrieves the raw socket file descriptor.
     *
     * This is intended for integration with system event notification mechanisms
     * like `epoll`. The caller should not `close()` this file descriptor directly,
     * as its ownership is managed by the `TcpSocket` object.
     *
     * @returns An `int` representing the socket file descriptor, or -1 if the socket is closed or invalid.
     */
    int get_file_descriptor() const;

    /**
     * @brief Shuts down one or both halves of the connection.
     *
     * This function allows for a graceful shutdown of the sending, receiving,
     * or both parts of the connection without closing the file descriptor.
     *
     * @param[in] how The direction of shutdown (SHUT_RD, SHUT_WR, or SHUT_RDWR).
     * @return A `std::tuple` containing:
     * - A `bool` indicating success (`true`) or failure (`false`).
     * - A `std::string` containing an error message if shutdown failed.
     * It will be empty on success.
     */
    [[nodiscard]] std::tuple<bool, std::string> shutdown(int how);

    /**
     * @brief Checks the status of a non-blocking connection attempt.
     *
     * This should be called after `connect()` returned `false` (indicating `EINPROGRESS`)
     * and the socket is reported as writable by the event loop.
     *
     * @returns A `std::tuple` containing:
     * - A `bool` indicating if the connection is now established (`true`) or
     * if it failed (`false`).
     * - A `std::string` containing an error message if the connection failed.
     * It will be empty on success.
     */
    [[nodiscard]] std::tuple<bool, std::string> finish_connect() const;

    bool is_connected() const;

  private:
    /**
     * @brief Private constructor to be used by the static factory methods.
     *
     * This ensures that `TcpSocket` instances are only created via `create()` or `adopt()`.
     * @param[in] socket_fd The socket file descriptor to manage.
     */
    TcpSocket(int socket_fd);

    /**
     * @brief The Pimpl pointer to the implementation details of TcpSocket.
     *
     * This `std::unique_ptr` manages the lifecycle of the `TcpSocketImpl` object,
     * effectively hiding internal data members and dependencies from the public interface.
     */
    std::unique_ptr<TcpSocketImpl> p_impl_;

    // Explicitly delete copy and move constructors/assignment operators to ensure
    // unique ownership of the socket file descriptor.
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;
};

} // namespace pubsub_itc_fw
