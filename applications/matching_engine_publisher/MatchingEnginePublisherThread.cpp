// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEnginePublisherThread.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

#include <topics.hpp>

namespace matching_engine_publisher {

// ---------------------------------------------------------------------------
// PDU ID constants
// ---------------------------------------------------------------------------

static constexpr int16_t pdu_status_query          = 100;
static constexpr int16_t pdu_status_response        = 101;
static constexpr int16_t pdu_heartbeat              = 102;
static constexpr int16_t pdu_wal_record             = 103;
static constexpr int16_t pdu_wal_ack                = 104;
static constexpr int16_t pdu_wal_subscribe_request  = 105;
static constexpr int16_t pdu_wal_subscribe_ack      = 106;
static constexpr int16_t pdu_topic_subscribe_request = 107;
static constexpr int16_t pdu_topic_subscribe_ack    = 108;
static constexpr int16_t pdu_topic_page             = 109;
static constexpr int16_t pdu_topic_ack              = 110;
static constexpr int16_t pdu_topic_not_leader       = 111;
static constexpr int16_t pdu_arbitration_report     = 200;
static constexpr int16_t pdu_arbitration_decision   = 201;

// Application pdu_ids carried inside WalRecord payloads.
static constexpr int16_t pdu_id_nos = 1000;
static constexpr int16_t pdu_id_ocr = 1001;
static constexpr int16_t pdu_id_er  = 1002;

static constexpr const char* topic_orders            = "orders";
static constexpr const char* topic_execution_reports = "execution_reports";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(
        const MatchingEnginePublisherConfiguration& config,
        pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name       = "MepPool";
    cfg.objects_per_pool = config.event_queue_pool_objects_per_slab;
    cfg.initial_pools   = config.event_queue_pool_initial_slabs;
    cfg.handler_for_pool_exhausted = [&logger](void*, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning,
                   "MepPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return cfg;
}

} // namespaces

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

MatchingEnginePublisherThread::MatchingEnginePublisherThread(
        pubsub_itc_fw::ApplicationThread::ConstructorToken token,
        pubsub_itc_fw::QuillLogger& logger,
        pubsub_itc_fw::Reactor& reactor,
        const MatchingEnginePublisherConfiguration& config)
    : ApplicationThread(token, logger, reactor, "MepThread", pubsub_itc_fw::ThreadID{1},
                        make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , orders_inbound_svc_("inbound:" + std::to_string(config.orders_listen_port))
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , peer_inbound_svc_("inbound:" + std::to_string(config.peer_listen_port)) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::on_initial_event() {
    const int64_t recovered_seq = wal_.open(config_.wal_directory, config_.wal_segment_size, nullptr);
    if (recovered_seq > 0) {
        sequencer_cursor_ = recovered_seq;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: WAL open complete: recovered seq_no={} record_count={}",
                   recovered_seq, wal_.record_count());
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: WAL is fresh");
    }

    start_recurring_timer("wal_snapshot", std::chrono::seconds(config_.snapshot_interval_seconds));

    if (!config_.ha_enabled) {
        ++epoch_;
        adopt_role(pubsub_itc_fw_app::Role::leader);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: ha_enabled=false -- starting as leader");
    } else {
        start_one_off_timer("peer_heartbeat_timeout",
                            std::chrono::seconds(config_.startup_election_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: ha_enabled=true -- startup election timeout armed ({}s)",
                   config_.startup_election_timeout_seconds);
    }
}

void MatchingEnginePublisherThread::on_app_ready_event() {
    connect_to_service("sequencer");
    connect_to_service("sequencer_secondary");
    if (config_.ha_enabled) {
        connect_to_service("arbiter_primary");
        connect_to_service("arbiter_secondary");
        connect_to_service("peer");
    }
}

// ---------------------------------------------------------------------------
// Connection events
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();

    if (svc == "sequencer") {
        sequencer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: sequencer (primary) connection {} established -- sending WalSubscribeRequest",
                   id.get_value());
        pubsub_itc_fw_app::WalSubscribeRequest req{};
        req.subscriber_id = "mep_primary";
        req.from_seq_no   = sequencer_cursor_;
        send_pdu(id, pdu_wal_subscribe_request, 0, req);
    } else if (svc == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: sequencer (secondary) connection {} established -- sending WalSubscribeRequest",
                   id.get_value());
        pubsub_itc_fw_app::WalSubscribeRequest req{};
        req.subscriber_id = "mep_primary";
        req.from_seq_no   = sequencer_cursor_;
        send_pdu(id, pdu_wal_subscribe_request, 0, req);
    } else if (svc == "arbiter_primary") {
        const bool first = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: arbiter-primary connection {} established", id.get_value());
        if (first) {
            start_recurring_timer("arbiter_heartbeat", std::chrono::seconds{30});
        }
    } else if (svc == "arbiter_secondary") {
        const bool first = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: arbiter-secondary connection {} established", id.get_value());
        if (first) {
            start_recurring_timer("arbiter_heartbeat", std::chrono::seconds{30});
        }
    } else if (svc == "peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: outbound peer connection {} established -- sending StatusQuery", id.get_value());
        send_status_query(id);
    } else if (svc == peer_inbound_svc_) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: inbound peer connection {} established -- sending StatusQuery", id.get_value());
        send_status_query(id);
    } else if (svc == orders_inbound_svc_ || svc == er_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: topic subscriber connection {} established on {} -- awaiting TopicSubscribeRequest",
                   id.get_value(), svc);
    }
}

void MatchingEnginePublisherThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == sequencer_conn_id_) {
        sequencer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: sequencer (primary) connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: sequencer (secondary) connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: outbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: inbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer("arbiter_heartbeat");
        }
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer("arbiter_heartbeat");
        }
    } else {
        // Topic subscriber connection lost.
        const auto it = conn_to_topic_.find(id);
        if (it != conn_to_topic_.end()) {
            const std::string& topic = it->second;
            if (topic == topic_orders) {
                orders_live_conn_ids_.erase(id);
                orders_registry_.remove_subscriber(id);
            } else {
                er_live_conn_ids_.erase(id);
                er_registry_.remove_subscriber(id);
            }
            conn_to_topic_.erase(it);
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MepThread: topic subscriber connection {} ({}) lost: {}", id.get_value(), topic, reason);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MepThread: connection {} lost: {}", id.get_value(), reason);
        }
    }
}

// ---------------------------------------------------------------------------
// PDU dispatch
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    // Peer PDUs.
    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // Arbiter PDUs.
    if (conn_id == arbiter_primary_conn_id_ || conn_id == arbiter_secondary_conn_id_) {
        handle_arbitration_decision(message);
        release_pdu_payload(message);
        return;
    }

    // Sequencer WAL follower PDUs.
    if (conn_id == sequencer_conn_id_ || conn_id == sequencer_secondary_conn_id_) {
        if (message.pdu_id() == pdu_wal_record) {
            handle_wal_record_from_sequencer(conn_id, message);
        } else if (message.pdu_id() == pdu_wal_subscribe_ack) {
            handle_wal_subscribe_ack(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: unexpected PDU {} from sequencer connection {} -- dropping",
                       message.pdu_id(), conn_id.get_value());
        }
        release_pdu_payload(message);
        return;
    }

    // Topic subscriber PDUs.
    if (svc == orders_inbound_svc_ || svc == er_inbound_svc_) {
        if (message.pdu_id() == pdu_topic_subscribe_request) {
            handle_topic_subscribe_request(conn_id, message);
        } else if (message.pdu_id() == pdu_topic_ack) {
            handle_topic_ack(conn_id, message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: unexpected PDU {} from topic subscriber {} -- dropping",
                       message.pdu_id(), conn_id.get_value());
        }
        release_pdu_payload(message);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "MepThread: PDU on unrecognised connection {} ({}) -- dropping",
               conn_id.get_value(), svc);
    release_pdu_payload(message);
}

// ---------------------------------------------------------------------------
// Timer events
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::on_timer_event(const std::string& name) {
    if (name == "wal_snapshot") {
        try {
            wal_.take_snapshot();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MepThread: WAL snapshot taken: last_seq_no={} record_count={}",
                       wal_.last_seq_no(), wal_.record_count());
        } catch (const std::exception& ex) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MepThread: WAL snapshot failed: {}", ex.what());
        }
        return;
    }

    if (name == "peer_heartbeat") {
        send_peer_heartbeat();
        return;
    }

    if (name == "arbiter_heartbeat") {
        send_arbiter_heartbeat();
        return;
    }

    if (name == "peer_heartbeat_timeout") {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: peer heartbeat timeout (role={})", pubsub_itc_fw_app::to_string(role_));
        if (arbiter_primary_conn_id_.is_valid() || arbiter_secondary_conn_id_.is_valid()) {
            send_arbitration_report();
            start_one_off_timer("arbitration_timeout",
                                std::chrono::seconds(config_.arbitration_timeout_seconds));
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "MepThread: no arbiter connected -- self-promoting (degraded)");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }

    if (name == "arbitration_timeout") {
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "MepThread: arbitration timeout -- self-promoting (degraded)");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }
}

void MatchingEnginePublisherThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// ---------------------------------------------------------------------------
// HA state machine (same pattern as SequencerThread)
// ---------------------------------------------------------------------------

pubsub_itc_fw::ConnectionID MatchingEnginePublisherThread::peer_active_conn() const {
    return peer_conn_id_.is_valid() ? peer_conn_id_ : peer_inbound_conn_id_;
}

void MatchingEnginePublisherThread::adopt_role(pubsub_itc_fw_app::Role new_role) {
    if (new_role == role_) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "MepThread: role transition {} -> {} (epoch={})",
               pubsub_itc_fw_app::to_string(role_), pubsub_itc_fw_app::to_string(new_role), epoch_);
    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        write_fence_file();
        cancel_timer("peer_heartbeat_timeout");
        start_recurring_timer("peer_heartbeat",
                              std::chrono::seconds(config_.heartbeat_interval_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: now LEADER -- heartbeat timer started ({}s)", config_.heartbeat_interval_seconds);
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        start_recurring_timer("peer_heartbeat",
                              std::chrono::seconds(config_.heartbeat_interval_seconds));
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout",
                            std::chrono::seconds(config_.heartbeat_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: now FOLLOWER -- heartbeat timer started, timeout armed ({}s)",
                   config_.heartbeat_timeout_seconds);
        // Disconnect all topic subscribers so they can find the new leader.
        disconnect_all_topic_subscribers();
    }
}

void MatchingEnginePublisherThread::elect_role(int64_t peer_iid, int32_t peer_epoch,
                                                pubsub_itc_fw_app::Role peer_current_role) {
    if (role_ == pubsub_itc_fw_app::Role::leader || role_ == pubsub_itc_fw_app::Role::follower) {
        return;
    }
    if (peer_epoch > epoch_) {
        epoch_ = peer_epoch;
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }
    if (peer_current_role == pubsub_itc_fw_app::Role::leader) {
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }
    if (static_cast<int64_t>(config_.instance_id) < peer_iid) {
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else {
        adopt_role(pubsub_itc_fw_app::Role::follower);
    }
}

void MatchingEnginePublisherThread::send_status_query(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusQuery sq{};
    sq.instance_id = static_cast<int64_t>(config_.instance_id);
    sq.epoch = epoch_;
    send_pdu(conn_id, pdu_status_query, 0, sq);
}

void MatchingEnginePublisherThread::send_status_response(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusResponse sr{};
    sr.self_instance_id    = static_cast<int64_t>(config_.instance_id);
    sr.peer_instance_id    = 0;
    sr.epoch               = epoch_;
    sr.current_role        = role_;
    sr.next_sequence_number = 0;
    send_pdu(conn_id, pdu_status_response, 0, sr);
}

void MatchingEnginePublisherThread::send_peer_heartbeat() {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    send_pdu(target, pdu_heartbeat, 0, hb);
}

void MatchingEnginePublisherThread::send_arbiter_heartbeat() {
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_heartbeat, 0, hb);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_heartbeat, 0, hb);
    }
}

void MatchingEnginePublisherThread::send_arbitration_report() {
    pubsub_itc_fw_app::ArbitrationReport report{};
    report.self_instance_id = static_cast<int64_t>(config_.instance_id);
    report.peer_instance_id = peer_instance_id_;
    report.epoch            = epoch_;
    report.proposed_role    = pubsub_itc_fw_app::Role::leader;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_arbitration_report, 0, report);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_arbitration_report, 0, report);
    }
}

void MatchingEnginePublisherThread::write_fence_file() const {
    const std::string content = std::to_string(config_.instance_id) + "\n";
    FILE* f = std::fopen(config_.fence_file_path.c_str(), "w");
    if (!f) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: failed to write fence file '{}': {}",
                   config_.fence_file_path, std::strerror(errno));
        return;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
}

void MatchingEnginePublisherThread::handle_peer_status_query(
        const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::StatusQueryView sq{};
    if (!pubsub_itc_fw_app::decode(sq, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode StatusQuery -- dropping");
        return;
    }
    peer_instance_id_ = sq.instance_id;
    send_status_response(conn_id);
    elect_role(sq.instance_id, sq.epoch, pubsub_itc_fw_app::Role::unknown);
}

void MatchingEnginePublisherThread::handle_peer_status_response(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::StatusResponseView sr{};
    if (!pubsub_itc_fw_app::decode(sr, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode StatusResponse -- dropping");
        return;
    }
    peer_instance_id_ = sr.self_instance_id;
    elect_role(sr.self_instance_id, sr.epoch, sr.current_role);
}

void MatchingEnginePublisherThread::handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::HeartbeatView hb{};
    if (!pubsub_itc_fw_app::decode(hb, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode Heartbeat -- dropping");
        return;
    }
    if (hb.epoch < epoch_) {
        return;
    }
    if (role_ == pubsub_itc_fw_app::Role::follower) {
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout",
                            std::chrono::seconds(config_.heartbeat_timeout_seconds));
    }
}

void MatchingEnginePublisherThread::handle_peer_pdu(
        const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());
    if (pdu_id == pdu_status_query) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pdu_status_response) {
        handle_peer_status_response(message);
    } else if (pdu_id == pdu_heartbeat) {
        handle_peer_heartbeat(message);
    } else if (pdu_id == pdu_arbitration_decision) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: ArbitrationDecision on peer channel (unexpected) -- dropping");
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: unknown peer PDU {} -- dropping", pdu_id);
    }
}

void MatchingEnginePublisherThread::handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message) {
    cancel_timer("arbitration_timeout");
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::ArbitrationDecisionView decision{};
    if (!pubsub_itc_fw_app::decode(decision, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode ArbitrationDecision -- dropping");
        return;
    }
    epoch_ = decision.epoch;
    if (decision.leader_instance_id == static_cast<int64_t>(config_.instance_id)) {
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else if (decision.follower_instance_id == static_cast<int64_t>(config_.instance_id)) {
        adopt_role(pubsub_itc_fw_app::Role::follower);
    }
}

// ---------------------------------------------------------------------------
// WAL follower helpers
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::handle_wal_subscribe_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::WalSubscribeAckView ack{};
    if (!pubsub_itc_fw_app::decode(ack, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode WalSubscribeAck -- dropping");
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MepThread: WalSubscribeAck accepted_from_seq_no={}", ack.accepted_from_seq_no);
}

void MatchingEnginePublisherThread::handle_wal_record_from_sequencer(
        const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode WalRecord from sequencer -- dropping");
        return;
    }

    wal_.append(view.seq_no, view.pdu_id,
                view.payload.data, static_cast<int>(view.payload.size),
                view.wall_time_ns);
    sequencer_cursor_ = view.seq_no;

    // Ack immediately.
    pubsub_itc_fw_app::WalAck ack{};
    ack.seq_no = view.seq_no;
    send_pdu(conn_id, pdu_wal_ack, 0, ack);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "MepThread: WalRecord seq={} pdu_id={} written to MEP WAL, WalAck sent",
               view.seq_no, view.pdu_id);

    // Fan out to live topic subscribers.
    stream_wal_record_to_topic_subscribers(view.seq_no, view.pdu_id, view.wall_time_ns,
                                           view.payload.data, view.payload.size);
}

// ---------------------------------------------------------------------------
// Topic publisher helpers
// ---------------------------------------------------------------------------

void MatchingEnginePublisherThread::handle_topic_subscribe_request(
        const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::TopicSubscribeRequestView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode TopicSubscribeRequest -- dropping");
        return;
    }

    const std::string subscriber_id(view.subscriber_id);
    const std::string topic_name(view.topic_name);
    const int64_t from_seq_no = view.from_seq_no;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MepThread: TopicSubscribeRequest subscriber_id={} topic={} from_seq_no={} conn={}",
               subscriber_id, topic_name, from_seq_no, conn_id.get_value());

    // Followers send TopicNotLeader immediately.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        pubsub_itc_fw_app::TopicNotLeader notleader{};
        send_pdu(conn_id, pdu_topic_not_leader, 0, notleader);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: sent TopicNotLeader to {} (we are {})",
                   subscriber_id, pubsub_itc_fw_app::to_string(role_));
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
        return;
    }

    const bool is_orders = (topic_name == topic_orders);
    const bool is_er     = (topic_name == topic_execution_reports);
    if (!is_orders && !is_er) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: unknown topic '{}' from subscriber {} -- disconnecting", topic_name, subscriber_id);
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
        return;
    }

    // Register subscriber, handle orphan.
    pubsub_itc_fw::ExternalWalSubscriberRegistry& registry = is_orders ? orders_registry_ : er_registry_;
    std::unordered_set<pubsub_itc_fw::ConnectionID>& live_set = is_orders ? orders_live_conn_ids_ : er_live_conn_ids_;

    const pubsub_itc_fw::ConnectionID orphan = registry.register_subscriber(conn_id, subscriber_id, from_seq_no);
    if (orphan.is_valid()) {
        live_set.erase(orphan);
        conn_to_topic_.erase(orphan);
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = orphan;
        get_reactor().enqueue_control_command(cmd);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: displaced orphan connection {} for subscriber_id={}", orphan.get_value(), subscriber_id);
    }

    live_set.insert(conn_id);
    conn_to_topic_[conn_id] = topic_name;

    const int64_t accepted = (from_seq_no == -1) ? wal_.last_seq_no() : from_seq_no;
    pubsub_itc_fw_app::TopicSubscribeAck ack{};
    ack.accepted_from_seq_no = accepted;
    send_pdu(conn_id, pdu_topic_subscribe_ack, 0, ack);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MepThread: TopicSubscribeAck sent subscriber_id={} topic={} accepted_from={}",
               subscriber_id, topic_name, accepted);

    // Replay WAL records from the accepted cursor.
    if (from_seq_no != -1) {
        replay_wal_for_subscriber(conn_id, from_seq_no, is_orders);
    }
}

void MatchingEnginePublisherThread::handle_topic_ack(
        const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::TopicAckView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                   consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MepThread: failed to decode TopicAck -- dropping");
        return;
    }

    const auto it = conn_to_topic_.find(conn_id);
    if (it == conn_to_topic_.end()) {
        return;
    }
    if (it->second == topic_orders) {
        orders_registry_.update_cursor(conn_id, view.last_seq_no);
    } else {
        er_registry_.update_cursor(conn_id, view.last_seq_no);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "MepThread: TopicAck conn={} last_seq_no={}", conn_id.get_value(), view.last_seq_no);
}

void MatchingEnginePublisherThread::stream_wal_record_to_topic_subscribers(
        int64_t seq_no, int16_t pdu_id, int64_t wall_time_ns,
        const uint8_t* pdu_payload, size_t pdu_size) {
    // Only the leader streams live records.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        return;
    }

    std::unordered_set<pubsub_itc_fw::ConnectionID>* live_set = nullptr;
    if (pdu_id == pdu_id_nos || pdu_id == pdu_id_ocr) {
        live_set = &orders_live_conn_ids_;
    } else if (pdu_id == pdu_id_er) {
        live_set = &er_live_conn_ids_;
    } else {
        return;
    }

    if (live_set->empty()) {
        return;
    }

    for (const auto& subscriber_conn_id : *live_set) {
        send_topic_page(subscriber_conn_id, seq_no, pdu_id, wall_time_ns, pdu_payload, pdu_size);
    }
}

void MatchingEnginePublisherThread::send_topic_page(
        const pubsub_itc_fw::ConnectionID& conn_id, int64_t seq_no, int16_t pdu_id,
        int64_t wall_time_ns, const uint8_t* pdu_payload, size_t pdu_size) {
    pubsub_itc_fw_app::TopicRecord record{};
    record.seq_no      = seq_no;
    record.pdu_id      = pdu_id;
    record.wall_time_ns = wall_time_ns;
    record.payload.data = pdu_payload;
    record.payload.size = pdu_size;

    pubsub_itc_fw_app::TopicPage page{};
    page.record_count  = 1;
    page.page_number   = 1;
    page.total_pages   = 1;
    page.records.data  = &record;
    page.records.size  = 1;

    send_pdu(conn_id, pdu_topic_page, seq_no, page);
}

void MatchingEnginePublisherThread::replay_wal_for_subscriber(
        const pubsub_itc_fw::ConnectionID& conn_id, int64_t from_seq_no, bool is_orders_topic) {
    int64_t records_sent = 0;
    [[maybe_unused]] auto end_pos = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, {0, 0},
        [this, &conn_id, from_seq_no, is_orders_topic, &records_sent](
                int64_t record_id, const void* payload, size_t size) {
            if (record_id <= from_seq_no) {
                return;
            }
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return;
            }
            int64_t wall_time_ns{};
            int16_t pdu_id{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));

            const bool is_orders_pdu = (pdu_id == pdu_id_nos || pdu_id == pdu_id_ocr);
            const bool is_er_pdu     = (pdu_id == pdu_id_er);
            if (is_orders_topic && !is_orders_pdu) {
                return;
            }
            if (!is_orders_topic && !is_er_pdu) {
                return;
            }

            const auto* pdu_payload = static_cast<const uint8_t*>(payload) + header_size;
            const size_t pdu_size   = size - header_size;
            send_topic_page(conn_id, record_id, pdu_id, wall_time_ns, pdu_payload, pdu_size);
            ++records_sent;
        });

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MepThread: WAL replay complete for conn={} topic={} records_sent={}",
               conn_id.get_value(), is_orders_topic ? topic_orders : topic_execution_reports,
               records_sent);
}

void MatchingEnginePublisherThread::disconnect_all_topic_subscribers() {
    for (const auto& conn_id : orders_live_conn_ids_) {
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }
    for (const auto& conn_id : er_live_conn_ids_) {
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }
    orders_live_conn_ids_.clear();
    er_live_conn_ids_.clear();
    conn_to_topic_.clear();
    orders_registry_ = pubsub_itc_fw::ExternalWalSubscriberRegistry{};
    er_registry_     = pubsub_itc_fw::ExternalWalSubscriberRegistry{};
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: disconnected all topic subscribers (role transition to follower)");
}

} // namespaces
