#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "RollingWindowThrottle.hpp"
#include "ThrottleLimits.hpp"
#include "ThrottleOutcome.hpp"
#include "ThrottledCommand.hpp"

namespace fix_common {

/**
 * @brief A session's three throttles: one each for placing, amending and cancelling orders.
 *
 * A gateway creates one for each session when its logon is granted, from the limits the comp id is
 * provisioned with, and keeps it for as long as the session is open; the limits do not change while
 * the session is open. Each kind of command is counted separately, and each session separately, so
 * two sessions of one comp id may each send up to the limits.
 *
 * A limit of zero means no limit. That kind then has no throttle, takes no memory, and every
 * command of it is accepted without a clock comparison.
 *
 * Besides accepting or refusing, it notes when a run of refusals of a kind begins and when it ends,
 * and counts the refusals in it, so that a gateway can log the two moments rather than every refused
 * command (ThrottleOutcome).
 *
 * All storage is taken when it is created. Deciding about a command never uses the heap.
 *
 * Not thread-safe. A gateway handles each session on one thread.
 */
class SessionThrottles {
  public:
    using Clock = RollingWindowThrottle::Clock;

    ~SessionThrottles() = default;

    /// Creates a set with no limits, which accepts every command.
    SessionThrottles() = default;

    /**
     * @brief Creates the throttles for a session.
     * @param[in] limits           The comp id's limits. Each from 0, meaning no limit, to
     *                             ThrottleLimits::max_permitted_per_second.
     * @param[in] growth_reporter  Non-owning, may be nullptr. Told the size of each throttle's storage.
     */
    explicit SessionThrottles(const ThrottleLimits& limits, pubsub_itc_fw::AllocationGrowthReporter* growth_reporter = nullptr) {
        create_throttle(place_, limits.max_place_per_second, "new order", "new orders", growth_reporter);
        create_throttle(amend_, limits.max_amend_per_second, "amend", "amends", growth_reporter);
        create_throttle(cancel_, limits.max_cancel_per_second, "cancel", "cancels", growth_reporter);
    }

    SessionThrottles(const SessionThrottles& other) = delete;
    SessionThrottles& operator=(const SessionThrottles& other) = delete;
    SessionThrottles(SessionThrottles&& other) = default;
    SessionThrottles& operator=(SessionThrottles&& other) = default;

    /**
     * @brief Decides whether a command of kind @p command arriving at @p now may be passed on.
     * @param[in] command The kind of command.
     * @param[in] now     When it arrived, from std::chrono::steady_clock.
     * @return Whether it was accepted, and whether a run of refusals began or ended with it.
     */
    [[nodiscard]] ThrottleOutcome try_accept(ThrottledCommand command, Clock::time_point now) {
        Kind& kind = kind_for(command);
        if (!kind.throttle.has_value() || kind.throttle->try_accept(now)) {
            if (kind.refusals_in_current_run == 0) {
                return ThrottleOutcome::Accepted;
            }
            kind.refusals_in_last_run = kind.refusals_in_current_run;
            kind.refusals_in_current_run = 0;
            return ThrottleOutcome::AcceptedAfterRefusals;
        }
        ++kind.refusals_in_current_run;
        return kind.refusals_in_current_run == 1 ? ThrottleOutcome::FirstRefusal : ThrottleOutcome::FurtherRefusal;
    }

    /**
     * @brief The limit applied to a kind of command.
     * @param[in] command The kind of command.
     * @return The largest number accepted in any one second, or 0 if there is no limit.
     */
    [[nodiscard]] int max_per_second(ThrottledCommand command) const {
        const Kind& kind = kind_for(command);
        return kind.throttle.has_value() ? kind.throttle->max_per_second() : 0;
    }

    /**
     * @brief How many commands of a kind were refused in the run of refusals that ended most
     *        recently, which is the run ended by the last AcceptedAfterRefusals outcome.
     * @param[in] command The kind of command.
     * @return The number refused, or 0 if no run has ended.
     */
    [[nodiscard]] int64_t refusals_in_last_run(ThrottledCommand command) const {
        return kind_for(command).refusals_in_last_run;
    }

    /**
     * @brief How many commands of a kind have been refused since the last one was accepted.
     * @param[in] command The kind of command.
     * @return The number refused so far in the current run, or 0 if the last command was accepted.
     */
    [[nodiscard]] int64_t refusals_in_current_run(ThrottledCommand command) const {
        return kind_for(command).refusals_in_current_run;
    }

    /**
     * @brief The text a refusal of a kind of command carries back to the member.
     *
     * It states the limit, the kind of command it applies to, and that it is counted for each
     * session, for example "Throttled: at most 50 cancels per second for this session", because a
     * member told only that it was throttled cannot tell how far to slow down. It is composed when
     * the throttles are created, so that refusing a command allocates nothing.
     *
     * @param[in] command The kind of command.
     * @return The text, or an empty string for a kind with no limit, which is never refused.
     */
    [[nodiscard]] const std::string& refusal_text(ThrottledCommand command) const {
        return kind_for(command).refusal_text;
    }

    /// The bytes of storage the three throttles took when they were created.
    [[nodiscard]] size_t allocated_bytes() const {
        return bytes_of(place_) + bytes_of(amend_) + bytes_of(cancel_);
    }

  private:
    struct Kind {
        std::optional<RollingWindowThrottle> throttle;
        int64_t refusals_in_current_run{0};
        int64_t refusals_in_last_run{0};
        std::string refusal_text;
    };

    /**
     * @brief Creates the throttle for one kind of command, unless its limit is zero.
     * @param[out] kind             Where the throttle and its refusal text are kept.
     * @param[in]  max_per_second   The limit, or 0 for none.
     * @param[in]  singular_name    The command's name for a limit of one, such as "new order".
     * @param[in]  plural_name      The command's name for any other limit, such as "new orders".
     * @param[in]  growth_reporter  Non-owning, may be nullptr.
     */
    static void create_throttle(Kind& kind, int max_per_second, const char* singular_name, const char* plural_name,
                                pubsub_itc_fw::AllocationGrowthReporter* growth_reporter) {
        if (max_per_second == 0) {
            return;
        }
        kind.throttle.emplace(max_per_second, growth_reporter);
        kind.refusal_text =
            fmt::format("Throttled: at most {} {} per second for this session", max_per_second, max_per_second == 1 ? singular_name : plural_name);
    }

    [[nodiscard]] static size_t bytes_of(const Kind& kind) {
        return kind.throttle.has_value() ? kind.throttle->allocated_bytes() : 0;
    }

    [[nodiscard]] Kind& kind_for(ThrottledCommand command) {
        switch (command) {
            case ThrottledCommand::Place:
                return place_;
            case ThrottledCommand::Amend:
                return amend_;
            case ThrottledCommand::Cancel:
                return cancel_;
        }
        throw pubsub_itc_fw::PreconditionAssertion("SessionThrottles: not a kind of throttled command", __FILE__, __LINE__);
    }

    [[nodiscard]] const Kind& kind_for(ThrottledCommand command) const {
        return const_cast<SessionThrottles*>(this)->kind_for(command);
    }

    Kind place_;
    Kind amend_;
    Kind cancel_;
};

} // namespaces
