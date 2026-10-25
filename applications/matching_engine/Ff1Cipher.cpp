// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Ff1Cipher.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#include <openssl/evp.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

// unsigned __int128 is a GNU extension, which GCC and Clang both provide. It holds the number FF1
// takes from each round's AES output, which is up to 16 bytes, so that it can be reduced modulo the
// radix to the power of a half's length without writing arithmetic on numbers of any size.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

namespace matching_engine {

namespace {

using Wide = unsigned __int128;

constexpr size_t block_size = 16;

// radix to the power of exponent, or 0 if it does not fit in 64 bits.
uint64_t power_or_zero(uint32_t radix, size_t exponent) {
    uint64_t result = 1;
    for (size_t step = 0; step < exponent; ++step) {
        if (result > UINT64_MAX / radix) {
            return 0;
        }
        result *= radix;
    }
    return result;
}

// The number the numerals stand for, most significant first (NUM_radix in SP 800-38G).
uint64_t number_of(const uint16_t* numerals, size_t count, uint32_t radix) {
    uint64_t value = 0;
    for (size_t index = 0; index < count; ++index) {
        value = value * radix + numerals[index];
    }
    return value;
}

// Writes value as count numerals, most significant first (STR_radix in SP 800-38G).
void numerals_of(uint64_t value, size_t count, uint32_t radix, uint16_t* numerals) {
    for (size_t index = count; index > 0; --index) {
        numerals[index - 1] = static_cast<uint16_t>(value % radix);
        value /= radix;
    }
}

// The number of bits needed for count numerals in this radix, rounded up: ceil(count * log2(radix)), computed exactly
// as the bit length of radix^count - 1, which is what SP 800-38G's definition of b comes to.
size_t bits_for(uint32_t radix, size_t count) {
    const uint64_t largest = power_or_zero(radix, count) - 1;
    size_t bits = 0;
    for (uint64_t remaining = largest; remaining != 0; remaining >>= 1) {
        ++bits;
    }
    return bits;
}

} // un-named namespace

Ff1Cipher::Ff1Cipher(const uint8_t* key, size_t key_size, uint32_t radix) : radix_(radix) {
    if (radix < 2 || radix > 65536) {
        throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: the radix must be from 2 to 65536", __FILE__, __LINE__);
    }
    const EVP_CIPHER* cipher = nullptr;
    switch (key_size) {
        case 16:
            cipher = EVP_aes_128_ecb();
            break;
        case 24:
            cipher = EVP_aes_192_ecb();
            break;
        case 32:
            cipher = EVP_aes_256_ecb();
            break;
        default:
            throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: the key must be 16, 24 or 32 bytes", __FILE__, __LINE__);
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        throw std::runtime_error("Ff1Cipher: OpenSSL could not allocate a cipher context");
    }
    // ECB on single blocks is the AES block function itself, which is all FF1 asks of the cipher: the chaining FF1
    // needs is done below, by hand, over a few blocks.
    if (EVP_EncryptInit_ex(context, cipher, nullptr, key, nullptr) != 1 || EVP_CIPHER_CTX_set_padding(context, 0) != 1) {
        EVP_CIPHER_CTX_free(context);
        throw std::runtime_error("Ff1Cipher: OpenSSL could not set up AES");
    }
    context_ = context;
}

Ff1Cipher::~Ff1Cipher() {
    EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(context_));
}

void Ff1Cipher::encrypt_block(const uint8_t* in, uint8_t* out) {
    int written = 0;
    if (EVP_EncryptUpdate(static_cast<EVP_CIPHER_CTX*>(context_), out, &written, in, static_cast<int>(block_size)) != 1 ||
        written != static_cast<int>(block_size)) {
        throw std::runtime_error("Ff1Cipher: OpenSSL failed to encrypt a block");
    }
}

void Ff1Cipher::encrypt(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result) {
    run(numerals, count, tweak, result, Direction::encrypt);
}

void Ff1Cipher::decrypt(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result) {
    run(numerals, count, tweak, result, Direction::decrypt);
}

void Ff1Cipher::run(const uint16_t* numerals, size_t count, std::string_view tweak, uint16_t* result, Direction direction) {
    // The names follow Algorithm 7 and 8 of SP 800-38G, so that the code can be read against it: n numerals, split into
    // A of u numerals and B of v; b bytes hold a number of v numerals; d bytes of each round's output are used.
    const size_t n = count;
    if (n < 2 || n > max_numerals) {
        throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: the number of numerals must be from 2 to max_numerals", __FILE__, __LINE__);
    }
    if (tweak.size() > max_tweak_bytes) {
        throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: the tweak is longer than max_tweak_bytes", __FILE__, __LINE__);
    }
    for (size_t index = 0; index < n; ++index) {
        if (numerals[index] >= radix_) {
            throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: a numeral is not below the radix", __FILE__, __LINE__);
        }
    }
    const size_t u = n / 2;
    const size_t v = n - u;
    const uint64_t modulus_u = power_or_zero(radix_, u);
    const uint64_t modulus_v = power_or_zero(radix_, v);
    if (modulus_v == 0) {
        throw pubsub_itc_fw::PreconditionAssertion("Ff1Cipher: radix to the power of half the numerals does not fit in 64 bits", __FILE__, __LINE__);
    }
    const size_t b = (bits_for(radix_, v) + 7) / 8;
    const size_t d = 4 * ((b + 3) / 4) + 4;
    // The output of a round is used as a number of d bytes, which this implementation holds in 128 bits. A number of v
    // numerals is below 2^64, so b is at most 8 and d at most 12, which needs only the first block of the round's
    // output and fits in 128 bits.
    const size_t t = tweak.size();
    const bool encrypting = direction == Direction::encrypt;

    // P: the fixed first block, which describes the parameters.
    std::array<uint8_t, block_size> p{};
    p[0] = 1;
    p[1] = 2;
    p[2] = 1;
    p[3] = static_cast<uint8_t>(radix_ >> 16);
    p[4] = static_cast<uint8_t>(radix_ >> 8);
    p[5] = static_cast<uint8_t>(radix_);
    p[6] = 10;
    p[7] = static_cast<uint8_t>(u % 256);
    p[8] = static_cast<uint8_t>(n >> 24);
    p[9] = static_cast<uint8_t>(n >> 16);
    p[10] = static_cast<uint8_t>(n >> 8);
    p[11] = static_cast<uint8_t>(n);
    p[12] = static_cast<uint8_t>(t >> 24);
    p[13] = static_cast<uint8_t>(t >> 16);
    p[14] = static_cast<uint8_t>(t >> 8);
    p[15] = static_cast<uint8_t>(t);
    // The chaining value after P, which is the same for every round.
    std::array<uint8_t, block_size> after_p{};
    encrypt_block(p.data(), after_p.data());

    // Q: the tweak, zeros to make P || Q a whole number of blocks, the round number, and b bytes of a number.
    const size_t padding = (block_size - ((t + b + 1) % block_size)) % block_size;
    const size_t q_size = t + padding + 1 + b;
    std::array<uint8_t, max_tweak_bytes + 2 * block_size> q{};
    std::memcpy(q.data(), tweak.data(), t);

    uint64_t a = number_of(numerals, u, radix_);
    uint64_t bee = number_of(numerals + u, v, radix_);
    for (size_t step = 0; step < 10; ++step) {
        // Encrypting goes through the rounds from 0 to 9; decrypting from 9 to 0, with the halves' parts swapped.
        const size_t i = encrypting ? step : 9 - step;
        const uint64_t round_input = encrypting ? bee : a;
        q[t + padding] = static_cast<uint8_t>(i);
        for (size_t byte = 0; byte < b; ++byte) {
            const size_t shift = 8 * (b - 1 - byte);
            q[t + padding + 1 + byte] = shift >= 64 ? 0 : static_cast<uint8_t>(round_input >> shift);
        }

        // R = PRF(P || Q): AES in CBC mode with a zero initial value, keeping only the last block.
        std::array<uint8_t, block_size> r = after_p;
        for (size_t offset = 0; offset < q_size; offset += block_size) {
            std::array<uint8_t, block_size> mixed{};
            for (size_t byte = 0; byte < block_size; ++byte) {
                mixed[byte] = static_cast<uint8_t>(r[byte] ^ q[offset + byte]);
            }
            encrypt_block(mixed.data(), r.data());
        }

        // S is the first d bytes of R, d being at most 16 here, so y is the number those bytes stand for.
        Wide y = 0;
        for (size_t byte = 0; byte < d; ++byte) {
            y = (y << 8) | r[byte];
        }

        // Rounds with an even number work on u numerals, odd ones on v.
        const uint64_t modulus = (i % 2 == 0) ? modulus_u : modulus_v;
        const auto y_reduced = static_cast<uint64_t>(y % modulus);
        if (encrypting) {
            const auto c = static_cast<uint64_t>((static_cast<Wide>(a) + y_reduced) % modulus);
            a = bee;
            bee = c;
        } else {
            const auto c = static_cast<uint64_t>((static_cast<Wide>(bee) + modulus - y_reduced) % modulus);
            bee = a;
            a = c;
        }
    }
    numerals_of(a, u, radix_, result);
    numerals_of(bee, v, radix_, result + u);
}

} // namespaces

#pragma GCC diagnostic pop
