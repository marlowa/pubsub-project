// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/ssl.h>

#include <utility>

#include <pubsub_itc_fw/TlsState.hpp>

namespace pubsub_itc_fw {

TlsState::~TlsState() {
    if (ssl != nullptr) {
        SSL_free(ssl); // Also releases rbio and wbio set via SSL_set_bio.
        ssl = nullptr;
        rbio = nullptr;
        wbio = nullptr;
    } else {
        // SSL_set_bio was not reached; free BIOs independently if allocated.
        if (rbio != nullptr) {
            BIO_free(rbio);
            rbio = nullptr;
        }
        if (wbio != nullptr) {
            BIO_free(wbio);
            wbio = nullptr;
        }
    }
}

TlsState::TlsState(TlsState&& other)
    : ssl(other.ssl)
    , rbio(other.rbio)
    , wbio(other.wbio)
    , handshake_phase(other.handshake_phase)
    , pending_outbound(std::move(other.pending_outbound))
    , pending_outbound_offset(other.pending_outbound_offset) {
    other.ssl = nullptr;
    other.rbio = nullptr;
    other.wbio = nullptr;
    other.pending_outbound_offset = 0;
}

TlsState& TlsState::operator=(TlsState&& other) {
    if (this != &other) {
        if (ssl != nullptr) {
            SSL_free(ssl);
        } else {
            if (rbio != nullptr) { BIO_free(rbio); }
            if (wbio != nullptr) { BIO_free(wbio); }
        }
        ssl = other.ssl;
        rbio = other.rbio;
        wbio = other.wbio;
        handshake_phase = other.handshake_phase;
        pending_outbound = std::move(other.pending_outbound);
        pending_outbound_offset = other.pending_outbound_offset;
        other.ssl = nullptr;
        other.rbio = nullptr;
        other.wbio = nullptr;
        other.pending_outbound_offset = 0;
    }
    return *this;
}

} // namespaces
