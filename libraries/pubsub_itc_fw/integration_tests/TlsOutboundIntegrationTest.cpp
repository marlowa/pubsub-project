// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TlsOutboundIntegrationTest.cpp
 * @brief Integration tests for the outbound TLS path: reactor as TLS client.
 *
 * Test protocol framing:
 *   [ uint32_t length (big-endian) ][ payload bytes ]
 *
 * The same framing as TlsProtocolHandlerIntegrationTest is reused. The roles
 * are reversed: the reactor connects outbound to a BlockingTlsServer that runs
 * in a background thread, while TlsProtocolHandlerIntegrationTest had the
 * reactor accepting inbound connections from a blocking OpenSSL client.
 *
 * Certificates:
 *   TlsCertDirectory generates all certificate material programmatically via
 *   the OpenSSL C API (EC prime256v1 keys, SHA-256 signatures). A primary CA
 *   signs the server and client certificates. A second independent CA is
 *   generated for the handshake-failure test (wrong trust anchor).
 *
 * Server side:
 *   BlockingTlsServer wraps a standard POSIX listening socket with a blocking
 *   SSL_CTX/SSL/SSL_accept. It is always run in a background std::thread so
 *   the test thread is free to wait_for() reactor events concurrently.
 *
 * Tests:
 *
 *   OutboundTlsHandshakeAndRoundTrip
 *     Reactor connects, TLS handshake completes, reactor sends one framed
 *     message in on_connection_established, server replies, reactor decodes
 *     the reply. ConnectionLost is delivered after the server closes the socket.
 *
 *   OutboundMutualTls
 *     Same as OutboundTlsHandshakeAndRoundTrip but the server requires a client
 *     certificate. The TlsClientConfiguration carries certificate_path and
 *     private_key_path. Both sides complete the handshake.
 *
 *   OutboundTlsServerDisconnect
 *     Server accepts the TLS connection, waits 100 ms, then closes. The 100 ms
 *     gap ensures the handshake completes before the server sends close_notify,
 *     so ConnectionEstablished is delivered before ConnectionLost. The
 *     passive connector thread does not send any data.
 *
 *   OutboundTlsHandshakeFailureNoConnectionEstablished
 *     The TlsClientConfiguration references a second CA that did not sign the
 *     server certificate. TLS certificate verification fails, the reactor tears
 *     down the connection silently (no ConnectionFailed is delivered for
 *     TCP/TLS errors, only for unknown service names), and ConnectionEstablished
 *     is never delivered. After 500 ms the test confirms the reactor is still
 *     alive and connection_established remains false.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TlsClientConfiguration.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Test protocol constants
// ============================================================

static const std::string outbound_request_payload = "HELLO_OUTBOUND_SERVER";
static const std::string outbound_response_payload = "HELLO_OUTBOUND_CLIENT";

static constexpr int64_t outbound_tls_buffer_capacity = 65536;
static constexpr size_t outbound_length_prefix_size = sizeof(uint32_t);

static const std::string outbound_service_name = "auth_service";

// ============================================================
// Framing helpers
// ============================================================

namespace {

std::string make_outbound_framed(const std::string& payload) {
    const uint32_t length_be = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame(outbound_length_prefix_size + payload.size(), '\0');
    std::memcpy(frame.data(), &length_be, outbound_length_prefix_size);
    std::memcpy(frame.data() + outbound_length_prefix_size, payload.data(), payload.size());
    return frame;
}

std::string try_decode_outbound_framed(const uint8_t* data, int available, int64_t& bytes_consumed) {
    bytes_consumed = 0;
    if (available < static_cast<int>(outbound_length_prefix_size)) {
        return {};
    }
    uint32_t length_be = 0;
    std::memcpy(&length_be, data, outbound_length_prefix_size);
    const uint32_t payload_length = ntohl(length_be);
    const int64_t total = static_cast<int64_t>(outbound_length_prefix_size + payload_length);
    if (available < static_cast<int>(total)) {
        return {};
    }
    std::string payload(reinterpret_cast<const char*>(data + outbound_length_prefix_size), payload_length);
    bytes_consumed = total;
    return payload;
}

// ============================================================
// Certificate generation helpers
// ============================================================

EVP_PKEY* generate_outbound_ec_key() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) {
        return nullptr;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

void add_outbound_basic_constraints(X509* x509, bool is_ca) {
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_basic_constraints,
        is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    if (extension) {
        X509_add_ext(x509, extension, -1);
        X509_EXTENSION_free(extension);
    }
}

X509* create_outbound_self_signed_cert(EVP_PKEY* key, const char* cn, long serial) {
    X509* x509 = X509_new();
    if (!x509) {
        return nullptr;
    }
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 86400L);
    X509_set_pubkey(x509, key);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    add_outbound_basic_constraints(x509, true);
    if (X509_sign(x509, key, EVP_sha256()) <= 0) {
        X509_free(x509);
        return nullptr;
    }
    return x509;
}

X509* create_outbound_signed_cert(EVP_PKEY* subject_key, const char* cn, long serial,
                                   X509* issuer_cert, EVP_PKEY* issuer_key) {
    X509* x509 = X509_new();
    if (!x509) {
        return nullptr;
    }
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 86400L);
    X509_set_pubkey(x509, subject_key);
    X509_NAME* subject_name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(subject_name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x509, X509_get_subject_name(issuer_cert));
    add_outbound_basic_constraints(x509, false);
    if (X509_sign(x509, issuer_key, EVP_sha256()) <= 0) {
        X509_free(x509);
        return nullptr;
    }
    return x509;
}

bool write_outbound_cert_pem(const std::string& path, X509* cert) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }
    const int result = PEM_write_X509(f, cert);
    fclose(f);
    return result == 1;
}

bool write_outbound_key_pem(const std::string& path, EVP_PKEY* key) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }
    const int result = PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);
    return result == 1;
}

} // un-named namespace

/**
 * @brief Generates all certificate material for the outbound TLS tests.
 *
 * Layout:
 *   ca.crt         -- primary CA (used by the reactor to verify the server cert)
 *   server.crt     -- CA-signed server certificate
 *   server.key     -- server private key
 *   client.crt     -- CA-signed client certificate (mutual TLS test only)
 *   client.key     -- client private key (mutual TLS test only)
 *   second_ca.crt  -- independent self-signed CA used for the handshake-failure
 *                    test; it did NOT sign the server cert so verification fails
 */
class TlsOutboundCertDirectory {
  public:
    std::string ca_cert_path;
    std::string server_cert_path;
    std::string server_key_path;
    std::string client_cert_path;
    std::string client_key_path;
    std::string second_ca_cert_path;
    bool valid{false};

    TlsOutboundCertDirectory() {
        char tmp_dir[] = "/tmp/tls_outbound_test_XXXXXX";
        char* dir = mkdtemp(tmp_dir);
        if (!dir) {
            return;
        }
        directory_ = dir;

        EVP_PKEY* ca_key = generate_outbound_ec_key();
        X509* ca_cert = ca_key ? create_outbound_self_signed_cert(ca_key, "Outbound Test CA", 1) : nullptr;

        EVP_PKEY* server_key = generate_outbound_ec_key();
        X509* server_cert = (server_key && ca_cert) ? create_outbound_signed_cert(server_key, "Outbound Test Server", 2, ca_cert, ca_key) : nullptr;

        EVP_PKEY* client_key = generate_outbound_ec_key();
        X509* client_cert = (client_key && ca_cert) ? create_outbound_signed_cert(client_key, "Outbound Test Client", 3, ca_cert, ca_key) : nullptr;

        EVP_PKEY* second_ca_key = generate_outbound_ec_key();
        X509* second_ca_cert = second_ca_key ? create_outbound_self_signed_cert(second_ca_key, "Outbound Second CA", 10) : nullptr;

        if (ca_cert && server_cert && client_cert && second_ca_cert) {
            ca_cert_path = directory_ + "/ca.crt";
            server_cert_path = directory_ + "/server.crt";
            server_key_path = directory_ + "/server.key";
            client_cert_path = directory_ + "/client.crt";
            client_key_path = directory_ + "/client.key";
            second_ca_cert_path = directory_ + "/second_ca.crt";

            valid = write_outbound_cert_pem(ca_cert_path, ca_cert)
                 && write_outbound_cert_pem(server_cert_path, server_cert)
                 && write_outbound_key_pem(server_key_path, server_key)
                 && write_outbound_cert_pem(client_cert_path, client_cert)
                 && write_outbound_key_pem(client_key_path, client_key)
                 && write_outbound_cert_pem(second_ca_cert_path, second_ca_cert);
        }

        if (second_ca_cert) { X509_free(second_ca_cert); }
        if (second_ca_key) { EVP_PKEY_free(second_ca_key); }
        if (client_cert) { X509_free(client_cert); }
        if (client_key) { EVP_PKEY_free(client_key); }
        if (server_cert) { X509_free(server_cert); }
        if (server_key) { EVP_PKEY_free(server_key); }
        if (ca_cert) { X509_free(ca_cert); }
        if (ca_key) { EVP_PKEY_free(ca_key); }
    }

    ~TlsOutboundCertDirectory() {
        ::unlink(ca_cert_path.c_str());
        ::unlink(server_cert_path.c_str());
        ::unlink(server_key_path.c_str());
        ::unlink(client_cert_path.c_str());
        ::unlink(client_key_path.c_str());
        ::unlink(second_ca_cert_path.c_str());
        if (!directory_.empty()) {
            ::rmdir(directory_.c_str());
        }
    }

    TlsOutboundCertDirectory(const TlsOutboundCertDirectory&) = delete;
    TlsOutboundCertDirectory& operator=(const TlsOutboundCertDirectory&) = delete;

  private:
    std::string directory_;
};

// ============================================================
// BlockingTlsServer
// ============================================================

/**
 * @brief A blocking TLS server that listens on a random port.
 *
 * Accepts one client at a time. Runs in a background thread in each test.
 * Uses SSL_set_fd (socket-BIO model) so SSL_read/SSL_write operate directly
 * on the socket fd -- the standard blocking server pattern, distinct from the
 * reactor's non-blocking memory-BIO model.
 */
class BlockingTlsServer {
  public:
    /**
     * @param[in] server_cert_path        PEM server certificate.
     * @param[in] server_key_path         PEM server private key.
     * @param[in] ca_path                 CA certificate for verifying client certs.
     *                                    Pass empty string if client certs are not required.
     * @param[in] require_client_cert     Pass true to enable mutual TLS.
     */
    BlockingTlsServer(const std::string& server_cert_path, const std::string& server_key_path,
                      const std::string& ca_path = {}, bool require_client_cert = false) {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) {
            return;
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_use_certificate_chain_file(ctx_, server_cert_path.c_str());
        SSL_CTX_use_PrivateKey_file(ctx_, server_key_path.c_str(), SSL_FILETYPE_PEM);

        if (require_client_cert && !ca_path.empty()) {
            SSL_CTX_load_verify_locations(ctx_, ca_path.c_str(), nullptr);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            return;
        }

        const int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return;
        }
        if (::listen(listen_fd_, 1) != 0) {
            return;
        }

        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
            port_ = ntohs(bound.sin_port);
        }

        ready_ = (port_ != 0);
    }

    ~BlockingTlsServer() {
        close_client();
        if (listen_fd_ != -1) {
            ::close(listen_fd_);
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
        }
    }

    BlockingTlsServer(const BlockingTlsServer&) = delete;
    BlockingTlsServer& operator=(const BlockingTlsServer&) = delete;

    [[nodiscard]] bool is_ready() const { return ready_; }
    [[nodiscard]] uint16_t port() const { return port_; }

    /**
     * @brief Waits up to 5 seconds for a client, then performs SSL_accept.
     * @return true if the handshake completed successfully.
     */
    bool accept_client() {
        struct pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 5000) <= 0) {
            return false;
        }

        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ == -1) {
            return false;
        }

        timeval timeout{5, 0};
        ::setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            ::close(client_fd_);
            client_fd_ = -1;
            return false;
        }
        SSL_set_fd(ssl_, client_fd_);
        const bool ok = SSL_accept(ssl_) == 1;
        if (!ok) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            ::close(client_fd_);
            client_fd_ = -1;
        }
        return ok;
    }

    /**
     * @brief Receives a single framed message from the connected client.
     * @return The decoded payload string, or empty string on error.
     */
    [[nodiscard]] std::string recv_framed() {
        if (!ssl_) {
            return {};
        }
        uint32_t length_be = 0;
        if (!recv_all(&length_be, sizeof(length_be))) {
            return {};
        }
        const uint32_t payload_length = ntohl(length_be);
        std::string payload(payload_length, '\0');
        if (!recv_all(payload.data(), payload_length)) {
            return {};
        }
        return payload;
    }

    /**
     * @brief Sends a single framed message to the connected client.
     * @return true on success.
     */
    bool send_framed(const std::string& payload) {
        if (!ssl_) {
            return false;
        }
        const uint32_t length_be = htonl(static_cast<uint32_t>(payload.size()));
        if (!send_all(&length_be, sizeof(length_be))) {
            return false;
        }
        return send_all(payload.data(), payload.size());
    }

    /**
     * @brief Closes the client connection with a TLS close_notify.
     */
    void close_client() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (client_fd_ != -1) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
    }

  private:
    [[nodiscard]] bool recv_all(void* buf, size_t size) {
        auto* ptr = static_cast<char*>(buf);
        size_t remaining = size;
        while (remaining > 0) {
            const int received = SSL_read(ssl_, ptr, static_cast<int>(remaining));
            if (received <= 0) {
                return false;
            }
            ptr += received;
            remaining -= static_cast<size_t>(received);
        }
        return true;
    }

    [[nodiscard]] bool send_all(const void* buf, size_t size) {
        const auto* ptr = static_cast<const char*>(buf);
        size_t remaining = size;
        while (remaining > 0) {
            const int sent = SSL_write(ssl_, ptr, static_cast<int>(remaining));
            if (sent <= 0) {
                return false;
            }
            ptr += sent;
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    SSL_CTX* ctx_{nullptr};
    SSL* ssl_{nullptr};
    int listen_fd_{-1};
    int client_fd_{-1};
    uint16_t port_{0};
    bool ready_{false};
};

// ============================================================
// Reactor configuration
// ============================================================

namespace {

ReactorConfiguration make_outbound_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    return cfg;
}

} // un-named namespace

// ============================================================
// Connector thread: sends one framed request then decodes the reply.
// Used by OutboundTlsHandshakeAndRoundTrip and OutboundMutualTls.
// ============================================================
class TlsOutboundConnectorThread : public ApplicationThread {
  public:
    TlsOutboundConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TlsOutboundConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("TlsOutboundConnectorPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> message_received{false};
    std::atomic<bool> connection_lost{false};
    std::string received_payload;
    ConnectionID conn_id{};

  protected:
    void on_initial_event() override {
        connect_to_service(outbound_service_name);
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
        const std::string frame = make_outbound_framed(outbound_request_payload);
        send_raw(conn_id, frame.data(), static_cast<uint32_t>(frame.size()));
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("server disconnected");
    }

    void on_raw_socket_message(const EventMessage& message) override {
        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0) {
            return;
        }

        if (available < last_available_) {
            bytes_decoded_ = 0;
        }
        last_available_ = available;

        const int unprocessed = available - bytes_decoded_;
        if (unprocessed <= 0) {
            return;
        }

        int64_t bytes_consumed = 0;
        const std::string payload = try_decode_outbound_framed(data + bytes_decoded_, unprocessed, bytes_consumed);
        if (bytes_consumed == 0) {
            return;
        }

        received_payload = payload;
        message_received.store(true, std::memory_order_release);

        bytes_decoded_ += static_cast<int>(bytes_consumed);
        commit_raw_bytes(conn_id, static_cast<int64_t>(bytes_decoded_));
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    int bytes_decoded_{0};
    int last_available_{0};
};

// ============================================================
// Passive connector thread: tracks connection events only.
// Used by OutboundTlsServerDisconnect and OutboundTlsHandshakeFailure.
// ============================================================
class TlsOutboundPassiveConnectorThread : public ApplicationThread {
  public:
    TlsOutboundPassiveConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TlsOutboundPassiveConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("TlsOutboundPassivePool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_lost{false};
    ConnectionID conn_id{};

  protected:
    void on_initial_event() override {
        connect_to_service(outbound_service_name);
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("server disconnected");
    }

    void on_raw_socket_message([[maybe_unused]] const EventMessage& msg) override {}
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class TlsOutboundIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        certs_ = std::make_unique<TlsOutboundCertDirectory>();
        ASSERT_TRUE(certs_->valid) << "Certificate generation failed -- check /tmp permissions and OpenSSL availability";
    }

    void TearDown() override {
        current_reactor_ = nullptr;
        certs_.reset();
        logger_.reset();
    }

    void set_current_reactor(Reactor& reactor) {
        current_reactor_ = &reactor;
    }

    bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        reactor_died_ = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (current_reactor_ != nullptr && current_reactor_->is_finished()) {
                reactor_died_ = true;
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    [[nodiscard]] std::string last_wait_failure_description() const {
        if (reactor_died_ && current_reactor_ != nullptr) {
            return "reactor terminated during wait; shutdown reason: " + current_reactor_->get_shutdown_reason();
        }
        return "predicate did not become true within timeout";
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread,
                                   const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable()) {
            reactor_thread.join();
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::unique_ptr<TlsOutboundCertDirectory> certs_;
    Reactor* current_reactor_{nullptr};
    bool reactor_died_{false};
};

// ============================================================
// Test: happy-path outbound TLS round trip
// ============================================================
TEST_F(TlsOutboundIntegrationTest, OutboundTlsHandshakeAndRoundTrip) {
    BlockingTlsServer server(certs_->server_cert_path, certs_->server_key_path);
    ASSERT_TRUE(server.is_ready()) << "BlockingTlsServer failed to bind";

    ServiceRegistry registry;
    TlsClientConfiguration tls_config;
    tls_config.ca_path = certs_->ca_cert_path;
    tls_config.raw_buffer_capacity = outbound_tls_buffer_capacity;
    registry.add_tls(outbound_service_name,
                     NetworkEndpointConfiguration{"127.0.0.1", server.port()},
                     NetworkEndpointConfiguration{},
                     tls_config);

    auto reactor = std::make_unique<Reactor>(make_outbound_reactor_config(), registry, logger_->logger);
    set_current_reactor(*reactor);

    auto connector = ApplicationThread::create<TlsOutboundConnectorThread>(logger_->logger, *reactor);
    reactor->register_thread(connector);

    std::thread server_thread([&]() {
        if (!server.accept_client()) {
            return;
        }
        const std::string request = server.recv_framed();
        server.send_framed(outbound_response_payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.close_client();
    });

    std::thread reactor_thread([&]() { reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return connector->connection_established.load(std::memory_order_acquire); }))
        << "Connector: ConnectionEstablished not received: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return connector->message_received.load(std::memory_order_acquire); }))
        << "Connector: reply not received from server: " << last_wait_failure_description();

    EXPECT_EQ(connector->received_payload, outbound_response_payload);

    EXPECT_TRUE(wait_for([&]() { return connector->connection_lost.load(std::memory_order_acquire); }))
        << "Connector: ConnectionLost not received after server closed: " << last_wait_failure_description();

    if (server_thread.joinable()) {
        server_thread.join();
    }
    shutdown_and_join(*reactor, reactor_thread);
}

// ============================================================
// Test: mutual TLS -- connector presents a client certificate
// ============================================================
TEST_F(TlsOutboundIntegrationTest, OutboundMutualTls) {
    BlockingTlsServer server(certs_->server_cert_path, certs_->server_key_path,
                              certs_->ca_cert_path, /*require_client_cert=*/true);
    ASSERT_TRUE(server.is_ready()) << "BlockingTlsServer failed to bind";

    ServiceRegistry registry;
    TlsClientConfiguration tls_config;
    tls_config.ca_path = certs_->ca_cert_path;
    tls_config.certificate_path = certs_->client_cert_path;
    tls_config.private_key_path = certs_->client_key_path;
    tls_config.raw_buffer_capacity = outbound_tls_buffer_capacity;
    registry.add_tls(outbound_service_name,
                     NetworkEndpointConfiguration{"127.0.0.1", server.port()},
                     NetworkEndpointConfiguration{},
                     tls_config);

    auto reactor = std::make_unique<Reactor>(make_outbound_reactor_config(), registry, logger_->logger);
    set_current_reactor(*reactor);

    auto connector = ApplicationThread::create<TlsOutboundConnectorThread>(logger_->logger, *reactor);
    reactor->register_thread(connector);

    std::thread server_thread([&]() {
        if (!server.accept_client()) {
            return;
        }
        const std::string request = server.recv_framed();
        server.send_framed(outbound_response_payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.close_client();
    });

    std::thread reactor_thread([&]() { reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return connector->connection_established.load(std::memory_order_acquire); }))
        << "Connector: ConnectionEstablished not received (mutual TLS): " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return connector->message_received.load(std::memory_order_acquire); }))
        << "Connector: reply not received after mutual TLS handshake: " << last_wait_failure_description();

    EXPECT_EQ(connector->received_payload, outbound_response_payload);

    EXPECT_TRUE(wait_for([&]() { return connector->connection_lost.load(std::memory_order_acquire); }))
        << "Connector: ConnectionLost not received after server closed: " << last_wait_failure_description();

    if (server_thread.joinable()) {
        server_thread.join();
    }
    shutdown_and_join(*reactor, reactor_thread);
}

// ============================================================
// Test: server closes after TLS handshake -- ConnectionLost is delivered
// ============================================================
TEST_F(TlsOutboundIntegrationTest, OutboundTlsServerDisconnect) {
    BlockingTlsServer server(certs_->server_cert_path, certs_->server_key_path);
    ASSERT_TRUE(server.is_ready()) << "BlockingTlsServer failed to bind";

    ServiceRegistry registry;
    TlsClientConfiguration tls_config;
    tls_config.ca_path = certs_->ca_cert_path;
    tls_config.raw_buffer_capacity = outbound_tls_buffer_capacity;
    registry.add_tls(outbound_service_name,
                     NetworkEndpointConfiguration{"127.0.0.1", server.port()},
                     NetworkEndpointConfiguration{},
                     tls_config);

    auto reactor = std::make_unique<Reactor>(make_outbound_reactor_config(), registry, logger_->logger);
    set_current_reactor(*reactor);

    auto connector = ApplicationThread::create<TlsOutboundPassiveConnectorThread>(logger_->logger, *reactor);
    reactor->register_thread(connector);

    std::thread server_thread([&]() {
        if (!server.accept_client()) {
            return;
        }
        // Sleep long enough that the handshake completes and the reactor delivers
        // ConnectionEstablished before the close_notify arrives.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.close_client();
    });

    std::thread reactor_thread([&]() { reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return connector->connection_established.load(std::memory_order_acquire); }))
        << "Connector: ConnectionEstablished not received before server disconnect: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return connector->connection_lost.load(std::memory_order_acquire); }))
        << "Connector: ConnectionLost not received after server closed: " << last_wait_failure_description();

    if (server_thread.joinable()) {
        server_thread.join();
    }
    shutdown_and_join(*reactor, reactor_thread);
}

// ============================================================
// Test: TLS handshake failure -- wrong CA, ConnectionEstablished never delivered
// ============================================================
TEST_F(TlsOutboundIntegrationTest, OutboundTlsHandshakeFailureNoConnectionEstablished) {
    BlockingTlsServer server(certs_->server_cert_path, certs_->server_key_path);
    ASSERT_TRUE(server.is_ready()) << "BlockingTlsServer failed to bind";

    ServiceRegistry registry;
    TlsClientConfiguration tls_config;
    // Deliberately use second_ca_cert_path: this CA did not sign the server cert,
    // so TLS certificate verification fails and the handshake is aborted.
    tls_config.ca_path = certs_->second_ca_cert_path;
    tls_config.raw_buffer_capacity = outbound_tls_buffer_capacity;
    registry.add_tls(outbound_service_name,
                     NetworkEndpointConfiguration{"127.0.0.1", server.port()},
                     NetworkEndpointConfiguration{},
                     tls_config);

    auto reactor = std::make_unique<Reactor>(make_outbound_reactor_config(), registry, logger_->logger);
    set_current_reactor(*reactor);

    auto connector = ApplicationThread::create<TlsOutboundPassiveConnectorThread>(logger_->logger, *reactor);
    reactor->register_thread(connector);

    std::thread server_thread([&]() {
        // SSL_accept will fail when the client sends a TLS alert after cert rejection.
        // The return value is intentionally ignored: the test is about the client side.
        server.accept_client();
    });

    std::thread reactor_thread([&]() { reactor->run(); });

    // Wait for the handshake to fail and the reactor to process the error.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_FALSE(connector->connection_established.load(std::memory_order_acquire))
        << "ConnectionEstablished must NOT be delivered when TLS cert verification fails";

    EXPECT_FALSE(reactor->is_finished())
        << "Reactor must remain alive after a TLS handshake failure (retries silently)";

    if (server_thread.joinable()) {
        server_thread.join();
    }
    shutdown_and_join(*reactor, reactor_thread);
}

} // namespaces
