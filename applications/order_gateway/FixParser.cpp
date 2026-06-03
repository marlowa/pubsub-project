// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixParser.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace order_gateway {

namespace {
constexpr char field_delimiter = '\x01'; // FIX SOH
} // namespace

FixParser::FixParser(pubsub_itc_fw::QuillLogger& logger, MessageCallback on_message) : on_message_(std::move(on_message)), logger_(logger) {}

size_t FixParser::feed(const uint8_t* data, size_t available) {
    // Wrap the MirroredBuffer window in a string_view. The double-mapping
    // of MirroredBuffer guarantees this region is contiguous in virtual address
    // space even if the ring's physical storage wraps around.
    const std::string_view window(reinterpret_cast<const char*>(data), available);

    // parse_cursor tracks how many bytes of window have been fully consumed by
    // complete messages. After the loop it equals the number of bytes the caller
    // must advance the MirroredBuffer read pointer by.
    size_t parse_cursor = 0;

    while (try_extract_message(window, parse_cursor)) {
        // keep extracting until no further complete message is found
    }

    return parse_cursor;
}

void FixParser::reset() {
    // Nothing to reset: the parser has no internal accumulation buffer.
    // Partial bytes in the MirroredBuffer are discarded when the caller
    // advances the ring's read pointer fully on connection close.
}

bool FixParser::try_extract_message(std::string_view window, size_t& parse_cursor) {
    // A FIX message must start with "8=".
    const size_t start = window.find("8=", parse_cursor);
    if (start == std::string_view::npos) {
        return false;
    }

    // Find the SOH that terminates the BeginString field (tag 8).
    const size_t begin_string_end = window.find(field_delimiter, start);
    if (begin_string_end == std::string_view::npos) {
        return false; // incomplete -- BeginString field not yet fully received
    }

    // The field immediately after BeginString must be BodyLength (tag 9).
    constexpr std::string_view body_length_tag = "9=";
    const size_t body_length_tag_start = begin_string_end + 1;
    if (window.size() < body_length_tag_start + body_length_tag.size()) {
        return false; // incomplete -- not enough bytes to check for "9="
    }
    if (window.compare(body_length_tag_start, body_length_tag.size(), body_length_tag) != 0) {
        // The bytes immediately after the BeginString SOH do not start with
        // "9=". This is a false-positive "8=" hit: the two-byte sequence "8="
        // appeared inside a field value of a previous or current message.
        //
        // Note: a partial message whose TCP segment ends exactly after the
        // BeginString SOH is handled by the buffer-size check above, which
        // returns false (incomplete) before reaching here.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "FixParser: malformed FIX message (tag 9 not second field) at window offset {} -- "
                   "skipping {} bytes and resyncing",
                   start, body_length_tag_start - start);
        parse_cursor = start + 1;
        return true;
    }

    // Find the SOH that terminates the BodyLength field.
    const size_t body_length_end = window.find(field_delimiter, body_length_tag_start);
    if (body_length_end == std::string_view::npos) {
        return false; // incomplete
    }

    // Parse the BodyLength value.
    const std::string_view body_length_text =
        window.substr(body_length_tag_start + body_length_tag.size(), body_length_end - body_length_tag_start - body_length_tag.size());

    int body_length = 0;
    const auto [parse_end, parse_error] = std::from_chars(body_length_text.data(), body_length_text.data() + body_length_text.size(), body_length);

    if (parse_error != std::errc{} || body_length <= 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "FixParser: malformed FIX message (non-positive or unparseable BodyLength) "
                   "at window offset {} -- skipping and resyncing",
                   start);
        parse_cursor = start + 1;
        return true;
    }

    // BodyLength counts from the byte immediately after the tag 9 SOH to the
    // byte immediately before the tag 10 SOH (inclusive). So the tag 10 field
    // starts at body_length_end + 1 + body_length.
    constexpr std::string_view checksum_tag = "10=";
    const size_t tag10_start = body_length_end + 1 + static_cast<size_t>(body_length);

    if (window.size() < tag10_start + checksum_tag.size()) {
        return false; // incomplete -- tag 10 not yet received
    }

    if (window.compare(tag10_start, checksum_tag.size(), checksum_tag) != 0) {
        // BodyLength pointed somewhere wrong -- the data is corrupt or from a
        // truncated message (e.g. TCP retransmit from the wrong offset).
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "FixParser: BodyLength ({}) points to wrong location "
                   "(expected '10=' at window offset {}, found '{}') -- "
                   "discarding {} bytes and resyncing",
                   body_length, tag10_start, window.substr(tag10_start, std::min(static_cast<size_t>(4), window.size() - tag10_start)),
                   body_length_tag_start - start);
        parse_cursor = start + 1;
        return true;
    }

    // Find the SOH that terminates the Checksum field (tag 10).
    const size_t checksum_end = window.find(field_delimiter, tag10_start);
    if (checksum_end == std::string_view::npos) {
        return false; // incomplete -- checksum field not yet fully received
    }

    // We now have a complete raw message: window[start .. checksum_end] inclusive.
    const std::string_view message_bytes = window.substr(start, tag10_start - start);
    const std::string_view received_checksum = window.substr(tag10_start + checksum_tag.size(), checksum_end - tag10_start - checksum_tag.size());

    if (!validate_checksum(message_bytes, received_checksum)) {
        int sum = 0;
        for (const unsigned char byte : message_bytes) {
            sum += byte;
        }
        const std::string computed = format_checksum(sum % 256);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "FixParser: bad checksum (computed {} received {}) -- discarding message of {} bytes", computed,
                   received_checksum, checksum_end + 1 - start);
        parse_cursor = checksum_end + 1;
        return true;
    }

    // Parse field tag-value pairs. All string_views in msg point into the
    // MirroredBuffer window and remain valid for the duration of on_message_.
    const std::string_view raw_message = window.substr(start, checksum_end - start + 1);
    ParsedFixMessage msg;
    if (parse_fields(raw_message, msg)) {
        on_message_(msg);
    }

    parse_cursor = checksum_end + 1;
    return true;
}

bool FixParser::parse_fields(std::string_view raw_message, ParsedFixMessage& msg) {
    size_t position = 0;
    while (position < raw_message.size()) {
        // Find the '=' separating tag from value.
        const size_t equals_sign = raw_message.find('=', position);
        if (equals_sign == std::string_view::npos) {
            break;
        }

        // Find the field delimiter terminating the value.
        const size_t field_end = raw_message.find(field_delimiter, equals_sign + 1);
        if (field_end == std::string_view::npos) {
            break;
        }

        const std::string_view tag_text = raw_message.substr(position, equals_sign - position);
        const std::string_view value = raw_message.substr(equals_sign + 1, field_end - equals_sign - 1);

        // Parse tag as integer via from_chars -- no locale, no exception, no allocation.
        int tag = 0;
        const auto [end_ptr, error_code] = std::from_chars(tag_text.data(), tag_text.data() + tag_text.size(), tag);
        if (error_code == std::errc{}) {
            msg.set(tag, value);
        }
        // Malformed tag -- skip and continue parsing remaining fields.

        position = field_end + 1;
    }

    return msg.has(Tag::MsgType);
}

bool FixParser::validate_checksum(std::string_view message_bytes, std::string_view expected_checksum) {
    int sum = 0;
    for (const unsigned char byte : message_bytes) {
        sum += byte;
    }
    return format_checksum(sum % 256) == expected_checksum;
}

std::string FixParser::format_checksum(int sum) {
    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%03u", static_cast<unsigned int>(sum) % 256u);
    return {buffer};
}

} // namespace order_gateway
