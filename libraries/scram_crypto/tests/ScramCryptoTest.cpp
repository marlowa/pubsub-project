// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <scram_crypto/ScramCrypto.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

/*
Known-answer tests, not round-trip tests.

A round-trip test -- derive a credential, then verify a proof against it -- passes
even when the algorithm is subtly wrong, because both sides make the same mistake.
The failure that matters here is silent non-interoperability: this system would
authenticate against itself perfectly while rejecting every other SCRAM
implementation, or worse, accept something a correct implementation would refuse.
Only a published vector catches that, so the primitives below are pinned to
RFC 4231 (HMAC-SHA-256), FIPS 180-4 (SHA-256) and RFC 7914 section 11 (PBKDF2-
HMAC-SHA-256), and compute_auth_message is pinned byte for byte.
*/

namespace {

std::string hex_from_bytes(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t value : bytes) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0F]);
    }
    return result;
}

std::vector<uint8_t> bytes_from_text(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

// The salt the deployed stub credential uses: 0x01 through 0x10.
std::vector<uint8_t> stub_salt() {
    std::vector<uint8_t> salt;
    for (int value = 1; value <= 16; ++value) {
        salt.push_back(static_cast<uint8_t>(value));
    }
    return salt;
}

constexpr int32_t stub_iterations = 4096;
constexpr std::string_view stub_password = "stubpassword";

TEST(ScramCryptoTest, Sha256MatchesFips180Vector) {
    const std::vector<uint8_t> input = bytes_from_text("abc");
    EXPECT_EQ(hex_from_bytes(scram_crypto::sha256(input.data(), input.size())), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(ScramCryptoTest, Sha256OfEmptyInputMatchesFips180Vector) {
    EXPECT_EQ(hex_from_bytes(scram_crypto::sha256(nullptr, 0)), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ScramCryptoTest, HmacSha256MatchesRfc4231FirstVector) {
    const std::vector<uint8_t> key(20, 0x0b);
    const std::vector<uint8_t> data = bytes_from_text("Hi There");
    EXPECT_EQ(hex_from_bytes(scram_crypto::hmac_sha256(key.data(), key.size(), data.data(), data.size())),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(ScramCryptoTest, HmacSha256MatchesRfc4231SecondVector) {
    const std::vector<uint8_t> key = bytes_from_text("Jefe");
    const std::vector<uint8_t> data = bytes_from_text("what do ya want for nothing?");
    EXPECT_EQ(hex_from_bytes(scram_crypto::hmac_sha256(key.data(), key.size(), data.data(), data.size())),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(ScramCryptoTest, Pbkdf2Sha256MatchesRfc7914SingleIterationVector) {
    const std::vector<uint8_t> salt = bytes_from_text("salt");
    // RFC 7914 gives 64 bytes; this implementation derives dkLen=32, which is the
    // leading half of the published value.
    EXPECT_EQ(hex_from_bytes(scram_crypto::pbkdf2_sha256("passwd", salt.data(), salt.size(), 1)),
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
}

TEST(ScramCryptoTest, Pbkdf2Sha256MatchesRfc7914HighIterationVector) {
    const std::vector<uint8_t> salt = bytes_from_text("NaCl");
    EXPECT_EQ(hex_from_bytes(scram_crypto::pbkdf2_sha256("Password", salt.data(), salt.size(), 80000)),
              "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56");
}

// A vector with a fixed iteration count cannot tell whether the count is used at
// all, so vary only that.
TEST(ScramCryptoTest, Pbkdf2Sha256IterationCountChangesTheResult) {
    const std::vector<uint8_t> salt = stub_salt();
    const std::vector<uint8_t> once = scram_crypto::pbkdf2_sha256(stub_password, salt.data(), salt.size(), 1);
    const std::vector<uint8_t> many = scram_crypto::pbkdf2_sha256(stub_password, salt.data(), salt.size(), stub_iterations);
    EXPECT_NE(hex_from_bytes(once), hex_from_bytes(many));
}

TEST(ScramCryptoTest, MakeScramCredentialDerivesTheDocumentedChain) {
    const std::vector<uint8_t> salt = stub_salt();
    const scram_crypto::ScramCredential credential = scram_crypto::make_scram_credential(stub_password, salt.data(), salt.size(), stub_iterations);

    ASSERT_EQ(credential.salt, salt);
    ASSERT_EQ(credential.iterations, stub_iterations);

    // Rebuild the chain from the primitives: the credential must be exactly
    // stored_key = SHA-256(HMAC(SaltedPassword, "Client Key")) and
    // server_key = HMAC(SaltedPassword, "Server Key").
    const std::vector<uint8_t> salted = scram_crypto::pbkdf2_sha256(stub_password, salt.data(), salt.size(), stub_iterations);
    const std::vector<uint8_t> client_key_label = bytes_from_text("Client Key");
    const std::vector<uint8_t> server_key_label = bytes_from_text("Server Key");
    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(salted.data(), salted.size(), client_key_label.data(), client_key_label.size());

    EXPECT_EQ(credential.stored_key, scram_crypto::sha256(client_key.data(), client_key.size()));
    EXPECT_EQ(credential.server_key, scram_crypto::hmac_sha256(salted.data(), salted.size(), server_key_label.data(), server_key_label.size()));
}

/*
The absolute values for the stub credential. These are the same constants
auth_service_test.py and fix_capture_test.py write into their generated
credentials.toml, so this test also proves those scripts are exercising a
credential the C++ side genuinely derives from "stubpassword".
*/
TEST(ScramCryptoTest, MakeScramCredentialMatchesTheDeployedStubCredential) {
    const std::vector<uint8_t> salt = stub_salt();
    const scram_crypto::ScramCredential credential = scram_crypto::make_scram_credential(stub_password, salt.data(), salt.size(), stub_iterations);

    EXPECT_EQ(hex_from_bytes(credential.stored_key), "e0eaf13bf630627621a7f47e378fb8c62c5b4bb709d42767d0193dc537f34be2");
    EXPECT_EQ(hex_from_bytes(credential.server_key), "c016b7864891fe5bad757b60de234df09dde5a4be4deb015e158ca1aae9bec7d");
}

/*
The AuthMessage framing is bespoke to this project rather than taken from
RFC 5802, so no external vector exists for it. Pinning the exact bytes is what
stops the encoding drifting: any change to field order, length-prefix width or
endianness would break interoperability with the Java admin client and the Python
test clients, all of which build the same structure independently.
*/
TEST(ScramCryptoTest, ComputeAuthMessageUsesLittleEndianLengthPrefixedFraming) {
    const std::vector<uint8_t> salt = stub_salt();
    const std::vector<uint8_t> client_nonce(4, 0xAA);
    const std::vector<uint8_t> server_nonce(6, 0xBB);

    const std::vector<uint8_t> message =
        scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations);

    const std::string expected = "0a000000"                         // length of "CLIENT_ONE", little-endian
                                 "434c49454e545f4f4e45"             // "CLIENT_ONE"
                                 "04000000aaaaaaaa"                 // client nonce, length-prefixed
                                 "06000000bbbbbbbbbbbb"             // server nonce, length-prefixed
                                 "10000000"                         // salt length, 16
                                 "0102030405060708090a0b0c0d0e0f10" // salt
                                 "00100000";                        // iterations 4096, little-endian
    EXPECT_EQ(hex_from_bytes(message), expected);
    EXPECT_EQ(message.size(), static_cast<size_t>(56));
}

// Every field must reach the message. A field silently dropped from the framing
// would still round-trip within this system while being wrong.
TEST(ScramCryptoTest, ComputeAuthMessageIsSensitiveToEveryField) {
    const std::vector<uint8_t> salt = stub_salt();
    const std::vector<uint8_t> other_salt(16, 0x77);
    const std::vector<uint8_t> client_nonce(4, 0xAA);
    const std::vector<uint8_t> server_nonce(6, 0xBB);
    const std::vector<uint8_t> other_nonce(4, 0xCC);

    const std::vector<uint8_t> baseline =
        scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations);

    EXPECT_NE(baseline, scram_crypto::compute_auth_message("CLIENT_TWO", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations));
    EXPECT_NE(baseline, scram_crypto::compute_auth_message("CLIENT_ONE", other_nonce, server_nonce, salt.data(), salt.size(), stub_iterations));
    EXPECT_NE(baseline, scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, other_nonce, salt.data(), salt.size(), stub_iterations));
    EXPECT_NE(baseline, scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, other_salt.data(), other_salt.size(), stub_iterations));
    EXPECT_NE(baseline, scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations + 1));
}

/*
The verification path the authentication service actually runs: it holds only
stored_key, never the password or SaltedPassword, and must recover ClientKey from
the proof to check it. This is the property that makes a database breach
non-impersonating, so it is worth asserting directly rather than inferring from a
successful logon.
*/
TEST(ScramCryptoTest, ServerVerifiesClientProofFromStoredKeyAlone) {
    const std::vector<uint8_t> salt = stub_salt();
    const std::vector<uint8_t> client_nonce(8, 0x11);
    const std::vector<uint8_t> server_nonce(8, 0x22);

    const scram_crypto::ScramCredential credential = scram_crypto::make_scram_credential(stub_password, salt.data(), salt.size(), stub_iterations);
    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations);

    // Client side: it knows the password, so it derives ClientKey and proves
    // possession without sending it.
    const std::vector<uint8_t> salted = scram_crypto::pbkdf2_sha256(stub_password, salt.data(), salt.size(), stub_iterations);
    const std::vector<uint8_t> client_key_label = bytes_from_text("Client Key");
    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(salted.data(), salted.size(), client_key_label.data(), client_key_label.size());
    const std::vector<uint8_t> stored_key = scram_crypto::sha256(client_key.data(), client_key.size());
    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(stored_key.data(), stored_key.size(), auth_message.data(), auth_message.size());

    std::vector<uint8_t> client_proof(client_key.size());
    for (size_t index = 0; index < client_key.size(); ++index) {
        client_proof[index] = static_cast<uint8_t>(client_key[index] ^ client_signature[index]);
    }

    // Server side: stored_key only. Recover the candidate ClientKey and check that
    // hashing it reproduces stored_key.
    const std::vector<uint8_t> server_signature_over_auth =
        scram_crypto::hmac_sha256(credential.stored_key.data(), credential.stored_key.size(), auth_message.data(), auth_message.size());
    std::vector<uint8_t> recovered_client_key(client_proof.size());
    for (size_t index = 0; index < client_proof.size(); ++index) {
        recovered_client_key[index] = static_cast<uint8_t>(client_proof[index] ^ server_signature_over_auth[index]);
    }

    EXPECT_EQ(scram_crypto::sha256(recovered_client_key.data(), recovered_client_key.size()), credential.stored_key);

    // A wrong password must not verify.
    const std::vector<uint8_t> wrong_salted = scram_crypto::pbkdf2_sha256("notthepassword", salt.data(), salt.size(), stub_iterations);
    const std::vector<uint8_t> wrong_client_key =
        scram_crypto::hmac_sha256(wrong_salted.data(), wrong_salted.size(), client_key_label.data(), client_key_label.size());
    EXPECT_NE(scram_crypto::sha256(wrong_client_key.data(), wrong_client_key.size()), credential.stored_key);
}

// The ServerSignature is what proves the service genuine to the client, so the
// client's mutual-authentication check has something to compare against.
TEST(ScramCryptoTest, ServerSignatureIsHmacOfServerKeyOverAuthMessage) {
    const std::vector<uint8_t> salt = stub_salt();
    const std::vector<uint8_t> client_nonce(8, 0x33);
    const std::vector<uint8_t> server_nonce(8, 0x44);

    const scram_crypto::ScramCredential credential = scram_crypto::make_scram_credential(stub_password, salt.data(), salt.size(), stub_iterations);
    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations);

    const std::vector<uint8_t> server_signature =
        scram_crypto::hmac_sha256(credential.server_key.data(), credential.server_key.size(), auth_message.data(), auth_message.size());

    EXPECT_EQ(server_signature.size(), static_cast<size_t>(32));
    // It must depend on the exchange, not just on the credential: a replayed
    // signature from a different exchange must not match.
    const std::vector<uint8_t> other_auth_message =
        scram_crypto::compute_auth_message("CLIENT_ONE", client_nonce, server_nonce, salt.data(), salt.size(), stub_iterations + 1);
    EXPECT_NE(server_signature,
              scram_crypto::hmac_sha256(credential.server_key.data(), credential.server_key.size(), other_auth_message.data(), other_auth_message.size()));
}

TEST(ScramCryptoTest, DerivedMaterialIsAlwaysThirtyTwoBytes) {
    const std::vector<uint8_t> salt = stub_salt();
    const scram_crypto::ScramCredential credential = scram_crypto::make_scram_credential(stub_password, salt.data(), salt.size(), stub_iterations);
    EXPECT_EQ(credential.stored_key.size(), static_cast<size_t>(32));
    EXPECT_EQ(credential.server_key.size(), static_cast<size_t>(32));
    EXPECT_EQ(scram_crypto::pbkdf2_sha256(stub_password, salt.data(), salt.size(), 1).size(), static_cast<size_t>(32));
}

} // namespaces
