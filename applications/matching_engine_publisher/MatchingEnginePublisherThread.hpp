#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/TopicPublisher.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include <leader_follower.hpp>
#include <topics.hpp>
#include <topics_registry.hpp>

#include "MatchingEnginePublisherConfiguration.hpp"

namespace matching_engine_publisher {

/**
 * @brief ApplicationThread implementing the Matching Engine Publisher logic.
 *
 * MEP has two roles:
 *
 * WAL follower (inbound from sequencer):
 *   Connects outbound to both sequencer WAL subscriber listeners. On
 *   connection, sends WalSubscribeRequest. Receives WalRecord PDUs,
 *   appends each to its own WAL, and notifies the topic publishers.
 *
 * Topic publisher (inbound from topic subscribers):
 *   Listens on two ports -- one for the "orders" topic (NOS/OCR) and one
 *   for "execution_reports" (ER). The reusable pubsub_itc_fw::TopicPublisher
 *   does the work per topic: it owns the subscribe handshake, socket-paced
 *   streaming straight from the WAL (batched TopicPages, one per writable),
 *   the optional control channel, and the slow-consumer policy. This thread
 *   is the TopicPublisherHost: it decodes inbound PDUs, routes them to the
 *   matching publisher, and performs the actual sends. Both publishers share
 *   this MEP's single WAL.
 *
 * HA:
 *   Same arbiter-mediated leader-follower state machine as the sequencer.
 *   Only the leader publishes: on loss of leadership both publishers are set
 *   non-leader (new TopicSubscribeRequests get TopicNotLeader) and all current
 *   subscribers are dropped so they rediscover the new leader.
 */
class MatchingEnginePublisherThread : public pubsub_itc_fw::ApplicationThread, public pubsub_itc_fw::TopicPublisherHost {
  public:
    MatchingEnginePublisherThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                                  const MatchingEnginePublisherConfiguration& config);

    // TopicPublisherHost -- each publisher decides what to send and to
    // whom; this thread performs the send/disconnect/pace on the reactor.
    void topic_send_subscribe_ack(pubsub_itc_fw::ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) override;
    void topic_send_page(pubsub_itc_fw::ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) override;
    void topic_send_not_leader(pubsub_itc_fw::ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) override;
    void topic_send_lagged(pubsub_itc_fw::ConnectionID control_connection_id, const pubsub_itc_fw_app::TopicLagged& lagged) override;
    void topic_disconnect(pubsub_itc_fw::ConnectionID connection_id) override;
    void topic_request_writable_notification(pubsub_itc_fw::ConnectionID connection_id) override;
    void topic_truncate_wal(int64_t safe_seq_no) override;

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_connection_writable(pubsub_itc_fw::ConnectionID id) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const MatchingEnginePublisherConfiguration& config_;

    // Precomputed strings.
    const std::string wal_subscriber_id_;
    const std::string orders_inbound_svc_;
    const std::string er_inbound_svc_;
    const std::string peer_inbound_svc_;

    // MEP's own WAL.
    pubsub_itc_fw::Wal wal_;

    // Sequencer WAL follower connections (both kept warm).
    pubsub_itc_fw::ConnectionID sequencer_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_secondary_conn_id_;

    // Last seq_no received and acked from the sequencer.
    int64_t sequencer_cursor_{0};

    // HA state machine (same structure as the sequencer).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};
    int64_t peer_instance_id_{0};

    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // Reusable publisher per topic, both streaming from this MEP's shared WAL.
    pubsub_itc_fw::TopicPublisher orders_publisher_;
    pubsub_itc_fw::TopicPublisher er_publisher_;

    // HA helpers (same state machine as the sequencer)
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_arbiter_heartbeat();
    void send_arbitration_report();
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);

    void handle_wal_subscribe_ack(const pubsub_itc_fw::EventMessage& message);
    void handle_wal_record_from_sequencer(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // Topic publisher helpers -- decode + route to the owning publisher
    void handle_topic_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_topic_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void set_publishers_leader(bool is_leader);
};

} // namespaces
