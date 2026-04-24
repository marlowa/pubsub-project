#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Magic value written into every PDU frame header.
 *
 * Chosen to be visually distinctive in a hex dump and unambiguous:
 * 0xC0FFEE00 identifies a PDU frame boundary, as distinct from the
 * allocator slot canary (0xDEADC0DEFEEDFACE). A value other than
 * this in the canary field indicates wire corruption or a framing error.
 *
 * This value is stored and compared in network byte order on the wire.
 */
static constexpr uint32_t pdu_canary_value = 0xC0FFEE00U;

/*
 * PduHeader is the 16-byte frame header prepended to every PDU on the wire.
 *
 * Layout (network byte order, 16 bytes total, no implicit padding):
 *
 *   Offset  Size  Field
 *   0       4     byte_count   -- payload size in bytes, excluding this header
 *   4       2     pdu_id       -- DSL message ID (signed)
 *   6       1     version      -- message version (signed)
 *   7       1     filler_a     -- reserved, must be zero on send
 *   8       4     canary       -- must equal pdu_canary_value
 *   12      4     filler_b     -- reserved, must be zero on send
 *
 * Endianness:
 *   All multi-byte fields in this header are in network byte order (big-endian)
 *   on the wire. This makes the protocol architecture-neutral: the sender and
 *   receiver may be any mix of big-endian and little-endian hosts.
 *
 *   The framing layer must apply conversions as follows:
 *     On send:    htonl() for uint32_t fields, htons() for int16_t fields.
 *     On receive: ntohl() for uint32_t fields, ntohs() for int16_t fields.
 *   Single-byte fields (version, filler_a) require no conversion.
 *
 *   Note: the DSL payload that follows this header uses little-endian encoding,
 *   as specified by the DSL binary format. The header and payload endianness are
 *   intentionally different: the header is architecture-neutral, the payload
 *   encoding is an application-level concern documented in the DSL specification.
 *
 * The static_assert below guarantees the layout at compile time.
 */

/**
 * @brief Frame header prepended to every PDU transmitted over TCP.
 *
 * All multi-byte fields are in network byte order on the wire.
 * The framing layer is responsible for applying htonl/htons on send
 * and ntohl/ntohs on receive. See the endianness note above.
 *
 * Receivers must validate the canary field after conversion to host byte order.
 * A mismatched canary indicates wire corruption or a framing error and the
 * connection must be closed.
 */
struct PduHeader {
    uint32_t byte_count; ///< Payload size in bytes, excluding this header. Network byte order.
    int16_t pdu_id;      ///< DSL message ID, as defined in the .dsl file. Network byte order.
    int8_t version;      ///< Message version. No conversion needed.
    uint8_t filler_a;    ///< Reserved. Set to zero on send. No conversion needed.
    uint32_t canary;     ///< Must equal pdu_canary_value after ntohl(). Network byte order.
    uint32_t filler_b;   ///< Reserved. Set to zero on send. Network byte order.
};

static_assert(sizeof(PduHeader) == 16, "PduHeader must be exactly 16 bytes. "
                                       "Check for unexpected compiler padding.");

static_assert(alignof(PduHeader) == 4, "PduHeader must have 4-byte alignment.");

} // namespace pubsub_itc_fw
