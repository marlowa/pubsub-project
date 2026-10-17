#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

#include <leader_follower.hpp>

#include "EngineRecordFromWal.hpp"

namespace sequencer {

/// What engine_catch_up_from_wal did.
struct EngineCatchUpFromWal {
    /// How many orders and cancels were sent.
    int64_t records_sent{0};
    /// The first record that could not be read, after which nothing more was sent; 0 if there was none.
    int64_t damaged_seq_no{0};
};

/**
 * @brief Reads the WAL on the calling thread and sends a matching engine every order and cancel numbered
 *        after @p after_seq_no and up to @p through_seq_no, stopping at the first record that cannot be read.
 *
 * Used by the sequencer when it has no reading thread, which is when high availability is off; with one,
 * the reading thread does this instead (docs/availability/following_the_wal.md). A record that cannot be
 * read -- one whose contents do not parse, or a WalRecord envelope that does not decode -- may have been an
 * order, so nothing after it is sent, and its number is returned: the engine must then not be told it has
 * caught up, because what it holds would be incomplete without anything showing it.
 *
 * @param[in] directory      The directory of the WAL's segment files.
 * @param[in] start          Where to start reading: at or before the segment holding the record after @p after_seq_no.
 * @param[in] after_seq_no   The last record the engine holds.
 * @param[in] through_seq_no The last record the engine may be sent.
 * @param[in] arena          Scratch space for decoding.
 * @param[in] send           Sends the engine one order or cancel, with its record number.
 * @return How many were sent, and the first record that could not be read, if any.
 */
[[nodiscard]] inline EngineCatchUpFromWal engine_catch_up_from_wal(const std::string& directory, pubsub_itc_fw::WalPosition start, int64_t after_seq_no,
                                                                   int64_t through_seq_no, std::vector<uint8_t>& arena,
                                                                   const std::function<void(int64_t, const pubsub_itc_fw_app::WalRecord&)>& send) {
    EngineCatchUpFromWal result;
    [[maybe_unused]] const auto end = pubsub_itc_fw::WalReader::replay(
        directory, start, [&result, &arena, &send, after_seq_no, through_seq_no](int64_t record_id, const void* payload, size_t size) {
            // The reader cannot be stopped part way, so after a damaged record every later one is passed over.
            if (result.damaged_seq_no != 0 || record_id <= after_seq_no || record_id > through_seq_no) {
                return;
            }
            const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
            if (!contents.has_value()) {
                result.damaged_seq_no = record_id;
                return;
            }
            pubsub_itc_fw_app::WalRecord envelope{};
            const EngineRecordFromWal found = engine_record_from_wal(contents->pdu_id, contents->bytes, contents->size, arena, envelope);
            if (found == EngineRecordFromWal::NotDecodable) {
                result.damaged_seq_no = record_id;
                return;
            }
            if (found == EngineRecordFromWal::ForEngine) {
                send(record_id, envelope);
                ++result.records_sent;
            }
        });
    return result;
}

} // namespaces
