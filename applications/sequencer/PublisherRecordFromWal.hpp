#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>

#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief Turns a record of the sequencer's WAL into the message a matching engine publisher is sent: the
 *        record's WalRecord envelope, byte for byte, as it lies in the WAL.
 *
 * Every record the sequencer writes is a WalRecord envelope, and a publisher writes every record into its
 * own WAL, so every record is sent: none is passed over. A record that is not an envelope, or whose
 * envelope does not decode, is damaged. Passing over it would leave a gap in the publisher's numbering,
 * which the publisher would take for records lost on the way, and ask for again, for ever; so the
 * publisher is not served past it, and the sequencer says why.
 *
 * Used on the reading thread (WalReadingThread) and on the sequencer's own thread (WalHandedOverFollowers),
 * each with a converter of its own, since each holds its own scratch space for decoding.
 *
 * @param[in]  entry The record's contents, as read from the WAL.
 * @param[in]  arena Scratch space for decoding the envelope, to check that it decodes.
 * @param[out] out   The bytes to send.
 */
[[nodiscard]] inline pubsub_itc_fw::ConvertedRecord publisher_record_from_wal(const pubsub_itc_fw::WalEntryContents& entry, std::vector<uint8_t>& arena,
                                                                              std::vector<uint8_t>& out) {
    if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return pubsub_itc_fw::ConvertedRecord::damaged();
    }
    pubsub_itc_fw::BumpAllocator allocator(arena.data(), arena.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, entry.bytes, entry.size, bytes_consumed, allocator, arena_bytes_needed)) {
        return pubsub_itc_fw::ConvertedRecord::damaged();
    }
    out.assign(entry.bytes, entry.bytes + entry.size);
    return pubsub_itc_fw::ConvertedRecord::send(pubsub_itc_fw_app::WalRecord::message_pdu_id);
}

/// A converter for WalReadingThread or WalHandedOverFollowers that uses publisher_record_from_wal, with scratch space of its own.
[[nodiscard]] inline pubsub_itc_fw::WalRecordConverter make_publisher_converter(size_t arena_bytes) {
    auto arena = std::make_shared<std::vector<uint8_t>>(arena_bytes);
    return [arena](int64_t /*seq_no*/, const pubsub_itc_fw::WalEntryContents& entry, std::vector<uint8_t>& out) {
        return publisher_record_from_wal(entry, *arena, out);
    };
}

} // namespaces
