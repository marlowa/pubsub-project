#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include <openssl/ssl.h>

namespace pubsub_itc_fw {

/*
 * Per-connection TLS state owned by TlsRawBytesProtocolHandler.
 *
 * Owns the SSL object and therefore the two memory BIOs (rbio and wbio).
 * After SSL_set_bio() the SSL object takes ownership; SSL_free() releases
 * all three. If SSL_set_bio() was never reached (partial construction
 * failure) the BIOs are freed independently.
 *
 * pending_outbound holds ciphertext bytes that OpenSSL placed in the write
 * BIO but which could not be sent immediately due to EAGAIN. Both handshake
 * records and application-data ciphertext share this buffer.
 */
struct TlsState {
    enum class HandshakePhase { Pending, Complete, Failed };

    TlsState() = default;
    ~TlsState();

    TlsState(const TlsState&) = delete;
    TlsState& operator=(const TlsState&) = delete;

    TlsState(TlsState&& other);
    TlsState& operator=(TlsState&& other);

    SSL* ssl{nullptr};
    BIO* rbio{nullptr};
    BIO* wbio{nullptr};

    HandshakePhase handshake_phase{HandshakePhase::Pending};

    std::vector<uint8_t> pending_outbound;
    size_t pending_outbound_offset{0};

    [[nodiscard]] bool has_pending_outbound() const {
        return pending_outbound_offset < pending_outbound.size();
    }

    void clear_pending_outbound() {
        pending_outbound.clear();
        pending_outbound_offset = 0;
    }
};

} // namespace pubsub_itc_fw
