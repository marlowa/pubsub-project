#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>

#include "GatewayIds.hpp"

namespace sequencer {

/**
 * @brief Writes the identity of a command as the replicated WAL compares it: the member's comp id, the
 *        protocol the command arrived by, and its `ClOrdID` (docs/availability/one_check_against_sequencing_twice.md).
 *
 * The comp id is written with its length first, then the protocol as two bytes, then the `ClOrdID`, which
 * takes the rest. So no two different commands are written as the same bytes, whatever characters their
 * comp ids and `ClOrdID`s hold.
 *
 * @return The number of bytes written to @p out, which has room for @p capacity, or zero when the command does
 *         not say whose it is, or the identity does not fit.
 */
[[nodiscard]] inline size_t write_command_identity(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id, char* out, size_t capacity) {
    if (comp_id.empty() || cl_ord_id.empty() || comp_id.size() > 0xFFFFU) {
        return 0;
    }
    const size_t size = 2 + comp_id.size() + 2 + cl_ord_id.size();
    if (size > capacity) {
        return 0;
    }
    char* next = out;
    const auto comp_id_size = static_cast<uint16_t>(comp_id.size());
    std::memcpy(next, &comp_id_size, 2);
    next += 2;
    std::memcpy(next, comp_id.data(), comp_id.size());
    next += comp_id.size();
    std::memcpy(next, &protocol, 2);
    next += 2;
    std::memcpy(next, cl_ord_id.data(), cl_ord_id.size());
    return size;
}

/**
 * @brief The identity of the command a decoded WAL record holds: a new order or a request to cancel, saying
 *        whose it is. Zero for any other record.
 */
[[nodiscard]] inline size_t write_command_identity(const pubsub_itc_fw_app::WalRecordView& view, char* out, size_t capacity) {
    if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        return 0;
    }
    return write_command_identity(view.has_sender_comp_id ? view.sender_comp_id : std::string_view{},
                                  view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent,
                                  view.has_cl_ord_id ? view.cl_ord_id : std::string_view{}, out, capacity);
}

/**
 * @brief How the sequencer's replicated WAL reads the identity of the command a record holds.
 *
 * Every member's command the sequencer writes is a `WalRecord` envelope carrying the comp id, the protocol and
 * the `ClOrdID`, so the identity is read from the envelope without decoding the command inside it. It uses
 * nothing but its arguments, so the WAL's own thread and the thread that builds its record of identifiers can
 * call it at the same time.
 */
class SequencerCommandIdentity : public pubsub_itc_fw::CommandIdentityInterface {
  public:
    [[nodiscard]] size_t identity_of(const pubsub_itc_fw::WalEntryContents& entry, uint8_t* scratch, char* identity) const override {
        if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id || entry.size == 0) {
            return 0;
        }
        pubsub_itc_fw::BumpAllocator arena(scratch, scratch_size);
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::WalRecordView view{};
        if (!pubsub_itc_fw_app::decode(view, entry.bytes, entry.size, bytes_consumed, arena, arena_bytes_needed)) {
            return 0;
        }
        return write_command_identity(view, identity, max_identity_size);
    }
};

} // namespaces
