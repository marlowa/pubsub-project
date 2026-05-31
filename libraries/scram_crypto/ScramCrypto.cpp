// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "ScramCrypto.hpp"

#include <stdexcept>
#include <string>

namespace scram_crypto {

std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t key_size,
                                  const uint8_t* data, size_t data_size) {
    unsigned char result[32];
    unsigned int length = 32;
    if (HMAC(EVP_sha256(),
             key, static_cast<int>(key_size),
             data, data_size,
             result, &length) == nullptr) {
        throw std::runtime_error("hmac_sha256: OpenSSL HMAC failed");
    }
    return std::vector<uint8_t>(result, result + 32);
}

std::vector<uint8_t> sha256(const uint8_t* data, size_t data_size) {
    unsigned char result[32];
    SHA256(data, data_size, result);
    return std::vector<uint8_t>(result, result + 32);
}

std::vector<uint8_t> pbkdf2_sha256(std::string_view password,
                                    const uint8_t* salt, size_t salt_size,
                                    int32_t iterations) {
    std::vector<uint8_t> result(32);
    if (!PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             salt, static_cast<int>(salt_size),
                             iterations, EVP_sha256(),
                             32, result.data())) {
        throw std::runtime_error("pbkdf2_sha256: OpenSSL PKCS5_PBKDF2_HMAC failed");
    }
    return result;
}

ScramCredential make_scram_credential(std::string_view password,
                                      const uint8_t* salt, size_t salt_size,
                                      int32_t iterations) {
    const std::vector<uint8_t> salted_password = pbkdf2_sha256(password, salt, salt_size, iterations);

    static const std::string client_key_label = "Client Key";
    static const std::string server_key_label = "Server Key";

    const std::vector<uint8_t> client_key = hmac_sha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(client_key_label.data()), client_key_label.size());

    const std::vector<uint8_t> stored_key = sha256(client_key.data(), client_key.size());

    const std::vector<uint8_t> server_key = hmac_sha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(server_key_label.data()), server_key_label.size());

    return ScramCredential{
        std::vector<uint8_t>(salt, salt + salt_size),
        iterations,
        stored_key,
        server_key
    };
}

std::vector<uint8_t> compute_auth_message(std::string_view comp_id,
                                           const std::vector<uint8_t>& client_nonce,
                                           const std::vector<uint8_t>& server_nonce,
                                           const uint8_t* salt, size_t salt_size,
                                           int32_t iterations) {
    std::vector<uint8_t> message;

    auto append_u32_le = [&message](uint32_t value) {
        message.push_back(static_cast<uint8_t>(value & 0xFFU));
        message.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
        message.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
        message.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
    };
    auto append_blob = [&message, &append_u32_le](const uint8_t* data, size_t size) {
        append_u32_le(static_cast<uint32_t>(size));
        message.insert(message.end(), data, data + size);
    };

    append_blob(reinterpret_cast<const uint8_t*>(comp_id.data()), comp_id.size());
    append_blob(client_nonce.data(), client_nonce.size());
    append_blob(server_nonce.data(), server_nonce.size());
    append_blob(salt, salt_size);
    append_u32_le(static_cast<uint32_t>(iterations));

    return message;
}

} // namespace scram_crypto
