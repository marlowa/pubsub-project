// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TlsContextTest.cpp
 * @brief Unit tests for TlsContext::create_server and TlsContext::create_client.
 *
 * Covers both success paths and every error branch (bad cert path, bad key
 * path, cert/key mismatch, bad CA path). Certificates are generated in
 * memory with the OpenSSL C API and written to a temporary directory so
 * that no external tooling is required.
 */

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/TlsContext.hpp>

namespace pubsub_itc_fw::tests {

namespace {

EVP_PKEY* generate_ec_key() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) return nullptr;
    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

void add_basic_constraints(X509* x509, bool is_ca) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints,
        is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    if (ext) {
        X509_add_ext(x509, ext, -1);
        X509_EXTENSION_free(ext);
    }
}

X509* make_self_signed(EVP_PKEY* key, const char* cn) {
    X509* x = X509_new();
    if (!x) return nullptr;
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 86400L);
    X509_set_pubkey(x, key);
    X509_NAME* n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x, n);
    add_basic_constraints(x, true);
    if (X509_sign(x, key, EVP_sha256()) <= 0) { X509_free(x); return nullptr; }
    return x;
}

X509* make_signed(EVP_PKEY* subject_key, const char* cn,
                  X509* ca_cert, EVP_PKEY* ca_key) {
    X509* x = X509_new();
    if (!x) return nullptr;
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 2);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 86400L);
    X509_set_pubkey(x, subject_key);
    X509_NAME* n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(ca_cert));
    add_basic_constraints(x, false);
    if (X509_sign(x, ca_key, EVP_sha256()) <= 0) { X509_free(x); return nullptr; }
    return x;
}

bool write_pem_cert(const std::string& path, X509* cert) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const int ok = PEM_write_X509(f, cert);
    std::fclose(f);
    return ok == 1;
}

bool write_pem_key(const std::string& path, EVP_PKEY* key) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const int ok = PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(f);
    return ok == 1;
}

} // namespaces

// ============================================================
// Fixture: generates CA, server cert+key, and a spare key (for
// mismatch tests) in a temporary directory.
// ============================================================
class TlsContextTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/dev/shm/tls_ctx_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;

        EVP_PKEY* ca_key = generate_ec_key();
        X509* ca_cert = ca_key ? make_self_signed(ca_key, "Test CA") : nullptr;

        EVP_PKEY* server_key = generate_ec_key();
        X509* server_cert = (server_key && ca_cert)
            ? make_signed(server_key, "Test Server", ca_cert, ca_key) : nullptr;

        EVP_PKEY* other_key = generate_ec_key();

        bool ok = ca_cert && server_cert && other_key;
        if (ok) {
            ca_cert_path_   = dir_ + "/ca.crt";
            server_cert_path_ = dir_ + "/server.crt";
            server_key_path_  = dir_ + "/server.key";
            other_key_path_   = dir_ + "/other.key";

            ok = write_pem_cert(ca_cert_path_, ca_cert)
              && write_pem_cert(server_cert_path_, server_cert)
              && write_pem_key(server_key_path_, server_key)
              && write_pem_key(other_key_path_, other_key);
        }

        if (ca_cert)    X509_free(ca_cert);
        if (server_cert) X509_free(server_cert);
        if (ca_key)     EVP_PKEY_free(ca_key);
        if (server_key) EVP_PKEY_free(server_key);
        if (other_key)  EVP_PKEY_free(other_key);

        ASSERT_TRUE(ok) << "Certificate generation failed";
        valid_ = ok;
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::string dir_;
    std::string ca_cert_path_;
    std::string server_cert_path_;
    std::string server_key_path_;
    std::string other_key_path_;
    bool valid_{false};
};

// ============================================================
// create_server: success paths
// ============================================================

TEST_F(TlsContextTest, CreateServerSuccessNoCA) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, server_key_path_, "", false);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
    EXPECT_NE(ctx->get(), nullptr);
}

TEST_F(TlsContextTest, CreateServerSuccessWithCANoClientCert) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, server_key_path_, ca_cert_path_, false);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
}

TEST_F(TlsContextTest, CreateServerSuccessWithCARequireClientCert) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, server_key_path_, ca_cert_path_, true);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
}

// ============================================================
// create_server: error paths
// ============================================================

TEST_F(TlsContextTest, CreateServerBadCertPath) {
    auto [ctx, err] = TlsContext::create_server("/nonexistent/cert.pem", server_key_path_, "", false);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load certificate"), std::string::npos);
}

TEST_F(TlsContextTest, CreateServerBadKeyPath) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, "/nonexistent/key.pem", "", false);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load private key"), std::string::npos);
}

TEST_F(TlsContextTest, CreateServerMismatchedKey) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, other_key_path_, "", false);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    // OpenSSL may detect the mismatch either at key-load time or at the
    // explicit check_private_key call, so we just assert that a failure occurred.
}

TEST_F(TlsContextTest, CreateServerBadCAPath) {
    auto [ctx, err] = TlsContext::create_server(server_cert_path_, server_key_path_, "/nonexistent/ca.pem", false);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load CA"), std::string::npos);
}

// ============================================================
// create_client: success paths
// ============================================================

TEST_F(TlsContextTest, CreateClientSuccessNoCerts) {
    auto [ctx, err] = TlsContext::create_client("", "", "");
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
    EXPECT_NE(ctx->get(), nullptr);
}

TEST_F(TlsContextTest, CreateClientSuccessWithCA) {
    auto [ctx, err] = TlsContext::create_client(ca_cert_path_, "", "");
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
}

TEST_F(TlsContextTest, CreateClientSuccessWithCAAndClientCert) {
    auto [ctx, err] = TlsContext::create_client(ca_cert_path_, server_cert_path_, server_key_path_);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(err.empty()) << "Unexpected error: " << err;
}

// ============================================================
// create_client: error paths
// ============================================================

TEST_F(TlsContextTest, CreateClientBadCAPath) {
    auto [ctx, err] = TlsContext::create_client("/nonexistent/ca.pem", "", "");
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load CA"), std::string::npos);
}

TEST_F(TlsContextTest, CreateClientBadCertPath) {
    auto [ctx, err] = TlsContext::create_client(ca_cert_path_, "/nonexistent/cert.pem", server_key_path_);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load certificate"), std::string::npos);
}

TEST_F(TlsContextTest, CreateClientBadKeyPath) {
    auto [ctx, err] = TlsContext::create_client(ca_cert_path_, server_cert_path_, "/nonexistent/key.pem");
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("failed to load private key"), std::string::npos);
}

TEST_F(TlsContextTest, CreateClientMismatchedKey) {
    auto [ctx, err] = TlsContext::create_client(ca_cert_path_, server_cert_path_, other_key_path_);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
}

} // namespaces
