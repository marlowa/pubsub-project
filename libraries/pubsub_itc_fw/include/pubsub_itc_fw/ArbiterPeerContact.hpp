#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <pubsub_itc_fw/LeaseTiming.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What one arbiter knows of its contact with the other arbiter, and the two decisions that depend on it.
 *
 * A leader that says its follower may not lead acts on a command the follower lacks only once that statement is held
 * by its follower, or by both arbiters, so that whichever arbiter is active later holds it. While the other arbiter
 * cannot be reached, the active arbiter's confirmation is enough on its own, but only once it has heard nothing from
 * the other arbiter for confirms_alone_after(). An arbiter that becomes active then knows whether the other may have
 * confirmed a statement it lacks:
 *
 *   - The two arbiters send each other a heartbeat every tick. Each heartbeat carries the moment it was sent, on the
 *     sender's steady clock, and echoes the newest such moment the sender has received from the other arbiter.
 *   - An arbiter that has confirmed a statement alone sends the other its whole state, on a connection, before the
 *     next echo it sends on that connection. A connection delivers in order, so an arbiter that has received an echo
 *     has received every statement the other confirmed alone before sending it.
 *   - Any statement the other arbiter confirmed alone after that echo was confirmed at least confirms_alone_after()
 *     after the echoed heartbeat was sent. It was confirmed while the other arbiter was still active, so before this
 *     one became active.
 *
 * So an arbiter that becomes active less than may_be_behind_after() after sending the heartbeat whose echo it last
 * received holds every statement a leader relied on, and grants as usual. One that becomes active later may lack
 * such a statement, and grants nothing until a fresh echo arrives (docs/availability/a_follower_behind_does_not_lead.md,
 * and section 12.6 of docs/availability/tla/findings.md).
 *
 * may_be_behind_after() is two lease periods and one renewal interval. When the active arbiter's machine dies, the
 * other becomes active once its promise to the dead one has run out and the witness has granted it: about one lease
 * period and one renewal interval. confirms_alone_after() is one lease period longer still, which covers the
 * difference between the two machines' clocks over that time, and the time a heartbeat takes to travel.
 */
class ArbiterPeerContact {
  public:
    using Clock = std::chrono::steady_clock;

    /**
     * @param[in] timing The lease timings, which every voter shares.
     * @param[in] started_at When this arbiter started. Until it first hears from the other arbiter, it counts that as
     *            the last time it did.
     */
    ArbiterPeerContact(const LeaseTiming& timing, Clock::time_point started_at)
        : may_be_behind_after_(2 * timing.period + timing.renewal_interval)
        , confirms_alone_after_(may_be_behind_after_ + timing.period)
        , last_heard_(started_at) {}

    /// Any message has arrived from the other arbiter.
    void heard(Clock::time_point now) {
        if (now > last_heard_) {
            last_heard_ = now;
        }
    }

    /**
     * @brief A heartbeat from the other arbiter has arrived.
     * @param[in] sent_at_nanoseconds When the other arbiter sent it, on its own clock.
     * @param[in] echoed_nanoseconds The newest moment, on this arbiter's clock, at which this arbiter sent a heartbeat
     *            the other has received; zero for none.
     * @param[in] now When it arrived, on this arbiter's clock.
     */
    void on_heartbeat(int64_t sent_at_nanoseconds, int64_t echoed_nanoseconds, Clock::time_point now) {
        heard(now);
        if (sent_at_nanoseconds > newest_received_) {
            newest_received_ = sent_at_nanoseconds;
        }
        if (echoed_nanoseconds > newest_echo_) {
            newest_echo_ = echoed_nanoseconds;
        }
    }

    /// What this arbiter's next heartbeat echoes: the newest send time it has received from the other arbiter, or zero.
    [[nodiscard]] int64_t echo() const {
        return newest_received_;
    }

    /// The send time to put on a heartbeat sent at @p now.
    [[nodiscard]] static int64_t sent_at(Clock::time_point now) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    /// Whether, while active, this arbiter's confirmation of a statement is enough on its own: it has heard nothing
    /// from the other arbiter for confirms_alone_after().
    [[nodiscard]] bool may_confirm_alone(Clock::time_point now) const {
        return now - last_heard_ >= confirms_alone_after_;
    }

    /// Whether the other arbiter may have confirmed alone a statement this one lacks: no echo has arrived of a heartbeat
    /// this arbiter sent less than may_be_behind_after() ago. Asked when this arbiter becomes active, and again while it
    /// grants nothing for that reason.
    [[nodiscard]] bool may_be_behind(Clock::time_point now) const {
        return newest_echo_ == 0 || sent_at(now) - newest_echo_ >= std::chrono::duration_cast<std::chrono::nanoseconds>(may_be_behind_after_).count();
    }

    [[nodiscard]] Clock::duration may_be_behind_after() const {
        return may_be_behind_after_;
    }

    [[nodiscard]] Clock::duration confirms_alone_after() const {
        return confirms_alone_after_;
    }

  private:
    Clock::duration may_be_behind_after_;
    Clock::duration confirms_alone_after_;
    Clock::time_point last_heard_;
    int64_t newest_received_{0}; ///< the newest send time received from the other arbiter, on its clock
    int64_t newest_echo_{0};     ///< the newest send time of this arbiter's own heartbeats that the other has echoed, on this clock
};

} // namespaces
