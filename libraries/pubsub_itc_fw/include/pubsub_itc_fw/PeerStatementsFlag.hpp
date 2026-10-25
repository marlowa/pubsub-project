#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/**
 * @brief Whether a pair's leader says, on its lease requests, whether its peer may lead, and what it says first.
 *
 * - SayWhetherPeerMayLead: the sequencer pair. Its leader sometimes has the matching engine act on a
 *   command its peer does not hold, and must then keep that peer from leading (LeaderStatement, and
 *   docs/availability/a_follower_behind_does_not_lead.md). Every leadership starts by saying the peer may
 *   not lead, because until the peer has acknowledged every record it may lack one.
 * - SayWhenPeerMayNotLead: the matching engine pair. A behind engine catches up from the sequencer's WAL
 *   before it acts, so its peer may lead unless the leader has a particular reason to say otherwise, such
 *   as the two engines being configured to apply commands differently
 *   (docs/availability/deterministic_matching_engine.md, section 5). Every leadership starts by saying the
 *   peer may lead, and the owner says otherwise when it has that reason.
 * - PeerAlwaysMayLead: the publisher pair and the arbiters, whose leaders make no statements.
 */
class PeerStatementsFlag {
  public:
    enum PeerStatementsFlagTag { PeerAlwaysMayLead = 0, SayWhetherPeerMayLead = 1, SayWhenPeerMayNotLead = 2 };

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
