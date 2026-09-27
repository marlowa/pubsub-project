#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <LeaseVoter.hpp>
#include <leader_follower.hpp>

#include "WitnessConfiguration.hpp"

namespace witness {

/**
 * @brief ApplicationThread subclass implementing the witness: the third voter in deciding which arbiter is active.
 *
 * An arbiter is active only while a majority of three voters -- the two arbiters and the witness --
 * has granted it a lease that has not run out. The witness is the voter that is never a candidate:
 * it answers each arbiter's LeaseRequest with a LeaseGrant or a LeaseRefusal, by the rules in
 * fix_common/LeaseVoter.hpp. It grants a lease to at most one arbiter at a time, grants nothing for
 * one lease period after it starts, and never grants an epoch below one it has granted.
 *
 * It keeps nothing on disk. Waiting out one lease period at startup is what makes that safe: any
 * promise it made before it stopped has run out by the time it grants again.
 *
 * The witness never interacts with sequencer or matching engine instances.
 *
 * Threading: ThreadID 1.
 */
class WitnessThread : public pubsub_itc_fw::ApplicationThread {
  public:
    WitnessThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                  const WitnessConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const WitnessConfiguration& config_;

    // The witness's promises. Constructed at the initial event, because it needs the moment the
    // witness started.
    std::optional<fix_common::LeaseVoter> voter_;

    // The arbiter the witness last granted to, so that a change of holder is logged and a renewal is not.
    int64_t last_granted_to_{0};

    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
};

} // namespaces
