#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <deque>
#include <utility>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <matching_engine_test_events.hpp>

#include "HeldEngineReport.hpp"

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that FollowerReportComparison can act on what it finds.
 */
class FollowerReportComparisonLinksInterface {
  public:
    virtual ~FollowerReportComparisonLinksInterface() = default;

    /**
     * @brief This instance is not to lead until it is restarted: it stops asking for a lease, its region is marked
     *        unusable, so that, restarted, it rebuilds its book from the WAL, and the test is told (FollowerCannotLead).
     * @param[in] command The command at which the reason was found.
     * @param[in] reason  Why.
     */
    virtual void stop_leading_for_good(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason) = 0;

    /// One more difference from the leader's reports has been found, for the counter of them.
    virtual void difference_found() = 0;

    /// The test event KeptReportsSent: on taking the lead, @p sent kept reports, of commands after @p reported_through,
    /// were handed on to be sent.
    virtual void kept_reports_sent(int64_t sent, int64_t reported_through) = 0;
};

/**
 * @brief The reports of the commands this instance applies while it follows the WAL, kept until the WAL holds the
 *        leader's reports of the same commands, and compared with them
 *        (docs/availability/both_engines_apply_the_wal.md, sections 3 and 4;
 *        docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.3).
 *
 * An engine that follows the WAL applies every command the leader applies, and sends none of the reports. It keeps
 * them, in the order of their commands. When a report record of the leader's arrives, answering command N, the WAL
 * holds every report of every command before N, so:
 *
 * - each kept report of a command before N that was never compared is one the leader did not send, a difference;
 * - the first kept report of N not yet compared is compared byte for byte with the leader's, and a mismatch is a
 *   difference;
 * - with no kept report of N to compare, the leader sent a report this instance did not produce, a difference, unless
 *   this instance does not keep reports of N: one the WAL held every report of when it began following, one before it
 *   began keeping, or one after its store was full.
 *
 * **Refusing to lead.** The first difference means the two engines did not apply the same command alike, so this
 * instance's book may differ from the one the members were told of. It refuses to lead until it is restarted: it stops
 * asking for a lease and its region is marked unusable, and it goes on following, so that the venue knows it has no
 * standby it can trust (principle V11). A full store, and a void learned after applying a command it names
 * (refuse_to_lead, called by the thread), have the same effect. The refusal never ends.
 *
 * **Taking the lead.** The kept reports of commands after the last the WAL holds every report of are handed on to be
 * sent, marked as possible repeats, and the rest dropped (take_reports_after).
 *
 * One thread, the engine's, calls every member function.
 */
class FollowerReportComparison {
  public:
    /// The most reports kept. Reaching it means the leading engine has had no report written for that many commands.
    static constexpr size_t max_kept_reports = 100000;

    /**
     * @param[in] links  How what is found is acted on.
     * @param[in] logger Where what happens is logged.
     */
    FollowerReportComparison(FollowerReportComparisonLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// Whether there is room for another report. When there is not, keep() refuses this instance the lead.
    [[nodiscard]] bool has_room() const {
        return kept_.size() < max_kept_reports;
    }

    /**
     * @brief Keeps @p report, of a command this instance applied while following, until the WAL holds the leader's
     *        reports of that command. When the store is full it is not kept, and the first time that happens, which is
     *        said at Error, this instance refuses to lead.
     */
    void keep(HeldEngineReport report);

    /**
     * @brief The catch-up answering a request to follow has started, after command @p after_seq_no: reports are kept
     *        from the next command, unless some are kept already.
     */
    void start_keeping_after(int64_t after_seq_no);

    /**
     * @brief The leader's report record @p record_seq_no, answering command @p command, has arrived, as @p size bytes at
     *        @p bytes.
     * @param[in] record_seq_no          The report record's own number. A record numbered at or below the last one
     *                                   accounted for has been accounted for already, and is passed over: an answer to
     *                                   a request made again sends again the report records after the last command
     *                                   applied.
     * @param[in] command                The command it answers.
     * @param[in] bytes                  The leader's encoded report.
     * @param[in] size                   Its size.
     * @param[in] reports_in_wal_through The last command the WAL held every report of when the latest catch-up started.
     */
    void leader_report(int64_t record_seq_no, int64_t command, const uint8_t* bytes, size_t size, int64_t reports_in_wal_through);

    /// From now on this instance does not lead until it is restarted, because of @p why, found at @p command.
    void refuse_to_lead(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason, const char* why);

    /// Whether this instance has refused to lead.
    [[nodiscard]] bool refused_to_lead() const {
        return refused_to_lead_;
    }

    /// How many differences from the leader's reports have been found.
    [[nodiscard]] int64_t differences() const {
        return differences_;
    }

    /// How many reports are kept.
    [[nodiscard]] size_t kept() const {
        return kept_.size();
    }

    /**
     * @brief Taking the lead: each kept report of a command after @p reported_through, the last command the WAL holds
     *        every report of, is given to @p take, oldest first, and every kept report is then dropped.
     * @return How many were given to @p take.
     */
    template <typename Take> int64_t take_reports_after(int64_t reported_through, Take&& take) {
        int64_t sent = 0;
        for (HeldEngineReport& kept : kept_) {
            if (kept.seq_no > reported_through) {
                take(std::move(kept));
                ++sent;
            }
        }
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "FollowerReportComparison: taking the lead -- sending {} report(s) kept while following, of commands after {}, the last the WAL holds "
                   "every report of, and dropping {} the WAL holds",
                   sent, reported_through, static_cast<int64_t>(kept_.size()) - sent);
        links_.kept_reports_sent(sent, reported_through);
        kept_.clear();
        overflowed_ = false;
        return sent;
    }

  private:
    // Says that this instance's report of @p command and the leader's differ, as @p reason says.
    void report_difference(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason);

    FollowerReportComparisonLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    // In the order of their commands, which is the order they were applied in.
    std::deque<HeldEngineReport> kept_;
    // Whether the store has been full, which is said once, until the reports kept are next handed on.
    bool overflowed_{false};
    // The first command whose reports are kept: the one after the position the request to follow named. A leader's
    // report of an earlier command has nothing kept to be compared with.
    int64_t keeping_from_{0};
    // The number of the last report record accounted for.
    int64_t last_record_accounted_{0};
    // The highest command a report of has been compared, so that a report of an earlier command, written again into the
    // WAL by a sequencer that took the lead, is recognised as one accounted for already.
    int64_t highest_command_compared_{0};
    bool refused_to_lead_{false};
    int64_t differences_{0};
};

} // namespaces
