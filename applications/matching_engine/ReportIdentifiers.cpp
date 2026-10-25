// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ReportIdentifiers.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "TradingDate.hpp"

namespace matching_engine {

namespace {

using Digest = std::array<uint8_t, 32>;

Digest hmac_sha256(const uint8_t* key, size_t key_size, std::string_view text) {
    Digest digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), key, static_cast<int>(key_size), reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data(), &length) ==
            nullptr ||
        length != digest.size()) {
        throw std::runtime_error("ReportIdentifiers: OpenSSL HMAC failed");
    }
    return digest;
}

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

constexpr std::string_view hex_digits = "0123456789ABCDEF";

} // un-named namespace

ReportIdentifiers::ReportIdentifiers(const Secret& secret) : secret_(secret) {}

ReportIdentifiers::Secret ReportIdentifiers::read_secret_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("the identifier secret file " + path + " cannot be read");
    }
    std::stringstream contents;
    contents << file.rdbuf();
    std::string text = contents.str();
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    if (text.size() != 2 * secret_size) {
        throw std::runtime_error("the identifier secret file " + path + " must hold 64 hexadecimal digits, and holds " + std::to_string(text.size()) +
                                 " character(s)");
    }
    Secret secret{};
    for (size_t index = 0; index < secret_size; ++index) {
        const int high = hex_value(text[2 * index]);
        const int low = hex_value(text[2 * index + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("the identifier secret file " + path + " holds a character that is not a hexadecimal digit");
        }
        secret[index] = static_cast<uint8_t>(high * 16 + low);
    }
    return secret;
}

void ReportIdentifiers::use_trading_date(int32_t trading_date) {
    if (trading_date == trading_date_) {
        return;
    }
    const Digest day_key = hmac_sha256(secret_.data(), secret_.size(), fix_common::trading_date_text(trading_date));
    const Digest order_key = hmac_sha256(day_key.data(), day_key.size(), "OrderID");
    const Digest report_key = hmac_sha256(day_key.data(), day_key.size(), "ExecID");
    order_cipher_.reset();
    report_cipher_.reset();
    order_cipher_.emplace(order_key.data(), order_key.size(), 16);
    report_cipher_.emplace(report_key.data(), report_key.size(), 16);
    trading_date_ = trading_date;
}

std::string_view ReportIdentifiers::order_id(Buffer& buffer, int64_t placed_at_seq_no) {
    if (!order_cipher_.has_value()) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportIdentifiers: an OrderID was asked for before a trading date was given", __FILE__, __LINE__);
    }
    return make(buffer, 'O', *order_cipher_, placed_at_seq_no, 0);
}

std::string_view ReportIdentifiers::exec_id(Buffer& buffer, int64_t sequence_number, int64_t place) {
    if (!report_cipher_.has_value()) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportIdentifiers: an ExecID was asked for before a trading date was given", __FILE__, __LINE__);
    }
    return make(buffer, 'E', *report_cipher_, sequence_number, place);
}

std::string_view ReportIdentifiers::make(Buffer& buffer, char letter, Ff1Cipher& cipher, int64_t sequence_number, int64_t place) {
    if (!fits(sequence_number, place)) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportIdentifiers: a sequence number or a place does not fit in an identifier", __FILE__, __LINE__);
    }
    const uint64_t packed = (static_cast<uint64_t>(sequence_number) << 28) | static_cast<uint64_t>(place);
    std::array<uint16_t, 16> numerals{};
    for (size_t index = 0; index < numerals.size(); ++index) {
        numerals[index] = static_cast<uint16_t>((packed >> (4 * (15 - index))) & 0xF);
    }
    cipher.encrypt(numerals.data(), numerals.size(), {}, numerals.data());
    buffer[0] = letter;
    for (size_t index = 0; index < numerals.size(); ++index) {
        buffer[index + 1] = hex_digits[numerals[index]];
    }
    return {buffer.data(), buffer.size()};
}

std::optional<ReportIdentifiers::Position> ReportIdentifiers::position_of(std::string_view identifier) {
    if (!order_cipher_.has_value()) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportIdentifiers: an identifier was given before a trading date", __FILE__, __LINE__);
    }
    if (identifier.size() != identifier_length || (identifier[0] != 'O' && identifier[0] != 'E')) {
        return std::nullopt;
    }
    std::array<uint16_t, 16> numerals{};
    for (size_t index = 0; index < numerals.size(); ++index) {
        const int value = hex_value(identifier[index + 1]);
        if (value < 0) {
            return std::nullopt;
        }
        numerals[index] = static_cast<uint16_t>(value);
    }
    Position position;
    position.kind = identifier[0] == 'O' ? Kind::order : Kind::report;
    Ff1Cipher& cipher = position.kind == Kind::order ? *order_cipher_ : *report_cipher_;
    cipher.decrypt(numerals.data(), numerals.size(), {}, numerals.data());
    uint64_t packed = 0;
    for (const uint16_t numeral : numerals) {
        packed = (packed << 4) | numeral;
    }
    position.sequence_number = static_cast<int64_t>(packed >> 28);
    position.place = static_cast<int64_t>(packed & (static_cast<uint64_t>(place_limit) - 1));
    return position;
}

int64_t ReportIdentifiers::secret_fingerprint() const {
    const Digest digest = hmac_sha256(secret_.data(), secret_.size(), "fingerprint");
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value = (value << 8) | digest[index];
    }
    return static_cast<int64_t>(value);
}

} // namespaces
