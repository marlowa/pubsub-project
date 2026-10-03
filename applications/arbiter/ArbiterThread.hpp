#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <map>
#include <optional>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <BackgroundPromiseRecorder.hpp>
#include <LeaseLinksInterface.hpp>
#include <LeasePromiseStore.hpp>
#include <PairLeaseAgent.hpp>
#include <leader_follower.hpp>

#include "ArbiterConfiguration.hpp"
#include "ComponentLeaseVoters.hpp"

namespace arbiter {

/**
 * @brief ApplicationThread subclass implementing the arbiter.
 *
 * The arbiter pool is the third voter in deciding which instance of each component pair leads: the
 * sequencers, the matching engines and the matching engine publishers. An instance leads only while
 * a majority of three voters -- itself, its peer and the arbiter pool -- has granted it a lease that
 * has not run out. See docs/availability/majority_leases.md.
 *
 * Two arbiters form the pool, and it votes through whichever of them is active. An arbiter is active
 * only while a majority of three other voters -- the two arbiters and the witness -- has granted it a
 * lease in turn. Its own vote is one of the three, so it needs a grant from its peer or the witness.
 * An active arbiter that can reach neither stops being active when its lease runs out, so at most one
 * arbiter is ever active.
 *
 * Components connect to both arbiters and send each request for a lease to both. The active arbiter
 * answers; the passive one stays silent (see ComponentLeaseVoters). An arbiter that becomes active
 * grants nothing to any component for one lease period, because it does not know what the previously
 * active arbiter promised.
 *
 * Threading: ThreadID 1.
 */
class ArbiterThread : public pubsub_itc_fw::ApplicationThread {
  public:
    ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                  const ArbiterConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // How the lease rules reach the other two voters in deciding which arbiter is active: the peer
    // arbiter and the witness.
    class PoolLinks : public fix_common::LeaseLinksInterface {
      public:
        explicit PoolLinks(ArbiterThread& owner) : owner_(owner) {}
        void send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) override;
        void send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) override;
        void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) override;
        void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseRefusal& refusal) override;

      private:
        ArbiterThread& owner_;
    };

    const ArbiterConfiguration& config_;

    // Whether this arbiter is active, as last decided by the lease rules, and the epoch it is active in.
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};

    // Drives the lease rules. Recurring, every LeaseTiming::tick_interval.
    pubsub_itc_fw::TimerID lease_tick_timer_id_{};

    // Peer arbiter connections (outbound + inbound).
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_;

    // The peer's instance_id, from configuration.
    int64_t peer_instance_id_{0};

    // Witness connection (outbound).
    pubsub_itc_fw::ConnectionID witness_conn_id_;

    PoolLinks pool_links_{*this};

    // This arbiter's side of the lease rules for deciding which arbiter is active. Constructed at
    // the initial event, because it needs the moment this arbiter started.
    std::optional<fix_common::PairLeaseAgent> pool_lease_;

    // The pool's vote on which instance of each component pair leads, held while this arbiter is active.
    ComponentLeaseVoters component_voters_;

    // Where this arbiter's promise in deciding which arbiter is active outlives the process, until the
    // machine reboots.
    fix_common::LeasePromiseStore lease_promise_store_;
    // Writes promise records on a thread of its own, so that refreshing one does not stop this
    // instance answering lease requests while the disk is written (BUG-0107). Declared after the
    // store it writes to, so it is destroyed first. Constructed only when there is a lease agent.
    std::optional<fix_common::BackgroundPromiseRecorder> background_promise_recorder_;

    // The instance last granted a lease in each group, so that a change of leader is logged and a
    // renewal is not.
    std::map<pubsub_itc_fw_app::ComponentGroup, int64_t> last_granted_to_;

    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);

    /// Changes this arbiter's role to follow what the lease rules have just decided.
    void act_on(fix_common::PairLeaseAgent::Change change);

    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_grant(const pubsub_itc_fw::EventMessage& message);
    void handle_lease_refusal(const pubsub_itc_fw::EventMessage& message);

    /// A component instance asks the arbiter pool for a lease.
    void handle_component_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    /// Tells the peer arbiter the highest epoch granted in @p group, so that it is not forgotten if the peer becomes active.
    void send_highest_epoch_to_peer(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::ComponentGroup group, int32_t epoch);

    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbiter_state_record(const pubsub_itc_fw::EventMessage& message);
};

} // namespaces
