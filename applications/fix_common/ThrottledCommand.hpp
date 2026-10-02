#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace fix_common {

/**
 * @brief The kinds of member command a gateway limits the rate of.
 *
 * Each kind has its own limit and is counted separately: a new order counts only towards the limit
 * on new orders, and so on. See docs/venue/gateway_throttles.md.
 */
enum class ThrottledCommand {
    /// Placing a new order: FIX NewOrderSingle (35=D), or the binary NewOrderSingle.
    Place,
    /// Amending an open order: FIX OrderCancelReplaceRequest (35=G). The venue does not yet amend.
    Amend,
    /// Cancelling an open order: FIX OrderCancelRequest (35=F), or the binary OrderCancelRequest.
    Cancel
};

/**
 * @brief The kind's name as an operator reads it in a log line, for example "new order".
 * @param[in] command The kind of command.
 * @return A string with static storage.
 */
[[nodiscard]] inline const char* throttled_command_name(ThrottledCommand command) {
    switch (command) {
        case ThrottledCommand::Place:
            return "new order";
        case ThrottledCommand::Amend:
            return "amend";
        case ThrottledCommand::Cancel:
            return "cancel";
    }
    return "unknown command";
}

} // namespaces
