#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace fix_common {

/**
 * @brief The three rate limits a comp id is provisioned with, as they reach a gateway at logon.
 *
 * Each is the largest number of commands of that kind a session may send in any one second. Zero
 * means no limit. The values come from the comp_id table, through the credentials export and the
 * authentication service, on AuthenticationResult; a gateway has no default of its own.
 */
struct ThrottleLimits {
    /// The largest limit that may be configured for any one kind of command. The venue takes tens
    /// of microseconds to process an order from start to end, so one command every 10 microseconds
    /// is the fastest any member could usefully send. The database, the admin service and the
    /// gateways all refuse a larger value.
    static constexpr int max_permitted_per_second = 100000;

    int max_place_per_second{0};
    int max_amend_per_second{0};
    int max_cancel_per_second{0};

    /// True if every limit is from 0 to max_permitted_per_second.
    [[nodiscard]] bool all_in_permitted_range() const {
        return is_permitted(max_place_per_second) && is_permitted(max_amend_per_second) && is_permitted(max_cancel_per_second);
    }

  private:
    [[nodiscard]] static bool is_permitted(int limit) {
        return limit >= 0 && limit <= max_permitted_per_second;
    }
};

} // namespaces
