#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/**
 * @brief Whether a member is waiting for a message being sent.
 *
 * A component sends far more than the messages on an order's journey. A sequencer also sends
 * replication records, the external subscriber stream, write-ahead log acknowledgements,
 * heartbeats and arbitration reports, and nobody is sitting on the end of a client connection
 * waiting for any of those. Timing all of them together gives a figure that describes none of
 * them: measured that way, one component's sends appeared to account for more time than the
 * journey they were part of, which cannot be true, and that impossibility is how the mixture was
 * noticed.
 *
 * So a send says which it is. The flag changes nothing about what is sent or where it goes; it
 * decides only which of two histograms records how long the send waited for its reactor and how
 * long the reactor then took over it.
 *
 * A class rather than a bool because `send_pdu(conn, id, seq, msg, true)` tells a reader nothing,
 * and this is a decision a reader of the call site needs to be able to check.
 */
class MemberIsWaitingFlag {
  public:
    enum MemberIsWaitingFlagTag {
        NoMemberIsWaiting = 0, ///< Replication, subscriber streams, heartbeats, acknowledgements.
        MemberIsWaiting = 1    ///< An order on its way to be matched, or its report on the way back.
    };

    explicit MemberIsWaitingFlag(MemberIsWaitingFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const MemberIsWaitingFlag& rhs) const {
        return value_ == rhs.value_;
    }

    [[nodiscard]] bool is_equal(const MemberIsWaitingFlagTag& rhs) const {
        return value_ == rhs;
    }

    [[nodiscard]] MemberIsWaitingFlagTag value() const {
        return value_;
    }

    /** @brief True when a member is waiting, for the one place that has to branch on it. */
    [[nodiscard]] bool is_set() const {
        return value_ == MemberIsWaiting;
    }

  private:
    MemberIsWaitingFlagTag value_;
};

inline bool operator==(const MemberIsWaitingFlag& lhs, const MemberIsWaitingFlag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const MemberIsWaitingFlag& lhs, const MemberIsWaitingFlag::MemberIsWaitingFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const MemberIsWaitingFlag::MemberIsWaitingFlagTag& lhs, const MemberIsWaitingFlag& rhs) {
    return rhs.is_equal(lhs);
}

} // namespaces
