#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef> // IWYU pragma: keep
#include <string_view>

#include <fix_orders.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace fix_order_gateway {

// Starting size for the ExecutionReport wire buffer -- NOT a hard cap. The flat fields
// are ~290 bytes; the echoed NoUnderlyings/NoPartyIDs groups add a variable amount, so
// the caller grows a reusable buffer and retries if encode_execution_report reports an
// overflow (it returns an empty view when output_buffer is too small). This value just
// avoids a resize for the common case.
static constexpr size_t execution_report_initial_buffer_size = 512;

// Sanity ceiling for the grow-and-retry loop: an ExecutionReport larger than this is a
// bug, not a real message, so the caller gives up and logs rather than growing forever.
static constexpr size_t max_execution_report_buffer_size = 64 * 1024;

/**
 * Encodes an ExecutionReport into output_buffer as FIX 5.0SP2 / FIXT 1.1 wire
 * bytes using fix_codec::FixMessageWriter -- framing (BeginString, BodyLength,
 * Checksum) and tag numbers come from the generated FIX dictionary. No heap
 * allocation; enum fields are cast to their single-char wire values.
 *
 * The returned view is the complete wire message. It does NOT start at
 * output_buffer[0]: FixMessageWriter writes the header backward into a reserved
 * prefix, so the caller must send view.data()/view.size(), not the buffer base.
 *
 * @param[in]  view            Decoded execution report fields (string_views into arena).
 * @param[in]  sender_comp_id  SenderCompID for the outbound FIX header.
 * @param[in]  target_comp_id  TargetCompID for the outbound FIX header.
 * @param[in]  seq_num         Outbound sequence number.
 * @param[in]  wall_clock      Clock used to generate SendingTime (tag 52).
 * @param[out] output_buffer        Caller-supplied output buffer.
 * @param[in]  output_buffer_size   Size of output_buffer in bytes.
 * @return A view of the wire bytes within output_buffer; empty on overflow.
 */
/**
 * @param[in] poss_dup              True for a report being RESENT to a member that asked for
 *                                  messages it missed. Adds PossDupFlag=Y, which is what
 *                                  tells the member this may be a second copy rather than a
 *                                  new event -- without it, a replayed fill reads as a fresh
 *                                  one and the member's position is wrong.
 * @param[in] orig_sending_time_ns  When the venue originally sent it, wall-clock nanoseconds,
 *                                  or 0 when not resending. FIX requires OrigSendingTime
 *                                  alongside PossDupFlag: SendingTime is when this copy went
 *                                  out, and only OrigSendingTime says when the event happened.
 */
[[nodiscard]] std::string_view encode_execution_report(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id,
                                                       std::string_view target_comp_id, int seq_num, const pubsub_itc_fw::WallClock& wall_clock,
                                                       char* output_buffer, size_t output_buffer_size, bool poss_dup = false, int64_t orig_sending_time_ns = 0);

} // namespaces
