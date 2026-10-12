#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace fix_common {

/**
 * @brief Whether a pair's leader says, on its lease requests, whether its peer may lead.
 *
 * Only the sequencer pair needs this. Its leader sometimes has the matching engine act on a command its
 * peer does not hold, and must then keep that peer from leading (LeaderStatement, and
 * docs/availability/a_follower_behind_does_not_lead.md). The matching engine pair and the publisher pair
 * take their state from the sequencer's log, so a behind instance catches up from there before it acts.
 * The arbiters hold no log. For them, the peer may always lead.
 */
class PeerStatementsFlag {
  public:
    enum PeerStatementsFlagTag { PeerAlwaysMayLead = 0, SayWhetherPeerMayLead = 1 };

    explicit PeerStatementsFlag(PeerStatementsFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const PeerStatementsFlag& rhs) const {
        return value_ == rhs.value_;
    }

    [[nodiscard]] bool is_equal(const PeerStatementsFlagTag& rhs) const {
        return value_ == rhs;
    }

    [[nodiscard]] PeerStatementsFlagTag value() const {
        return value_;
    }

  private:
    PeerStatementsFlagTag value_;
};

inline bool operator==(const PeerStatementsFlag& lhs, const PeerStatementsFlag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const PeerStatementsFlag& lhs, const PeerStatementsFlag::PeerStatementsFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const PeerStatementsFlag::PeerStatementsFlagTag& lhs, const PeerStatementsFlag& rhs) {
    return rhs.is_equal(lhs);
}

} // namespaces
