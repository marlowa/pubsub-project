#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace fix_common {

/**
 * @brief An instance's own record of the leases it holds, and so whether it may act as leader.
 *
 * An instance leads only while it holds an unexpired lease from at least one voter other than itself.
 * With its own vote that is two of three, a majority. See docs/availability/majority_leases.md.
 *
 * The holder counts each lease from the moment it SENT the request, not from the moment the grant
 * arrived. The voter counts its promise from the moment it granted, which is later. So the holder
 * always believes its lease ends no later than the voter believes its promise ends, however long the
 * request and the grant took to travel, and by the time a voter feels free to grant someone else the
 * holder has already stopped relying on it. The holder also shortens each lease by the largest
 * difference in clock rate allowed between two machines. Model checking showed that counting from
 * the grant's arrival instead lets two instances act as leader at once
 * (docs/availability/tla/traces/lease-1-holder-counts-from-arrival.txt).
 *
 * A grant is matched to its request by a request id, which the holder issues and the voter echoes.
 * That is how the holder knows when the request was sent.
 */
class LeaseHolder {
  public:
    using Clock = std::chrono::steady_clock;

    enum class State {
        Idle,      ///< neither leading nor asking to lead
        Candidate, ///< asking to lead
        Leading    ///< has been granted a lease; acts as leader only while one is unexpired
    };

    /// What a reply changed.
    enum class Event {
        Nothing,        ///< the reply was out of date, or changed nothing that matters
        BecameLeader,   ///< a candidate received its first grant, and now leads
        LeaseExtended,  ///< a leader's lease was renewed
        NewerEpochKnown ///< a voter has granted an epoch above this instance's; it must stop and ask again above it
    };

    /**
     * @param[in] lease_period How long a grant lasts.
     * @param[in] drift_allowance How much shorter than the lease period this instance takes a lease to be,
     *            to allow for clocks that do not run at exactly the same rate.
     */
    LeaseHolder(Clock::duration lease_period, Clock::duration drift_allowance) : lease_period_(lease_period), drift_allowance_(drift_allowance) {
        if (drift_allowance < Clock::duration::zero() || drift_allowance >= lease_period) {
            throw pubsub_itc_fw::PreconditionAssertion("LeaseHolder: the drift allowance must be at least zero and less than the lease period", __FILE__,
                                                       __LINE__);
        }
    }

    /// Start asking to lead at @p epoch. Any lease held before is forgotten.
    void begin_candidacy(int32_t epoch) {
        state_ = State::Candidate;
        epoch_ = epoch;
        expiries_.clear();
        outstanding_.clear();
    }

    /**
     * @brief Note that a request is being sent to @p voter_id now, and return the id to put on it.
     */
    [[nodiscard]] int64_t record_request(int64_t voter_id, Clock::time_point now) {
        // A grant that arrives more than a lease period after its request is worth nothing, so a
        // request older than that will never be matched and is forgotten.
        for (auto it = outstanding_.begin(); it != outstanding_.end();) {
            if (now - it->second.sent_at > lease_period_) {
                it = outstanding_.erase(it);
            } else {
                ++it;
            }
        }
        const int64_t request_id = ++last_request_id_;
        outstanding_[request_id] = Outstanding{voter_id, now};
        return request_id;
    }

    /**
     * @brief A voter granted a request.
     * @param[in] voter_id The voter that granted it.
     * @param[in] request_id The id the request carried.
     * @param[in] epoch The epoch the grant is for.
     * @param[in] now The time on this instance's clock.
     */
    [[nodiscard]] Event on_grant(int64_t voter_id, int64_t request_id, int32_t epoch, Clock::time_point now) {
        const auto request = outstanding_.find(request_id);
        if (request == outstanding_.end() || request->second.voter_id != voter_id) {
            return Event::Nothing;
        }
        const Clock::time_point expires = request->second.sent_at + lease_period_ - drift_allowance_;
        outstanding_.erase(request);
        if (epoch != epoch_ || state_ == State::Idle || expires <= now) {
            return Event::Nothing;
        }
        Clock::time_point& held = expiries_[voter_id];
        if (expires > held) {
            held = expires;
        }
        if (state_ == State::Candidate) {
            state_ = State::Leading;
            return Event::BecameLeader;
        }
        return Event::LeaseExtended;
    }

    /**
     * @brief A voter refused a request, saying the highest epoch it has granted.
     * @return NewerEpochKnown when that epoch is above the one this instance leads in or asks to lead in.
     */
    [[nodiscard]] Event on_refusal(int64_t request_id, int32_t highest_epoch) {
        outstanding_.erase(request_id);
        if (state_ != State::Idle && highest_epoch > epoch_) {
            return Event::NewerEpochKnown;
        }
        return Event::Nothing;
    }

    /// Whether this instance leads and holds an unexpired lease, and so may act as leader.
    [[nodiscard]] bool acting(Clock::time_point now) const {
        return state_ == State::Leading && now < lease_expires_at();
    }

    /// When the latest lease this instance holds runs out, as it counts it.
    [[nodiscard]] Clock::time_point lease_expires_at() const {
        Clock::time_point latest{};
        for (const auto& entry : expiries_) {
            if (entry.second > latest) {
                latest = entry.second;
            }
        }
        return latest;
    }

    /// Stop leading or asking to lead.
    void stop() {
        state_ = State::Idle;
        expiries_.clear();
        outstanding_.clear();
    }

    [[nodiscard]] State state() const {
        return state_;
    }

    /// The epoch this instance leads in, or asks to lead in.
    [[nodiscard]] int32_t epoch() const {
        return epoch_;
    }

  private:
    struct Outstanding {
        int64_t voter_id{0};
        Clock::time_point sent_at{};
    };

    Clock::duration lease_period_;
    Clock::duration drift_allowance_;
    State state_{State::Idle};
    int32_t epoch_{0};
    int64_t last_request_id_{0};
    std::map<int64_t, Outstanding> outstanding_;
    std::map<int64_t, Clock::time_point> expiries_;
};

} // namespaces
