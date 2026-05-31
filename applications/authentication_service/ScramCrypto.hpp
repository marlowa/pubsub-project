#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string_view>
#include <vector>

namespace authentication_service {

/**
 * @brief Derived SCRAM-SHA-256 credential stored by the authentication service.
 *
 * The server never stores a plaintext password. Instead it stores the four values
 * below, derived once from the password via make_scram_credential(). These are
 * sufficient to verify a ClientProof and compute a ServerSignature without ever
 * reconstructing the plaintext or the SaltedPassword.
 *
 * stored_key = SHA-256( HMAC-SHA-256(SaltedPassword, "Client Key") )
 * server_key = HMAC-SHA-256(SaltedPassword, "Server Key")
 * where SaltedPassword = PBKDF2-HMAC-SHA-256(password, salt, iterations, dkLen=32)
 */
struct ScramCredential {
    std::vector<uint8_t> salt;
    int32_t iterations{};
    std::vector<uint8_t> stored_key;
    std::vector<uint8_t> server_key;
};

/**
 * @brief Computes HMAC-SHA-256(key, data).
 * @throws std::runtime_error if the OpenSSL call fails.
 */
[[nodiscard]] std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t key_size,
                                                 const uint8_t* data, size_t data_size);

/**
 * @brief Computes SHA-256(data).
 */
[[nodiscard]] std::vector<uint8_t> sha256(const uint8_t* data, size_t data_size);

/**
 * @brief Computes PBKDF2-HMAC-SHA-256(password, salt, iterations, dkLen=32).
 * @throws std::runtime_error if the OpenSSL call fails.
 */
[[nodiscard]] std::vector<uint8_t> pbkdf2_sha256(std::string_view password,
                                                   const uint8_t* salt, size_t salt_size,
                                                   int32_t iterations);

/**
 * @brief Derives a ScramCredential from a plaintext password, salt, and iteration count.
 *
 * This is used to initialise the stub credential at startup. In a production system
 * credentials are pre-derived and stored; make_scram_credential() is only called
 * during account provisioning, not at authentication time.
 *
 * @throws std::runtime_error if any underlying OpenSSL call fails.
 */
[[nodiscard]] ScramCredential make_scram_credential(std::string_view password,
                                                     const uint8_t* salt, size_t salt_size,
                                                     int32_t iterations);

/**
 * @brief Builds the canonical AuthMessage used for HMAC inputs on both sides.
 *
 * The AuthMessage is a deterministic binary encoding of the exchange parameters
 * that both the client and server can reproduce independently:
 *
 *   uint32_le(len(comp_id))     || comp_id_utf8
 *   uint32_le(len(client_nonce))|| client_nonce
 *   uint32_le(len(server_nonce))|| server_nonce
 *   uint32_le(len(salt))        || salt
 *   uint32_le(iterations)
 *
 * All length prefixes and the iterations field are little-endian unsigned 32-bit.
 */
[[nodiscard]] std::vector<uint8_t> compute_auth_message(std::string_view comp_id,
                                                         const std::vector<uint8_t>& client_nonce,
                                                         const std::vector<uint8_t>& server_nonce,
                                                         const uint8_t* salt, size_t salt_size,
                                                         int32_t iterations);

} // namespace authentication_service
