// C headers
// none here yet

#include <sys/socket.h> // For accept4

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
#include <pubsub_itc_fw/TcpAcceptor.hpp>       // Header for this class
#include <pubsub_itc_fw/TcpSocket.hpp>         // For the listening TcpSocket and accepted client sockets
#include <pubsub_itc_fw/StringUtils.hpp> // For StringUtils::get_error_string

namespace pubsub_itc_fw {

/**
 * @brief Private implementation class for `TcpAcceptor` (Pimpl idiom).
 *
 * This class holds the actual `InetAddress` of the local bind address and the
 * `std::unique_ptr` to the `TcpSocket` used for listening. It manages the
 * lifecycle and state of the listening socket and handles accepting new clients.
 */
class TcpAcceptorImpl {
public:
    /**
     * @brief Destructor for `TcpAcceptorImpl`.
     * Ensures the listening `TcpSocket` is closed upon destruction.
     */
    ~TcpAcceptorImpl() {
        close_listening_socket(); // Ensure the socket is closed and resources are freed.
    }

    /**
     * @brief Constructor for `TcpAcceptorImpl`.
     * @param local_address The local address to bind and listen on.
     * @param backlog The maximum length of the queue of pending connections.
     */
    explicit TcpAcceptorImpl(const InetAddress& local_address, int backlog);

    // Deleted copy operations, as TcpAcceptorImpl manages unique resources.
    TcpAcceptorImpl(const TcpAcceptorImpl&) = delete;
    TcpAcceptorImpl& operator=(const TcpAcceptorImpl&) = delete;

    /**
     * @brief Move constructor for `TcpAcceptorImpl`.
     * @param other The other `TcpAcceptorImpl` to move from.
     */
    TcpAcceptorImpl(TcpAcceptorImpl&& other)
        : local_address_(std::move(other.local_address_)),
          listening_socket_(std::move(other.listening_socket_)) {}

    /**
     * @brief Move assignment operator for `TcpAcceptorImpl`.
     * @param other The other `TcpAcceptorImpl` to move from.
     * @return A reference to this `TcpAcceptorImpl` instance.
     */
    TcpAcceptorImpl& operator=(TcpAcceptorImpl&& other) {
        if (this != &other) {
            close_listening_socket(); // Close current resources before taking over others.
            local_address_ = std::move(other.local_address_);
            listening_socket_ = std::move(other.listening_socket_);
        }
        return *this;
    }

    /**
     * @brief Accepts a new incoming client connection.
     * @return A tuple containing the new client socket, its remote address, and an error string.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> accept_connection();

    /**
     * @brief Retrieves the raw socket file descriptor of the listening socket.
     * @return An int representing the listening socket file descriptor, or -1 if invalid.
     */
    [[nodiscard]] int get_listening_file_descriptor() const;

private:
    /**
     * @brief Helper to close the listening socket.
     */
    void close_listening_socket();

    InetAddress local_address_;                /**< @brief The local address this acceptor is bound to. */
    std::unique_ptr<TcpSocket> listening_socket_; /**< @brief The socket used for listening for connections. */
};

// --- TcpAcceptorImpl Method Implementations ---

TcpAcceptorImpl::TcpAcceptorImpl(const InetAddress& local_address, int backlog)
    : local_address_(local_address),
      listening_socket_(nullptr) // Initialize to nullptr, create in constructor body
{
    // 1. Create the listening socket
    auto [socket_ptr, create_error] = TcpSocket::create(local_address_.get_sockaddr()->sa_family);
    if (!socket_ptr) {
        throw std::runtime_error(fmt::format("Failed to create listening TcpSocket: {}", create_error));
    }
    listening_socket_ = std::move(socket_ptr);

    // 2. Bind the socket to the local address
    auto [bind_success, bind_error] = listening_socket_->bind(local_address_);
    if (!bind_success) {
        listening_socket_.reset(); // Ensure socket is closed on error
        throw std::runtime_error(fmt::format("Failed to bind listening socket to {}:{}: {}",
                                             local_address_.get_ip_address_string(),
                                             local_address_.get_port(),
                                             bind_error));
    }

    // 3. Put the socket into listening mode
    auto [listen_success, listen_error] = listening_socket_->listen(backlog);
    if (!listen_success) {
        listening_socket_.reset(); // Ensure socket is closed on error
        throw std::runtime_error(fmt::format("Failed to set listening socket to listen mode: {}", listen_error));
    }
}

void TcpAcceptorImpl::close_listening_socket() {
    if (listening_socket_) {
        listening_socket_->close();
        listening_socket_.reset(); // Release the unique_ptr
    }
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> TcpAcceptorImpl::accept_connection() {
    if (!listening_socket_) {
        return {nullptr, nullptr, "Acceptor is not initialized or listening socket is closed."};
    }

    // Call the underlying TcpSocket's accept method
    auto [client_socket_ptr, client_address_ptr, accept_error] = listening_socket_->accept();

    // If client_socket_ptr is nullptr and accept_error is empty, it means EWOULDBLOCK/EAGAIN
    // and no connection is ready, which is not an error in a non-blocking context.
    // We just forward the result.
    return {std::move(client_socket_ptr), std::move(client_address_ptr), accept_error};
}

[[nodiscard]] int TcpAcceptorImpl::get_listening_file_descriptor() const {
    if (listening_socket_) {
        return listening_socket_->get_file_descriptor();
    }
    return -1; // Invalid file descriptor
}

// --- TcpAcceptor Public Method Implementations (forwarding to Pimpl) ---

// Private constructor, used by the static factory method
TcpAcceptor::TcpAcceptor(std::unique_ptr<TcpAcceptorImpl> p_impl)
    : p_impl_(std::move(p_impl)) {}

TcpAcceptor::~TcpAcceptor() = default; // Destructor is defaulted in header and uses Pimpl's dtor

// Move constructor and assignment operator are defaulted in header

TcpAcceptor::TcpAcceptor(TcpAcceptor&& other) = default;
TcpAcceptor& TcpAcceptor::operator=(TcpAcceptor&& other) = default;

[[nodiscard]] std::tuple<std::unique_ptr<TcpAcceptor>, std::string> TcpAcceptor::create(const InetAddress& local_address, int backlog) {
    try {
        // Create the implementation object. Its constructor performs bind and listen.
        auto p_impl = std::make_unique<TcpAcceptorImpl>(local_address, backlog);
        // If successful, wrap it in a TcpAcceptor and return.
        return {std::unique_ptr<TcpAcceptor>(new TcpAcceptor(std::move(p_impl))), ""};
    } catch (const std::runtime_error& ex) {
        // Catch exceptions from TcpAcceptorImpl constructor and return as error tuple.
        return {nullptr, fmt::format("Failed to create TcpAcceptor: {}", ex.what())};
    }
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> TcpAcceptor::accept_connection() {
    if (!p_impl_) {
        return {nullptr, nullptr, "TcpAcceptor is not initialized (p_impl_ is null)."};
    }
    return p_impl_->accept_connection();
}

[[nodiscard]] int TcpAcceptor::get_listening_file_descriptor() const {
    if (!p_impl_) {
        return -1; // No implementation, so no file descriptor
    }
    return p_impl_->get_listening_file_descriptor();
}

} // namespace pubsub_itc_fw
