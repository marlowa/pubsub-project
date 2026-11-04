#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string_view>

#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReplicatedWal.hpp>

#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that CommandChecks can look a command up in the WAL, answer a command
 *        sent again from the WAL, and refuse a command that reuses a ClOrdID.
 */
class CommandChecksLinksInterface {
  public:
    virtual ~CommandChecksLinksInterface() = default;

    /// The record in the WAL that holds the command with identity @p identity, if any, first sent no earlier than
    /// @p first_sent_no_earlier_than (ReplicatedWal::find_command).
    [[nodiscard]] virtual std::optional<pubsub_itc_fw::CommandInWal> find_command(std::string_view identity, int64_t first_sent_no_earlier_than) = 0;

    /**
     * @brief Answers @p inbound, a command sent again that the WAL holds as @p command, with the first report of it the
     *        WAL holds, forwarded to the member's session marked as a possible repeat.
     * @return Whether a report was forwarded. When the WAL holds none, or the session is bound to no gateway, nothing is
     *         sent, and the matching engine answers the command by the ordinary path.
     */
    virtual bool answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command) = 0;

    /// Sends CommandRefused for @p inbound on @p came_by, the connection it arrived on, and raises the test event
    /// IdentifierRefused; @p first_seq_no is the record that already holds its ClOrdID.
    virtual void refuse(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound, int64_t first_seq_no) = 0;

    /// Gives back the memory of any search of the WAL. @return How many records the search had read.
    virtual int64_t discard_command_search() = 0;

    /// The WAL's own counts of how each check was settled, since this instance started.
    [[nodiscard]] virtual pubsub_itc_fw::ReplicatedWal::CommandCheckCounts command_check_counts() const = 0;
};

/**
 * @brief The check of each command against those already in the WAL, and what follows from a match
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.13, and
 *        docs/availability/one_check_against_sequencing_twice.md).
 *
 * Every command the leader receives is checked before it is given a number, whatever path it came by, so that the WAL
 * holds each command at most once. A command is known by its comp id, its gateway's protocol, and its ClOrdID. It goes
 * one of five ways:
 *
 * | The command | Done |
 * |-------------|------|
 * | carries no comp id or no ClOrdID | sequenced, unchecked, and counted; the first such command is logged at Error |
 * | is not in the WAL | sequenced; if it was marked as sent again, counted |
 * | is in the WAL, not marked as sent again | refused with CommandRefused on the connection it came by, nothing written, and counted |
 * | is in the WAL, marked as sent again, and the WAL holds a report answering it | that report forwarded to the member's session, marked as a possible repeat,
 * and counted | | is in the WAL, marked as sent again, and the WAL holds no report of it | passed over: the engine answers it by the ordinary path; counted |
 *
 * A command marked as sent again had no answer: its gateway sent it again after a change of leader, or after its
 * connection to this sequencer was closed and opened again. One not marked whose identity the WAL holds is a member
 * using a ClOrdID it has already used today.
 *
 * A summary of the counts is logged a minute after the last command sent again or refused, or when this instance stops
 * leading; the counts of commands sent again and refused then start again from zero.
 *
 * One thread, the sequencer's, calls every member function.
 */
class CommandChecks {
  public:
    using Clock = std::chrono::steady_clock;

    /// What the thread does with a command once it has been checked.
    enum class Outcome { sequence_it, not_sequenced };

    /// How long after the last command sent again or refused the summary is logged.
    static constexpr std::chrono::seconds summary_idle_limit{60};

    CommandChecks(CommandChecksLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /**
     * @brief Checks @p inbound, which arrived on @p came_by, against the WAL, and answers or refuses it if the WAL holds
     *        it already.
     * @return sequence_it if the thread is to give it a number and write it; not_sequenced if it has been answered,
     *         passed over, or refused, and nothing is to be written.
     */
    Outcome check(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound, Clock::time_point now);

    /// Logs the summary if a command was sent again or refused, and none has been for summary_idle_limit.
    void tick(Clock::time_point now);

    /// This instance has stopped leading: the summary is logged if a command was sent again or refused since the last.
    void stopped_leading();

    /// Since the last summary: commands sent again that the WAL already held.
    [[nodiscard]] int64_t sent_again_already_in_wal() const {
        return sent_again_already_in_wal_;
    }
    /// Since the last summary: commands sent again that were answered by a report the WAL held.
    [[nodiscard]] int64_t answered_from_wal() const {
        return answered_from_wal_;
    }
    /// Since the last summary: commands sent again that the WAL did not hold, and were sequenced as new.
    [[nodiscard]] int64_t sent_again_sequenced() const {
        return sent_again_sequenced_;
    }
    /// Since the last summary: commands refused because they reused a ClOrdID.
    [[nodiscard]] int64_t refused_reused_identifier() const {
        return refused_reused_identifier_;
    }
    /// Since this instance started: commands sequenced unchecked because they had no comp id or no ClOrdID.
    [[nodiscard]] int64_t unchecked_without_identity() const {
        return unchecked_without_identity_;
    }

  private:
    // The record that holds a command with the same identity as @p inbound, if any. A command that does not say whose it
    // is cannot be checked, and is counted and treated as not held.
    [[nodiscard]] std::optional<pubsub_itc_fw::CommandInWal> find(const pubsub_itc_fw_app::WalRecordView& inbound, bool sent_again, Clock::time_point now);
    void checked(Clock::time_point now);
    void summarise(const char* reason);

    CommandChecksLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    // Whether a command has been sent again or refused since the last summary, and when the last was.
    bool checked_since_summary_{false};
    Clock::time_point last_checked_at_{};
    int64_t sent_again_already_in_wal_{0};
    int64_t answered_from_wal_{0};
    int64_t sent_again_sequenced_{0};
    int64_t refused_reused_identifier_{0};
    int64_t unchecked_without_identity_{0};
    // Where the identity of a command to be checked is written.
    std::array<char, pubsub_itc_fw::CommandIdentityInterface::max_identity_size> identity_buffer_{};
};

} // namespaces
