// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixParser.hpp"

#include <cstdio>
#include <string>

namespace sample_fix_gateway {

static constexpr char SOH = '\x01';

FixParser::FixParser(MessageCallback on_message)
    : on_message_(std::move(on_message))
{
}

void FixParser::feed(const uint8_t* data, int length)
{
    buffer_.append(reinterpret_cast<const char*>(data),
                   static_cast<std::size_t>(length));

    while (try_extract_message()) {
        // keep extracting until we run out of complete messages
    }

    // Discard consumed bytes to keep the buffer from growing unboundedly.
    if (offset_ > 0) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
}

void FixParser::reset()
{
    buffer_.clear();
    offset_ = 0;
}

bool FixParser::try_extract_message()
{
    // A FIX message must start with "8=FIXT.1.1" or "8=FIX.5.0SP2".
    // Find the start of the next message beginning at offset_.
    const std::string begin_tag = "8=";
    const std::size_t start = buffer_.find(begin_tag, offset_);
    if (start == std::string::npos) {
        return false;
    }

    // Find tag 9 (BodyLength) -- it must be the second field.
    // Format: "8=...<SOH>9=<digits><SOH>"
    const std::size_t soh1 = buffer_.find(SOH, start);
    if (soh1 == std::string::npos) {
        return false; // incomplete
    }

    const std::string body_len_tag = "9=";
    const std::size_t bl_start = soh1 + 1;
    if (buffer_.compare(bl_start, body_len_tag.size(), body_len_tag) != 0) {
        // Malformed -- skip past the start tag and try again.
        offset_ = start + 1;
        return true;
    }

    const std::size_t soh2 = buffer_.find(SOH, bl_start);
    if (soh2 == std::string::npos) {
        return false; // incomplete
    }

    const std::string body_len_str =
        buffer_.substr(bl_start + body_len_tag.size(),
                       soh2 - bl_start - body_len_tag.size());
    const int body_length = std::stoi(body_len_str);
    if (body_length <= 0) {
        offset_ = start + 1;
        return true; // malformed, skip
    }

    // BodyLength counts from the byte after the tag 9 SOH delimiter to the
    // byte before the tag 10 SOH delimiter (inclusive).
    // So the tag 10 field starts at soh2 + 1 + body_length.
    const std::size_t tag10_start = soh2 + 1 + static_cast<std::size_t>(body_length);

    const std::string checksum_tag = "10=";
    if (buffer_.size() < tag10_start + checksum_tag.size()) {
        return false; // incomplete
    }

    if (buffer_.compare(tag10_start, checksum_tag.size(), checksum_tag) != 0) {
        // BodyLength pointed somewhere wrong -- skip and resync.
        offset_ = start + 1;
        return true;
    }

    const std::size_t soh3 = buffer_.find(SOH, tag10_start);
    if (soh3 == std::string::npos) {
        return false; // incomplete
    }

    // We now have a complete raw message from start to soh3 (inclusive).
    const std::string raw_msg = buffer_.substr(start, soh3 - start + 1);

    // Validate checksum.
    const std::string checksum_value =
        buffer_.substr(tag10_start + checksum_tag.size(),
                       soh3 - tag10_start - checksum_tag.size());

    // Checksum covers bytes from start of message up to (not including) "10=".
    const std::string msg_for_checksum = buffer_.substr(start, tag10_start - start);
    if (!validate_checksum(msg_for_checksum, checksum_value)) {
        // Bad checksum -- skip this message.
        offset_ = soh3 + 1;
        return true;
    }

    // Parse fields and dispatch.
    FixMessage msg;
    if (parse_fields(raw_msg, msg)) {
        on_message_(msg);
    }

    offset_ = soh3 + 1;
    return true;
}

bool FixParser::parse_fields(const std::string& buf, FixMessage& msg)
{
    std::size_t pos = 0;
    while (pos < buf.size()) {
        // Find the '=' separating tag from value.
        const std::size_t eq = buf.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }

        // Find the SOH terminating the value.
        const std::size_t soh = buf.find(SOH, eq + 1);
        if (soh == std::string::npos) {
            break;
        }

        const std::string tag_str = buf.substr(pos, eq - pos);
        const std::string value   = buf.substr(eq + 1, soh - eq - 1);

        // Parse tag as integer -- ignore malformed tags.
        try {
            const int tag = std::stoi(tag_str);
            msg.set(tag, value);
        } catch (...) {
            // malformed tag -- skip
        }

        pos = soh + 1;
    }

    return msg.has(Tag::MsgType);
}

bool FixParser::validate_checksum(const std::string& msg_bytes,
                                  const std::string& expected)
{
    int sum = 0;
    for (const unsigned char c : msg_bytes) {
        sum += c;
    }
    const std::string computed = format_checksum(sum % 256);
    return computed == expected;
}

std::string FixParser::format_checksum(int sum)
{
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%03u", static_cast<unsigned int>(sum) % 256u);
    return {buf};
}

} // namespace sample_fix_gateway
