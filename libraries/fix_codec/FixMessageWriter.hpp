#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstddef>
#include <cstring>
#include <string_view>

#include <fix_codec/FixChecksum.hpp>

namespace fix_codec {

/**
 * @brief Zero-copy builder for one outbound FIX message in a caller buffer.
 *
 * FixMessageWriter writes tag=value fields directly into a caller-supplied
 * buffer (for example a slab chunk) with no intermediate std::string and no
 * allocation. The body fields are written first; @ref finish then writes the
 * BeginString (tag 8) and BodyLength (tag 9) header into a reserved prefix in
 * front of the body and appends the CheckSum (tag 10), computing both length
 * and checksum in place -- the hffix push_back_header / push_back_trailer idea.
 *
 * The caller sets every session and application field except tags 8, 9 and 10,
 * which this class owns. If any write would exceed the buffer capacity the
 * writer records an overflow and @ref finish returns an empty view.
 */
class FixMessageWriter {
  public:
    FixMessageWriter(char* buffer, size_t capacity, std::string_view begin_string = "FIXT.1.1")
        : buffer_(buffer), capacity_(capacity), begin_string_(begin_string) {
        // Reserve room at the front for "8=<begin>\x01" and "9=<up to 10 digits>\x01",
        // so the header can be written backward to end exactly at the body start.
        body_start_ = begin_string_.size() + header_reserve_slack;
        length_ = body_start_;
        overflow_ = capacity_ < body_start_;
    }

    void push_back_field(int tag, std::string_view value) {
        write_int(tag);
        write_byte('=');
        write_bytes(value);
        write_byte(soh);
    }

    void push_back_field(int tag, int value) {
        char digits[16];
        const std::to_chars_result result = std::to_chars(digits, digits + sizeof(digits), value);
        if (result.ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        push_back_field(tag, std::string_view(digits, static_cast<size_t>(result.ptr - digits)));
    }

    void push_back_field(int tag, char value) {
        push_back_field(tag, std::string_view(&value, 1));
    }

    [[nodiscard]] bool overflowed() const {
        return overflow_;
    }

    /**
     * @brief Completes the message and returns a view of the full wire bytes.
     *
     * Writes the tag 8 / tag 9 header in front of the body and the tag 10
     * checksum after it. Returns a view spanning the whole message, or an empty
     * view if the buffer overflowed at any point.
     */
    [[nodiscard]] std::string_view finish() {
        if (overflow_) {
            return {};
        }
        const size_t body_length = length_ - body_start_;
        char body_length_digits[16];
        const std::to_chars_result result = std::to_chars(body_length_digits, body_length_digits + sizeof(body_length_digits), body_length);
        if (result.ec != std::errc{}) {
            overflow_ = true;
            return {};
        }
        const size_t body_length_size = static_cast<size_t>(result.ptr - body_length_digits);
        const size_t header_size = 2 + begin_string_.size() + 1 + 2 + body_length_size + 1;
        const size_t header_start = body_start_ - header_size;

        size_t position = header_start;
        position = write_prefix_bytes(position, "8=", 2);
        position = write_prefix_bytes(position, begin_string_.data(), begin_string_.size());
        buffer_[position++] = soh;
        position = write_prefix_bytes(position, "9=", 2);
        position = write_prefix_bytes(position, body_length_digits, body_length_size);
        buffer_[position++] = soh;
        // position now equals body_start_ by construction of header_size.

        const unsigned int checksum = compute_checksum(std::string_view(buffer_ + header_start, length_ - header_start));
        if (!append_checksum(checksum)) {
            return {};
        }
        return std::string_view(buffer_ + header_start, length_ - header_start);
    }

  private:
    static constexpr char soh = '\x01';
    // "9=" plus up to ten body-length digits plus its SOH, with slack.
    static constexpr size_t header_reserve_slack = 16;

    void write_bytes(std::string_view value) {
        if (length_ + value.size() > capacity_) {
            overflow_ = true;
            return;
        }
        std::memcpy(buffer_ + length_, value.data(), value.size());
        length_ += value.size();
    }

    void write_byte(char value) {
        if (length_ + 1 > capacity_) {
            overflow_ = true;
            return;
        }
        buffer_[length_++] = value;
    }

    void write_int(int value) {
        char digits[16];
        const std::to_chars_result result = std::to_chars(digits, digits + sizeof(digits), value);
        if (result.ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        write_bytes(std::string_view(digits, static_cast<size_t>(result.ptr - digits)));
    }

    size_t write_prefix_bytes(size_t position, const char* source, size_t count) {
        std::memcpy(buffer_ + position, source, count);
        return position + count;
    }

    bool append_checksum(unsigned int checksum) {
        if (length_ + 7 > capacity_) {
            overflow_ = true;
            return false;
        }
        buffer_[length_++] = '1';
        buffer_[length_++] = '0';
        buffer_[length_++] = '=';
        buffer_[length_++] = static_cast<char>('0' + (checksum / 100) % 10);
        buffer_[length_++] = static_cast<char>('0' + (checksum / 10) % 10);
        buffer_[length_++] = static_cast<char>('0' + checksum % 10);
        buffer_[length_++] = soh;
        return true;
    }

    char* buffer_;
    size_t capacity_;
    std::string_view begin_string_;
    size_t body_start_{0};
    size_t length_{0};
    bool overflow_{false};
};

} // namespaces
