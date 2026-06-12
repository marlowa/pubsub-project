#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw {

/**
 * @brief TLS configuration carried by InboundListenerConfiguration for
 *        TlsRawBytes listeners.
 *
 * When an InboundListenerConfiguration has protocol_type == TlsRawBytes,
 * the Reactor reads these fields during initialisation to construct a
 * TlsContext (loading certificates from disk once) and stores it in the
 * InboundListener. Each accepted connection then receives a per-connection
 * SSL object created from that shared TlsContext.
 */
struct TlsListenerConfiguration {
    /**
     * @brief Path to the PEM-encoded server certificate chain file.
     */
    std::string certificate_path;

    /**
     * @brief Path to the PEM-encoded private key file matching the certificate.
     */
    std::string private_key_path;

    /**
     * @brief Path to the PEM-encoded CA certificate used to verify client
     *        certificates.
     *
     * Pass an empty string to disable client certificate verification. When
     * non-empty, require_client_certificate controls whether a missing client
     * certificate is a hard error.
     */
    std::string ca_path;

    /**
     * @brief If true, clients must present a valid certificate signed by the CA.
     *
     * Ignored when ca_path is empty.
     */
    bool require_client_certificate{false};
};

} // namespaces
