// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "DiedApplyingCheck.hpp"

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace matching_engine {

void DiedApplyingCheck::begin(bool ha_enabled) {
    if (command_ == 0) {
        return;
    }
    if (!ha_enabled) {
        ask_for_void("high availability is off, so there is no peer engine to ask");
        return;
    }
    question_ = Question::asking_peer;
    links_.start_peer_answer_timer();
    peer_timer_running_ = true;
    ask_peer();
}

void DiedApplyingCheck::peer_connected() {
    ask_peer();
}

void DiedApplyingCheck::ask_peer() {
    if (question_ != Question::asking_peer || peer_asked_) {
        return;
    }
    const pubsub_itc_fw::ConnectionID asked_on = links_.ask_peer_which_commands_it_applied();
    if (!asked_on.is_valid()) {
        return; // asked when a connection to the peer is made
    }
    peer_asked_ = true;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "DiedApplyingCheck: asked the peer engine on connection {} which commands it has applied, to learn whether command seq={} is what killed the "
               "process before this one",
               asked_on.get_value(), command_);
}

void DiedApplyingCheck::sequencer_connected() {
    if (question_ == Question::asking_sequencers) {
        links_.send_void_request(command_);
    }
}

void DiedApplyingCheck::peer_answered(int64_t highest_applied) {
    if (question_ != Question::asking_peer) {
        return; // the lease period ran out first, and the sequencers have been asked
    }
    if (highest_applied < command_) {
        ask_for_void(highest_applied == command_ - 1 ? "the peer engine has applied every command before it and not it" : "the peer engine has not applied it");
        return;
    }
    // The two engines apply the same commands in the same way, so the command did not kill the process before this one:
    // the peer applied it and lived.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "DiedApplyingCheck: the peer engine has applied through seq={}, including command seq={}, which the process before this one died applying -- "
               "so the command did not cause the death, and it is applied as usual",
               highest_applied, command_);
    settle(pubsub_itc_fw_app::DiedApplyingOutcome::PeerAppliedIt);
}

void DiedApplyingCheck::peer_answer_timer_fired() {
    peer_timer_running_ = false;
    if (question_ == Question::asking_peer) {
        ask_for_void("the peer engine did not say within a lease period which commands it has applied");
    }
}

void DiedApplyingCheck::void_request_timer_fired() {
    // A request lost to a change of sequencer leader, or made before any sequencer led, is asked for again.
    if (question_ == Question::asking_sequencers) {
        links_.send_void_request(command_);
    }
}

void DiedApplyingCheck::ask_for_void(const char* why) {
    question_ = Question::asking_sequencers;
    if (peer_timer_running_) {
        links_.stop_peer_answer_timer();
        peer_timer_running_ = false;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "DiedApplyingCheck: asking the leading sequencer to void command seq={}, which the process before this one died applying, because {} "
               "(docs/availability/voided_commands.md, section 4.2)",
               command_, why);
    links_.send_void_request(command_);
    links_.start_void_request_timer();
    void_timer_running_ = true;
}

void DiedApplyingCheck::voids_learned(bool names_the_command) {
    if (question_ != Question::asking_sequencers || !names_the_command) {
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "DiedApplyingCheck: command seq={}, which the process before this one died applying, is voided -- it is answered with a rejection, and this "
               "instance asks to be caught up",
               command_);
    settle(pubsub_itc_fw_app::DiedApplyingOutcome::Voided);
}

void DiedApplyingCheck::declined(int64_t command, pubsub_itc_fw_app::VoidDeclinedReason reason) {
    if (question_ != Question::asking_sequencers || command != command_) {
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "DiedApplyingCheck: the leading sequencer will not void command seq={}, which the process before this one died applying ({}) -- it is applied "
               "as usual, and if it kills this process too, the supervisor does not restart it again",
               command_, pubsub_itc_fw_app::to_string(reason));
    settle(pubsub_itc_fw_app::DiedApplyingOutcome::Declined);
}

void DiedApplyingCheck::settle(pubsub_itc_fw_app::DiedApplyingOutcome outcome) {
    question_ = Question::none;
    outcome_ = outcome;
    if (peer_timer_running_) {
        links_.stop_peer_answer_timer();
        peer_timer_running_ = false;
    }
    if (void_timer_running_) {
        links_.stop_void_request_timer();
        void_timer_running_ = false;
    }
    links_.settled(command_, outcome);
}

} // namespaces
