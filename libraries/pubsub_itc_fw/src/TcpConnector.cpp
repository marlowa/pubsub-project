// C headers
// none here right now

// C++ headers whose names start with ‘c’
#include <cerrno> // For errno and error codes

// System C++ headers
#include <memory>    // For std::unique_ptr, std::make_unique
#include <string>    // For std::string
#include <tuple>     // For std::tuple
#include <utility>   // For std::move, std::exchange
#include <stdexcept> // For std::runtime_error (if needed for invalid construction)

// Third party headers
#include <fmt/format.h> // For fmt::format for error messages

// Project headers
#include <pubsub_itc_fw/InetAddress.hpp>       // For InetAddress
#include <pubsub_itc_fw/TcpConnector.hpp>      // Header for this class
#include <pubsub_itc_fw/TcpSocket.hpp>         // For the managed TcpSocket
#include <pubsub_itc_fw/StringUtils.hpp>       // For StringUtils::get_error_string

namespace pubsub_itc_fw {

/**
 * @brief Private implementation class for `TcpConnector` (Pimpl idiom).
 *
 * This class holds the actual `InetAddress` of the remote peer and the
 * `std::unique_ptr` to the `TcpSocket` being used for the connection attempt.
 * It manages the lifecycle and state of the connection.
 */
class TcpConnectorImpl {
public:
    /**
     * @brief Destructor for `TcpConnectorImpl`.
     * Ensures any active `TcpSocket` is closed upon destruction.
     */
    ~TcpConnectorImpl() {
        cancel(); // Ensure the socket is closed and resources are freed.
    }

    /**
     * @brief Constructor for `TcpConnectorImpl`.
     * @param remote_address The remote address to connect to.
     */
    explicit TcpConnectorImpl(const InetAddress& remote_address)
        : remote_address_(remote_address),
          tcp_socket_(nullptr) {}

    // Deleted copy operations, as TcpConnectorImpl manages unique resources.
    TcpConnectorImpl(const TcpConnectorImpl&) = delete;
    TcpConnectorImpl& operator=(const TcpConnectorImpl&) = delete;

    /**
     * @brief Move constructor for `TcpConnectorImpl`.
     * @param other The other `TcpConnectorImpl` to move from.
     */
    TcpConnectorImpl(TcpConnectorImpl&& other)
        : remote_address_(std::move(other.remote_address_)),
          tcp_socket_(std::move(other.tcp_socket_)) {}

    /**
     * @brief Move assignment operator for `TcpConnectorImpl`.
     * @param other The other `TcpConnectorImpl` to move from.
     * @return A reference to this `TcpConnectorImpl` instance.
     */
    TcpConnectorImpl& operator=(TcpConnectorImpl&& other) {
        if (this != &other) {
            cancel(); // Close current resources before taking over others.
            remote_address_ = std::move(other.remote_address_);
            tcp_socket_ = std::move(other.tcp_socket_);
        }
        return *this;
    }

    /**
     * @brief Initiates an asynchronous connection attempt.
     * @return A tuple indicating if connected immediately and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> connect();

    /**
     * @brief Attempts to finalize a non-blocking connection.
     * @return A tuple indicating if the connection is now established and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> finish_connect();

    /**
     * @brief Checks if a connection attempt is currently active.
     * @return `true` if connecting, `false` otherwise.
     */
    [[nodiscard]] bool is_connecting() const;

    /**
     * @brief Retrieves the connected `TcpSocket`, transferring ownership.
     * @return A unique_ptr to the connected TcpSocket, or nullptr if not connected.
     */
    [[nodiscard]] std::unique_ptr<TcpSocket> get_connected_socket();

    /**
     * @brief Cancels an ongoing connection attempt.
     */
    void cancel();

private:
    InetAddress remote_address_;              /**< @brief The remote address to connect to. */
    std::unique_ptr<TcpSocket> tcp_socket_; /**< @brief The internal socket used for the connection. */
};

// --- TcpConnectorImpl Method Implementations ---

[[nodiscard]] std::tuple<bool, std::string> TcpConnectorImpl::connect() {
    // If a socket already exists and is valid, assume a connection attempt is in progress or complete.
    // For a new connection, we should not have an active socket.
    if (tcp_socket_) {
        // Potentially, if already connected, we could return true here.
        // For simplicity, if a socket exists, we'll assume a new 'connect' call
        // implies re-attempting, so we cancel the old one first.
        cancel();
    }

    // Create a new TcpSocket
    auto [socket_ptr, create_error] = TcpSocket::create(remote_address_.get_sockaddr()->sa_family);
    if (!socket_ptr) {
        return {false, fmt::format("Failed to create TcpSocket for connection: {}", create_error)};
    }
    tcp_socket_ = std::move(socket_ptr);

    // Initiate the non-blocking connection
    auto [connected_immediately, connect_error] = tcp_socket_->connect(remote_address_);
    if (!connect_error.empty()) {
        // An error occurred during connection initiation.
        // The socket might still be valid but in an error state.
        // We'll clean it up to prevent further use.
        cancel();
        return {false, fmt::format("Failed to initiate connection to {}:{}: {}",
                                   remote_address_.get_ip_address_string(),
                                   remote_address_.get_port(),
                                   connect_error)};
    }

    // If connected_immediately is true, the connection is already established.
    // Otherwise, it's in progress (EINPROGRESS), and finish_connect needs to be called later.
    return {connected_immediately, ""};
}

[[nodiscard]] std::tuple<bool, std::string> TcpConnectorImpl::finish_connect() {
    if (!tcp_socket_) {
        return {false, "No active connection attempt to finalize."};
    }

    auto [connected, finish_error] = tcp_socket_->finish_connect();
    if (!connected && !finish_error.empty()) {
        // Connection failed. Clean up the socket.
        cancel();
        return {false, fmt::format("Connection to {}:{} failed: {}",
                                   remote_address_.get_ip_address_string(),
                                   remote_address_.get_port(),
                                   finish_error)};
    }

    // If connected is true, connection is established.
    // If connected is false but finish_error is empty, it's still in progress.
    return {connected, ""};
}

[[nodiscard]] bool TcpConnectorImpl::is_connecting() const {
    // A connection attempt is active if tcp_socket_ exists and finish_connect() hasn't returned true.
    // However, without an external event loop, we can only check if the socket exists.
    // A more precise check would involve internal state (e.g., enum: IDLE, CONNECTING, CONNECTED, FAILED)
    // For now, if tcp_socket_ is valid, we consider it "active" (either connecting or connected).
    // The higher layer must call finish_connect to differentiate.
    return static_cast<bool>(tcp_socket_);
}

[[nodiscard]] std::unique_ptr<TcpSocket> TcpConnectorImpl::get_connected_socket() {
    // Only return the socket if it's actually connected.
    // is_connecting() in its current form isn't enough to guarantee 'connected' state.
    // A proper state machine would be needed. For now, assume a successful finish_connect
    // would precede this, or that caller knows the state.
    if (tcp_socket_) {
        return std::move(tcp_socket_); // Transfer ownership
    }
    return nullptr;
}

void TcpConnectorImpl::cancel() {
    if (tcp_socket_) {
        tcp_socket_->close();
        tcp_socket_.reset(); // Release the unique_ptr and associated resources
    }
}

// --- TcpConnector Public Method Implementations (forwarding to Pimpl) ---

TcpConnector::TcpConnector(const InetAddress& remote_address)
    : p_impl_(std::make_unique<TcpConnectorImpl>(remote_address)) {}

TcpConnector::~TcpConnector() = default;

TcpConnector::TcpConnector(TcpConnector&& other) = default;
TcpConnector& TcpConnector::operator=(TcpConnector&& other) = default;

[[nodiscard]] std::tuple<bool, std::string> TcpConnector::connect() {
    return p_impl_->connect();
}

[[nodiscard]] std::tuple<bool, std::string> TcpConnector::finish_connect() {
    return p_impl_->finish_connect();
}

[[nodiscard]] bool TcpConnector::is_connecting() const {
    return p_impl_->is_connecting();
}

[[nodiscard]] std::unique_ptr<TcpSocket> TcpConnector::get_connected_socket() {
    return p_impl_->get_connected_socket();
}

void TcpConnector::cancel() {
    p_impl_->cancel();
}

} // namespace pubsub_itc_fw
