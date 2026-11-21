#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <matching_engine_test_events.hpp>

#include "HeldEngineReport.hpp"
#include "SessionIdentity.hpp"

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
 * them, in the order of their commands. Each report is kept, and compared, under the command whose application produced
 * it, which the leader's report record carries as produced_by_seq_no. That is not always the command the report
 * answers: a cancel of the orders of a session that has not come back answers no member's command, and carries 0 as the
 * command it answers, but it was produced by applying the command that cancelled them (docs/bug_list.md, BUG-0263).
 * When a report record of the leader's arrives, produced by command N, the WAL holds every report of every command
 * before N, so:
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
 * **Where the reports are kept.** Everything is allocated, and written once, when this object is made, so that keeping
 * a report never calls the heap allocator on the thread that applies every command (principle F5). The encoded bytes
 * of the reports lie one after another in a single block of bytes, and beside it a ring of fixed-size entries, one per
 * report, says which command each answers, which session it is for, and where its bytes are. Reports are kept in the
 * order of their commands and leave in the same order, oldest first, so the bytes they occupy are freed in order too.
 * A report's bytes are never split: when there is not room for them between the newest report and the end of the
 * block, they are placed at the start of the block if there is room there before the oldest report, and the space left
 * at the end is not used until the reports before it have left. The size of a report is not fixed, because the engine
 * echoes groups from the order whose number has no limit, so the store can be full for want of entries or for want of
 * bytes, and the two are treated alike.
 *
 * One thread, the engine's, calls every member function.
 */
class FollowerReportComparison {
  public:
    /// The most reports kept. Reaching it means the leading engine has had no report written for that many commands.
    static constexpr size_t max_kept_reports = 100'000;
    /// The most bytes of encoded reports kept: 100,000 reports of 671 bytes on average.
    static constexpr size_t max_kept_report_bytes = 64 * 1024 * 1024;

    /// Unlocks the store, if hold_in_memory locked it.
    ~FollowerReportComparison();

    /**
     * @param[in] links       How what is found is acted on.
     * @param[in] logger      Where what happens is logged.
     * @param[in] most_reports The most reports kept at once. Zero for an engine that never follows the WAL, which is
     *                        one with high availability off: nothing is then allocated.
     * @param[in] most_bytes  The most bytes of encoded reports kept at once, zero likewise.
     */
    FollowerReportComparison(FollowerReportComparisonLinksInterface& links, pubsub_itc_fw::QuillLogger& logger, size_t most_reports, size_t most_bytes);

    FollowerReportComparison(const FollowerReportComparison&) = delete;
    FollowerReportComparison& operator=(const FollowerReportComparison&) = delete;
    FollowerReportComparison(FollowerReportComparison&&) = delete;
    FollowerReportComparison& operator=(FollowerReportComparison&&) = delete;

    /// How many bytes hold_in_memory locks: the block of encoded reports and the ring of entries. Zero for an engine
    /// that never follows the WAL.
    [[nodiscard]] size_t bytes_to_hold_in_memory() const {
        return bytes_.size() + entries_.size() * sizeof(KeptReport);
    }

    /**
     * @brief Locks the store into memory (`mlock`), so that the kernel supplies every page now and never takes one away,
     *        and the machine being short of memory is found at the start of the day rather than in the middle of it.
     * @return 0 when both parts are locked, or nothing needs locking; otherwise the error `mlock` gave, with neither part
     *         left locked.
     */
    [[nodiscard]] int hold_in_memory();

    /**
     * @brief Keeps a report produced by command @p produced_by, for @p session, until the WAL holds the leader's reports
     *        of that command. When the store is full it is not kept, and the first time that happens, which is said at
     *        Error, this instance refuses to lead.
     * @param[in] produced_by The command being applied when the report was produced, which it is compared under.
     * @param[in] answers     The command the report answers, or 0 for a report that answers no member's command. It is
     *                        handed on with the report if this instance takes the lead, because the sequencer routes a
     *                        report by it.
     * @param[in] session     The member's session the report is for.
     * @param[in] size    How many bytes the encoded report needs.
     * @param[in] write   Called as `size_t write(uint8_t* destination, size_t room)` when there is room, to write the
     *                    encoded report at @p destination, which has @p room bytes, @p size of them. It returns how many
     *                    bytes it wrote, or zero when it could not write the report, which is then not kept.
     */
    template <typename Write> void keep(int64_t produced_by, int64_t answers, const fix_common::SessionIdentity& session, size_t size, Write&& write) {
        const std::optional<size_t> offset = place_for(size);
        if (!offset.has_value()) {
            store_full(produced_by, size);
            return;
        }
        const size_t written = write(bytes_.data() + *offset, size);
        if (written == 0) {
            return;
        }
        KeptReport& entry = entries_[(oldest_ + count_) % entries_.size()];
        entry.produced_by = produced_by;
        entry.answers = answers;
        entry.session = session;
        entry.offset = *offset;
        entry.size = written;
        entry.compared = false;
        ++count_;
        kept_bytes_ += written;
    }

    /**
     * @brief The catch-up answering a request to follow has started, after command @p after_seq_no: reports are kept
     *        from the next command, unless some are kept already.
     */
    void start_keeping_after(int64_t after_seq_no);

    /**
     * @brief The leader's report record @p record_seq_no, produced by command @p command, has arrived, as @p size bytes
     *        at @p bytes.
     * @param[in] record_seq_no          The report record's own number. A record numbered at or below the last one
     *                                   accounted for has been accounted for already, and is passed over: an answer to
     *                                   a request made again sends again the report records after the last command
     *                                   applied.
     * @param[in] command                The command being applied when the leader produced it, its produced_by_seq_no.
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
        return count_;
    }

    /// How many bytes of encoded reports are kept.
    [[nodiscard]] size_t kept_bytes() const {
        return kept_bytes_;
    }

    /**
     * @brief Taking the lead: each kept report of a command after @p reported_through, the last command the WAL holds
     *        every report of, is given to @p take as a HeldEngineReport marked as a possible repeat, oldest first, and
     *        every kept report is then dropped.
     * @return How many were given to @p take.
     */
    template <typename Take> int64_t take_reports_after(int64_t reported_through, Take&& take) {
        int64_t sent = 0;
        for (size_t position = 0; position < count_; ++position) {
            const KeptReport& kept = entry_at(position);
            if (kept.produced_by > reported_through) {
                HeldEngineReport report;
                report.payload.assign(bytes_.data() + kept.offset, bytes_.data() + kept.offset + kept.size);
                report.seq_no = kept.answers;
                report.produced_by_seq_no = kept.produced_by;
                report.session = kept.session;
                // Sent by an instance taking the lead after the leader that may have sent it already.
                report.poss_resend = true;
                take(std::move(report));
                ++sent;
            }
        }
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "FollowerReportComparison: taking the lead -- sending {} report(s) kept while following, of commands after {}, the last the WAL holds "
                   "every report of, and dropping {} the WAL holds",
                   sent, reported_through, static_cast<int64_t>(count_) - sent);
        links_.kept_reports_sent(sent, reported_through);
        oldest_ = 0;
        count_ = 0;
        kept_bytes_ = 0;
        overflowed_ = false;
        return sent;
    }

  private:
    // One kept report: the command that produced it, which it is compared under, the command it answers, or 0, and where
    // its encoded bytes lie in bytes_.
    struct KeptReport {
        int64_t produced_by{0};
        int64_t answers{0};
        fix_common::SessionIdentity session;
        size_t offset{0};
        size_t size{0};
        // Whether the leader's report of the same command has been compared with it.
        bool compared{false};
    };

    // The kept report @p position places after the oldest, which is at position 0.
    [[nodiscard]] KeptReport& entry_at(size_t position) {
        return entries_[(oldest_ + position) % entries_.size()];
    }
    // Where in bytes_ a report of @p size bytes can be put, or nothing when there is no room for it.
    [[nodiscard]] std::optional<size_t> place_for(size_t size) const;
    // Drops the oldest kept report.
    void drop_oldest();
    // Unlocks both parts of the store, if they are locked.
    void release_from_memory();
    // There is no room for a report produced by command @p produced_by, of @p size bytes: said once, and the lead refused.
    void store_full(int64_t produced_by, size_t size);
    // Says that this instance's report of @p command and the leader's differ, as @p reason says.
    void report_difference(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason);

    FollowerReportComparisonLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    // The encoded reports, one after another, allocated and written once when this object is made.
    std::vector<uint8_t> bytes_;
    // A ring of the kept reports, in the order of the commands that produced them, which is the order they were applied
    // in: count_ of them, starting at oldest_.
    std::vector<KeptReport> entries_;
    size_t oldest_{0};
    size_t count_{0};
    size_t kept_bytes_{0};
    // Whether hold_in_memory locked the store.
    bool locked_{false};
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
