// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/TlsContext.hpp>

namespace pubsub_itc_fw {

namespace {

std::string collect_openssl_errors() {
    std::string result;
    std::array<char, 256> buffer{};
    unsigned long code{0};
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer.data(), buffer.size());
        if (!result.empty()) {
            result += "; ";
        }
        result += buffer.data();
    }
    return result.empty() ? "unknown OpenSSL error" : result;
}

void apply_common_tls_options(SSL_CTX* context) {
    // Cap at TLS 1.2: QuickFIX/J's MINA SslFilter does not handle TLS 1.3
    // NewSessionTicket records correctly and deadlocks waiting for a response
    // that it never sends, causing the FIX client to time out on logon.
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(context, TLS1_2_VERSION);
    SSL_CTX_set_options(context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    // TLS 1.2 cipher suites: AEAD only.
    SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384");
}

} // namespaces

TlsContext::TlsContext(SSL_CTX* context) : context_(context) {}

TlsContext::~TlsContext() {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
}

std::tuple<std::unique_ptr<TlsContext>, std::string>
TlsContext::create_server(const std::string& certificate_path, const std::string& private_key_path,
                          const std::string& ca_path, bool require_client_certificate) {
    SSL_CTX* context = SSL_CTX_new(TLS_server_method());
    if (context == nullptr) {
        return {nullptr, "TlsContext::create_server: SSL_CTX_new failed: " + collect_openssl_errors()};
    }

    apply_common_tls_options(context);

    if (SSL_CTX_use_certificate_chain_file(context, certificate_path.c_str()) != 1) {
        SSL_CTX_free(context);
        return {nullptr, "TlsContext::create_server: failed to load certificate '" + certificate_path + "': " + collect_openssl_errors()};
    }

    if (SSL_CTX_use_PrivateKey_file(context, private_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(context);
        return {nullptr, "TlsContext::create_server: failed to load private key '" + private_key_path + "': " + collect_openssl_errors()};
    }

    if (SSL_CTX_check_private_key(context) != 1) {
        SSL_CTX_free(context);
        return {nullptr, "TlsContext::create_server: certificate and private key do not match: " + collect_openssl_errors()};
    }

    if (!ca_path.empty()) {
        if (SSL_CTX_load_verify_locations(context, ca_path.c_str(), nullptr) != 1) {
            SSL_CTX_free(context);
            return {nullptr, "TlsContext::create_server: failed to load CA '" + ca_path + "': " + collect_openssl_errors()};
        }
        const int verify_mode = require_client_certificate
            ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
            : SSL_VERIFY_PEER;
        SSL_CTX_set_verify(context, verify_mode, nullptr);
    }

    return {std::unique_ptr<TlsContext>(new TlsContext(context)), ""};
}

std::tuple<std::unique_ptr<TlsContext>, std::string>
TlsContext::create_client(const std::string& ca_path, const std::string& certificate_path,
                          const std::string& private_key_path) {
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr) {
        return {nullptr, "TlsContext::create_client: SSL_CTX_new failed: " + collect_openssl_errors()};
    }

    apply_common_tls_options(context);

    if (!ca_path.empty()) {
        if (SSL_CTX_load_verify_locations(context, ca_path.c_str(), nullptr) != 1) {
            SSL_CTX_free(context);
            return {nullptr, "TlsContext::create_client: failed to load CA '" + ca_path + "': " + collect_openssl_errors()};
        }
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    }

    if (!certificate_path.empty()) {
        if (SSL_CTX_use_certificate_chain_file(context, certificate_path.c_str()) != 1) {
            SSL_CTX_free(context);
            return {nullptr, "TlsContext::create_client: failed to load certificate '" + certificate_path + "': " + collect_openssl_errors()};
        }

        if (SSL_CTX_use_PrivateKey_file(context, private_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(context);
            return {nullptr, "TlsContext::create_client: failed to load private key '" + private_key_path + "': " + collect_openssl_errors()};
        }

        if (SSL_CTX_check_private_key(context) != 1) {
            SSL_CTX_free(context);
            return {nullptr, "TlsContext::create_client: certificate and private key do not match: " + collect_openssl_errors()};
        }
    }

    return {std::unique_ptr<TlsContext>(new TlsContext(context)), ""};
}

} // namespaces
