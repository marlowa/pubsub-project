#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>

#include <pubsub_itc_fw/BumpAllocator.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief The command a CommandRefused message carries: a member's command the leading sequencer refused without
 *        writing it into the WAL.
 *
 * Both gateways answer such a command with a rejection built from the command's own fields: an execution report for a
 * new order, an order cancel reject for a cancel (docs/availability/one_check_against_sequencing_twice.md, section
 * 4.2). The message carries the command exactly as the gateway sent it, so that a gateway can answer whether or not it
 * kept a copy of the command.
 *
 * The views point into the bytes of the message and into the arena given to decode_refused_command, and are valid only
 * while both are.
 */
struct RefusedCommand {
    enum class Kind { NewOrder, Cancel };
    Kind kind{Kind::NewOrder};
    pubsub_itc_fw_app::NewOrderSingleView order{};      ///< the refused new order, when kind is NewOrder
    pubsub_itc_fw_app::OrderCancelRequestView cancel{}; ///< the refused request to cancel, when kind is Cancel
};

/**
 * @brief Decodes the command a CommandRefused message carries.
 * @param[in] refusal The decoded CommandRefused message.
 * @param[in,out] arena Where the command's fields of variable length are decoded to.
 * @return The command; or no command and why it could not be decoded, in words for the Quill log.
 */
[[nodiscard]] inline std::tuple<std::optional<RefusedCommand>, std::string_view> decode_refused_command(const pubsub_itc_fw_app::CommandRefusedView& refusal,
                                                                                                        pubsub_itc_fw::BumpAllocator& arena) {
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    RefusedCommand command{};
    if (refusal.command_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        command.kind = RefusedCommand::Kind::NewOrder;
        if (!pubsub_itc_fw_app::decode(command.order, refusal.command.data, refusal.command.size, bytes_consumed, arena, arena_bytes_needed)) {
            return {std::nullopt, "the NewOrderSingle it carries cannot be decoded"};
        }
        return {command, std::string_view{}};
    }
    if (refusal.command_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        command.kind = RefusedCommand::Kind::Cancel;
        if (!pubsub_itc_fw_app::decode(command.cancel, refusal.command.data, refusal.command.size, bytes_consumed, arena, arena_bytes_needed)) {
            return {std::nullopt, "the OrderCancelRequest it carries cannot be decoded"};
        }
        return {command, std::string_view{}};
    }
    return {std::nullopt, "it carries a command that is neither a NewOrderSingle nor an OrderCancelRequest"};
}

} // namespaces
