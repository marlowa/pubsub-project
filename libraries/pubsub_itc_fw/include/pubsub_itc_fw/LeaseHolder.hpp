#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

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
 *
 * The requests waiting for an answer, and the leases held, are kept in storage of a fixed size made with the holder,
 * because an instance renews on the thread that handles its orders, where nothing is allocated from the heap
 * (principle F5; docs/bug_list.md, BUG-0272). Neither can overflow unsafely. A request forgotten for want of room can
 * only cost a grant that would have extended the lease, and a lease forgotten for want of room can only make the
 * holder believe its lease ends sooner than it does; neither can let it act as leader when it should not.
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

    /**
     * @brief How many requests can wait for an answer at once.
     *
     * An instance asks each of its two other voters once a renewal interval, and forgets a request older than a lease
     * period, so with the usual timing, a renewal each second and a lease of three seconds, about eight are ever
     * waiting. Room for many more allows a much shorter renewal interval. Were it ever full, the oldest request, the
     * one nearest to being worthless, gives way to the new one.
     */
    static constexpr size_t max_outstanding_requests = 64;

    /// How many voters' leases can be held at once. An instance holds leases from at most two voters, its peer and the
    /// third voter; the rest is spare. Were it ever full, the lease that runs out first gives way to a later one.
    static constexpr size_t max_voters_holding_leases = 4;

    /// Start asking to lead at @p epoch. Any lease held before is forgotten.
    void begin_candidacy(int32_t epoch) {
        state_ = State::Candidate;
        epoch_ = epoch;
        forget_leases_and_requests();
    }

    /**
     * @brief Note that a request is being sent to @p voter_id now, and return the id to put on it.
     */
    [[nodiscard]] int64_t record_request(int64_t voter_id, Clock::time_point now) {
        // A grant that arrives more than a lease period after its request is worth nothing, so a
        // request older than that will never be matched and is forgotten.
        Outstanding* oldest = &outstanding_[0];
        Outstanding* free_slot = nullptr;
        for (Outstanding& request : outstanding_) {
            if (request.in_use && now - request.sent_at > lease_period_) {
                request.in_use = false;
            }
            if (!request.in_use) {
                if (free_slot == nullptr) {
                    free_slot = &request;
                }
            } else if (request.sent_at < oldest->sent_at || !oldest->in_use) {
                oldest = &request;
            }
        }
        Outstanding& slot = free_slot != nullptr ? *free_slot : *oldest;
        const int64_t request_id = ++last_request_id_;
        slot = Outstanding{true, request_id, voter_id, now};
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
        Outstanding* const request = find_request(request_id);
        if (request == nullptr || request->voter_id != voter_id) {
            return Event::Nothing;
        }
        const Clock::time_point expires = request->sent_at + lease_period_ - drift_allowance_;
        request->in_use = false;
        if (epoch != epoch_ || state_ == State::Idle || expires <= now) {
            return Event::Nothing;
        }
        hold_lease(voter_id, expires);
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
        Outstanding* const request = find_request(request_id);
        if (request != nullptr) {
            request->in_use = false;
        }
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
        for (const HeldLease& lease : leases_) {
            if (lease.in_use && lease.expires > latest) {
                latest = lease.expires;
            }
        }
        return latest;
    }

    /// Stop leading or asking to lead.
    void stop() {
        state_ = State::Idle;
        forget_leases_and_requests();
    }

    [[nodiscard]] State state() const {
        return state_;
    }

    /// The epoch this instance leads in, or asks to lead in.
    [[nodiscard]] int32_t epoch() const {
        return epoch_;
    }

  private:
    // A request waiting for an answer.
    struct Outstanding {
        bool in_use{false};
        int64_t request_id{0};
        int64_t voter_id{0};
        Clock::time_point sent_at{};
    };

    // A lease granted by one voter, and when this instance counts it as running out.
    struct HeldLease {
        bool in_use{false};
        int64_t voter_id{0};
        Clock::time_point expires{};
    };

    [[nodiscard]] Outstanding* find_request(int64_t request_id) {
        for (Outstanding& request : outstanding_) {
            if (request.in_use && request.request_id == request_id) {
                return &request;
            }
        }
        return nullptr;
    }

    // Keeps @p expires as the lease from @p voter_id if it is later than the one held. With no room, it replaces the
    // lease that runs out first, if that is earlier: forgetting the earlier lease can only shorten what is held.
    void hold_lease(int64_t voter_id, Clock::time_point expires) {
        HeldLease* earliest = nullptr;
        HeldLease* free_slot = nullptr;
        for (HeldLease& lease : leases_) {
            if (lease.in_use && lease.voter_id == voter_id) {
                if (expires > lease.expires) {
                    lease.expires = expires;
                }
                return;
            }
            if (!lease.in_use) {
                if (free_slot == nullptr) {
                    free_slot = &lease;
                }
            } else if (earliest == nullptr || lease.expires < earliest->expires) {
                earliest = &lease;
            }
        }
        if (free_slot != nullptr) {
            *free_slot = HeldLease{true, voter_id, expires};
        } else if (earliest != nullptr && earliest->expires < expires) {
            *earliest = HeldLease{true, voter_id, expires};
        }
    }

    void forget_leases_and_requests() {
        outstanding_.fill(Outstanding{});
        leases_.fill(HeldLease{});
    }

    Clock::duration lease_period_;
    Clock::duration drift_allowance_;
    State state_{State::Idle};
    int32_t epoch_{0};
    int64_t last_request_id_{0};
    std::array<Outstanding, max_outstanding_requests> outstanding_{};
    std::array<HeldLease, max_voters_holding_leases> leases_{};
};

} // namespaces
