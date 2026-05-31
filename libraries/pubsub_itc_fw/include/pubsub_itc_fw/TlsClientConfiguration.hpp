#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief TLS configuration for outbound connections to a named service.
 *
 * Carried by ServiceEndpoints when a service requires TLS. The framework
 * creates one TlsContext per outbound connection attempt (cert files are loaded
 * once at that point) and one SSL object per connection.
 */
struct TlsClientConfiguration {
    /**
     * @brief Path to the PEM-encoded CA certificate for verifying the server.
     *
     * Pass an empty string to skip server certificate verification (not
     * recommended outside of testing).
     */
    std::string ca_path;

    /**
     * @brief Path to the PEM-encoded client certificate for mutual TLS.
     *
     * Pass an empty string if the server does not require a client certificate.
     */
    std::string certificate_path;

    /**
     * @brief Path to the PEM-encoded client private key matching certificate_path.
     *
     * Pass an empty string if the server does not require a client certificate.
     */
    std::string private_key_path;

    /**
     * @brief Minimum plaintext MirroredBuffer capacity in bytes for inbound data.
     */
    int64_t raw_buffer_capacity{65536};
};

} // namespace pubsub_itc_fw
