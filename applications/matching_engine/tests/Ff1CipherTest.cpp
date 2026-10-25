// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "Ff1Cipher.hpp"

namespace matching_engine {
namespace {

std::vector<uint8_t> bytes_of_hex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t index = 0; index + 1 < hex.size(); index += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

// Numerals written as the NIST samples write them: 0-9 then a-z, for a radix of up to 36.
std::vector<uint16_t> numerals_of_text(const std::string& text) {
    std::vector<uint16_t> numerals;
    for (const char digit : text) {
        numerals.push_back(static_cast<uint16_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10));
    }
    return numerals;
}

std::string text_of_numerals(const std::vector<uint16_t>& numerals) {
    std::string text;
    for (const uint16_t numeral : numerals) {
        text.push_back(static_cast<char>(numeral < 10 ? '0' + numeral : 'a' + numeral - 10));
    }
    return text;
}

struct Sample {
    const char* name;
    const char* key;
    uint32_t radix;
    const char* tweak;
    const char* plaintext;
    const char* ciphertext;
};

// The nine FF1 samples NIST publishes with SP 800-38G, from AES-128 (samples 1 to 3), AES-192 (4 to 6) and AES-256
// (7 to 9), each with an empty tweak, a tweak of ten bytes, and a radix of 36 with a tweak of eleven bytes.
constexpr const char* key_128 = "2B7E151628AED2A6ABF7158809CF4F3C";
constexpr const char* key_192 = "2B7E151628AED2A6ABF7158809CF4F3CEF4359D8D580AA4F";
constexpr const char* key_256 = "2B7E151628AED2A6ABF7158809CF4F3CEF4359D8D580AA4F7F036D6F04FC6A94";
constexpr const char* tweak_10 = "39383736353433323130";
constexpr const char* tweak_11 = "3737373770717273373737";

const Sample samples[] = {
    {"sample 1", key_128, 10, "", "0123456789", "2433477484"},
    {"sample 2", key_128, 10, tweak_10, "0123456789", "6124200773"},
    {"sample 3", key_128, 36, tweak_11, "0123456789abcdefghi", "a9tv40mll9kdu509eum"},
    {"sample 4", key_192, 10, "", "0123456789", "2830668132"},
    {"sample 5", key_192, 10, tweak_10, "0123456789", "2496655549"},
    {"sample 6", key_192, 36, tweak_11, "0123456789abcdefghi", "xbj3kv35jrawxv32ysr"},
    {"sample 7", key_256, 10, "", "0123456789", "6657667009"},
    {"sample 8", key_256, 10, tweak_10, "0123456789", "1001623463"},
    {"sample 9", key_256, 36, tweak_11, "0123456789abcdefghi", "xs8a0azh2avyalyzuwd"},
};

TEST(Ff1CipherTest, EncryptsEveryNistSampleAsNistPublishesIt) {
    for (const Sample& sample : samples) {
        const std::vector<uint8_t> key = bytes_of_hex(sample.key);
        const std::vector<uint8_t> tweak_bytes = bytes_of_hex(sample.tweak);
        const std::string tweak(tweak_bytes.begin(), tweak_bytes.end());
        Ff1Cipher cipher(key.data(), key.size(), sample.radix);
        const std::vector<uint16_t> plaintext = numerals_of_text(sample.plaintext);
        std::vector<uint16_t> ciphertext(plaintext.size());
        cipher.encrypt(plaintext.data(), plaintext.size(), tweak, ciphertext.data());
        EXPECT_EQ(text_of_numerals(ciphertext), sample.ciphertext) << sample.name;
    }
}

TEST(Ff1CipherTest, DecryptsEveryNistSampleBackToItsPlaintext) {
    for (const Sample& sample : samples) {
        const std::vector<uint8_t> key = bytes_of_hex(sample.key);
        const std::vector<uint8_t> tweak_bytes = bytes_of_hex(sample.tweak);
        const std::string tweak(tweak_bytes.begin(), tweak_bytes.end());
        Ff1Cipher cipher(key.data(), key.size(), sample.radix);
        const std::vector<uint16_t> ciphertext = numerals_of_text(sample.ciphertext);
        std::vector<uint16_t> plaintext(ciphertext.size());
        cipher.decrypt(ciphertext.data(), ciphertext.size(), tweak, plaintext.data());
        EXPECT_EQ(text_of_numerals(plaintext), sample.plaintext) << sample.name;
    }
}

TEST(Ff1CipherTest, SixteenHexadecimalDigitsComeBackFromDecryptingWhatWasEncrypted) {
    const std::vector<uint8_t> key = bytes_of_hex(key_256);
    Ff1Cipher cipher(key.data(), key.size(), 16);
    std::mt19937_64 random(20261025);
    for (int round = 0; round < 1000; ++round) {
        std::vector<uint16_t> numerals(16);
        for (uint16_t& numeral : numerals) {
            numeral = static_cast<uint16_t>(random() % 16);
        }
        std::vector<uint16_t> encrypted(16);
        cipher.encrypt(numerals.data(), numerals.size(), {}, encrypted.data());
        std::vector<uint16_t> decrypted(16);
        cipher.decrypt(encrypted.data(), encrypted.size(), {}, decrypted.data());
        ASSERT_EQ(decrypted, numerals) << "round " << round;
    }
}

TEST(Ff1CipherTest, RefusesAKeyThatIsNotAnAesKeySize) {
    const std::vector<uint8_t> key(20, 0);
    EXPECT_THROW(Ff1Cipher(key.data(), key.size(), 16), pubsub_itc_fw::PreconditionAssertion);
}

TEST(Ff1CipherTest, RefusesANumeralThatIsNotBelowTheRadix) {
    const std::vector<uint8_t> key = bytes_of_hex(key_128);
    Ff1Cipher cipher(key.data(), key.size(), 10);
    std::vector<uint16_t> numerals = numerals_of_text("0123456789");
    numerals[3] = 10;
    std::vector<uint16_t> result(numerals.size());
    EXPECT_THROW(cipher.encrypt(numerals.data(), numerals.size(), {}, result.data()), pubsub_itc_fw::PreconditionAssertion);
}

TEST(Ff1CipherTest, RefusesMoreNumeralsThanItsArithmeticHolds) {
    const std::vector<uint8_t> key = bytes_of_hex(key_128);
    Ff1Cipher cipher(key.data(), key.size(), 16);
    // 32 hexadecimal digits: each half is 16 of them, and 16 to the power of 16 does not fit in 64 bits.
    const std::vector<uint16_t> numerals(32, 1);
    std::vector<uint16_t> result(numerals.size());
    EXPECT_THROW(cipher.encrypt(numerals.data(), numerals.size(), {}, result.data()), pubsub_itc_fw::PreconditionAssertion);
}

} // un-named namespace
} // namespaces
