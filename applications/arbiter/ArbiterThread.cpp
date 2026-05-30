// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterThread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace arbiter {

namespace {

static constexpr int16_t pdu_status_query = 100;
static constexpr int16_t pdu_status_response = 101;
static constexpr int16_t pdu_heartbeat = 102;
static constexpr int16_t pdu_arbitration_report = 200;
static constexpr int16_t pdu_arbitration_decision = 201;
static constexpr int16_t pdu_arbiter_heartbeat = 300;
static constexpr int16_t pdu_arbiter_vote_request = 301;
static constexpr int16_t pdu_arbiter_vote_response = 302;
static constexpr int16_t pdu_arbiter_state_record = 400;
static constexpr int16_t pdu_arbiter_state_ack = 401;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const ArbiterConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "ArbiterPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "ArbiterPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

ArbiterThread::ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                             pubsub_itc_fw::Reactor& reactor, const ArbiterConfiguration& config)
    : ApplicationThread(token, logger, reactor, "ArbiterThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(),
                        make_allocator_config(config, logger), pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , peer_conn_id_{}
    , peer_inbound_conn_id_{}
    , witness_conn_id_{} {}

void ArbiterThread::on_initial_event() {
    // Arm startup election window.
    start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.startup_election_timeout_seconds));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: startup election timeout armed ({}s)",
               config_.startup_election_timeout_seconds);
}

void ArbiterThread::on_app_ready_event() {
    connect_to_service("peer");
    connect_to_service("witness");
}

void ArbiterThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    if (svc == "peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: outbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
    } else if (svc == "witness") {
        witness_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: witness connection {} established", id.get_value());
        start_recurring_timer("witness_heartbeat", std::chrono::seconds(config_.witness_heartbeat_interval_seconds));
        send_witness_heartbeat();
    } else {
        // Inbound connection from a component (sequencer primary, sequencer secondary, ME, etc.)
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component connection {} established ({})", id.get_value(), svc);
    }
}

void ArbiterThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: outbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: inbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == witness_conn_id_) {
        witness_conn_id_ = pubsub_itc_fw::ConnectionID{};
        cancel_timer("witness_heartbeat");
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: witness connection {} lost: {}", id.get_value(), reason);
    } else {
        // Component connection lost -- remove from tracking maps.
        const auto conn_it = conn_to_component_instance_.find(id.get_value());
        if (conn_it != conn_to_component_instance_.end()) {
            const int64_t instance_id = conn_it->second;
            component_connections_.erase(instance_id);
            conn_to_component_instance_.erase(conn_it);
            pending_requests_.erase(instance_id);
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component instance_id={} disconnected", instance_id);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: inbound connection {} lost: {}", id.get_value(), reason);
        }
    }
}

void ArbiterThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();

    const bool is_peer_pdu = (conn_id == peer_conn_id_) || (conn_id == peer_inbound_conn_id_);
    if (is_peer_pdu) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    if (conn_id == witness_conn_id_) {
        const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());
        if (pdu_id == pdu_arbiter_vote_response) {
            handle_arbiter_vote_response(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from witness -- dropping", pdu_id);
        }
        release_pdu_payload(message);
        return;
    }

    // Component PDU (sequencer, ME, or other registered component).
    const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());
    if (pdu_id == pdu_heartbeat) {
        handle_component_heartbeat(conn_id, message);
    } else if (pdu_id == pdu_arbitration_report) {
        handle_arbitration_report(conn_id, message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from component on connection {} -- dropping",
                   pdu_id, conn_id.get_value());
    }
    release_pdu_payload(message);
}

void ArbiterThread::on_timer_event(const std::string& name) {
    if (name == "peer_heartbeat") {
        send_peer_heartbeat();
        return;
    }

    if (name == "witness_heartbeat") {
        send_witness_heartbeat();
        return;
    }

    if (name == "peer_heartbeat_timeout") {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return; // already active, nothing to do
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: peer heartbeat timeout (role={})", pubsub_itc_fw_app::to_string(role_));

        if (witness_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: requesting vote from witness");
            request_witness_vote();
            start_one_off_timer("vote_timeout", std::chrono::seconds(config_.vote_timeout_seconds));
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: witness not connected -- self-promoting using instance-id rule");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }

    if (name == "vote_timeout") {
        if (role_ != pubsub_itc_fw_app::Role::leader && role_ != pubsub_itc_fw_app::Role::follower) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: vote timeout -- witness unreachable, self-promoting");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }
}

void ArbiterThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// ---------------------------------------------------------------------------
// Arbiter-pair election helpers (mirror sequencer peer protocol)
// ---------------------------------------------------------------------------

pubsub_itc_fw::ConnectionID ArbiterThread::peer_active_conn() const {
    if (peer_conn_id_.is_valid())
        return peer_conn_id_;
    return peer_inbound_conn_id_;
}

void ArbiterThread::adopt_role(pubsub_itc_fw_app::Role new_role) {
    if (new_role == role_)
        return;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        write_fence_file();
        cancel_timer("peer_heartbeat_timeout");
        cancel_timer("vote_timeout");
        start_recurring_timer("peer_heartbeat", std::chrono::seconds(config_.heartbeat_interval_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: now ACTIVE -- heartbeat timer started ({}s interval)",
                   config_.heartbeat_interval_seconds);
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        start_recurring_timer("peer_heartbeat", std::chrono::seconds(config_.heartbeat_interval_seconds));
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.heartbeat_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: now PASSIVE -- heartbeat timer started, timeout armed ({}s)",
                   config_.heartbeat_timeout_seconds);
    }
}

void ArbiterThread::elect_role(int64_t peer_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role) {
    if (role_ == pubsub_itc_fw_app::Role::leader || role_ == pubsub_itc_fw_app::Role::follower) {
        if (peer_epoch > epoch_) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: peer epoch {} > my epoch {} (already elected as {})", peer_epoch,
                       epoch_, pubsub_itc_fw_app::to_string(role_));
        }
        return;
    }

    if (peer_epoch > epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "ArbiterThread: peer epoch {} > my epoch {} -- adopting passive (peer is newer generation)", peer_epoch, epoch_);
        epoch_ = peer_epoch;
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }

    if (peer_current_role == pubsub_itc_fw_app::Role::leader) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: peer (instance_id={}) is already active -- adopting passive", peer_id);
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }

    if (static_cast<int64_t>(config_.instance_id) < peer_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: my instance_id={} < peer instance_id={} -- adopting active",
                   config_.instance_id, peer_id);
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: my instance_id={} >= peer instance_id={} -- adopting passive",
                   config_.instance_id, peer_id);
        adopt_role(pubsub_itc_fw_app::Role::follower);
    }
}

void ArbiterThread::send_status_query(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusQuery sq{};
    sq.instance_id = static_cast<int64_t>(config_.instance_id);
    sq.epoch = epoch_;
    send_pdu(conn_id, pdu_status_query, 0, sq);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: StatusQuery sent on connection {} (instance_id={} epoch={})",
               conn_id.get_value(), sq.instance_id, sq.epoch);
}

void ArbiterThread::send_status_response(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusResponse sr{};
    sr.self_instance_id = static_cast<int64_t>(config_.instance_id);
    sr.peer_instance_id = 0;
    sr.epoch = epoch_;
    sr.current_role = role_;
    send_pdu(conn_id, pdu_status_response, 0, sr);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: StatusResponse sent on connection {} (role={} epoch={})", conn_id.get_value(),
               pubsub_itc_fw_app::to_string(role_), epoch_);
}

void ArbiterThread::send_peer_heartbeat() {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: heartbeat timer fired but no peer connection -- skipping");
        return;
    }
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    send_pdu(target, pdu_heartbeat, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: Heartbeat sent to peer (epoch={})", epoch_);
}

void ArbiterThread::send_witness_heartbeat() {
    if (!witness_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::ArbiterHeartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    send_pdu(witness_conn_id_, pdu_arbiter_heartbeat, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: ArbiterHeartbeat sent to witness (instance_id={} epoch={})",
               hb.instance_id, hb.epoch);
}

void ArbiterThread::request_witness_vote() {
    if (!witness_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::ArbiterVoteRequest req{};
    req.self_instance_id = static_cast<int64_t>(config_.instance_id);
    req.peer_instance_id = peer_instance_id_;
    req.epoch = epoch_;
    send_pdu(witness_conn_id_, pdu_arbiter_vote_request, 0, req);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbiterVoteRequest sent to witness (self_instance_id={} peer_instance_id={} epoch={})", req.self_instance_id,
               req.peer_instance_id, req.epoch);
}

void ArbiterThread::write_fence_file() const {
    const std::string& path = config_.fence_file_path;
    const std::string content = std::to_string(config_.instance_id) + "\n";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to write fence file '{}': {}", path, std::strerror(errno));
        return;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: fence file written: {}", path);
}

// ---------------------------------------------------------------------------
// Peer PDU handlers
// ---------------------------------------------------------------------------

void ArbiterThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());

    if (pdu_id == pdu_status_query) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pdu_status_response) {
        handle_peer_status_response(message);
    } else if (pdu_id == pdu_heartbeat) {
        handle_peer_heartbeat(message);
    } else if (pdu_id == pdu_arbiter_state_record) {
        handle_arbiter_state_record(message);
    } else if (pdu_id == pdu_arbiter_state_ack) {
        handle_arbiter_state_ack(message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unknown peer PDU id {} -- dropping", pdu_id);
    }
}

void ArbiterThread::handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::StatusQueryView sq{};

    if (!pubsub_itc_fw_app::decode(sq, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode StatusQuery -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: StatusQuery received from peer (instance_id={} epoch={})", sq.instance_id,
               sq.epoch);

    peer_instance_id_ = sq.instance_id;
    send_status_response(conn_id);
    elect_role(sq.instance_id, sq.epoch, pubsub_itc_fw_app::Role::unknown);
}

void ArbiterThread::handle_peer_status_response(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::StatusResponseView sr{};

    if (!pubsub_itc_fw_app::decode(sr, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode StatusResponse -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: StatusResponse received from peer (self_id={} epoch={} role={})",
               sr.self_instance_id, sr.epoch, pubsub_itc_fw_app::to_string(sr.current_role));

    peer_instance_id_ = sr.self_instance_id;
    elect_role(sr.self_instance_id, sr.epoch, sr.current_role);
}

void ArbiterThread::handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::HeartbeatView hb{};

    if (!pubsub_itc_fw_app::decode(hb, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode Heartbeat -- dropping");
        return;
    }

    if (hb.epoch < epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: Heartbeat from stale peer (epoch={} < my epoch={}) -- ignoring",
                   hb.epoch, epoch_);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: Heartbeat received from peer (instance_id={} epoch={})", hb.instance_id,
               hb.epoch);

    if (role_ == pubsub_itc_fw_app::Role::follower) {
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.heartbeat_timeout_seconds));
    }
}

void ArbiterThread::handle_arbiter_state_record(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbiterStateRecordView record{};

    if (!pubsub_itc_fw_app::decode(record, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode ArbiterStateRecord -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbiterStateRecord received (component={} leader={} epoch={})", record.component_instance_id, record.leader_instance_id,
               record.epoch);

    leadership_state_[record.component_instance_id] = ComponentState{record.leader_instance_id, 0, record.epoch};

    // Acknowledge the replication record.
    const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
    if (peer.is_valid()) {
        pubsub_itc_fw_app::ArbiterStateAck ack{};
        ack.component_instance_id = record.component_instance_id;
        ack.epoch = record.epoch;
        send_pdu(peer, pdu_arbiter_state_ack, 0, ack);
    }
}

void ArbiterThread::handle_arbiter_state_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbiterStateAckView ack{};

    if (!pubsub_itc_fw_app::decode(ack, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode ArbiterStateAck -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: ArbiterStateAck received (component={} epoch={})",
               ack.component_instance_id, ack.epoch);
}

// ---------------------------------------------------------------------------
// Witness PDU handlers
// ---------------------------------------------------------------------------

void ArbiterThread::handle_arbiter_vote_response(const pubsub_itc_fw::EventMessage& message) {
    cancel_timer("vote_timeout");

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbiterVoteResponseView resp{};

    if (!pubsub_itc_fw_app::decode(resp, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode ArbiterVoteResponse -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: ArbiterVoteResponse received (granted_to={} epoch={})",
               resp.granted_to_instance_id, resp.epoch);

    epoch_ = resp.epoch;

    if (resp.granted_to_instance_id == static_cast<int64_t>(config_.instance_id)) {
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else {
        adopt_role(pubsub_itc_fw_app::Role::follower);
    }
}

// ---------------------------------------------------------------------------
// Component PDU handlers
// ---------------------------------------------------------------------------

void ArbiterThread::handle_component_heartbeat(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::HeartbeatView hb{};

    if (!pubsub_itc_fw_app::decode(hb, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode component Heartbeat -- dropping");
        return;
    }

    const bool already_known = conn_to_component_instance_.count(conn_id.get_value()) > 0;
    conn_to_component_instance_[conn_id.get_value()] = hb.instance_id;
    component_connections_[hb.instance_id] = conn_id;

    if (!already_known) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component instance_id={} registered on connection {} (epoch={})",
                   hb.instance_id, conn_id.get_value(), hb.epoch);

        // If we already have a decision for this component, send it now so it doesn't
        // have to wait for the peer to also connect before getting its role.
        const auto it = leadership_state_.find(hb.instance_id);
        if (it != leadership_state_.end() && role_ == pubsub_itc_fw_app::Role::leader) {
            const ComponentState& state = it->second;
            send_arbitration_decision(conn_id, state.leader_instance_id, state.follower_instance_id, state.epoch);
        }
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: component Heartbeat from instance_id={} (epoch={})", hb.instance_id,
                   hb.epoch);
    }
}

void ArbiterThread::handle_arbitration_report(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbitrationReportView report{};

    if (!pubsub_itc_fw_app::decode(report, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode ArbitrationReport -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbitrationReport from component instance_id={} (peer={} epoch={} proposed_role={})", report.self_instance_id,
               report.peer_instance_id, report.epoch, pubsub_itc_fw_app::to_string(report.proposed_role));

    if (role_ != pubsub_itc_fw_app::Role::leader) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: ArbitrationReport received but I am {} (not active) -- dropping; component should retry with active arbiter",
                   pubsub_itc_fw_app::to_string(role_));
        return;
    }

    decide_and_broadcast(report.self_instance_id, report.peer_instance_id, report.epoch, conn_id);
}

// ---------------------------------------------------------------------------
// Decision helpers
// ---------------------------------------------------------------------------

void ArbiterThread::decide_and_broadcast(int64_t self_instance_id, int64_t peer_instance_id, int32_t epoch,
                                         const pubsub_itc_fw::ConnectionID& requester_conn_id) {
    const bool peer_connected = component_connections_.count(peer_instance_id) > 0;
    const int64_t leader_id = peer_connected ? std::min(self_instance_id, peer_instance_id) : self_instance_id;
    const int64_t follower_id = (leader_id == self_instance_id) ? peer_instance_id : self_instance_id;
    const int32_t new_epoch = epoch + 1;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: decision: leader={} follower={} epoch={} (peer_connected={})", leader_id, follower_id, new_epoch, peer_connected);

    // Store in leadership-state map.
    leadership_state_[self_instance_id] = ComponentState{leader_id, follower_id, new_epoch};

    // Send to the requesting component.
    send_arbitration_decision(requester_conn_id, leader_id, follower_id, new_epoch);

    // Send to the peer if it is connected.
    const auto peer_it = component_connections_.find(peer_instance_id);
    if (peer_it != component_connections_.end()) {
        send_arbitration_decision(peer_it->second, leader_id, follower_id, new_epoch);
    }

    // Replicate to passive arbiter.
    replicate_state_to_peer(self_instance_id, leader_id, new_epoch);
}

void ArbiterThread::send_arbitration_decision(const pubsub_itc_fw::ConnectionID& conn_id, int64_t leader_id, int64_t follower_id, int32_t epoch) {
    pubsub_itc_fw_app::ArbitrationDecision decision{};
    decision.leader_instance_id = leader_id;
    decision.follower_instance_id = follower_id;
    decision.epoch = epoch;
    send_pdu(conn_id, pdu_arbitration_decision, 0, decision);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbitrationDecision sent to connection {} (leader={} follower={} epoch={})", conn_id.get_value(), leader_id, follower_id,
               epoch);
}

void ArbiterThread::replicate_state_to_peer(int64_t component_instance_id, int64_t leader_id, int32_t epoch) {
    const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
    if (!peer.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: no peer connection -- skipping state replication");
        return;
    }
    pubsub_itc_fw_app::ArbiterStateRecord record{};
    record.component_instance_id = component_instance_id;
    record.leader_instance_id = leader_id;
    record.epoch = epoch;
    send_pdu(peer, pdu_arbiter_state_record, 0, record);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "ArbiterThread: ArbiterStateRecord replicated to peer (component={} leader={} epoch={})", component_instance_id, leader_id, epoch);
}

} // namespace arbiter
