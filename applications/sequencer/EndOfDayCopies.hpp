#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>

namespace sequencer {

/**
 * @brief Decides, for the leading sequencer, whether every copy of the WAL holds every record up to the end-of-day begin,
 *        so that the end-of-day end may be written (docs/venue/start_and_end_of_day.md, section 4.2, decisions 3 and 4).
 *
 * Two kinds of copy are waited for:
 *
 * - **the follower sequencer's**, which acknowledges every record it writes. The leading sequencer passes in the last
 *   record it has acknowledged, or nothing when high availability is off and there is no follower sequencer;
 * - **each matching engine publisher's**. A publisher acknowledges nothing as it goes, so after the begin the leading
 *   sequencer asks each one, once a second, for the last record its copy holds (WalHeldRequest), and passes each answer
 *   in here (publisher_holds). The number of publishers waited for is the configured one, so a publisher that is down or
 *   restarting is waited for, not left out.
 *
 * The wait is limited. It starts when this instance, leading, first finds the venue in the end-of-day begin (start), so a
 * new leader waits the whole time again: waiting longer only makes the machines shut down later, while giving up early
 * leaves a copy incomplete. Once the limit has passed, assess says so, and the end is written with a copy incomplete.
 *
 * One thread, the sequencer's, calls every member function.
 */
class EndOfDayCopies {
  public:
    /// Whether the end-of-day end may be written.
    enum class Verdict {
        Waiting,           ///< a copy does not yet hold the begin, and the limit has not passed
        EveryCopyComplete, ///< the follower sequencer, if there is one, and every publisher waited for hold the begin
        LimitPassed        ///< a copy does not yet hold the begin, and the limit has passed
    };

    /// The verdict, and the state of every copy in words, for the Quill log and an operator.
    struct Assessment {
        Verdict verdict{Verdict::Waiting};
        std::string explanation;
    };

    /**
     * @param[in] publishers_waited_for How many publishers must say they hold the begin; zero where the environment runs
     *                                  none.
     * @param[in] longest_wait_ns       How long to wait at most, in nanoseconds, from start.
     */
    EndOfDayCopies(int32_t publishers_waited_for, int64_t longest_wait_ns);

    /**
     * @brief The venue is in the end-of-day begin, written at @p begin_seq_no, at @p now_utc_ns by this instance's clock.
     *
     * The first call for a given begin starts the wait; later calls for the same begin change nothing. A call for a
     * different begin, after a change of leader whose WAL holds a different one, starts afresh.
     */
    void start(int64_t begin_seq_no, int64_t now_utc_ns);

    /// Whether a wait has been started and not forgotten since.
    [[nodiscard]] bool started() const {
        return begin_seq_no_ != 0;
    }

    /// The position of the end-of-day begin being waited for; zero when no wait is started.
    [[nodiscard]] int64_t begin_seq_no() const {
        return begin_seq_no_;
    }

    /// The publisher on @p connection says its copy holds every record up to @p last_seq_no.
    void publisher_holds(const pubsub_itc_fw::ConnectionID& connection, int64_t last_seq_no);

    /// The connection to a publisher has gone: what it said no longer counts, because the publisher that connects again
    /// may be another process, and it is asked again.
    void publisher_gone(const pubsub_itc_fw::ConnectionID& connection);

    /**
     * @brief Whether the end-of-day end may be written now, at @p now_utc_ns.
     *
     * @param[in] follower_sequencer_holds_through The last record the follower sequencer has acknowledged, or nothing
     *                                             when there is no follower sequencer because high availability is off.
     * @param[in] now_utc_ns                       Now, by the same clock as start.
     */
    [[nodiscard]] Assessment assess(std::optional<int64_t> follower_sequencer_holds_through, int64_t now_utc_ns) const;

    /// Forgets the wait and every publisher's answer: this instance has stopped leading.
    void forget();

  private:
    struct PublisherAnswer {
        pubsub_itc_fw::ConnectionID connection;
        int64_t last_seq_no{0};
    };

    // How many publishers' answers say they hold the begin.
    [[nodiscard]] int32_t publishers_holding_the_begin() const;

    int32_t publishers_waited_for_{0};
    int64_t longest_wait_ns_{0};
    int64_t begin_seq_no_{0};
    int64_t wait_started_utc_ns_{0};
    // The latest answer from each publisher connected; there are few.
    std::vector<PublisherAnswer> publisher_answers_;
};

} // namespaces
