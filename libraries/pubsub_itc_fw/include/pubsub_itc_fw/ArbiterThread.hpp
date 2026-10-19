#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <lease_messages.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>

#include <pubsub_itc_fw/ArbiterConfiguration.hpp>
#include <pubsub_itc_fw/ComponentLeaseVoters.hpp>
#include <pubsub_itc_fw/ComponentStatementStore.hpp>
#include <pubsub_itc_fw/LeaderStatement.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Whether an arbiter is active (leader) or passive (follower), or has not yet been decided (unknown).
 *
 * to_string() gives these three names, which the Quill log prints on every change of role.
 */
enum class ArbiterRole { unknown, leader, follower };

/// The name of @p role as the Quill log prints it: "unknown", "leader" or "follower".
[[nodiscard]] std::string_view to_string(ArbiterRole role);

/**
 * @brief ApplicationThread subclass implementing the arbiter.
 *
 * The arbiter pool is the third voter in deciding which instance of each pair leads, for every pair
 * the configuration lists in ArbiterConfiguration::voting_pairs. An instance leads only while a
 * majority of three voters -- itself, its peer and the arbiter pool -- has granted it a lease that has
 * not run out. See docs/availability/majority_leases.md.
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
    class PoolLinks : public pubsub_itc_fw::LeaseLinksInterface {
      public:
        explicit PoolLinks(ArbiterThread& owner) : owner_(owner) {}
        void send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) override;
        void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) override;

      private:
        ArbiterThread& owner_;
    };

    // How the test event channel sends its messages, when a test started this arbiter (docs/availability/test_events.md).
    class ArbiterTestEventLinks : public pubsub_itc_fw::TestEventLinksInterface {
      public:
        explicit ArbiterTestEventLinks(ArbiterThread& owner) : owner_(owner) {}
        void send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override;

      private:
        ArbiterThread& owner_;
    };

    const ArbiterConfiguration& config_;

    // Whether this arbiter is active, as last decided by the lease rules, and the epoch it is active in.
    ArbiterRole role_{ArbiterRole::unknown};
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
    std::optional<pubsub_itc_fw::PairLeaseAgent> pool_lease_;

    // The test events this thread sends, when a test started this arbiter. Constructed at the initial event, before
    // pool_lease_, so that the lease agent can be given it.
    ArbiterTestEventLinks test_event_links_{*this};
    std::optional<pubsub_itc_fw::TestEventChannel> test_events_;

    // The pool's vote on which instance of each component pair leads, held while this arbiter is active.
    ComponentLeaseVoters component_voters_;

    // Where this arbiter's promise in deciding which arbiter is active outlives the process, until the
    // machine reboots.
    pubsub_itc_fw::LeasePromiseStore lease_promise_store_;
    // Writes promise records on a thread of its own, so that refreshing one does not stop this
    // instance answering lease requests while the disk is written (BUG-0107). Declared after the
    // store it writes to, so it is destroyed first. Constructed only when there is a lease agent.
    std::optional<pubsub_itc_fw::BackgroundPromiseRecorder> background_promise_recorder_;

    // The newest statement from each component pair's leader about whether its peer may lead,
    // kept on disk beside the promise record, so that a restart does not forget it.
    ComponentStatementStore component_statement_store_;

    // The statements found on disk at start that say an instance may not lead, by pair, reported to the test when its
    // connection is made, since they are read before it can be (docs/availability/test_events.md).
    std::vector<std::pair<int32_t, pubsub_itc_fw::LeaderStatement>> statements_found_at_start_;

    // The instance last granted a lease in each pair, so that a change of leader is logged and a
    // renewal is not.
    std::map<int32_t, int64_t> last_granted_to_;

    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(ArbiterRole new_role);

    /// Whether @p pair_id is one of the pairs this arbiter votes in, from configuration.
    [[nodiscard]] bool votes_in(int32_t pair_id) const;

    /// The configured name of @p pair_id, as the Quill log prints it; "unknown" for a pair not configured.
    [[nodiscard]] std::string pair_name(int32_t pair_id) const;

    /// Changes this arbiter's role to follow what the lease rules have just decided.
    void act_on(pubsub_itc_fw::PairLeaseAgent::Change change);

    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_grant(const pubsub_itc_fw::EventMessage& message);
    void handle_lease_refusal(const pubsub_itc_fw::EventMessage& message);

    /// A component instance asks the arbiter pool for a lease.
    void handle_component_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    /// Tells the peer arbiter the highest epoch granted in @p pair_id, and the newest statement held from its leader, so that
    /// neither is forgotten if the peer becomes active.
    void send_group_state_to_peer(const pubsub_itc_fw::ConnectionID& conn_id, int32_t pair_id);

    /// Writes every statement held to disk. Logs, and returns false, when it cannot.
    [[nodiscard]] bool save_component_statements();

    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbiter_state_record(const pubsub_itc_fw::EventMessage& message);
};

} // namespaces
