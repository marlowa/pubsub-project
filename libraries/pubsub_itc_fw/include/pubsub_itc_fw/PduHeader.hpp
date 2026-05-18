#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <cstdint>
#include <endian.h>

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
 * PduHeader is the 24-byte frame header prepended to every PDU on the wire.
 *
 * Layout (network byte order, 24 bytes total, no implicit padding):
 *
 *   Offset  Size  Field
 *   0       4     byte_count   -- payload size in bytes, excluding this header
 *   4       2     pdu_id       -- DSL message ID (signed)
 *   6       1     version      -- message version (signed)
 *   7       1     filler_a     -- reserved, must be zero on send
 *   8       8     seq_no       -- sequencer-assigned sequence number (signed),
 *                                 0 on PDUs not yet stamped by the sequencer
 *   16      4     canary       -- must equal pdu_canary_value
 *   20      4     filler_b     -- reserved, must be zero on send
 *
 * Endianness:
 *   All multi-byte fields in this header are in network byte order (big-endian)
 *   on the wire. This makes the protocol architecture-neutral: the sender and
 *   receiver may be any mix of big-endian and little-endian hosts.
 *
 *   The framing layer must apply conversions as follows:
 *     On send:    htonl() for uint32_t, htons() for int16_t, htobe64() for int64_t.
 *     On receive: ntohl() for uint32_t, ntohs() for int16_t, be64toh() for int64_t.
 *   Single-byte fields (version, filler_a) require no conversion.
 *
 *   htobe64()/be64toh() come from <endian.h> and are glibc-standard on Linux.
 *   They are the 64-bit equivalent of htonl()/ntohl().
 *
 *   Note: the DSL payload that follows this header uses little-endian encoding,
 *   as specified by the DSL binary format. The header and payload endianness are
 *   intentionally different: the header is architecture-neutral, the payload
 *   encoding is an application-level concern documented in the DSL specification.
 *
 * Sequence numbers:
 *   The seq_no field carries the sequencer-assigned monotonic sequence number
 *   for ordered messages. Senders that have not yet been stamped by the
 *   sequencer (e.g. a gateway emitting an inbound order PDU to the sequencer)
 *   write 0. The sequencer stamps a non-zero value (starting at 1) on every
 *   PDU it forwards to downstream consumers.
 *
 * The static_assert below guarantees the layout at compile time.
 */

/**
 * @brief Frame header prepended to every PDU transmitted over TCP.
 *
 * All multi-byte fields are in network byte order on the wire.
 * The framing layer is responsible for applying htonl/htons/htobe64 on send
 * and ntohl/ntohs/be64toh on receive. See the endianness note above.
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
    int64_t seq_no;      ///< Sequencer-assigned sequence number; 0 if not yet stamped. Network byte order.
    uint32_t canary;     ///< Must equal pdu_canary_value after ntohl(). Network byte order.
    uint32_t filler_b;   ///< Reserved. Set to zero on send. Network byte order.
};

static_assert(sizeof(PduHeader) == 24, "PduHeader must be exactly 24 bytes. "
                                       "Check for unexpected compiler padding.");

static_assert(alignof(PduHeader) == 8, "PduHeader must have 8-byte alignment.");

} // namespace pubsub_itc_fw
