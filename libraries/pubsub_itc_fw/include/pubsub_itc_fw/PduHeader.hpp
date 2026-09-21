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
 * PduHeader is the 32-byte frame header prepended to every PDU on the wire.
 *
 * Layout (network byte order, 32 bytes total, no implicit padding):
 *
 *   Offset  Size  Field
 *   0       4     byte_count   -- payload size in bytes, excluding this header
 *   4       2     pdu_id       -- DSL message ID (signed)
 *   6       1     version      -- message version (signed)
 *   7       1     alignment_a  -- makes the padding before seq_no explicit; zero on send
 *   8       8     seq_no       -- sequencer-assigned sequence number (signed),
 *                                 0 on PDUs not yet stamped by the sequencer
 *   16      8     sent_at_ns   -- when the sending reactor wrote these bytes (signed),
 *                                 0 when not stamped
 *   24      4     canary       -- must equal pdu_canary_value
 *   28      4     alignment_b  -- makes the trailing padding explicit; zero on send
 *
 * Why there is padding at all, and why it is written down:
 *   The fields carry 27 bytes of information and the structure contains 64-bit members, so it
 *   is 8-byte aligned and 32 bytes long whatever order the fields are in. Five of those bytes
 *   are therefore slack. They are declared rather than left to the compiler because a frame is
 *   written to a socket by copying this structure: implicit padding is padding whose position
 *   and contents are the compiler's business, and two compilers that disagree about it produce
 *   two different wire formats from the same source. Declaring it removes the question.
 *
 *   The slack sits at the end, apart from the single byte that has to sit between version and
 *   seq_no. Fields worth reading come first.
 *
 * Endianness:
 *   All multi-byte fields in this header are in network byte order (big-endian)
 *   on the wire. This makes the protocol architecture-neutral: the sender and
 *   receiver may be any mix of big-endian and little-endian hosts.
 *
 *   The framing layer must apply conversions as follows:
 *     On send:    htonl() for uint32_t, htons() for int16_t, htobe64() for int64_t.
 *     On receive: ntohl() for uint32_t, ntohs() for int16_t, be64toh() for int64_t.
 *   Single-byte fields (version, alignment_a) require no conversion.
 *
 * Send time:
 *   sent_at_ns carries CLOCK_MONOTONIC nanoseconds, read by the sending reactor at the
 *   moment it writes the frame, so that the receiver can measure the crossing itself
 *   rather than infer it by subtracting one median from another.
 *
 *   It is only meaningful between two processes on ONE host. CLOCK_MONOTONIC is
 *   system-wide on Linux, so two processes on the same machine read the same clock;
 *   two machines do not, and the difference between their readings is arbitrary. A
 *   receiver must therefore treat an implausible interval as no reading at all rather
 *   than as a measurement, and must never act on it. Nothing in the venue's behaviour
 *   depends on this field: it exists to be measured and for no other purpose.
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
    uint32_t byte_count;  ///< Payload size in bytes, excluding this header. Network byte order.
    int16_t pdu_id;       ///< DSL message ID, as defined in the .dsl file. Network byte order.
    int8_t version;       ///< Message version. No conversion needed.
    uint8_t alignment_a;  ///< The one byte that must sit between version and seq_no. Zero on send.
    int64_t seq_no;       ///< Sequencer-assigned sequence number; 0 if not yet stamped. Network byte order.
    int64_t sent_at_ns;   ///< CLOCK_MONOTONIC nanoseconds when the sending reactor wrote this
                          ///< frame; 0 when not stamped. Comparable only between processes on one
                          ///< host, and carried for measurement alone. Network byte order.
    uint32_t canary;      ///< Must equal pdu_canary_value after ntohl(). Network byte order.
    uint32_t alignment_b; ///< The trailing slack, declared so no compiler chooses it. Zero on send.
};

static_assert(sizeof(PduHeader) == 32, "PduHeader must be exactly 32 bytes. "
                                       "Check for unexpected compiler padding.");

static_assert(alignof(PduHeader) == 8, "PduHeader must have 8-byte alignment.");

} // namespaces
