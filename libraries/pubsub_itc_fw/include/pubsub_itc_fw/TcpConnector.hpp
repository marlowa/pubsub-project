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
#include <pubsub_itc_fw/InetAddress.hpp> // For specifying the remote address
#include <pubsub_itc_fw/TcpSocket.hpp>   // For the managed socket

namespace pubsub_itc_fw {

// Forward declaration of the private implementation class (Pimpl idiom).
// This hides implementation details from the header, reducing compile times
// and improving encapsulation.
class TcpConnectorImpl;

/**
 * @brief Manages the process of establishing an outbound TCP connection.
 *
 * This class encapsulates the client-side logic for connecting to a remote
 * `InetAddress` using a non-blocking `TcpSocket`. It handles the asynchronous
 * nature of connection attempts and provides methods to check connection status,
 * retrieve the connected socket, or cancel the attempt.
 *
 * It uses the Pimpl idiom to hide its implementation details. Error reporting
 * is done via `std::tuple<bool, std::string>` or `std::unique_ptr<T>, std::string`.
 */
class TcpConnector  {
public:
    /**
     * @brief Constructs a `TcpConnector` instance for a specific remote address.
     *
     * This constructor prepares the connector to initiate a connection to the
     * specified remote endpoint. It does not start the connection process immediately.
     *
     * @param remote_address The `InetAddress` of the remote server to connect to.
     */
    explicit TcpConnector(const InetAddress& remote_address);

    /**
     * @brief Destructor for `TcpConnector`.
     * Ensures proper cleanup of the internal implementation and any associated
     * `TcpSocket` if the connection is still in progress or failed internally.
     */
    ~TcpConnector();

    // Deleted copy operations to ensure unique ownership of the underlying Pimpl.
    TcpConnector(const TcpConnector&) = delete;
    TcpConnector& operator=(const TcpConnector&) = delete;

    // Move operations to allow transferring ownership of the connector.
    TcpConnector(TcpConnector&& other);
    TcpConnector& operator=(TcpConnector&& other);

    /**
     * @brief Initiates an asynchronous connection attempt to the remote address.
     *
     * This method creates an internal `TcpSocket` and begins the non-blocking
     * connection process. If the connection is established immediately (e.g., to
     * localhost), `true` is returned. Otherwise, `false` indicates the connection
     * is in progress, and its completion must be checked later via `finish_connect()`.
     *
     * @returns A `std::tuple` containing:
     * - A `bool` indicating if the connection was established immediately (`true`)
     * or if it's in progress (`false`).
     * - A `std::string` containing an error message if the initiation failed.
     * It will be empty if successful or in progress.
     */
    [[nodiscard]] std::tuple<bool, std::string> connect();

    /**
     * @brief Attempts to finalize a non-blocking connection.
     *
     * This method should be called when the underlying `TcpSocket` signals that
     * it is writable (e.g., from an event loop). It checks the status of the
     * asynchronous `connect()` call.
     *
     * @returns A `std::tuple` containing:
     * - A `bool` indicating if the connection is now fully established (`true`)
     * or if it ultimately failed (`false`).
     * - A `std::string` containing an error message if the connection failed.
     * It will be empty on successful completion.
     */
    [[nodiscard]] std::tuple<bool, std::string> finish_connect();

    /**
     * @brief Checks if the connector is currently in the process of connecting.
     * @returns `true` if a connection attempt is active, `false` otherwise.
     */
    [[nodiscard]] bool is_connecting() const;

    /**
     * @brief Retrieves the connected `TcpSocket` if the connection is established.
     *
     * This method transfers ownership of the `TcpSocket` from the connector
     * to the caller. After this call, the `TcpConnector` should no longer be
     * used as it will no longer own the socket.
     *
     * @returns A `std::unique_ptr<TcpSocket>` representing the connected socket
     * on success. Returns `nullptr` if no connection is established or if an
     * error occurs during retrieval.
     */
    [[nodiscard]] std::unique_ptr<TcpSocket> get_connected_socket();

    /**
     * @brief Cancels an ongoing connection attempt.
     *
     * This closes the internal `TcpSocket` and cleans up resources,
     * effectively aborting any pending connection.
     */
    void cancel();

private:
    std::unique_ptr<TcpConnectorImpl> p_impl_; /**< @brief Pointer to the private implementation. */
};

} // namespace pubsub_itc_fw
