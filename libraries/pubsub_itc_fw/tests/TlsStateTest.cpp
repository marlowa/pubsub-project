// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TlsStateTest.cpp
 * @brief Unit tests for pubsub_itc_fw::TlsState.
 *
 * TlsState is a value type that owns an SSL object and two memory BIOs.
 * The destructor has two paths: when ssl is set (SSL_free releases all three)
 * and when ssl is null but BIOs were allocated (they must be freed individually).
 * The move constructor and move assignment operator transfer ownership and
 * null out the source.
 *
 * These tests exercise the struct directly using raw OpenSSL API calls so
 * that no TLS connection is required.
 */

#include <cstdint>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/TlsState.hpp>

namespace pubsub_itc_fw::tests {

namespace {

SSL_CTX* make_client_ctx() {
    return SSL_CTX_new(TLS_client_method());
}

} // namespaces

class TlsStateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ctx_ = make_client_ctx();
        ASSERT_NE(ctx_, nullptr);
    }

    void TearDown() override {
        SSL_CTX_free(ctx_);
    }

    SSL_CTX* ctx_{nullptr};
};

// ---------------------------------------------------------------------------
// Default construction
// ---------------------------------------------------------------------------

TEST_F(TlsStateTest, DefaultConstructedIsEmpty) {
    TlsState state;
    EXPECT_EQ(state.ssl, nullptr);
    EXPECT_EQ(state.rbio, nullptr);
    EXPECT_EQ(state.wbio, nullptr);
    EXPECT_EQ(state.handshake_phase, TlsState::HandshakePhase::Pending);
    EXPECT_TRUE(state.pending_outbound.empty());
    EXPECT_EQ(state.pending_outbound_offset, 0u);
}

// ---------------------------------------------------------------------------
// Destructor: else branch - ssl==nullptr, BIOs allocated independently
// ---------------------------------------------------------------------------

TEST_F(TlsStateTest, DestructorFreesOrphanedBios) {
    // Allocate BIOs without ever calling SSL_set_bio(). On destruction the
    // else branch must free them. This test relies on the destructor not
    // crashing or leaking (verified by valgrind/asan in CI).
    {
        TlsState state;
        state.rbio = BIO_new(BIO_s_mem());
        state.wbio = BIO_new(BIO_s_mem());
        ASSERT_NE(state.rbio, nullptr);
        ASSERT_NE(state.wbio, nullptr);
        // state goes out of scope here; destructor must free both BIOs.
    }
}

TEST_F(TlsStateTest, DestructorWithOnlyRbioSet) {
    {
        TlsState state;
        state.rbio = BIO_new(BIO_s_mem());
        ASSERT_NE(state.rbio, nullptr);
        // wbio remains nullptr; destructor must handle that gracefully.
    }
}

TEST_F(TlsStateTest, DestructorWithOnlyWbioSet) {
    {
        TlsState state;
        state.wbio = BIO_new(BIO_s_mem());
        ASSERT_NE(state.wbio, nullptr);
    }
}

TEST_F(TlsStateTest, DestructorEmptyStateIsNoop) {
    { TlsState state; }
}

// ---------------------------------------------------------------------------
// Move constructor
// ---------------------------------------------------------------------------

TEST_F(TlsStateTest, MoveConstructorTransfersFields) {
    TlsState source;
    source.ssl = SSL_new(ctx_);
    source.rbio = BIO_new(BIO_s_mem());
    source.wbio = BIO_new(BIO_s_mem());
    source.handshake_phase = TlsState::HandshakePhase::Complete;
    source.pending_outbound = {0x01, 0x02, 0x03};
    source.pending_outbound_offset = 1;

    SSL* expected_ssl = source.ssl;
    BIO* expected_rbio = source.rbio;
    BIO* expected_wbio = source.wbio;

    TlsState dest(std::move(source));

    EXPECT_EQ(dest.ssl, expected_ssl);
    EXPECT_EQ(dest.rbio, expected_rbio);
    EXPECT_EQ(dest.wbio, expected_wbio);
    EXPECT_EQ(dest.handshake_phase, TlsState::HandshakePhase::Complete);
    EXPECT_EQ(dest.pending_outbound, (std::vector<uint8_t>{0x01, 0x02, 0x03}));
    EXPECT_EQ(dest.pending_outbound_offset, 1u);
}

TEST_F(TlsStateTest, MoveConstructorNullsOutSource) {
    TlsState source;
    source.ssl = SSL_new(ctx_);
    source.rbio = BIO_new(BIO_s_mem());
    source.wbio = BIO_new(BIO_s_mem());
    source.pending_outbound_offset = 5;

    TlsState dest(std::move(source));

    EXPECT_EQ(source.ssl, nullptr);
    EXPECT_EQ(source.rbio, nullptr);
    EXPECT_EQ(source.wbio, nullptr);
    EXPECT_EQ(source.pending_outbound_offset, 0u);
    // dest's destructor will free ssl (and the BIOs via SSL_free).
}

TEST_F(TlsStateTest, MoveConstructorFromEmptyState) {
    TlsState source;
    TlsState dest(std::move(source));
    EXPECT_EQ(dest.ssl, nullptr);
    EXPECT_EQ(dest.rbio, nullptr);
    EXPECT_EQ(dest.wbio, nullptr);
}

// ---------------------------------------------------------------------------
// Move assignment
// ---------------------------------------------------------------------------

TEST_F(TlsStateTest, MoveAssignmentTransfersFields) {
    TlsState source;
    source.ssl = SSL_new(ctx_);
    source.handshake_phase = TlsState::HandshakePhase::Failed;
    source.pending_outbound = {0xAA, 0xBB};

    SSL* expected_ssl = source.ssl;

    TlsState dest;
    dest = std::move(source);

    EXPECT_EQ(dest.ssl, expected_ssl);
    EXPECT_EQ(dest.handshake_phase, TlsState::HandshakePhase::Failed);
    EXPECT_EQ(dest.pending_outbound, (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST_F(TlsStateTest, MoveAssignmentNullsOutSource) {
    TlsState source;
    source.ssl = SSL_new(ctx_);
    source.pending_outbound_offset = 3;

    TlsState dest;
    dest = std::move(source);

    EXPECT_EQ(source.ssl, nullptr);
    EXPECT_EQ(source.rbio, nullptr);
    EXPECT_EQ(source.wbio, nullptr);
    EXPECT_EQ(source.pending_outbound_offset, 0u);
}

TEST_F(TlsStateTest, MoveAssignmentFreesExistingDestinationSsl) {
    // dest already owns an SSL object. Assignment must free it, then take ownership
    // of source's SSL. If it doesn't free the old one, asan/valgrind will catch it.
    TlsState dest;
    dest.ssl = SSL_new(ctx_);

    TlsState source;
    source.ssl = SSL_new(ctx_);

    SSL* expected_ssl = source.ssl;
    dest = std::move(source);

    EXPECT_EQ(dest.ssl, expected_ssl);
    EXPECT_EQ(source.ssl, nullptr);
}

TEST_F(TlsStateTest, MoveAssignmentFreesExistingDestinationBios) {
    // dest has orphaned BIOs (ssl==nullptr). Assignment must free them via
    // the else branch before taking ownership of source.
    TlsState dest;
    dest.rbio = BIO_new(BIO_s_mem());
    dest.wbio = BIO_new(BIO_s_mem());

    TlsState source;
    source.ssl = SSL_new(ctx_);

    SSL* expected_ssl = source.ssl;
    dest = std::move(source);

    EXPECT_EQ(dest.ssl, expected_ssl);
    EXPECT_EQ(source.ssl, nullptr);
}

TEST_F(TlsStateTest, MoveAssignmentSelfAssignmentIsNoop) {
    TlsState state;
    state.ssl = SSL_new(ctx_);
    SSL* original = state.ssl;

    // Route through a reference to defeat the -Wself-move diagnostic while
    // still exercising the if (this != &other) guard in operator=.
    TlsState& alias = state;
    state = std::move(alias);

    EXPECT_EQ(state.ssl, original);
}

// ---------------------------------------------------------------------------
// has_pending_outbound and clear_pending_outbound
// ---------------------------------------------------------------------------

TEST_F(TlsStateTest, HasPendingOutboundFalseWhenEmpty) {
    TlsState state;
    EXPECT_FALSE(state.has_pending_outbound());
}

TEST_F(TlsStateTest, HasPendingOutboundTrueWhenDataPresent) {
    TlsState state;
    state.pending_outbound = {0x01, 0x02};
    state.pending_outbound_offset = 0;
    EXPECT_TRUE(state.has_pending_outbound());
}

TEST_F(TlsStateTest, HasPendingOutboundFalseWhenOffsetAtEnd) {
    TlsState state;
    state.pending_outbound = {0x01, 0x02};
    state.pending_outbound_offset = 2;
    EXPECT_FALSE(state.has_pending_outbound());
}

TEST_F(TlsStateTest, ClearPendingOutboundResetsBuffer) {
    TlsState state;
    state.pending_outbound = {0x01, 0x02, 0x03};
    state.pending_outbound_offset = 1;
    state.clear_pending_outbound();
    EXPECT_TRUE(state.pending_outbound.empty());
    EXPECT_EQ(state.pending_outbound_offset, 0u);
    EXPECT_FALSE(state.has_pending_outbound());
}

} // namespaces
