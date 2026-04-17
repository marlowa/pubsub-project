#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FixMessage.hpp"

namespace sample_fix_gateway {

/**
 * @brief Minimal stateful FIX 5.0SP2 / FIXT 1.1 stream parser.
 *
 * FixParser sits between the raw byte stream delivered by RawBytesProtocolHandler
 * and the application logic in FixGatewayThread. It accumulates raw bytes,
 * extracts complete FIX messages, and invokes a callback for each one.
 *
 * FIX framing:
 *   Each FIX message begins with tag 8 (BeginString) and ends with tag 10
 *   (Checksum). The BodyLength field (tag 9) gives the number of bytes from
 *   the byte immediately after the tag 9 field delimiter to the byte
 *   immediately before the tag 10 field delimiter (inclusive). This parser
 *   uses BodyLength to locate the end of each message without scanning for
 *   the checksum tag.
 *
 * Field delimiter:
 *   FIX fields are separated by SOH (ASCII 0x01). Each field has the form:
 *   tag=value<SOH>
 *
 * Checksum validation:
 *   The checksum is the sum of all byte values from tag 8 up to and including
 *   the SOH after the last application field, modulo 256, formatted as a
 *   zero-padded three-digit string. This parser validates the checksum and
 *   discards messages with an invalid checksum.
 *
 * Statefulness:
 *   The parser maintains an internal accumulation buffer. Incomplete messages
 *   are retained across calls to feed(). This handles TCP fragmentation
 *   transparently.
 *
 * Usage:
 *   FixParser parser([](const FixMessage& msg) {
 *       // handle complete message
 *   });
 *   parser.feed(data, length);  // call from on_raw_socket_message()
 */
class FixParser {
public:
    using MessageCallback = std::function<void(const FixMessage&)>;

    /**
     * @brief Constructs a FixParser with the given message callback.
     *
     * @param[in] on_message Called once for each complete, valid FIX message
     *                       extracted from the byte stream.
     */
    explicit FixParser(MessageCallback on_message);

    /**
     * @brief Feeds raw bytes from the socket into the parser.
     *
     * May result in zero, one, or multiple calls to the message callback
     * depending on how many complete messages are present in the accumulated
     * buffer.
     *
     * @param[in] data   Pointer to the raw bytes. Must not be nullptr.
     * @param[in] length Number of bytes available at data.
     */
    void feed(const uint8_t* data, int length);

    /**
     * @brief Resets the parser state, discarding any partially accumulated data.
     *
     * Should be called when the connection is re-established after a disconnect.
     */
    void reset();

private:
    /*
     * Attempts to extract one complete FIX message from buffer_ starting at
     * offset_. Returns true and advances offset_ if a complete message was
     * found and dispatched. Returns false if more data is needed.
     */
    bool try_extract_message();

    /*
     * Parses all tag=value fields from the raw FIX bytes in buf into msg.
     * Returns true on success.
     */
    static bool parse_fields(const std::string& buf, FixMessage& msg);

    /*
     * Extracts the integer value of a specific tag from a raw FIX string.
     * Returns -1 if the tag is not found or the value is not a valid integer.
     */
    static int extract_int_tag(const std::string& buf, int tag);

    /*
     * Validates the FIX checksum. msg_bytes is the raw bytes of the message
     * up to but not including the tag 10 field. expected is the value in
     * the Checksum field.
     */
    static bool validate_checksum(const std::string& msg_bytes,
                                  const std::string& expected);

    /*
     * Formats a three-digit zero-padded checksum string from a byte sum.
     */
    static std::string format_checksum(int sum);

    MessageCallback on_message_;
    std::string     buffer_;
    std::size_t     offset_{0};
};

} // namespace sample_fix_gateway
