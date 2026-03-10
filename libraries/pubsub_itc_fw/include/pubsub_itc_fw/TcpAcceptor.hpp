#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <memory>  // For std::unique_ptr
#include <string>  // For std::string in error messages
#include <tuple>   // For std::tuple return types
#include <utility> // For std::move

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/InetAddress.hpp> // For specifying the local address to bind to
#include <pubsub_itc_fw/TcpSocket.hpp>   // For the listening socket and accepted client sockets

namespace pubsub_itc_fw {

// Forward declaration of the private implementation class (Pimpl idiom).
// This hides implementation details from the header, reducing compile times
// and improving encapsulation.
class TcpAcceptorImpl;

/**
 * @brief Manages the process of accepting inbound TCP connections.
 *
 * This class encapsulates the server-side logic for binding to a local address,
 * listening for incoming connections, and accepting new client `TcpSocket` instances.
 * It is designed to work with non-blocking sockets, where `accept()` will return
 * immediately if no connections are pending.
 *
 * It uses the Pimpl idiom to hide its implementation details. Error reporting
 * is done via `std::tuple<std::unique_ptr<T>, std::string>`.
 */
class TcpAcceptor  {
public:
    /**
     * @brief Destructor for `TcpAcceptor`.
     * Ensures proper cleanup of the internal implementation and any associated
     * listening `TcpSocket`.
     */
    ~TcpAcceptor();

    /**
     * @brief Constructs a `TcpAcceptor` instance.
     *
     * This constructor prepares the acceptor to listen on the specified local
     * address. It does not start the listening process immediately.
     *
     * @param local_address The `InetAddress` (IP and port) to bind and listen on.
     * @param backlog The maximum length of the queue of pending connections.
     * @return A `std::tuple` containing:
     * - A `std::unique_ptr<TcpAcceptor>` to the newly created acceptor on success.
     * This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if construction failed.
     * It will be empty on success.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<TcpAcceptor>, std::string> create(const InetAddress& local_address, int backlog);

    // Deleted copy operations to ensure unique ownership of the underlying Pimpl.
    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    // Move operations to allow transferring ownership of the acceptor.
    TcpAcceptor(TcpAcceptor&& other);
    TcpAcceptor& operator=(TcpAcceptor&& other);

    /**
     * @brief Accepts a new incoming client connection.
     *
     * This is a non-blocking operation. If no pending connections are available,
     * it will return `nullptr` for the new socket and an empty error string
     * (the underlying `TcpSocket::accept()` will report `EWOULDBLOCK` or `EAGAIN`).
     * The returned `TcpSocket` for the client will already be set to non-blocking mode.
     *
     * @returns A `std::tuple` containing:
     * - A `std::unique_ptr<TcpSocket>` to the newly accepted client socket on success.
     * This pointer will be `nullptr` if no connection is available or on error.
     * - A `std::unique_ptr<InetAddress>` to the remote address of the accepted client.
     * This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if accepting failed.
     * It will be empty on success (including when no connection is available).
     */
    [[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> accept_connection();

    /**
     * @brief Retrieves the raw socket file descriptor of the listening socket.
     *
     * This is intended for integration with system event notification mechanisms
     * like `epoll`. The caller should not `close()` this file descriptor directly,
     * as its ownership is managed by the `TcpAcceptor` object.
     *
     * @returns An `int` representing the listening socket file descriptor, or -1 if invalid.
     */
    [[nodiscard]] int get_listening_file_descriptor() const;

private:
    /**
     * @brief Private constructor to be used by the static factory method.
     * @param p_impl A `std::unique_ptr` to the pre-initialized implementation.
     */
    explicit TcpAcceptor(std::unique_ptr<TcpAcceptorImpl> p_impl);

    std::unique_ptr<TcpAcceptorImpl> p_impl_; /**< @brief Pointer to the private implementation. */
};

} // namespace pubsub_itc_fw
