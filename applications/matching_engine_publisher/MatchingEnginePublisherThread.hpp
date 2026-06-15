#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include <leader_follower.hpp>

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
 *   appends each to its own WAL, and sends WalAck. Routes records to
 *   live topic subscribers based on pdu_id.
 *
 * Topic publisher (inbound from topic subscribers):
 *   Listens on two ports -- one for the "orders" topic (NOS/OCR) and
 *   one for "execution_reports" (ER). On TopicSubscribeRequest, if the
 *   instance is the leader it replays WAL records from the subscriber's
 *   cursor, then streams live records as TopicPage PDUs. If the instance
 *   is the follower it immediately replies with TopicNotLeader.
 *
 * HA:
 *   Same arbiter-mediated leader-follower state machine as the sequencer.
 *   Only the leader streams topic records; the follower holds topic
 *   subscriber connections warm and sends TopicNotLeader on any new
 *   TopicSubscribeRequest.
 */
class MatchingEnginePublisherThread : public pubsub_itc_fw::ApplicationThread {
  public:
    MatchingEnginePublisherThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                                  pubsub_itc_fw::QuillLogger& logger,
                                  pubsub_itc_fw::Reactor& reactor,
                                  const MatchingEnginePublisherConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const MatchingEnginePublisherConfiguration& config_;

    // Precomputed inbound service name strings.
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

    // Topic subscriber state.
    // Two separate registries track WAL cursors per topic so MEP can
    // compute independent truncation floors for each topic's subscriber set.
    pubsub_itc_fw::ExternalWalSubscriberRegistry orders_registry_;
    pubsub_itc_fw::ExternalWalSubscriberRegistry er_registry_;

    // Connections that have completed the TopicSubscribeRequest handshake
    // and are receiving live TopicPage PDUs.
    std::unordered_set<pubsub_itc_fw::ConnectionID> orders_live_conn_ids_;
    std::unordered_set<pubsub_itc_fw::ConnectionID> er_live_conn_ids_;

    // Per-subscriber topic name (for ConnectionLost routing).
    std::unordered_map<pubsub_itc_fw::ConnectionID, std::string> conn_to_topic_;

    // ----------------------------------------------------------------
    // HA helpers (same state machine as the sequencer)
    // ----------------------------------------------------------------
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_arbiter_heartbeat();
    void send_arbitration_report();
    void write_fence_file() const;
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);

    // ----------------------------------------------------------------
    // WAL follower helpers
    // ----------------------------------------------------------------
    void handle_wal_subscribe_ack(const pubsub_itc_fw::EventMessage& message);
    void handle_wal_record_from_sequencer(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // ----------------------------------------------------------------
    // Topic publisher helpers
    // ----------------------------------------------------------------
    void handle_topic_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_topic_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void stream_wal_record_to_topic_subscribers(int64_t seq_no, int16_t pdu_id, int64_t wall_time_ns,
                                                 const uint8_t* pdu_payload, size_t pdu_size);
    void send_topic_page(const pubsub_itc_fw::ConnectionID& conn_id, int64_t seq_no, int16_t pdu_id,
                         int64_t wall_time_ns, const uint8_t* pdu_payload, size_t pdu_size);
    void replay_wal_for_subscriber(const pubsub_itc_fw::ConnectionID& conn_id, int64_t from_seq_no,
                                    bool is_orders_topic);
    void disconnect_all_topic_subscribers();
};

} // namespaces
