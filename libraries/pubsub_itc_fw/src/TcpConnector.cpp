#include <cerrno>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpConnector.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

enum class ConnectState {
    idle,
    connecting,
    connected
};

class TcpConnectorImpl {
public:
    TcpConnectorImpl()
        : state_(ConnectState::idle),
          remote_address_(nullptr),
          tcp_socket_(nullptr) {}

    ~TcpConnectorImpl() {
        cancel();
    }

    TcpConnectorImpl(const TcpConnectorImpl&) = delete;
    TcpConnectorImpl& operator=(const TcpConnectorImpl&) = delete;

    TcpConnectorImpl(TcpConnectorImpl&& other)
        : state_(other.state_),
          remote_address_(std::move(other.remote_address_)),
          tcp_socket_(std::move(other.tcp_socket_)) {
        other.state_ = ConnectState::idle;
    }

    TcpConnectorImpl& operator=(TcpConnectorImpl&& other) {
        if (this != &other) {
            cancel();
            state_ = other.state_;
            remote_address_ = std::move(other.remote_address_);
            tcp_socket_ = std::move(other.tcp_socket_);
            other.state_ = ConnectState::idle;
        }
        return *this;
    }

    std::tuple<bool, std::string> connect(const InetAddress& remote_address) {
        cancel();

        remote_address_ = std::make_unique<InetAddress>(remote_address);

        auto [socket_ptr, create_error] = TcpSocket::create(remote_address_->address_family());
        if (!socket_ptr) {
            remote_address_.reset();
            return {false, fmt::format("Failed to create TcpSocket for connection: {}", create_error)};
        }

        tcp_socket_ = std::move(socket_ptr);

        auto [connected_immediately, connect_error] = tcp_socket_->connect(*remote_address_);
        if (!connect_error.empty()) {
            const std::string message = fmt::format("Failed to initiate connection to {}:{}: {}",
                                                    remote_address_->get_ip_address_string(),
                                                    remote_address_->get_port(),
                                                    connect_error);
            cancel();
            return {false, message};
        }

        state_ = connected_immediately ? ConnectState::connected : ConnectState::connecting;
        return {connected_immediately, ""};
    }

    std::tuple<bool, std::string> finish_connect() {
        if (state_ != ConnectState::connecting || !tcp_socket_) {
            return {false, "No active connection attempt to finalize."};
        }

        auto [connected, finish_error] = tcp_socket_->finish_connect();
        if (!finish_error.empty()) {
            const std::string message = fmt::format("Connection to {}:{} failed: {}",
                                                    remote_address_ ? remote_address_->get_ip_address_string() : "",
                                                    remote_address_ ? remote_address_->get_port() : 0,
                                                    finish_error);
            cancel();
            return {false, message};
        }

        if (connected) {
            state_ = ConnectState::connected;
            return {true, ""};
        }

        return {false, ""};
    }

    bool is_connecting() const {
        return state_ == ConnectState::connecting && tcp_socket_ != nullptr;
    }

    std::unique_ptr<TcpSocket> get_connected_socket() {
        if (state_ == ConnectState::connected && tcp_socket_ && tcp_socket_->is_connected()) {
            state_ = ConnectState::idle;
            remote_address_.reset();
            return std::move(tcp_socket_);
        }
        return nullptr;
    }

    void cancel() {
        if (tcp_socket_) {
            tcp_socket_->close();
            tcp_socket_.reset();
        }
        remote_address_.reset();
        state_ = ConnectState::idle;
    }

private:
    ConnectState state_;
    std::unique_ptr<InetAddress> remote_address_;
    std::unique_ptr<TcpSocket> tcp_socket_;
};

// --- TcpConnector public methods ---

TcpConnector::TcpConnector()
    : p_impl_(std::make_unique<TcpConnectorImpl>()) {}

TcpConnector::~TcpConnector() = default;

TcpConnector::TcpConnector(TcpConnector&& other) = default;
TcpConnector& TcpConnector::operator=(TcpConnector&& other) = default;

std::tuple<bool, std::string> TcpConnector::connect(const InetAddress& remote_address) {
    return p_impl_->connect(remote_address);
}

std::tuple<bool, std::string> TcpConnector::finish_connect() {
    return p_impl_->finish_connect();
}

bool TcpConnector::is_connecting() const {
    return p_impl_->is_connecting();
}

std::unique_ptr<TcpSocket> TcpConnector::get_connected_socket() {
    return p_impl_->get_connected_socket();
}

void TcpConnector::cancel() {
    p_impl_->cancel();
}

} // namespace pubsub_itc_fw
