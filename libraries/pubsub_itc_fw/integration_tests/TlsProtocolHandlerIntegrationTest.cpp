// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TlsProtocolHandlerIntegrationTest.cpp
 * @brief Integration tests for TlsRawBytesProtocolHandler and the TLS inbound listener path.
 *
 * Test protocol framing:
 *   [ uint32_t length (big-endian) ][ payload bytes ]
 *
 * The same framing as RawBytesProtocolHandlerIntegrationTest is reused. The
 * TlsRawBytesProtocolHandler must deliver the same plaintext view to the
 * application as RawBytesProtocolHandler — the only difference is that the
 * byte stream is protected by TLS on the wire.
 *
 * Certificates:
 *   Each test constructs a TlsCertDirectory that generates all certificate
 *   material programmatically via the OpenSSL C API (EC prime256v1 keys,
 *   SHA-256 signatures). A CA signs both the server certificate and, for the
 *   mutual TLS test, the client certificate. No external tooling is needed.
 *
 * Client side:
 *   Tests use a blocking OpenSSL TLS client (SSL_CTX + SSL + socket fd with
 *   SSL_set_fd and SSL_connect). The reactor drives the server-side non-blocking
 *   TLS via the memory BIO model in its own thread.
 *
 * Tests:
 *
 *   TlsHandshakeAndRoundTrip
 *     Happy-path: client establishes TLS, sends one framed message, receives reply.
 *
 *   FragmentedCiphertextDelivery
 *     Client sends the 4-byte length prefix in one SSL_write, sleeps 20 ms, then
 *     sends the payload in a second SSL_write. The framework accumulates both
 *     TLS records in the plaintext MirroredBuffer before decoding.
 *
 *   PeerDisconnect
 *     Client calls SSL_shutdown then closes the socket. The server receives
 *     SSL_ERROR_ZERO_RETURN (TLS close_notify) and delivers ConnectionLost.
 *
 *   MutualTlsHandshake
 *     Server requires a client certificate. Client presents one signed by the
 *     shared CA. Both sides authenticate and complete a round trip.
 *
 *   HandshakeFailure
 *     Client provides no trusted CA so it rejects the server certificate and
 *     sends a TLS alert. The server detects the alert, tears down the connection,
 *     and delivers ConnectionLost.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/bio.h>
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
#include <pubsub_itc_fw/TlsListenerConfiguration.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Test protocol constants
// ============================================================

static const std::string tls_request_payload = "HELLO_TLS_SERVER";
static const std::string tls_response_payload = "HELLO_TLS_CLIENT";

static constexpr int64_t tls_raw_buffer_capacity = 65536;
static constexpr size_t tls_length_prefix_size = sizeof(uint32_t);

// ============================================================
// Framing helpers (shared between server-side decode and client-side send)
// ============================================================

static std::string make_tls_framed(const std::string& payload) {
    const uint32_t length_be = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame(tls_length_prefix_size + payload.size(), '\0');
    std::memcpy(frame.data(), &length_be, tls_length_prefix_size);
    std::memcpy(frame.data() + tls_length_prefix_size, payload.data(), payload.size());
    return frame;
}

static std::string try_decode_tls_framed(const uint8_t* data, int available, int64_t& bytes_consumed) {
    bytes_consumed = 0;
    if (available < static_cast<int>(tls_length_prefix_size))
        return {};
    uint32_t length_be = 0;
    std::memcpy(&length_be, data, tls_length_prefix_size);
    const uint32_t payload_length = ntohl(length_be);
    const int64_t total = static_cast<int64_t>(tls_length_prefix_size + payload_length);
    if (available < static_cast<int>(total))
        return {};
    std::string payload(reinterpret_cast<const char*>(data + tls_length_prefix_size), payload_length);
    bytes_consumed = total;
    return payload;
}

// ============================================================
// Certificate generation helpers
// ============================================================

static EVP_PKEY* generate_ec_key() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx)
        return nullptr;
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

// OpenSSL 3.0 chain verification requires basicConstraints: CA:TRUE on any cert
// used as a trust anchor. Without it, SSL_connect fails with unknown_ca.
static void add_basic_constraints(X509* x509, bool is_ca) {
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_basic_constraints,
        is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    if (extension) {
        X509_add_ext(x509, extension, -1);
        X509_EXTENSION_free(extension);
    }
}

static X509* create_self_signed_cert(EVP_PKEY* key, const char* cn, long serial) {
    X509* x509 = X509_new();
    if (!x509)
        return nullptr;
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 86400L);
    X509_set_pubkey(x509, key);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    add_basic_constraints(x509, true);
    if (X509_sign(x509, key, EVP_sha256()) <= 0) {
        X509_free(x509);
        return nullptr;
    }
    return x509;
}

static X509* create_signed_cert(EVP_PKEY* subject_key, const char* cn, long serial,
                                 X509* issuer_cert, EVP_PKEY* issuer_key) {
    X509* x509 = X509_new();
    if (!x509)
        return nullptr;
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 86400L);
    X509_set_pubkey(x509, subject_key);
    X509_NAME* subject_name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(subject_name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x509, X509_get_subject_name(issuer_cert));
    add_basic_constraints(x509, false);
    if (X509_sign(x509, issuer_key, EVP_sha256()) <= 0) {
        X509_free(x509);
        return nullptr;
    }
    return x509;
}

static bool write_cert_pem(const std::string& path, X509* cert) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return false;
    const int result = PEM_write_X509(f, cert);
    fclose(f);
    return result == 1;
}

static bool write_key_pem(const std::string& path, EVP_PKEY* key) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return false;
    const int result = PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);
    return result == 1;
}

/**
 * @brief Generates all certificate material for the TLS tests in a temporary directory.
 *
 * Layout:
 *   ca.crt       — self-signed CA (used by clients to verify the server, and by the
 *                  server to verify client certs in the mutual TLS test)
 *   server.crt   — CA-signed server certificate
 *   server.key   — server private key
 *   client.crt   — CA-signed client certificate (mutual TLS test only)
 *   client.key   — client private key (mutual TLS test only)
 */
class TlsCertDirectory {
  public:
    std::string ca_cert_path;
    std::string server_cert_path;
    std::string server_key_path;
    std::string client_cert_path;
    std::string client_key_path;
    bool valid{false};

    TlsCertDirectory() {
        char tmp_dir[] = "/tmp/tls_test_XXXXXX";
        char* dir = mkdtemp(tmp_dir);
        if (!dir)
            return;
        directory_ = dir;

        EVP_PKEY* ca_key = generate_ec_key();
        X509* ca_cert = ca_key ? create_self_signed_cert(ca_key, "Test CA", 1) : nullptr;

        EVP_PKEY* server_key = generate_ec_key();
        X509* server_cert = (server_key && ca_cert) ? create_signed_cert(server_key, "Test Server", 2, ca_cert, ca_key) : nullptr;

        EVP_PKEY* client_key = generate_ec_key();
        X509* client_cert = (client_key && ca_cert) ? create_signed_cert(client_key, "Test Client", 3, ca_cert, ca_key) : nullptr;

        if (ca_cert && server_cert && client_cert) {
            ca_cert_path = directory_ + "/ca.crt";
            server_cert_path = directory_ + "/server.crt";
            server_key_path = directory_ + "/server.key";
            client_cert_path = directory_ + "/client.crt";
            client_key_path = directory_ + "/client.key";

            valid = write_cert_pem(ca_cert_path, ca_cert)
                 && write_cert_pem(server_cert_path, server_cert)
                 && write_key_pem(server_key_path, server_key)
                 && write_cert_pem(client_cert_path, client_cert)
                 && write_key_pem(client_key_path, client_key);
        }

        if (client_cert) X509_free(client_cert);
        if (client_key) EVP_PKEY_free(client_key);
        if (server_cert) X509_free(server_cert);
        if (server_key) EVP_PKEY_free(server_key);
        if (ca_cert) X509_free(ca_cert);
        if (ca_key) EVP_PKEY_free(ca_key);
    }

    ~TlsCertDirectory() {
        ::unlink(ca_cert_path.c_str());
        ::unlink(server_cert_path.c_str());
        ::unlink(server_key_path.c_str());
        ::unlink(client_cert_path.c_str());
        ::unlink(client_key_path.c_str());
        if (!directory_.empty())
            ::rmdir(directory_.c_str());
    }

    TlsCertDirectory(const TlsCertDirectory&) = delete;
    TlsCertDirectory& operator=(const TlsCertDirectory&) = delete;

  private:
    std::string directory_;
};

// ============================================================
// TLS client connection
// ============================================================

/**
 * @brief RAII wrapper for a blocking OpenSSL TLS client connection.
 *
 * Uses SSL_set_fd so that SSL_read/SSL_write operate directly on the
 * underlying socket fd. This is the standard blocking client model and
 * is intentionally different from the server's non-blocking memory BIO model.
 */
struct TlsClientConnection {
    int fd{-1};
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};

    TlsClientConnection() = default;

    TlsClientConnection(TlsClientConnection&& other) noexcept
        : fd(other.fd), ctx(other.ctx), ssl(other.ssl) {
        other.fd = -1;
        other.ctx = nullptr;
        other.ssl = nullptr;
    }

    ~TlsClientConnection() {
        disconnect();
    }

    void disconnect() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    [[nodiscard]] bool connected() const { return ssl != nullptr; }

    TlsClientConnection(const TlsClientConnection&) = delete;
    TlsClientConnection& operator=(const TlsClientConnection&) = delete;
};

/**
 * @brief Connects to a TLS server at the given port using a blocking SSL_connect.
 *
 * @param[in] port              Server port to connect to.
 * @param[in] ca_path           CA certificate path for server verification.
 *                              Pass an empty string to skip server cert verification.
 * @param[in] client_cert_path  Client certificate for mutual TLS. Empty if not required.
 * @param[in] client_key_path   Client private key for mutual TLS. Empty if not required.
 *
 * @return A TlsClientConnection. Check connected() for success.
 */
static TlsClientConnection connect_tls(uint16_t port, const std::string& ca_path,
                                        const std::string& client_cert_path = {},
                                        const std::string& client_key_path = {}) {
    TlsClientConnection conn;

    conn.ctx = SSL_CTX_new(TLS_client_method());
    if (!conn.ctx)
        return conn;

    SSL_CTX_set_min_proto_version(conn.ctx, TLS1_2_VERSION);

    if (!ca_path.empty()) {
        SSL_CTX_load_verify_locations(conn.ctx, ca_path.c_str(), nullptr);
        SSL_CTX_set_verify(conn.ctx, SSL_VERIFY_PEER, nullptr);
    }

    if (!client_cert_path.empty()) {
        SSL_CTX_use_certificate_chain_file(conn.ctx, client_cert_path.c_str());
        SSL_CTX_use_PrivateKey_file(conn.ctx, client_key_path.c_str(), SSL_FILETYPE_PEM);
    }

    conn.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (conn.fd == -1)
        return conn;

    timeval timeout{5, 0};
    ::setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(conn.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return conn;

    conn.ssl = SSL_new(conn.ctx);
    if (!conn.ssl)
        return conn;

    SSL_set_fd(conn.ssl, conn.fd);

    if (SSL_connect(conn.ssl) != 1) {
        SSL_free(conn.ssl);
        conn.ssl = nullptr;
    }

    return conn;
}

static bool tls_send_all(SSL* ssl, const void* buf, size_t size) {
    const auto* ptr = static_cast<const char*>(buf);
    size_t remaining = size;
    while (remaining > 0) {
        const int sent = SSL_write(ssl, ptr, static_cast<int>(remaining));
        if (sent <= 0)
            return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

static bool tls_recv_all(SSL* ssl, void* buf, size_t size) {
    auto* ptr = static_cast<char*>(buf);
    size_t remaining = size;
    while (remaining > 0) {
        const int received = SSL_read(ssl, ptr, static_cast<int>(remaining));
        if (received <= 0)
            return false;
        ptr += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

static std::string tls_recv_framed(SSL* ssl) {
    uint32_t length_be = 0;
    if (!tls_recv_all(ssl, &length_be, sizeof(length_be)))
        return {};
    const uint32_t payload_length = ntohl(length_be);
    std::string payload(payload_length, '\0');
    if (!tls_recv_all(ssl, payload.data(), payload_length))
        return {};
    return payload;
}

// ============================================================
// Reactor configuration helpers
// ============================================================

static ReactorConfiguration make_tls_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    return cfg;
}

// ============================================================
// Listener thread: decodes one framed message then replies via send_raw().
// Used by TlsHandshakeAndRoundTrip, FragmentedCiphertextDelivery, MutualTlsHandshake.
// ============================================================
class TlsListenerThread : public ApplicationThread {
  public:
    TlsListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TlsListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("TlsListenerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> message_received{false};
    std::atomic<bool> reply_sent{false};
    std::atomic<bool> connection_lost{false};

    std::string received_payload;
    ConnectionID conn_id{};

  protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("peer disconnected");
    }

    void on_raw_socket_message(const EventMessage& message) override {
        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0)
            return;

        if (available < last_available_)
            bytes_decoded_ = 0;
        last_available_ = available;

        const int unprocessed = available - bytes_decoded_;
        if (unprocessed <= 0)
            return;

        int64_t bytes_consumed = 0;
        std::string payload = try_decode_tls_framed(data + bytes_decoded_, unprocessed, bytes_consumed);
        if (bytes_consumed == 0)
            return;

        received_payload = payload;
        message_received.store(true, std::memory_order_release);

        bytes_decoded_ += static_cast<int>(bytes_consumed);
        commit_raw_bytes(conn_id, static_cast<int64_t>(bytes_decoded_));

        const std::string reply = make_tls_framed(tls_response_payload);
        send_raw(conn_id, reply.data(), static_cast<uint32_t>(reply.size()));
        reply_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    int bytes_decoded_{0};
    int last_available_{0};
};

// ============================================================
// Passive listener thread: tracks connection events only.
// Used by PeerDisconnect and HandshakeFailure.
// ============================================================
class TlsPassiveListenerThread : public ApplicationThread {
  public:
    TlsPassiveListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TlsPassiveListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("TlsPassiveListenerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_lost{false};
    ConnectionID conn_id{};

  protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_raw_socket_message([[maybe_unused]] const EventMessage& msg) override {}
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class TlsProtocolHandlerIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        certs_ = std::make_unique<TlsCertDirectory>();
        ASSERT_TRUE(certs_->valid) << "Certificate generation failed — check /tmp permissions and OpenSSL availability";
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
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    std::string last_wait_failure_description() const {
        if (reactor_died_ && current_reactor_ != nullptr) {
            return "reactor terminated during wait; shutdown reason: " + current_reactor_->get_shutdown_reason();
        }
        return "predicate did not become true within timeout";
    }

    uint16_t start_listener_reactor(Reactor& reactor) {
        EXPECT_TRUE(wait_for([&]() { return reactor.is_initialized(); }))
            << "Listener reactor did not initialise within timeout";
        const uint16_t port = reactor.get_inbound_listener_port(0);
        EXPECT_NE(port, 0u) << "OS did not assign a valid listening port";
        return port;
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread,
                                   const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable())
            reactor_thread.join();
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::unique_ptr<TlsCertDirectory> certs_;
    Reactor* current_reactor_{nullptr};
    bool reactor_died_{false};
};

// ============================================================
// Test: happy-path TLS round trip
// ============================================================
TEST_F(TlsProtocolHandlerIntegrationTest, TlsHandshakeAndRoundTrip) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_tls_reactor_config(), listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    TlsListenerConfiguration tls_config;
    tls_config.certificate_path = certs_->server_cert_path;
    tls_config.private_key_path = certs_->server_key_path;

    listener_reactor->register_inbound_tls_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2}, tls_raw_buffer_capacity, tls_config);

    auto listener_thread = ApplicationThread::create<TlsListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    TlsClientConnection client = connect_tls(listen_port, certs_->ca_cert_path);
    ASSERT_TRUE(client.connected()) << "TLS handshake failed — server may have rejected the connection";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    const std::string frame = make_tls_framed(tls_request_payload);
    ASSERT_TRUE(tls_send_all(client.ssl, frame.data(), frame.size())) << "Failed to send framed request over TLS";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->reply_sent.load(std::memory_order_acquire); }))
        << "Listener: reply not sent: " << last_wait_failure_description();

    const std::string reply = tls_recv_framed(client.ssl);
    EXPECT_EQ(reply, tls_response_payload);
    EXPECT_EQ(listener_thread->received_payload, tls_request_payload);

    client.disconnect();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after TLS client disconnected: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: two TLS records per on_data_ready call (fragmented plaintext accumulation)
// ============================================================
TEST_F(TlsProtocolHandlerIntegrationTest, FragmentedCiphertextDelivery) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_tls_reactor_config(), listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    TlsListenerConfiguration tls_config;
    tls_config.certificate_path = certs_->server_cert_path;
    tls_config.private_key_path = certs_->server_key_path;

    listener_reactor->register_inbound_tls_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2}, tls_raw_buffer_capacity, tls_config);

    auto listener_thread = ApplicationThread::create<TlsListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    TlsClientConnection client = connect_tls(listen_port, certs_->ca_cert_path);
    ASSERT_TRUE(client.connected()) << "TLS handshake failed";

    // Disable Nagle so each SSL_write produces a separate TCP segment.
    const int one = 1;
    ::setsockopt(client.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    // Send length prefix and payload in two separate SSL_write calls.
    const uint32_t length_be = htonl(static_cast<uint32_t>(tls_request_payload.size()));
    ASSERT_TRUE(tls_send_all(client.ssl, &length_be, sizeof(length_be))) << "Failed to send TLS length prefix";

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    ASSERT_TRUE(tls_send_all(client.ssl, tls_request_payload.data(), tls_request_payload.size()))
        << "Failed to send TLS payload";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->message_received.load(std::memory_order_acquire); }))
        << "Listener: complete message not received after fragmented TLS send: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->reply_sent.load(std::memory_order_acquire); }))
        << "Listener: reply not sent: " << last_wait_failure_description();

    const std::string reply = tls_recv_framed(client.ssl);
    EXPECT_EQ(reply, tls_response_payload);
    EXPECT_EQ(listener_thread->received_payload, tls_request_payload);

    client.disconnect();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after disconnect: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: graceful TLS close delivers ConnectionLost
// ============================================================
TEST_F(TlsProtocolHandlerIntegrationTest, PeerDisconnect) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_tls_reactor_config(), listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    TlsListenerConfiguration tls_config;
    tls_config.certificate_path = certs_->server_cert_path;
    tls_config.private_key_path = certs_->server_key_path;

    listener_reactor->register_inbound_tls_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2}, tls_raw_buffer_capacity, tls_config);

    auto listener_thread = ApplicationThread::create<TlsPassiveListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    TlsClientConnection client = connect_tls(listen_port, certs_->ca_cert_path);
    ASSERT_TRUE(client.connected()) << "TLS handshake failed";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    // SSL_shutdown sends close_notify; server sees SSL_ERROR_ZERO_RETURN.
    client.disconnect();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after TLS close_notify: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: mutual TLS — both sides present certificates
// ============================================================
TEST_F(TlsProtocolHandlerIntegrationTest, MutualTlsHandshake) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_tls_reactor_config(), listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    TlsListenerConfiguration tls_config;
    tls_config.certificate_path = certs_->server_cert_path;
    tls_config.private_key_path = certs_->server_key_path;
    tls_config.ca_path = certs_->ca_cert_path;
    tls_config.require_client_certificate = true;

    listener_reactor->register_inbound_tls_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2}, tls_raw_buffer_capacity, tls_config);

    auto listener_thread = ApplicationThread::create<TlsListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    // Client presents its certificate — required by the server.
    TlsClientConnection client = connect_tls(listen_port, certs_->ca_cert_path,
                                              certs_->client_cert_path, certs_->client_key_path);
    ASSERT_TRUE(client.connected()) << "Mutual TLS handshake failed";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    const std::string frame = make_tls_framed(tls_request_payload);
    ASSERT_TRUE(tls_send_all(client.ssl, frame.data(), frame.size()))
        << "Failed to send framed request over mutual TLS";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->reply_sent.load(std::memory_order_acquire); }))
        << "Listener: reply not sent: " << last_wait_failure_description();

    const std::string reply = tls_recv_framed(client.ssl);
    EXPECT_EQ(reply, tls_response_payload);
    EXPECT_EQ(listener_thread->received_payload, tls_request_payload);

    client.disconnect();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: TLS handshake failure — client rejects server certificate
//
// The client is constructed with no trusted CA. OpenSSL certificate
// verification fails, the client sends a TLS alert, and the server
// tears down the connection. ConnectionEstablished is delivered (at
// TCP-accept time) before the TLS handshake is attempted; ConnectionLost
// follows when the alert is processed.
// ============================================================
TEST_F(TlsProtocolHandlerIntegrationTest, HandshakeFailure) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_tls_reactor_config(), listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    TlsListenerConfiguration tls_config;
    tls_config.certificate_path = certs_->server_cert_path;
    tls_config.private_key_path = certs_->server_key_path;

    listener_reactor->register_inbound_tls_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2}, tls_raw_buffer_capacity, tls_config);

    auto listener_thread = ApplicationThread::create<TlsPassiveListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    // Run the client in a background thread because SSL_connect blocks while
    // the server drives the TLS handshake, and the test thread must be free
    // to wait_for() server-side events concurrently.
    std::atomic<bool> ssl_connect_failed{false};
    std::atomic<bool> client_thread_done{false};

    std::thread client_thread([&]() {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { client_thread_done.store(true, std::memory_order_release); return; }

        // Deliberately do NOT load any CA cert. SSL_VERIFY_PEER with no trusted
        // roots causes SSL_connect to fail when the server presents its certificate.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        const int sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd == -1) {
            SSL_CTX_free(ctx);
            client_thread_done.store(true, std::memory_order_release);
            return;
        }

        timeval timeout{5, 0};
        ::setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(listen_port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            SSL* ssl = SSL_new(ctx);
            if (ssl) {
                SSL_set_fd(ssl, sock_fd);
                if (SSL_connect(ssl) != 1) {
                    ssl_connect_failed.store(true, std::memory_order_release);
                }
                SSL_free(ssl);
            }
        }

        ::close(sock_fd);
        SSL_CTX_free(ctx);
        client_thread_done.store(true, std::memory_order_release);
    });

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after handshake failure: " << last_wait_failure_description();

    EXPECT_TRUE(wait_for([&]() { return client_thread_done.load(std::memory_order_acquire); }))
        << "Client thread did not complete within timeout";

    EXPECT_TRUE(ssl_connect_failed.load(std::memory_order_acquire)) << "SSL_connect should have failed (no trusted CA)";

    client_thread.join();
    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

} // namespace pubsub_itc_fw::tests
