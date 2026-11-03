#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <optional>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>
#include <matching_engine_replication.hpp>
#include <matching_engine_test_events.hpp>

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that DiedApplyingCheck can ask its questions.
 */
class DiedApplyingCheckLinksInterface {
  public:
    virtual ~DiedApplyingCheckLinksInterface() = default;

    /// Asks the peer engine, on whichever connection to it is up, for the highest command it has applied
    /// (AppliedThroughQuery). @return The connection asked on, or an invalid one when no connection to the peer is up.
    virtual pubsub_itc_fw::ConnectionID ask_peer_which_commands_it_applied() = 0;

    /// Starts the timer, of one lease period, that stops waiting for the peer; its firing is given to
    /// peer_answer_timer_fired().
    virtual void start_peer_answer_timer() = 0;
    virtual void stop_peer_answer_timer() = 0;

    /// Asks every sequencer to void command @p command (VoidRequest); only the leading sequencer acts on it.
    virtual void send_void_request(int64_t command) = 0;

    /// Starts the timer that asks the sequencers again every second; its ticks are given to void_request_timer_fired().
    virtual void start_void_request_timer() = 0;
    virtual void stop_void_request_timer() = 0;

    /// The question is settled, with @p outcome: the test is told (DiedApplying), and a catch-up that was waiting for
    /// it is asked for.
    virtual void settled(int64_t command, pubsub_itc_fw_app::DiedApplyingOutcome outcome) = 0;
};

/**
 * @brief Whether to ask for the command the previous process died applying to be voided
 *        (docs/availability/voided_commands.md, section 4; docs/venue/dividing_the_sequencer_and_matching_engine.md,
 *        section 6.5).
 *
 * The open-order region records which command is being applied before the engine applies it, and that none is once it
 * has. A process that finds a command recorded above the position the region published died applying it. That command
 * may be what killed it, and applying it again would kill this process too. So, before this instance asks to be caught
 * up, which would apply it again:
 *
 * | From              | Event                                                   | To                | Done |
 * |-------------------|---------------------------------------------------------|-------------------|------|
 * | none              | begin(), with high availability on                      | asking_peer       | the peer is asked, once, when a connection to it is up; a
 * timer of one lease period is started | | none              | begin(), with high availability off                     | asking_sequencers | every sequencer is
 * asked to void it, and again every second | | asking_peer       | the peer has applied it                                 | none              | settled:
 * PeerAppliedIt, for the command did not kill the peer | | asking_peer       | the peer has not applied it, or the timer runs out      | asking_sequencers | as
 * above | | asking_sequencers | the voids the leading sequencer sends name it           | none              | settled: Voided | | asking_sequencers | the
 * leading sequencer declines to void it               | none              | settled: Declined |
 *
 * While the question is open, no catch-up is asked for.
 *
 * One thread, the engine's, calls every member function.
 */
class DiedApplyingCheck {
  public:
    enum class Question {
        none,             ///< the previous process did not die applying a command, or the question is settled
        asking_peer,      ///< waiting, for at most a lease period, for the peer to say the highest command it has applied
        asking_sequencers ///< asking the leading sequencer, every second, to void the command
    };

    DiedApplyingCheck(DiedApplyingCheckLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

    /// The region recorded command @p command as being applied, above the position it published.
    void died_applying(int64_t command) {
        command_ = command;
    }

    /// The command the previous process died applying, or 0.
    [[nodiscard]] int64_t command() const {
        return command_;
    }

    [[nodiscard]] Question question() const {
        return question_;
    }

    /// Whether the question is open, so that no catch-up may be asked for.
    [[nodiscard]] bool open() const {
        return question_ != Question::none;
    }

    /// How the question was settled, if it has been.
    [[nodiscard]] const std::optional<pubsub_itc_fw_app::DiedApplyingOutcome>& outcome() const {
        return outcome_;
    }

    /// The application is ready: the question begins, if there is a command to ask about.
    void begin(bool ha_enabled);

    /// A connection to the peer is up: it is asked, if it is to be and has not been.
    void peer_connected();

    /// A sequencer connection for orders is up: it is asked to void the command, if the sequencers are being asked.
    void sequencer_connected();

    /// The peer has applied every command through @p highest_applied.
    void peer_answered(int64_t highest_applied);

    /// The lease period given the peer to answer has run out.
    void peer_answer_timer_fired();

    /// A tick of the timer that asks the sequencers again.
    void void_request_timer_fired();

    /// The leading sequencer has sent the voids in its WAL; @p names_the_command says whether they void the command.
    void voids_learned(bool names_the_command);

    /// The leading sequencer will not void command @p command, for @p reason.
    void declined(int64_t command, pubsub_itc_fw_app::VoidDeclinedReason reason);

  private:
    void ask_peer();
    void ask_for_void(const char* why);
    void settle(pubsub_itc_fw_app::DiedApplyingOutcome outcome);

    DiedApplyingCheckLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;
    int64_t command_{0};
    Question question_{Question::none};
    bool peer_asked_{false};
    bool peer_timer_running_{false};
    bool void_timer_running_{false};
    std::optional<pubsub_itc_fw_app::DiedApplyingOutcome> outcome_;
};

} // namespaces
