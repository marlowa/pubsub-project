#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>

namespace pubsub_itc_fw {

/**
 * @brief The newest statement the leader of another pair has made about whether an instance of this pair may lead.
 *
 * In the venue, the leading sequencer says that a matching engine it has judged to have stopped working may not lead,
 * and later that it may again (docs/availability/a_matching_engine_that_stops_working.md, sections 4 and 5). The other
 * engine and the arbiters keep the statement with this class, and refuse the instance it names as long as it stands.
 *
 * A statement lasts only as long as the leadership that made it. Each is made at the epoch the maker leads in, and an
 * epoch records which instance leads in it, so only the instance leading now can make a statement at the highest epoch
 * known. This class therefore:
 *
 * - accepts a statement only if its maker's epoch is at least the highest epoch of that pair it knows;
 * - within one leadership, keeps the statement with the highest number, so that a late copy of an older statement
 *   cannot undo a newer one;
 * - drops the statement when it learns of a higher epoch of the maker's pair, since a new leadership starts with no
 *   statement standing and judges afresh.
 *
 * Held in memory only: the maker repeats its statement while it stands, and losing it can only let the instance lead
 * for longer, which is the failure the statement ends, not a new one.
 */
class HeldStatementFromAnotherPair {
  public:
    /// One statement, as its maker sends it.
    struct Statement {
        // The epoch the maker leads in, in its own pair.
        int32_t maker_epoch{0};
        // Goes up by one each time that leadership changes what it says.
        int64_t number{0};
        // The instance of this pair the statement is about.
        int64_t instance_id{0};
        // false: that instance may not lead; true: it may again.
        bool may_lead{true};
    };

    /**
     * @brief A statement has arrived. Kept if it is newer than the one held and its maker leads at the highest epoch known.
     * @param[in] statement The statement.
     * @return Whether the instance barred from leading changed.
     */
    bool offer(const Statement& statement) {
        if (statement.maker_epoch < highest_maker_epoch_known_) {
            return false;
        }
        if (held_.has_value() &&
            (statement.maker_epoch < held_->maker_epoch || (statement.maker_epoch == held_->maker_epoch && statement.number <= held_->number))) {
            return false;
        }
        const int64_t barred_before = barred_instance();
        held_ = statement;
        highest_maker_epoch_known_ = statement.maker_epoch;
        return barred_instance() != barred_before;
    }

    /**
     * @brief An epoch of the maker's pair has become known, from wherever this component learns of them.
     * @param[in] epoch The epoch.
     * @return Whether the instance barred from leading changed, because the statement held was dropped.
     */
    bool maker_epoch_known(int32_t epoch) {
        if (epoch <= highest_maker_epoch_known_) {
            return false;
        }
        highest_maker_epoch_known_ = epoch;
        if (!held_.has_value() || held_->maker_epoch >= epoch) {
            return false;
        }
        const int64_t barred_before = barred_instance();
        held_.reset();
        return barred_instance() != barred_before;
    }

    /// The instance the statement held says may not lead, or zero when none is barred.
    [[nodiscard]] int64_t barred_instance() const {
        return held_.has_value() && !held_->may_lead ? held_->instance_id : 0;
    }

    /// The statement held, if any.
    [[nodiscard]] const std::optional<Statement>& held() const {
        return held_;
    }

  private:
    std::optional<Statement> held_;
    int32_t highest_maker_epoch_known_{0};
};

} // namespaces
