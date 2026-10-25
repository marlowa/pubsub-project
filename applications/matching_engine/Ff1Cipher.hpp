#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <string_view>

namespace matching_engine {

/**
 * @brief FF1, the format-preserving encryption of NIST Special Publication 800-38G, with AES.
 *
 * FF1 takes a string of numerals in a given radix, for example 16 hexadecimal digits, and gives back
 * another string of the same length in the same radix. It is a one-to-one mapping for each key and
 * tweak, so two different inputs never give the same output, and it can be reversed only with the
 * key. The matching engine uses it to turn the position of a report in the WAL into an identifier
 * that is unique for the day and says nothing about the venue's sequence numbers to a member who
 * does not hold the key (docs/availability/deterministic_matching_engine.md, section 3).
 *
 * This implementation holds each half of the numeral string as a 64-bit number, so it accepts a
 * radix and a length only where radix to the power of the longer half fits in 64 bits. That covers
 * 16 hexadecimal digits, which is what the engine uses, and the NIST sample vectors, which its unit
 * tests check it against. The tweak may be of any length up to 255 bytes.
 *
 * Not thread-safe: it keeps one OpenSSL cipher context, which each call uses.
 */
class Ff1Cipher {
  public:
    /// The most numerals one call accepts.
    static constexpr size_t max_numerals = 64;

    /// The longest tweak one call accepts, in bytes.
    static constexpr size_t max_tweak_bytes = 255;

    ~Ff1Cipher();

    /**
     * @param[in] key      The AES key: 16, 24 or 32 bytes, for AES-128, AES-192 or AES-256.
     * @param[in] key_size Its length in bytes.
     * @param[in] radix    The radix of the numerals, from 2 to 65536.
     * @throws PreconditionAssertion if the key size or the radix is not one of those.
     * @throws std::runtime_error if OpenSSL cannot set up the cipher.
     */
    Ff1Cipher(const uint8_t* key, size_t key_size, uint32_t radix);

    Ff1Cipher(const Ff1Cipher&) = delete;
    Ff1Cipher& operator=(const Ff1Cipher&) = delete;

    /**
     * @brief Encrypts a string of numerals.
     * @param[in]  numerals The numerals, each below the radix, most significant first.
     * @param[in]  count    How many there are: at least 2, at most max_numerals, and few enough that radix to the power
     *                      of the longer half fits in 64 bits.
     * @param[in]  tweak    The tweak, at most max_tweak_bytes; may be empty.
     * @param[out] result   Where the encrypted numerals are written: count of them. May be the same as numerals.
     * @throws PreconditionAssertion if a numeral, the count or the tweak is outside those limits.
     */
    void encrypt(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result);

    /**
     * @brief Decrypts a string of numerals that encrypt produced with the same key, radix and tweak.
     *
     * Parameters and limits as for encrypt.
     */
    void decrypt(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result);

  private:
    // Which way run() goes.
    enum class Direction { encrypt, decrypt };

    // Both directions are the same Feistel rounds with the arithmetic reversed.
    void run(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result, Direction direction);

    // One AES encryption of a 16-byte block.
    void encrypt_block(const uint8_t* in, uint8_t* out);

    // The OpenSSL cipher context, held as void* so that this header does not need OpenSSL's.
    void* context_{nullptr};
    uint32_t radix_{0};
};

} // namespaces
