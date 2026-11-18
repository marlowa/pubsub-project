#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <deque>
#include <utility>

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that CancelInPieces can pause the commands, ask for the next piece,
 *        carry it out, and act on a message it held.
 *
 * @tparam Message The message held while a cancel has pieces left: the framework's EventMessage in the engine, something
 *                 simpler in a test.
 */
template <typename Message> class CancelInPiecesLinksInterface {
  public:
    virtual ~CancelInPiecesLinksInterface() = default;

    /// Stops reading the commands from the sequencers when @p paused is true, and reads them again when it is false.
    virtual void set_commands_paused(bool paused) = 0;

    /// Asks for continuation_arrived() to be called once the thread has dealt with what is already waiting.
    virtual void post_continuation() = 0;

    /// Carries out the next piece of the cancel under way. Returns true when that was its last piece.
    virtual bool carry_out_piece() = 0;

    /// Whether a cancel has pieces left to carry out.
    [[nodiscard]] virtual bool cancel_under_way() const = 0;

    /// Acts on @p message, held while a cancel had pieces left, as it would have been acted on when it arrived.
    virtual void act_on(Message&& message) = 0;
};

/**
 * @brief Carries out a command that cancels many orders one piece at a time, with the engine's run loop dealing with
 *        what is waiting between the pieces (docs/availability/pacing_the_engines_reports.md, section 7).
 *
 * While a cancel has pieces left, the commands from the sequencers are not read, and every message from a sequencer's
 * order connection that has already arrived is held, in order, and acted on once the cancel is finished. Each piece is
 * asked for by a message the thread sends itself, so that the lease, and the notice that a connection for reports has
 * written what was sent on it, are dealt with between the pieces. While report pacing holds the commands, because too
 * many reports wait unwritten, no piece is carried out; the next is asked for when pacing lets the commands go on.
 *
 * One thread, the engine's, calls every member function.
 *
 * @tparam Message As for CancelInPiecesLinksInterface.
 */
template <typename Message> class CancelInPieces {
  public:
    explicit CancelInPieces(CancelInPiecesLinksInterface<Message>& links) : links_(links) {}

    /// Whether @p message, from a sequencer's order connection, must be held: true while a cancel has pieces left.
    [[nodiscard]] bool holds() const {
        return links_.cancel_under_way();
    }

    /// Holds @p message until the cancel under way is finished.
    void hold(Message&& message) {
        held_.push_back(std::move(message));
    }

    /// Called after every command is applied: if it left a cancel with pieces to carry out, pauses the commands and asks
    /// for the next piece.
    void after_command() {
        if (!links_.cancel_under_way()) {
            return;
        }
        set_paused();
        ask_for_next_piece();
    }

    /// The message the thread sent itself has arrived: carries out the next piece, unless pacing holds the commands, and
    /// once the last is done acts on what was held, until one of those starts another cancel in pieces.
    void continuation_arrived() {
        continuation_posted_ = false;
        if (!links_.cancel_under_way() || pacing_holds_commands_) {
            return;
        }
        if (!links_.carry_out_piece()) {
            ask_for_next_piece();
            return;
        }
        while (!held_.empty() && !links_.cancel_under_way()) {
            Message message = std::move(held_.front());
            held_.pop_front();
            links_.act_on(std::move(message));
        }
        // An act_on that applied a command has called after_command(), which asks for the next piece of a new cancel.
        set_paused();
    }

    /// Report pacing wants the commands paused, because too many reports wait unwritten.
    void pacing_pauses() {
        pacing_holds_commands_ = true;
        set_paused();
    }

    /// Report pacing lets the commands go on: they are read again unless a cancel has pieces left, which goes on.
    void pacing_resumes() {
        pacing_holds_commands_ = false;
        set_paused();
        if (links_.cancel_under_way()) {
            ask_for_next_piece();
        }
    }

    /// How many messages are held.
    [[nodiscard]] size_t held() const {
        return held_.size();
    }

  private:
    void set_paused() {
        const bool pause = pacing_holds_commands_ || links_.cancel_under_way();
        if (pause != commands_paused_) {
            commands_paused_ = pause;
            links_.set_commands_paused(pause);
        }
    }

    void ask_for_next_piece() {
        if (continuation_posted_ || pacing_holds_commands_) {
            return; // one is on its way already, or pacing asks for one once the reports waiting are written
        }
        continuation_posted_ = true;
        links_.post_continuation();
    }

    CancelInPiecesLinksInterface<Message>& links_;
    std::deque<Message> held_;
    bool pacing_holds_commands_{false};
    bool commands_paused_{false};
    bool continuation_posted_{false};
};

} // namespaces
