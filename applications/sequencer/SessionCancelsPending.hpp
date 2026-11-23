#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "SessionIdentity.hpp"

namespace sequencer {

/// How a member's session ended: whether the member logged out cleanly, saying it was going, or its connection dropped or
/// its gateway was lost.
class LoggedOutFlag {
  public:
    enum LoggedOutFlagTag {
        ConnectionLost = 0, ///< the connection dropped, or the gateway holding it was lost: the grace period applies
        LoggedOut = 1       ///< the member logged out cleanly: no grace period
    };

    explicit LoggedOutFlag(LoggedOutFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_set() const {
        return value_ == LoggedOut;
    }

  private:
    LoggedOutFlagTag value_;
};

/**
 * @brief The cancels on disconnect the venue is waiting to carry out: sessions that have ended, whose member asked for its
 *        resting orders to be cancelled when its connection ends, and whose grace period has not yet run out
 *        (docs/availability/cancel_on_disconnect_where_the_orders_are.md).
 *
 * Kept from the WAL by every instance, leading or not, from three kinds of record: SessionEnded starts a wait,
 * SessionEndWithdrawn ends it because the session bound again in time, and CancelSessionOrders ends it because the
 * cancel was written. So a sequencer that takes the lead finds every cancel still pending in its own WAL, and goes on
 * waiting from the time recorded rather than from when it took the lead. Only the leader acts on what is due, by writing
 * CancelSessionOrders.
 *
 * Times are wall-clock nanoseconds since the Unix epoch, as the WAL records them. A wait is due once the time of the
 * SessionEnded record plus its grace period has been reached.
 *
 * One thread, the sequencer's, calls every member function.
 */
class SessionCancelsPending {
  public:
    /**
     * @brief The session's end was recorded at @p seq_no, at @p ended_at_ns, with @p grace_period_seconds to wait.
     *
     * A session already waiting keeps the wait it has: the earlier end is the one the member's grace period runs from.
     */
    void ended(const fix_common::SessionIdentity& identity, int64_t seq_no, int64_t ended_at_ns, int32_t grace_period_seconds);

    /// The session bound again before its grace period ran out; its orders are not to be cancelled.
    void withdrawn(const fix_common::SessionIdentity& identity);

    /**
     * @brief The cancel of the session's orders was written, at @p seq_no; nothing is waiting for it any longer.
     *
     * From then on the session counts as ended until it binds again (bound_again): the member logged out, or its
     * grace period ran out, and no gateway serves it.
     */
    void carried_out(const fix_common::SessionIdentity& identity, int64_t seq_no);

    /// The session bound again, so it no longer counts as ended.
    void bound_again(const fix_common::SessionIdentity& identity);

    /**
     * @brief Whether the session has ended: its orders were cancelled because its member logged out or its grace period
     *        ran out, and it has not bound since.
     *
     * A report for an ended session is not held while it is connected nowhere: no gateway will announce it, so the
     * report goes to the WAL, from which a member that logs on again is sent it (docs/bug_list.md, BUG-0258). A
     * session whose member did not ask for its orders to be cancelled has no cancel written, and never counts as
     * ended here.
     */
    [[nodiscard]] bool has_ended(const fix_common::SessionIdentity& identity) const;

    /// Whether the session's orders are waiting to be cancelled.
    [[nodiscard]] bool pending(const fix_common::SessionIdentity& identity) const;

    /// Calls @p each with the identity of every session whose wait is due at @p now_ns.
    template <typename Each> void for_each_due(int64_t now_ns, Each&& each) const {
        for (const auto& [identity, wait] : waiting_) {
            if (wait.due_ns <= now_ns) {
                each(identity);
            }
        }
    }

    /**
     * @brief Records after @p seq_no have been discarded from the WAL: a wait that one of them started is forgotten, and
     *        so is a session's end that one of them recorded.
     *
     * A wait that one of them ended, by a SessionEndWithdrawn or a CancelSessionOrders that has been discarded, is not
     * started again, because what started it is no longer known here. The leader whose records are kept writes its own
     * records for the session, and this instance follows them.
     */
    void discard_after(int64_t seq_no);

    /// How many sessions are waiting.
    [[nodiscard]] size_t size() const {
        return waiting_.size();
    }

  private:
    struct Wait {
        int64_t seq_no{0};
        int64_t due_ns{0};
    };

    std::unordered_map<fix_common::SessionIdentity, Wait, fix_common::SessionIdentityHash> waiting_;
    // Each ended session, with the position of the cancel that ended it.
    std::unordered_map<fix_common::SessionIdentity, int64_t, fix_common::SessionIdentityHash> ended_;
};

} // namespaces
