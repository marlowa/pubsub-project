#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "VoidedCommandList.hpp"

namespace sequencer {

/// What void_from_command or void_from_wal found.
enum class VoidFromWal : int8_t {
    Void,        ///< A VoidCommands command: the void has been filled in.
    NotAVoid,    ///< Some other record or command.
    NotDecodable ///< It did not decode: kept out of the list, and worth an Error.
};

/**
 * @brief The void a command of the sequencer's holds, if it is a VoidCommands command.
 *
 * The sequencer keeps the voids in its WAL from the commands themselves, as it writes them as leader, as it writes its
 * leader's records as follower, and as the replicated WAL reads the whole WAL when it opens
 * (docs/availability/voided_commands.md, section 3.3). All three find the void with this, or with void_from_wal, which
 * calls it, so that a list rebuilt from the WAL is the list kept while the records were written.
 *
 * @param[in]  seq_no       The position of the record holding the command.
 * @param[in]  inner_pdu_id The command's message type, as its envelope carries it.
 * @param[in]  bytes        The command's bytes.
 * @param[in]  size         How many bytes.
 * @param[in]  arena        Scratch space for decoding.
 * @param[out] voided       The void, when Void is returned.
 * @return What the command was found to be.
 */
[[nodiscard]] inline VoidFromWal void_from_command(int64_t seq_no, int16_t inner_pdu_id, const uint8_t* bytes, size_t size, std::vector<uint8_t>& arena,
                                                   fix_common::VoidedRange& voided) {
    if (inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id)) {
        return VoidFromWal::NotAVoid;
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::VoidCommandsView command{};
    if (!pubsub_itc_fw_app::decode(command, bytes, size, bytes_consumed, allocator, arena_bytes_needed)) {
        return VoidFromWal::NotDecodable;
    }
    voided = fix_common::VoidedRange{seq_no, command.first_seq_no, command.last_seq_no, command.new_orders_only, command.reason};
    return VoidFromWal::Void;
}

/**
 * @brief The void a record read from the WAL holds, if its command is a VoidCommands command.
 *
 * @param[in]  seq_no The record's position.
 * @param[in]  entry  The record as stored in the WAL: a WalRecord envelope.
 * @param[in]  arena  Scratch space for decoding.
 * @param[out] voided The void, when Void is returned.
 * @return What the record was found to be. A record that is not an envelope is not a void.
 */
[[nodiscard]] inline VoidFromWal void_from_wal(int64_t seq_no, const pubsub_itc_fw::WalEntryContents& entry, std::vector<uint8_t>& arena,
                                               fix_common::VoidedRange& voided) {
    if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return VoidFromWal::NotAVoid;
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, entry.bytes, entry.size, bytes_consumed, allocator, arena_bytes_needed)) {
        return VoidFromWal::NotDecodable;
    }
    if (envelope.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id)) {
        return VoidFromWal::NotAVoid;
    }
    // The command's bytes point into the record, not into the arena, so the arena may be used again to decode them.
    return void_from_command(seq_no, envelope.pdu_id, envelope.payload.data, envelope.payload.size, arena, voided);
}

/// What command_in_record found a record to hold.
enum class CommandInRecord : int8_t {
    MembersCommand, ///< A member's new order or cancel: one a void may name.
    OtherRecord,    ///< Anything else, a command the sequencer wrote itself or a report for instance: never voided.
    NotDecodable    ///< It did not decode.
};

/**
 * @brief Whether a record read from the WAL holds a member's new order or cancel, which are the only commands a void may
 *        name (docs/availability/voided_commands.md, section 4.3).
 *
 * @param[in] entry The record as stored in the WAL: a WalRecord envelope.
 * @param[in] arena Scratch space for decoding.
 * @return What the record holds. A record that is not an envelope holds no member's command.
 */
[[nodiscard]] inline CommandInRecord command_in_record(const pubsub_itc_fw::WalEntryContents& entry, std::vector<uint8_t>& arena) {
    if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return CommandInRecord::OtherRecord;
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, entry.bytes, entry.size, bytes_consumed, allocator, arena_bytes_needed)) {
        return CommandInRecord::NotDecodable;
    }
    const bool members = envelope.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) ||
                         envelope.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
    return members ? CommandInRecord::MembersCommand : CommandInRecord::OtherRecord;
}

} // namespaces
