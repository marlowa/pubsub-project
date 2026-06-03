#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <functional>
#include <string_view>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "FixMessage.hpp"

namespace order_gateway {

/**
 * @brief Stateless FIX 5.0SP2 / FIXT 1.1 stream parser.
 *
 * FixParser is given a contiguous window of bytes on each call to feed() and
 * extracts every complete FIX message it finds. Partial messages at the end of
 * the window are not consumed: feed() returns the count of fully consumed bytes
 * so the caller can retain the partial bytes in the MirroredBuffer by
 * committing only that count.
 *
 * The parser has no internal accumulation buffer. Partial-message bytes are
 * preserved automatically because the MirroredBuffer's read pointer advances
 * only by the consumed count. On the next call to feed() the window begins at
 * those same partial bytes, now followed by newly arrived TCP data, and the
 * parser continues as if there had been no interruption. The virtual-memory
 * double-mapping of MirroredBuffer guarantees that the window is always
 * contiguous in virtual address space even if the ring wraps.
 *
 * ParsedFixMessage lifetime:
 *   The ParsedFixMessage passed to the callback holds string_views into the
 *   MirroredBuffer memory. Those views are valid only for the duration of the
 *   callback. The callback must not store any string_view (or the
 *   ParsedFixMessage itself) beyond its own scope. ParsedFixMessage is
 *   non-copyable and non-movable to enforce this statically.
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
 *   zero-padded three-digit string. Messages with an invalid checksum are
 *   discarded with a Warning log.
 */
class FixParser {
  public:
    using MessageCallback = std::function<void(const ParsedFixMessage&)>;

    /**
     * @brief Constructs a FixParser with the given logger and message callback.
     *
     * @param[in] logger     Logger instance. Must outlive this object.
     * @param[in] on_message Called once for each complete, valid FIX message
     *                       found in the window passed to feed().
     */
    FixParser(pubsub_itc_fw::QuillLogger& logger, MessageCallback on_message);

    /**
     * @brief Parses FIX messages from a contiguous byte window.
     *
     * The window is typically the readable region of a MirroredBuffer starting
     * at its current read pointer. It covers any partial message bytes left
     * over from the previous call (still in the ring because they were not
     * committed) followed by newly arrived TCP data.
     *
     * Returns the number of bytes fully consumed, i.e. the total length of all
     * complete FIX messages found and dispatched. The caller must advance the
     * MirroredBuffer read pointer by exactly this count. Bytes beyond the
     * returned count belong to an incomplete message and must stay in the ring.
     *
     * @param[in] data      Start of the readable window. Must not be nullptr.
     * @param[in] available Total bytes in the window.
     * @return              Number of bytes consumed (0 if no complete message found).
     */
    [[nodiscard]] size_t feed(const uint8_t* data, size_t available);

    /**
     * @brief No-op: the parser holds no internal state between calls.
     *
     * Provided for API symmetry with connection lifecycle callers that may
     * want to signal a stream reset. Partial bytes left in the MirroredBuffer
     * are discarded by the caller advancing the ring fully on disconnect,
     * independently of this function.
     */
    void reset();

  private:
    /*
     * Attempts to extract one complete FIX message from window starting at
     * parse_cursor. If found: invokes on_message_, advances parse_cursor past
     * the message, returns true. If the window contains no complete message:
     * returns false, parse_cursor is unchanged (points to start of partial data).
     */
    bool try_extract_message(std::string_view window, size_t& parse_cursor);

    /*
     * Parses all tag=value fields from raw_message (a string_view into the
     * MirroredBuffer window) into msg. Field values are string_views into the
     * same window -- zero copy. Returns true if tag 35 (MsgType) is present.
     */
    static bool parse_fields(std::string_view raw_message, ParsedFixMessage& msg);

    /*
     * Returns true if the FIX checksum computed over message_bytes (bytes from
     * tag 8 up to but not including the "10=" field) matches expected_checksum.
     */
    static bool validate_checksum(std::string_view message_bytes, std::string_view expected_checksum);

    /*
     * Formats a three-digit zero-padded checksum string from a byte sum.
     */
    static std::string format_checksum(int sum);

    MessageCallback on_message_;
    pubsub_itc_fw::QuillLogger& logger_;
};

} // namespace order_gateway
