#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <openssl/ssl.h>

namespace pubsub_itc_fw {

/**
 * @brief Wraps an OpenSSL SSL_CTX for use with TlsRawBytesProtocolHandler.
 *
 * A TlsContext is created once per listener or outbound service during
 * framework initialisation. Creating a per-connection SSL object from a
 * TlsContext is cheap; loading certificates (which happens here) is expensive
 * and must not be repeated per connection.
 *
 * TLS 1.2 is the minimum protocol version; TLS 1.3 is preferred. Only
 * authenticated encryption cipher suites are permitted.
 *
 * Thread safety: SSL_CTX is safe for creating SSL objects from the reactor
 * thread. TlsContext itself must not be modified after construction.
 */
class TlsContext {
public:
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    /**
     * @brief Creates a server-side TlsContext.
     *
     * @param[in] certificate_path          Path to the PEM-encoded server certificate chain.
     * @param[in] private_key_path          Path to the PEM-encoded private key.
     * @param[in] ca_path                   Path to the PEM-encoded CA certificate for
     *                                      verifying client certificates. Pass an empty
     *                                      string to disable client certificate verification.
     * @param[in] require_client_certificate If true, clients must present a valid certificate.
     *
     * @return {context, ""} on success, {nullptr, error_description} on failure.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<TlsContext>, std::string>
    create_server(const std::string& certificate_path, const std::string& private_key_path,
                  const std::string& ca_path, bool require_client_certificate);

    /**
     * @brief Creates a client-side TlsContext.
     *
     * @param[in] ca_path           Path to the PEM-encoded CA certificate for verifying
     *                              the server certificate. Pass an empty string to skip
     *                              server certificate verification (not recommended).
     * @param[in] certificate_path  Path to the PEM-encoded client certificate for mutual TLS.
     *                              Pass an empty string if mutual TLS is not required.
     * @param[in] private_key_path  Path to the PEM-encoded client private key.
     *                              Pass an empty string if mutual TLS is not required.
     *
     * @return {context, ""} on success, {nullptr, error_description} on failure.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<TlsContext>, std::string>
    create_client(const std::string& ca_path, const std::string& certificate_path,
                  const std::string& private_key_path);

    /**
     * @brief Returns the underlying SSL_CTX pointer.
     *
     * Used by TlsRawBytesProtocolHandler to create per-connection SSL objects.
     * The caller must not free the returned pointer.
     */
    [[nodiscard]] SSL_CTX* get() const {
        return context_;
    }

private:
    explicit TlsContext(SSL_CTX* context);

    SSL_CTX* context_{nullptr};
};

} // namespace pubsub_itc_fw
