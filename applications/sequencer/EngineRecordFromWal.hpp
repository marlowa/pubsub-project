#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

namespace sequencer {

/// What engine_record_from_wal found a WAL record to be.
enum class EngineRecordFromWal : int8_t {
    ForEngine,    ///< An order or a cancel: the message for the engine has been built.
    NotForEngine, ///< Some other record, an execution report for instance: the engine is not sent it.
    NotDecodable  ///< A WalRecord envelope that did not decode: not sent, and worth a Warning.
};

/**
 * @brief Builds the message the matching engine is sent for one WAL record read back from the WAL: when
 *        it is caught up, and when it is sent orders it lacks after a change of leader or that waited in
 *        the WAL.
 *
 * Each record in the sequencer's WAL is a WalRecord envelope. The engine is sent only orders and cancels;
 * every other record, an execution report for instance, is an output rather than an input, and is not
 * sent. The envelope sent carries the order, its sequencing time, its trading date, and who placed it: the session's
 * connection, its comp id, its gateway protocol and the gateway instance. The engine files an order under
 * the comp id and the protocol together, so both must travel.
 *
 * Used on the sequencer's own thread and on the reading thread (WalReadingThread), each with an arena of
 * its own: the strings and bytes in @p envelope point into @p arena, so @p envelope is valid only while
 * @p arena is unchanged.
 *
 * @param[in]  pdu_id   The message id of the record as stored in the WAL.
 * @param[in]  bytes    The record's bytes.
 * @param[in]  size     How many bytes.
 * @param[in]  arena    Scratch space for decoding.
 * @param[out] envelope The message for the engine, when ForEngine is returned.
 * @return What the record was found to be.
 */
[[nodiscard]] inline EngineRecordFromWal engine_record_from_wal(int16_t pdu_id, const uint8_t* bytes, size_t size, std::vector<uint8_t>& arena,
                                                                pubsub_itc_fw_app::WalRecord& envelope) {
    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return EngineRecordFromWal::NotForEngine;
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, bytes, size, bytes_consumed, allocator, arena_bytes_needed)) {
        return EngineRecordFromWal::NotDecodable;
    }
    if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest) &&
        view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)) {
        return EngineRecordFromWal::NotForEngine;
    }
    envelope = pubsub_itc_fw_app::WalRecord{};
    envelope.seq_no = view.seq_no;
    envelope.pdu_id = view.pdu_id;
    envelope.payload = view.payload;
    envelope.wall_time_ns = view.wall_time_ns;
    envelope.has_gateway_session_conn_id = view.has_gateway_session_conn_id;
    envelope.gateway_session_conn_id = view.gateway_session_conn_id;
    envelope.has_sender_comp_id = view.has_sender_comp_id;
    envelope.sender_comp_id = view.sender_comp_id;
    envelope.has_origin_gateway_id = view.has_origin_gateway_id;
    envelope.origin_gateway_id = view.origin_gateway_id;
    envelope.has_gateway_instance_id = view.has_gateway_instance_id;
    envelope.gateway_instance_id = view.gateway_instance_id;
    // The engine makes its identifiers with keys derived from the trading date, so a command sent without it would be
    // refused, where the same command sent live is not.
    envelope.has_trading_date = view.has_trading_date;
    envelope.trading_date = view.trading_date;
    return EngineRecordFromWal::ForEngine;
}

} // namespaces
