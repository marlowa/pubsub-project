// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixParser.hpp"

#include <cstdio>
#include <string>

namespace sample_fix_gateway {

namespace {
constexpr char fix_delimiter = '\x01';
} // namespace

FixParser::FixParser(MessageCallback on_message) : on_message_(std::move(on_message)) {}

void FixParser::feed(const uint8_t* data, int length) {
    buffer_.append(reinterpret_cast<const char*>(data), static_cast<size_t>(length));

    while (try_extract_message()) {
        // keep extracting until we run out of complete messages
    }

    // Discard consumed bytes to keep the buffer from growing unboundedly.
    if (offset_ > 0) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
}

void FixParser::reset() {
    buffer_.clear();
    offset_ = 0;
}

bool FixParser::try_extract_message() {
    // A FIX message must start with "8=FIXT.1.1" or "8=FIX.5.0SP2".
    // Find the start of the next message beginning at offset_.
    const std::string begin_tag = "8=";
    const size_t start = buffer_.find(begin_tag, offset_);
    if (start == std::string::npos) {
        return false;
    }

    // Find tag 9 (BodyLength) -- it must be the second field.
    // Format: "8=...<fix_delimiter>9=<digits><fix_delimiter>"
    const size_t begin_string_end = buffer_.find(fix_delimiter, start);
    if (begin_string_end == std::string::npos) {
        return false; // incomplete
    }

    const std::string body_len_tag = "9=";
    const size_t bl_start = begin_string_end + 1;
    if (buffer_.compare(bl_start, body_len_tag.size(), body_len_tag) != 0) {
        // Malformed -- skip past the start tag and try again.
        offset_ = start + 1;
        return true;
    }

    const size_t body_length_end = buffer_.find(fix_delimiter, bl_start);
    if (body_length_end == std::string::npos) {
        return false; // incomplete
    }

    const std::string body_len_str = buffer_.substr(bl_start + body_len_tag.size(), body_length_end - bl_start - body_len_tag.size());
    const int body_length = std::stoi(body_len_str);
    if (body_length <= 0) {
        offset_ = start + 1;
        return true; // malformed, skip
    }

    // BodyLength counts from the byte after the tag 9 field delimiter to the
    // byte before the tag 10 field delimiter (inclusive).
    // So the tag 10 field starts at body_length_end + 1 + body_length.
    const size_t tag10_start = body_length_end + 1 + static_cast<size_t>(body_length);

    const std::string checksum_tag = "10=";
    if (buffer_.size() < tag10_start + checksum_tag.size()) {
        return false; // incomplete
    }

    if (buffer_.compare(tag10_start, checksum_tag.size(), checksum_tag) != 0) {
        // BodyLength pointed somewhere wrong -- skip and resync.
        offset_ = start + 1;
        return true;
    }

    const size_t checksum_end = buffer_.find(fix_delimiter, tag10_start);
    if (checksum_end == std::string::npos) {
        return false; // incomplete
    }

    // We now have a complete raw message from start to checksum_end (inclusive).
    const std::string raw_msg = buffer_.substr(start, checksum_end - start + 1);

    // Validate checksum.
    const std::string checksum_value = buffer_.substr(tag10_start + checksum_tag.size(), checksum_end - tag10_start - checksum_tag.size());

    // Checksum covers bytes from start of message up to (not including) "10=".
    const std::string msg_for_checksum = buffer_.substr(start, tag10_start - start);
    if (!validate_checksum(msg_for_checksum, checksum_value)) {
        // Bad checksum -- skip this message.
        offset_ = checksum_end + 1;
        return true;
    }

    // Parse fields and dispatch.
    FixMessage msg;
    if (parse_fields(raw_msg, msg)) {
        on_message_(msg);
    }

    offset_ = checksum_end + 1;
    return true;
}

bool FixParser::parse_fields(const std::string& raw_message, FixMessage& msg) {
    size_t pos = 0;
    while (pos < raw_message.size()) {
        // Find the '=' separating tag from value.
        const size_t eq = raw_message.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }

        // Find the field delimiter terminating the value.
        const size_t field_end = raw_message.find(fix_delimiter, eq + 1);
        if (field_end == std::string::npos) {
            break;
        }

        const std::string tag_str = raw_message.substr(pos, eq - pos);
        const std::string value = raw_message.substr(eq + 1, field_end - eq - 1);

        // Parse tag as integer -- ignore malformed tags.
        try {
            const int tag = std::stoi(tag_str);
            msg.set(tag, value);
        } catch (...) {
            // malformed tag -- skip
        }

        pos = field_end + 1;
    }

    return msg.has(Tag::MsgType);
}

bool FixParser::validate_checksum(const std::string& msg_bytes, const std::string& expected) {
    int sum = 0;
    for (const unsigned char c : msg_bytes) {
        sum += c;
    }
    const std::string computed = format_checksum(sum % 256);
    return computed == expected;
}

std::string FixParser::format_checksum(int sum) {
    char checksum_buffer[5];
    std::snprintf(checksum_buffer, sizeof(checksum_buffer), "%03u", static_cast<unsigned int>(sum) % 256u);
    return {checksum_buffer};
}

} // namespace sample_fix_gateway
