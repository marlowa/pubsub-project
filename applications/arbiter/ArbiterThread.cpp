// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterThread.hpp"

#include <algorithm>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace arbiter {

namespace {

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

} // un-named namespace

ArbiterThread::ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                             const ArbiterConfiguration& config)
    : ApplicationThread(token, logger, reactor, "ArbiterThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , peer_conn_id_{}
    , peer_inbound_conn_id_{}
    , witness_conn_id_{} {}

void ArbiterThread::on_initial_event() {
    // Arm startup election window.
    peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.startup_election_timeout_seconds));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: startup election timeout armed ({}s)", config_.startup_election_timeout_seconds);
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
        replay_leadership_to_peer(id);
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
        replay_leadership_to_peer(id);
    } else if (svc == "witness") {
        witness_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: witness connection {} established", id.get_value());
        cancel_timer(witness_heartbeat_timer_id_);
        witness_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.witness_heartbeat_interval_seconds));
        send_witness_heartbeat();
    } else {
        // Inbound connection from a component (sequencer primary, sequencer secondary, ME, etc.)
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component connection {} established ({})", id.get_value(), svc);
    }
}

void ArbiterThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: outbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: inbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == witness_conn_id_) {
        witness_conn_id_ = pubsub_itc_fw::ConnectionID{};
        cancel_timer(witness_heartbeat_timer_id_);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: witness connection {} lost: {}", id.get_value(), reason);
    } else {
        // Component connection lost -- remove from tracking maps.
        const auto conn_it = conn_to_component_instance_.find(id.get_value());
        if (conn_it != conn_to_component_instance_.end()) {
            const ComponentKey key = conn_it->second;
            component_connections_.erase(key);
            conn_to_component_instance_.erase(conn_it);
            pending_requests_.erase(key);

            // If the instance that just left was the one recorded as leading, stop believing
            // it. The record exists to stop leadership being moved away from an instance that
            // is SERVING, and one that has gone is not -- it may come back having lost
            // everything it held. It becomes an incumbent again only by leasing again.
            const auto known = leadership_state_.find(key.group);
            if (known != leadership_state_.end() && known->second.leader_instance_id == key.instance_id && known->second.leadership_confirmed) {
                known->second.leadership_confirmed = false;
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                           "ArbiterThread: group={} leader instance {} disconnected -- no longer treated as the incumbent until it leases again",
                           pubsub_itc_fw_app::to_string(key.group), key.instance_id);
            }
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component group={} instance_id={} disconnected",
                       pubsub_itc_fw_app::to_string(key.group), key.instance_id);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: inbound connection {} lost: {}", id.get_value(), reason);
        }
    }
}

void ArbiterThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();

    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    if (conn_id == witness_conn_id_) {
        const auto pdu_id = message.pdu_id();
        if (pdu_id == pubsub_itc_fw_app::ArbiterVoteResponse::message_pdu_id) {
            handle_arbiter_vote_response(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from witness -- dropping", pdu_id);
        }
        release_pdu_payload(message);
        return;
    }

    // Component PDU (sequencer, ME, or other registered component).
    const auto pdu_id = message.pdu_id();
    if (pdu_id == pubsub_itc_fw_app::Heartbeat::message_pdu_id) {
        handle_component_heartbeat(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::LeadershipLease::message_pdu_id) {
        handle_leadership_lease(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::ArbitrationReport::message_pdu_id) {
        handle_arbitration_report(conn_id, message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from component on connection {} -- dropping",
                   pdu_id, conn_id.get_value());
    }
    release_pdu_payload(message);
}

void ArbiterThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == peer_heartbeat_timer_id_) {
        send_peer_heartbeat();
        return;
    }

    if (id == witness_heartbeat_timer_id_) {
        send_witness_heartbeat();
        return;
    }

    if (id == peer_heartbeat_timeout_timer_id_) {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return; // already active, nothing to do
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: peer heartbeat timeout (role={})", pubsub_itc_fw_app::to_string(role_));

        if (witness_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: requesting vote from witness");
            request_witness_vote();
            vote_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.vote_timeout_seconds));
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: witness not connected -- self-promoting using instance-id rule");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }

    if (id == vote_timeout_timer_id_) {
        if (role_ != pubsub_itc_fw_app::Role::leader && role_ != pubsub_itc_fw_app::Role::follower) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: vote timeout -- witness unreachable, self-promoting");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }
}

void ArbiterThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// Arbiter-pair election helpers (mirror sequencer peer protocol)

pubsub_itc_fw::ConnectionID ArbiterThread::peer_active_conn() const {
    if (peer_conn_id_.is_valid()) {
        return peer_conn_id_;
    }
    return peer_inbound_conn_id_;
}

void ArbiterThread::adopt_role(pubsub_itc_fw_app::Role new_role) {
    if (new_role == role_) {
        return;
    }

    const auto transition_level = (role_ == pubsub_itc_fw_app::Role::unknown) ? pubsub_itc_fw::FwLogLevel::Info : pubsub_itc_fw::FwLogLevel::Warning;
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), transition_level, "ArbiterThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        cancel_timer(vote_timeout_timer_id_);
        cancel_timer(peer_heartbeat_timer_id_);
        peer_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.heartbeat_interval_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: now ACTIVE -- heartbeat timer started ({}s interval)",
                   config_.heartbeat_interval_seconds);
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        cancel_timer(peer_heartbeat_timer_id_);
        peer_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.heartbeat_interval_seconds));
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: peer epoch {} > my epoch {} -- adopting passive (peer is newer generation)",
                   peer_epoch, epoch_);
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
    send_pdu(conn_id, pubsub_itc_fw_app::StatusQuery::message_pdu_id, 0, sq);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: StatusQuery sent on connection {} (instance_id={} epoch={})", conn_id.get_value(),
               sq.instance_id, sq.epoch);
}

void ArbiterThread::send_status_response(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusResponse sr{};
    sr.self_instance_id = static_cast<int64_t>(config_.instance_id);
    sr.peer_instance_id = 0;
    sr.epoch = epoch_;
    sr.current_role = role_;
    send_pdu(conn_id, pubsub_itc_fw_app::StatusResponse::message_pdu_id, 0, sr);
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
    send_pdu(target, pubsub_itc_fw_app::Heartbeat::message_pdu_id, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: Heartbeat sent to peer (epoch={})", epoch_);
}

void ArbiterThread::send_witness_heartbeat() {
    if (!witness_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::ArbiterHeartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    send_pdu(witness_conn_id_, pubsub_itc_fw_app::ArbiterHeartbeat::message_pdu_id, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: ArbiterHeartbeat sent to witness (instance_id={} epoch={})", hb.instance_id,
               hb.epoch);
}

void ArbiterThread::request_witness_vote() {
    if (!witness_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::ArbiterVoteRequest req{};
    req.self_instance_id = static_cast<int64_t>(config_.instance_id);
    req.peer_instance_id = peer_instance_id_;
    req.epoch = epoch_;
    send_pdu(witness_conn_id_, pubsub_itc_fw_app::ArbiterVoteRequest::message_pdu_id, 0, req);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbiterVoteRequest sent to witness (self_instance_id={} peer_instance_id={} epoch={})", req.self_instance_id,
               req.peer_instance_id, req.epoch);
}

// Peer PDU handlers

void ArbiterThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();

    if (pdu_id == pubsub_itc_fw_app::StatusQuery::message_pdu_id) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::StatusResponse::message_pdu_id) {
        handle_peer_status_response(message);
    } else if (pdu_id == pubsub_itc_fw_app::Heartbeat::message_pdu_id) {
        handle_peer_heartbeat(message);
    } else if (pdu_id == pubsub_itc_fw_app::ArbiterStateRecord::message_pdu_id) {
        handle_arbiter_state_record(message);
    } else if (pdu_id == pubsub_itc_fw_app::ArbiterStateAck::message_pdu_id) {
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: Heartbeat from stale peer (epoch={} < my epoch={}) -- ignoring", hb.epoch,
                   epoch_);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: Heartbeat received from peer (instance_id={} epoch={})", hb.instance_id,
               hb.epoch);

    if (role_ == pubsub_itc_fw_app::Role::follower) {
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
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

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: ArbiterStateRecord received (group={} component={} leader={} epoch={})",
               pubsub_itc_fw_app::to_string(record.group), record.component_instance_id, record.leader_instance_id, record.epoch);

    leadership_state_[record.group] = ComponentState{record.leader_instance_id, 0, record.epoch, true, std::chrono::steady_clock::now()};

    // Acknowledge the replication record.
    const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
    if (peer.is_valid()) {
        pubsub_itc_fw_app::ArbiterStateAck ack{};
        ack.component_instance_id = record.component_instance_id;
        ack.epoch = record.epoch;
        ack.group = record.group;
        send_pdu(peer, pubsub_itc_fw_app::ArbiterStateAck::message_pdu_id, 0, ack);
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

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: ArbiterStateAck received (group={} component={} epoch={})",
               pubsub_itc_fw_app::to_string(ack.group), ack.component_instance_id, ack.epoch);
}

// Witness PDU handlers

void ArbiterThread::handle_arbiter_vote_response(const pubsub_itc_fw::EventMessage& message) {
    cancel_timer(vote_timeout_timer_id_);

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

// Component PDU handlers

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

    const ComponentKey key{hb.group, hb.instance_id};
    const bool already_known = conn_to_component_instance_.count(conn_id.get_value()) > 0;
    conn_to_component_instance_[conn_id.get_value()] = key;
    component_connections_[key] = conn_id;

    if (!already_known) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component group={} instance_id={} registered on connection {} (epoch={})",
                   pubsub_itc_fw_app::to_string(hb.group), hb.instance_id, conn_id.get_value(), hb.epoch);

        // If we already have a decision for this component, send it now so it doesn't
        // have to wait for the peer to also connect before getting its role.
        // Only a CONFIRMED record may be handed out. An unconfirmed one names an instance
        // that held leadership once and has since gone, and telling a reconnecting instance
        // that it leads on the strength of it is how the stale belief comes back to life: the
        // instance believes it, leases, and re-confirms the very record that was invalidated
        // when it disconnected. Observed doing exactly that before this check existed.
        const auto it = leadership_state_.find(hb.group);
        const bool usable =
            it != leadership_state_.end() && it->second.leadership_confirmed && std::chrono::steady_clock::now() - it->second.leased_at <= leadership_lease_ttl;
        if (usable && role_ == pubsub_itc_fw_app::Role::leader) {
            const ComponentState& state = it->second;
            send_arbitration_decision(conn_id, hb.group, state.leader_instance_id, state.follower_instance_id, state.epoch);
        }
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: component Heartbeat from group={} instance_id={} (epoch={})",
                   pubsub_itc_fw_app::to_string(hb.group), hb.instance_id, hb.epoch);
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
               "ArbiterThread: ArbitrationReport from group={} instance_id={} (peer={} epoch={} proposed_role={})", pubsub_itc_fw_app::to_string(report.group),
               report.self_instance_id, report.peer_instance_id, report.epoch, pubsub_itc_fw_app::to_string(report.proposed_role));

    if (role_ != pubsub_itc_fw_app::Role::leader) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: ArbitrationReport received but I am {} (not active) -- dropping; component should retry with active arbiter",
                   pubsub_itc_fw_app::to_string(role_));
        return;
    }

    decide_and_broadcast(report.group, report.self_instance_id, report.peer_instance_id, report.epoch, conn_id);
}

// Decision helpers

void ArbiterThread::decide_and_broadcast(pubsub_itc_fw_app::ComponentGroup group, int64_t self_instance_id, int64_t peer_instance_id, int32_t epoch,
                                         const pubsub_itc_fw::ConnectionID& requester_conn_id) {
    const ComponentKey peer_key{group, peer_instance_id};

    LeadershipDecision::Inputs inputs;
    inputs.self_instance_id = self_instance_id;
    inputs.peer_instance_id = peer_instance_id;
    inputs.peer_connected = component_connections_.count(peer_key) > 0;
    inputs.reported_epoch = epoch;

    const auto incumbent = leadership_state_.find(group);
    const bool lease_expired = incumbent != leadership_state_.end() && std::chrono::steady_clock::now() - incumbent->second.leased_at > leadership_lease_ttl;
    if (incumbent != leadership_state_.end() && incumbent->second.leadership_confirmed && !lease_expired) {
        inputs.has_incumbent = true;
        inputs.incumbent_instance_id = incumbent->second.leader_instance_id;
        inputs.incumbent_epoch = incumbent->second.epoch;
        const ComponentKey incumbent_key{group, incumbent->second.leader_instance_id};
        inputs.incumbent_connected = component_connections_.count(incumbent_key) > 0;
    }

    // An arbiter that has just started has an empty map whether or not there is genuinely no
    // leader, and cannot tell those apart. Guessing means applying the cold-start tie-break,
    // which after a failover hands leadership to the lower instance id -- the instance that
    // just restarted holding nothing. Declining costs the asker a retry; guessing costs the
    // venue its book. See docs/availability/design_notes.md 11c.
    if (!inputs.has_incumbent && within_startup_learning_period()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: asked to arbitrate group={} by instance {} but nothing is known about it yet and this arbiter started "
                   "recently -- declining rather than guessing; the component should retry",
                   pubsub_itc_fw_app::to_string(group), self_instance_id);
        return;
    }

    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    const int64_t leader_id = decision.leader_instance_id;
    const int64_t follower_id = decision.follower_instance_id;
    const int32_t new_epoch = decision.epoch;

    // The two cases are logged apart because they mean different things to whoever is reading:
    // a confirmation is the arbiter refusing to move leadership, which is the interesting event
    // when an instance has just come back.
    if (decision.leadership_changed) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: decision: group={} leader={} follower={} epoch={} (peer_connected={})",
                   pubsub_itc_fw_app::to_string(group), leader_id, follower_id, new_epoch, inputs.peer_connected);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "ArbiterThread: decision: group={} leader={} unchanged -- incumbent still connected; follower={} epoch={} (asked by instance {})",
                   pubsub_itc_fw_app::to_string(group), leader_id, follower_id, new_epoch, self_instance_id);
    }

    leadership_state_[group] = ComponentState{leader_id, follower_id, new_epoch, true, std::chrono::steady_clock::now()};

    // Send to the requesting component.
    send_arbitration_decision(requester_conn_id, group, leader_id, follower_id, new_epoch);

    // Send to the peer if it is connected.
    const auto peer_it = component_connections_.find(peer_key);
    if (peer_it != component_connections_.end()) {
        send_arbitration_decision(peer_it->second, group, leader_id, follower_id, new_epoch);
    }

    // Replicate to passive arbiter.
    replicate_state_to_peer(group, self_instance_id, leader_id, new_epoch);
}

void ArbiterThread::send_arbitration_decision(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::ComponentGroup group, int64_t leader_id,
                                              int64_t follower_id, int32_t epoch) {
    pubsub_itc_fw_app::ArbitrationDecision decision{};
    decision.leader_instance_id = leader_id;
    decision.follower_instance_id = follower_id;
    decision.epoch = epoch;
    decision.group = group;
    send_pdu(conn_id, pubsub_itc_fw_app::ArbitrationDecision::message_pdu_id, 0, decision);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbitrationDecision sent to connection {} (group={} leader={} follower={} epoch={})", conn_id.get_value(),
               pubsub_itc_fw_app::to_string(group), leader_id, follower_id, epoch);
}

void ArbiterThread::handle_leadership_lease(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeadershipLeaseView lease{};

    if (!pubsub_itc_fw_app::decode(lease, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeadershipLease -- dropping");
        return;
    }

    // This is how an arbiter that has restarted learns who leads. It holds that knowledge only
    // in memory and reads nothing back at startup, so rather than persisting it, it is told --
    // by the only party entitled to assert leadership -- and the epoch settles disagreement
    // between two instances that both believe they lead.
    const auto known = leadership_state_.find(lease.group);
    if (known != leadership_state_.end() && lease.epoch < known->second.epoch) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: LeadershipLease from group={} instance {} at epoch {} is behind the {} on record -- refused",
                   pubsub_itc_fw_app::to_string(lease.group), lease.instance_id, lease.epoch, known->second.epoch);
        return;
    }

    const bool changed = known == leadership_state_.end() || known->second.leader_instance_id != lease.instance_id;
    const bool was_unconfirmed = known == leadership_state_.end() || !known->second.leadership_confirmed;
    ComponentState& state = leadership_state_[lease.group];
    // The follower is not carried on a lease -- it names only the instance asserting
    // leadership -- so an existing follower id is preserved rather than overwritten with zero.
    const int64_t known_follower = (known == leadership_state_.end() || changed) ? 0 : known->second.follower_instance_id;
    state = ComponentState{lease.instance_id, known_follower, lease.epoch, true, std::chrono::steady_clock::now()};
    if (changed || was_unconfirmed) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: group={} leadership confirmed for instance {} at epoch {} by its lease",
                   pubsub_itc_fw_app::to_string(lease.group), lease.instance_id, lease.epoch);
    }
    static_cast<void>(conn_id);
}

void ArbiterThread::replay_leadership_to_peer(const pubsub_itc_fw::ConnectionID& conn_id) {
    // Sent when a peer link comes up. A peer that has just restarted knows nothing, and
    // waiting for the next heartbeat from every leader would leave it ignorant for a whole
    // heartbeat interval. ArbiterStateRecord already exists and the peer already applies it,
    // so this is a replay of the same records rather than a new mechanism.
    if (leadership_state_.empty()) {
        return;
    }
    for (const auto& entry : leadership_state_) {
        pubsub_itc_fw_app::ArbiterStateRecord record{};
        record.component_instance_id = entry.second.leader_instance_id;
        record.leader_instance_id = entry.second.leader_instance_id;
        record.epoch = entry.second.epoch;
        record.group = entry.first;
        send_pdu(conn_id, pubsub_itc_fw_app::ArbiterStateRecord::message_pdu_id, 0, record);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: replayed {} leadership record(s) to peer connection {}", leadership_state_.size(),
               conn_id.get_value());
}

void ArbiterThread::replicate_state_to_peer(pubsub_itc_fw_app::ComponentGroup group, int64_t component_instance_id, int64_t leader_id, int32_t epoch) {
    const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
    if (!peer.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: no peer connection -- skipping state replication");
        return;
    }
    pubsub_itc_fw_app::ArbiterStateRecord record{};
    record.component_instance_id = component_instance_id;
    record.leader_instance_id = leader_id;
    record.epoch = epoch;
    record.group = group;
    send_pdu(peer, pubsub_itc_fw_app::ArbiterStateRecord::message_pdu_id, 0, record);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "ArbiterThread: ArbiterStateRecord replicated to peer (group={} component={} leader={} epoch={})", pubsub_itc_fw_app::to_string(group),
               component_instance_id, leader_id, epoch);
}

} // namespaces
