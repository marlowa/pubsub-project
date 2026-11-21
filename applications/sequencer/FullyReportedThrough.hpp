#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief What one WAL record says about how far the WAL holds every report: for a record of an execution report produced
 *        by command A, the command before A; for any other record, nothing.
 *
 * The matching engine applies commands one at a time, in the order of their numbers, and sends every report of a
 * command before it applies the next, on one connection to each sequencer, which writes them in the order they arrive.
 * So a committed record of a report produced by applying command A means the WAL holds every report of every command
 * before A. Command A itself may have more reports still to come. The command that produced a report is used rather
 * than the command it answers, because a report that answers no member's command, such as a cancel of the orders of a
 * session that has not come back, was still produced by one (docs/bug_list.md, BUG-0263). The highest such value over the committed records is the
 * last command the WAL holds every report of, which the leading sequencer tells a matching engine that is catching up,
 * so that it does not send those reports again (docs/availability/far_back_catch_up.md, section 2). A lower value is
 * always safe: the engine only sends more reports again, each marked as a possible repeat.
 *
 * Run on the reading thread for each record it reads from the newest segments (WalApplicationValueReader), with an
 * arena of its own.
 *
 * @param[in] pdu_id The message id of the record as stored in the WAL.
 * @param[in] bytes  The record's bytes.
 * @param[in] size   How many bytes.
 * @param[in] arena  Scratch space for decoding.
 * @return The command before the one that produced the report, or nothing if the record is not a report.
 */
[[nodiscard]] inline std::optional<int64_t> fully_reported_through(int16_t pdu_id, const uint8_t* bytes, size_t size, std::vector<uint8_t>& arena) {
    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return std::nullopt;
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, bytes, size, bytes_consumed, allocator, arena_bytes_needed)) {
        return std::nullopt;
    }
    if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) || view.produced_by_seq_no <= 0) {
        return std::nullopt;
    }
    return view.produced_by_seq_no - 1;
}

} // namespaces
