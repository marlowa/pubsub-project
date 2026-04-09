// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>      // For fcntl, O_NONBLOCK
#include <netinet/tcp.h> // For TCP_NODELAY
#include <sys/socket.h> // For socket(), bind(), listen(), accept(), connect(), shutdown(), setsockopt(), getsockopt(), socklen_t
#include <unistd.h>     // For close()

// C++ headers whose names start with ‘c’
#include <cstdint> // For int, uint16_t
#include <cstring> // For strerror, memset
#include <cerrno>  // For errno

// System C++ headers
#include <memory>    // For std::unique_ptr, std::make_unique
#include <string>    // For std::string
#include <tuple>     // For std::tuple
#include <utility>   // for std::move

// Third party headers
#include <fmt/format.h> // For fmt::format

// Project headers
#include <pubsub_itc_fw/InetAddress.hpp>       // For InetAddress
#include <pubsub_itc_fw/TcpSocket.hpp>         // Header for this class
#include <pubsub_itc_fw/utils/SimpleSpan.hpp> // For SimpleSpan
#include <pubsub_itc_fw/StringUtils.hpp>       // For StringUtils::get_error_string

namespace pubsub_itc_fw {

namespace {

/**
 * @brief Helper function to set a socket to non-blocking mode.
 *
 * This function modifies the file status flags of the given socket file descriptor
 * to include `O_NONBLOCK`.
 *
 * @param[in] socket_fd The socket file descriptor to modify.
 * @return A `std::tuple<bool, std::string>` indicating success or failure.
 */
[[nodiscard]] std::tuple<bool, std::string> set_non_blocking(int socket_fd) {
    const int raw_flags = fcntl(socket_fd, F_GETFL, 0);
    if (raw_flags == -1) {
        return {false, fmt::format("Failed to get socket flags for fd {}: {}",
                                   socket_fd, StringUtils::get_errno_string())};
    }

    auto flags = static_cast<unsigned>(raw_flags);
    flags |= O_NONBLOCK;

    if (fcntl(socket_fd, F_SETFL, static_cast<int>(flags)) == -1) {
        return {false, fmt::format("Failed to set socket {} to non-blocking mode: {}",
                                   socket_fd, StringUtils::get_errno_string())};
    }

    return {true, ""};
}

[[nodiscard]] std::tuple<bool, std::string> set_tcp_no_delay(int socket_fd) {
    int flag = 1;
    if (::setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        return {false, fmt::format("Failed to set TCP_NODELAY on socket {}: {}",
                                   socket_fd, StringUtils::get_errno_string())};
    }
    return {true, ""};
}

} // un-named namespace

/**
 * @brief Implementation details for `TcpSocket`.
 *
 * This class holds all private data members and implements the actual socket
 * operations, hidden behind the Pimpl pointer.
 */
class TcpSocketImpl {
  public:
    /**
     * @brief Destructor for `TcpSocketImpl`.
     * Ensures the underlying socket file descriptor is properly closed.
     */
    ~TcpSocketImpl();

    /**
     * @brief Constructor for `TcpSocketImpl`.
     * Initializes the implementation with a given socket file descriptor.
     * @param[in] socket_fd The socket file descriptor to manage.
     */
    explicit TcpSocketImpl(int socket_fd);

    /**
     * @brief Sends a specified amount of binary data over the TCP socket.
     *
     * @param[in] data A `utils::SimpleSpan<const uint8_t>` representing the binary data to be sent.
     * @return A `std::tuple` containing the number of bytes sent or a negative errno, and an error string.
     */
    [[nodiscard]] std::tuple<int, std::string> send(utils::SimpleSpan<const uint8_t> data);

    /**
     * @brief Receives binary data from the TCP socket into the provided buffer.
     *
     * @param[out] buffer A `utils::SimpleSpan<uint8_t>` representing the buffer where received data will be stored.
     * @return A `std::tuple` containing the number of bytes received or a negative errno, and an error string.
     */
    [[nodiscard]] std::tuple<int, std::string> receive(utils::SimpleSpan<uint8_t> buffer);

    /**
     * @brief Closes the TCP socket connection.
     */
    void close();

    /**
     * @brief Retrieves the remote (peer) network address associated with this socket.
     *
     * @returns A `std::tuple` containing a `std::unique_ptr<InetAddress>` and an error string.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> get_peer_address() const;

    /**
     * @brief Retrieves the local network address associated with this socket.
     *
     * @returns A `std::tuple` containing a `std::unique_ptr<InetAddress>` and an error string.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> get_local_address() const;

    /**
     * @brief Connects the socket to a remote `InetAddress`.
     *
     * @param[in] remote_address The `InetAddress` of the remote endpoint to connect to.
     * @return A `std::tuple` containing a `bool` (connected immediately or in progress) and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> connect(const InetAddress& remote_address);

    /**
     * @brief Binds the socket to a local `InetAddress`.
     *
     * @param[in] local_address The `InetAddress` to bind the socket to.
     * @return A `std::tuple` containing a `bool` (success) and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> bind(const InetAddress& local_address);

    /**
     * @brief Puts the socket into a listening state for incoming connections.
     *
     * @param[in] backlog The maximum length of the queue of pending connections.
     * @return A `std::tuple` containing a `bool` (success) and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> listen(int backlog);

    /**
     * @brief Accepts a new incoming connection on a listening socket.
     *
     * @returns A `std::tuple` containing a `std::unique_ptr<TcpSocket>`,
     * a `std::unique_ptr<InetAddress>`, and an error string.
     */
    [[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> accept();

    /**
     * @brief Retrieves the raw socket file descriptor.
     *
     * @returns An `int` representing the socket file descriptor, or -1 if invalid.
     */
    [[nodiscard]] int get_file_descriptor() const;

    /**
     * @brief Shuts down one or both halves of the connection.
     *
     * @param[in] how The direction of shutdown (SHUT_RD, SHUT_WR, or SHUT_RDWR).
     * @return A `std::tuple` containing a `bool` (success) and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> shutdown(int how);

    /**
     * @brief Checks the status of a non-blocking connection attempt.
     *
     * @returns A `std::tuple` containing a `bool` (established or failed) and an error string.
     */
    [[nodiscard]] std::tuple<bool, std::string> finish_connect() const;

  private:
    int socketFileDescriptor; /**< @brief The raw socket file descriptor. */
};

// --- TcpSocketImpl Implementation ---

// Destructor is first as per coding style.
TcpSocketImpl::~TcpSocketImpl() {
    close(); // Ensure socket is closed when the Impl object is destroyed
}

TcpSocketImpl::TcpSocketImpl(int socket_fd) : socketFileDescriptor(socket_fd) {
    if (socketFileDescriptor != -1) {
        auto [nb_success, nb_error] = set_non_blocking(socketFileDescriptor);
        if (!nb_success) {
            // Factory methods handle this; the fd remains as provided.
            return;
        }
        auto [nd_success, nd_error] = set_tcp_no_delay(socketFileDescriptor);
        if (!nd_success) {
            // Non-fatal: log in production via the caller if needed.
        }
    }
}

[[nodiscard]] std::tuple<int, std::string> TcpSocketImpl::send(utils::SimpleSpan<const uint8_t> data) {
    if (socketFileDescriptor == -1) {
        return {-1, "Cannot send on an invalid or closed socket."};
    }

    if (data.empty()) {
        return {0, ""}; // Nothing to send
    }

    // Using `send` with MSG_NOSIGNAL to prevent SIGPIPE
    // NOLINTNEXTLINE(misc-include-cleaner)
    const ssize_t bytes_sent = ::send(socketFileDescriptor, data.data(), data.size(), MSG_NOSIGNAL);

    if (bytes_sent == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return {-EAGAIN, ""}; // Socket send buffer full — caller should wait for EPOLLOUT.
        }
        return {-errno, fmt::format("Failed to send data on socket {}: {}", socketFileDescriptor,
                                    StringUtils::get_errno_string())};
    }

    return {static_cast<int>(bytes_sent), ""};
}

[[nodiscard]] std::tuple<int, std::string> TcpSocketImpl::receive(utils::SimpleSpan<uint8_t> buffer) {
    if (socketFileDescriptor == -1) {
        return {-1, "Cannot receive on an invalid or closed socket."};
    }

    if (buffer.empty()) {
        return {0, ""}; // No buffer to receive into
    }

    const ssize_t bytes_received = ::recv(socketFileDescriptor, buffer.data(), buffer.size(), 0);

    if (bytes_received == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return {-EAGAIN, ""}; // No data available — caller should wait for next EPOLLIN.
        }
        return {-errno, fmt::format("Failed to receive data on socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    // A return of 0 bytes with no error typically means the peer closed its writing half.
    return {static_cast<int>(bytes_received), ""};
}

void TcpSocketImpl::close() {
    if (socketFileDescriptor != -1) {
        // Attempt to shut down both send/receive directions before closing the FD.
        // This is a more graceful way to close. Errors during shutdown are typically
        // ignored as the socket is being closed anyway.
        ::shutdown(socketFileDescriptor, SHUT_RDWR);

        ::close(socketFileDescriptor);
        socketFileDescriptor = -1; // Invalidate the file descriptor
    }
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> TcpSocketImpl::get_peer_address() const {
    if (socketFileDescriptor == -1) {
        return {nullptr, "Socket is invalid or closed."};
    }

    sockaddr_storage remote_addr_storage{};
    socklen_t remote_addr_len = sizeof(remote_addr_storage);

    if (::getpeername(socketFileDescriptor, reinterpret_cast<sockaddr*>(&remote_addr_storage), &remote_addr_len) == -1) {
        return {nullptr, fmt::format("Failed to get peer address for socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    return InetAddress::create(reinterpret_cast<const sockaddr*>(&remote_addr_storage), remote_addr_len);
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> TcpSocketImpl::get_local_address() const {
    if (socketFileDescriptor == -1) {
        return {nullptr, "Socket is invalid or closed."};
    }

    sockaddr_storage local_addr_storage{};
    socklen_t local_addr_len = sizeof(local_addr_storage);

    if (::getsockname(socketFileDescriptor, reinterpret_cast<sockaddr*>(&local_addr_storage), &local_addr_len) == -1) {
        return {nullptr, fmt::format("Failed to get local address for socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    return InetAddress::create(reinterpret_cast<const sockaddr*>(&local_addr_storage), local_addr_len);
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocketImpl::connect(const InetAddress& remote_address) {
    if (socketFileDescriptor == -1) {
        return {false, "Cannot connect on an invalid or closed socket."};
    }

    const int result = ::connect(socketFileDescriptor, remote_address.get_sockaddr(), remote_address.get_sockaddr_size());

    if (result == -1) {
        if (errno == EINPROGRESS) {
            return {false, ""}; // Connection in progress, not an error
        }
        if (errno == EISCONN) {
            return {true, ""}; // Already connected
        }
        return {false, fmt::format("Failed to connect socket {} to {}:{}: {}",
                                   socketFileDescriptor,
                                   remote_address.get_ip_address_string(),
                                   remote_address.get_port(),
                                   StringUtils::get_errno_string())};
    }

    return {true, ""}; // Connection established immediately (e.g., to localhost)
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocketImpl::bind(const InetAddress& local_address) {
    if (socketFileDescriptor == -1) {
        return {false, "Cannot bind on an invalid or closed socket."};
    }

    // Set SO_REUSEADDR option to allow reuse of local addresses immediately after close
    int reuseAddressValue = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (::setsockopt(socketFileDescriptor, SOL_SOCKET, SO_REUSEADDR, &reuseAddressValue, sizeof(reuseAddressValue)) == -1) {
        return {false, fmt::format("Failed to set SO_REUSEADDR on socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    const int result = ::bind(socketFileDescriptor, local_address.get_sockaddr(), local_address.get_sockaddr_size());
    if (result == -1) {
        return {false, fmt::format("Failed to bind socket {} to {}:{}: {}",
                                   socketFileDescriptor,
                                   local_address.get_ip_address_string(),
                                   local_address.get_port(),
                                   StringUtils::get_errno_string())};
    }

    return {true, ""};
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocketImpl::listen(int backlog) {
    if (socketFileDescriptor == -1) {
        return {false, "Cannot listen on an invalid or closed socket."};
    }

    const int result = ::listen(socketFileDescriptor, backlog);
    if (result == -1) {
        return {false, fmt::format("Failed to listen on socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }
    return {true, ""};
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> TcpSocketImpl::accept() {
    if (socketFileDescriptor == -1) {
        return {nullptr, nullptr, "Cannot accept on an invalid or closed socket."};
    }

    sockaddr_storage remote_addr_storage{};
    socklen_t remote_addr_len = sizeof(remote_addr_storage);

    int client_fd = ::accept(socketFileDescriptor, reinterpret_cast<sockaddr*>(&remote_addr_storage), &remote_addr_len);

    if (client_fd == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return {nullptr, nullptr, ""}; // No pending connections, operation would block. Not an error to report in string.
        }
        return {nullptr, nullptr, fmt::format("Failed to accept new connection on socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    // Create InetAddress for the client
    auto [client_address, addr_error] = InetAddress::create(reinterpret_cast<const sockaddr*>(&remote_addr_storage), remote_addr_len);
    if (!client_address) {
        ::close(client_fd); // Close the accepted FD if address creation fails
        return {nullptr, nullptr, fmt::format("Failed to create InetAddress for accepted client: {}", addr_error)};
    }

    // Create TcpSocket for the client. The TcpSocket::adopt will set non-blocking.
    auto [client_socket, socket_error] = TcpSocket::adopt(client_fd);
    if (!client_socket) {
        // client_fd is already closed by adopt if it fails
        return {nullptr, nullptr, fmt::format("Failed to adopt accepted socket {}: {}", client_fd, socket_error)};
    }

    return {std::move(client_socket), std::move(client_address), ""};
}

int TcpSocketImpl::get_file_descriptor() const {
    return socketFileDescriptor;
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocketImpl::shutdown(int how) {
    if (socketFileDescriptor == -1) {
        return {false, "Cannot shutdown an invalid or closed socket."};
    }

    if (::shutdown(socketFileDescriptor, how) == -1) {
        return {false, fmt::format("Failed to shutdown socket {} (how={}): {}", socketFileDescriptor, how, StringUtils::get_errno_string())};
    }
    return {true, ""};
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocketImpl::finish_connect() const {
    if (socketFileDescriptor == -1) {
        return {false, "Socket is invalid or closed."};
    }

    int error = 0;
    socklen_t len = sizeof(error);
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (::getsockopt(socketFileDescriptor, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
        return {false, fmt::format("Failed to get SO_ERROR for socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
    }

    if (error == 0) {
        return {true, ""}; // Connection established
    }

    // An error occurred during connection
    return {false, fmt::format("Non-blocking connect failed for socket {}: {}", socketFileDescriptor, StringUtils::get_errno_string())};
}

// --- TcpSocket Public Methods (forwarding to Pimpl) ---

// Destructor forwards to Pimpl's destructor
TcpSocket::~TcpSocket() = default;

// Private constructor for TcpSocket. Only called by static factory methods.
TcpSocket::TcpSocket(int socket_fd) : p_impl_(std::make_unique<TcpSocketImpl>(socket_fd)) {
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::string> TcpSocket::create(int ip_version) {
    const int socket_fd = ::socket(ip_version, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        return {nullptr, fmt::format("Failed to create socket (IP version {}): {}", ip_version, StringUtils::get_errno_string())};
    }

    auto [nonBlockingSuccess, nonBlockingError] = set_non_blocking(socket_fd);
    if (!nonBlockingSuccess) {
        ::close(socket_fd); // Ensure to close the FD if setting non-blocking fails
        return {nullptr, fmt::format("Failed to configure non-blocking mode: {}", nonBlockingError)};
    }

    return {std::unique_ptr<TcpSocket>(new TcpSocket(socket_fd)), ""};
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::string> TcpSocket::adopt(int socket_fd) {
    if (socket_fd == -1) {
        return {nullptr, "Cannot adopt an invalid socket file descriptor (-1)."};
    }

    auto [nonBlockingSuccess, nonBlockingError] = set_non_blocking(socket_fd);
    if (!nonBlockingSuccess) {
        ::close(socket_fd); // Close the adopted FD if setting non-blocking fails
        return {nullptr, fmt::format("Failed to configure non-blocking mode for adopted socket {}: {}", socket_fd, nonBlockingError)};
    }

    return {std::unique_ptr<TcpSocket>(new TcpSocket(socket_fd)), ""};
}

[[nodiscard]] std::tuple<int, std::string> TcpSocket::send(utils::SimpleSpan<const uint8_t> data) {
    return p_impl_->send(data);
}

[[nodiscard]] std::tuple<int, std::string> TcpSocket::receive(utils::SimpleSpan<uint8_t> buffer) {
    return p_impl_->receive(buffer);
}

void TcpSocket::close() {
    p_impl_->close();
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> TcpSocket::get_peer_address() const {
    return p_impl_->get_peer_address();
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> TcpSocket::get_local_address() const {
    return p_impl_->get_local_address();
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocket::connect(const InetAddress& remote_address) {
    return p_impl_->connect(remote_address);
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocket::bind(const InetAddress& local_address) {
    return p_impl_->bind(local_address);
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocket::listen(int backlog) {
    return p_impl_->listen(backlog);
}

[[nodiscard]] std::tuple<std::unique_ptr<TcpSocket>, std::unique_ptr<InetAddress>, std::string> TcpSocket::accept() {
    return p_impl_->accept();
}

int TcpSocket::get_file_descriptor() const {
    return p_impl_->get_file_descriptor();
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocket::shutdown(int how) {
    return p_impl_->shutdown(how);
}

[[nodiscard]] std::tuple<bool, std::string> TcpSocket::finish_connect() const {
    return p_impl_->finish_connect();
}

bool TcpSocket::is_connected() const {
    const int fd = get_file_descriptor();
    if (fd < 0) {
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);

    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return false;
    }

    return error == 0;
}

} // namespace pubsub_itc_fw
