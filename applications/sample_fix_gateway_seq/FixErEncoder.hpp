#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string_view>

#include <fix_equity_orders.hpp>

namespace sample_fix_gateway_seq {

// Generous upper bound for a fully-populated ExecutionReport wire message.
// Actual size is ~290 bytes; 512 gives comfortable headroom on the stack.
static constexpr size_t execution_report_buffer_size = 512;

/**
 * Encodes an ExecutionReport directly into buf as FIX 5.0SP2 / FIXT 1.1
 * wire bytes. No heap allocation. All string_view fields from view are
 * written via memcpy; enum fields are cast to their single-char wire values.
 *
 * The field order matches FixSerialiser so existing FIX parsers see the
 * same message layout.
 *
 * Returns the number of bytes written. Returns 0 if buf_size is too small,
 * which should never happen when buf_size >= execution_report_buffer_size.
 *
 * @param[in]  view            Decoded execution report fields (string_views into arena).
 * @param[in]  sender_comp_id  SenderCompID for the outbound FIX header.
 * @param[in]  target_comp_id  TargetCompID for the outbound FIX header.
 * @param[in]  seq_num         Outbound sequence number.
 * @param[out] output_buffer        Caller-supplied output buffer.
 * @param[in]  output_buffer_size   Size of output_buffer in bytes.
 * @return Number of bytes written, or 0 on overflow.
 */
[[nodiscard]] size_t encode_execution_report(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id,
                                             std::string_view target_comp_id, int seq_num, char* output_buffer, size_t output_buffer_size);

} // namespace sample_fix_gateway_seq
