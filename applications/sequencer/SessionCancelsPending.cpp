// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SessionCancelsPending.hpp"

namespace sequencer {

namespace {
constexpr int64_t nanoseconds_per_second = 1'000'000'000;
} // namespaces

void SessionCancelsPending::ended(const fix_common::SessionIdentity& identity, int64_t seq_no, int64_t ended_at_ns, int32_t grace_period_seconds) {
    if (waiting_.count(identity) != 0) {
        return;
    }
    const int64_t grace_ns = static_cast<int64_t>(grace_period_seconds > 0 ? grace_period_seconds : 0) * nanoseconds_per_second;
    waiting_[identity] = Wait{seq_no, ended_at_ns + grace_ns};
}

void SessionCancelsPending::withdrawn(const fix_common::SessionIdentity& identity) {
    waiting_.erase(identity);
}

void SessionCancelsPending::carried_out(const fix_common::SessionIdentity& identity) {
    waiting_.erase(identity);
}

bool SessionCancelsPending::pending(const fix_common::SessionIdentity& identity) const {
    return waiting_.count(identity) != 0;
}

void SessionCancelsPending::discard_after(int64_t seq_no) {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (it->second.seq_no > seq_no) {
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespaces
