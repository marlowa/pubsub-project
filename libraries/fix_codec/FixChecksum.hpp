#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace fix_codec {

/**
 * @page fix_checksum_always_validated Checksum validation is not optional
 *
 * **Every inbound message has its checksum verified, and no configuration turns
 * that off.** This has been proposed and rejected, and the reasoning is recorded
 * here because the proposal is a reasonable-sounding one that will recur: a
 * profile shows checksum validation costing around 1% of a gateway thread,
 * localhost is not a hostile network, so make it a perf-mode flag.
 *
 * That mistakes what the check is for. Read as a defence against a hostile
 * sender it does look redundant on a trusted link. Its main work here is
 * catching **framing** errors -- proof that BodyLength was right and that the
 * message ended where the parser believed it did. That is the failure mode this
 * gateway is most likely to produce, because it reuses buffers, grows them on
 * demand and frames headers backward into a reserved prefix. The fault it
 * catches is far more likely to be ours than the sender's, and it is the only
 * end-to-end integrity check on the inbound path.
 *
 * Two further objections to the flag as proposed:
 *
 * - **A trusted local link describes the test rig, not a deployment.** Members
 *   connect from elsewhere; everything arriving over loopback is an artefact of
 *   running the whole venue on one machine. A setting that is safe in one
 *   configuration and unsafe in the other will eventually run in the wrong one,
 *   and the cost of that is silently accepting a malformed order.
 * - **It would make a benchmark measure something other than the venue.**
 *   Measuring with validation off and running with it on produces a number that
 *   is not about the deployed system.
 *
 * The cost is about 1% of one thread and allocation-free, which is a poor price
 * for giving up the detector for corrupt framing.
 */

/**
 * @brief Computes the FIX checksum of a byte range.
 *
 * The FIX checksum is the sum of every byte value modulo 256. The range passed
 * in must cover every byte from the start of the message (tag 8) up to and
 * including the SOH that terminates the last body field, i.e. everything before
 * the "10=" checksum field itself.
 *
 * This performs no allocation.
 */
[[nodiscard]] inline unsigned int compute_checksum(std::string_view message_bytes) {
    unsigned int sum = 0;
    for (const unsigned char byte : message_bytes) {
        sum += byte;
    }
    return sum % 256U;
}

/**
 * @brief Validates a received FIX checksum field against a computed checksum.
 *
 * @param[in] message_bytes     Bytes from tag 8 up to (not including) the "10=" field.
 * @param[in] received_checksum The value of the checksum field (tag 10), which must be
 *                              exactly three ASCII digits.
 * @return True if the received checksum is well formed and equals the computed value.
 *
 * Unlike a formatting-based comparison, this parses the three received digits into
 * an integer and compares numerically, so it performs no allocation on any path.
 */
[[nodiscard]] inline bool checksum_matches(std::string_view message_bytes, std::string_view received_checksum) {
    if (received_checksum.size() != 3) {
        return false;
    }
    unsigned int received = 0;
    const char* const first = received_checksum.data();
    const char* const last = received_checksum.data() + received_checksum.size();
    const std::from_chars_result result = std::from_chars(first, last, received);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    return compute_checksum(message_bytes) == received;
}

} // namespaces
