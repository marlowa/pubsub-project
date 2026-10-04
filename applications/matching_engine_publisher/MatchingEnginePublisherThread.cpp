// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEnginePublisherThread.hpp"

#include <LeaderStatement.hpp>
#include <PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <topics.hpp>
#include <topics_registry.hpp>

namespace matching_engine_publisher {

// Recognised topics and the pdu ids that belong to each are owned by the generated
// registry (topics_registry.hpp, from pubsub.dsl): pubsub_itc_fw_app::Topic,
// topic_from_name(), pdu_in_topic(). No topic names or application pdu ids are
// hardcoded here.

namespace {

// The arbiter pool's identity as a voter in deciding which publisher leads. The publishers are 1 and 2.
constexpr int64_t arbiter_pool_voter_id = 3;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 64;
    return cfg;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const MatchingEnginePublisherConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name = "MepPool";
    cfg.objects_per_pool = config.event_queue_pool_objects_per_slab;
    cfg.initial_pools = config.event_queue_pool_initial_slabs;
    cfg.handler_for_pool_exhausted = [&logger](void*, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "MepPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return cfg;
}

} // namespaces

MatchingEnginePublisherThread::MatchingEnginePublisherThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                                             pubsub_itc_fw::Reactor& reactor, const MatchingEnginePublisherConfiguration& config)
    : ApplicationThread(token, logger, reactor, "MepThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , wal_subscriber_id_("mep_" + std::to_string(config.instance_id))
    , orders_inbound_svc_("inbound:" + std::to_string(config.orders_listen_port))
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , peer_inbound_svc_("inbound:" + std::to_string(config.peer_listen_port))
    , orders_publisher_(
          *this, std::string(pubsub_itc_fw_app::to_string(pubsub_itc_fw_app::Topic::orders)),
          [](int16_t pdu_id) { return pubsub_itc_fw_app::pdu_in_topic(pdu_id, pubsub_itc_fw_app::Topic::orders); }, config.wal_directory)
    , er_publisher_(
          *this, std::string(pubsub_itc_fw_app::to_string(pubsub_itc_fw_app::Topic::execution_reports)),
          [](int16_t pdu_id) { return pubsub_itc_fw_app::pdu_in_topic(pdu_id, pubsub_itc_fw_app::Topic::execution_reports); }, config.wal_directory) {}

void MatchingEnginePublisherThread::on_initial_event() {
    // As the sequencer does, and for the same reason: the log's helper thread would otherwise
    // inherit this thread's processor, which is reserved for the order path. See BUG-0093.
    pubsub_itc_fw::CpuLayout helper_layout;
    const auto [helper_layout_loaded, helper_layout_error] = helper_layout.load(config_.cpu_layout_file, config_.cpu_layout_component);
    if (helper_layout_loaded) {
        wal_.set_helper_cores(helper_layout.background_cores());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MepThread: could not read the CPU layout, so the log's helper thread stays where it lands: {}", helper_layout_error);
    }

    const int64_t recovered_seq = wal_.open(config_.wal_directory, config_.wal_segment_size, nullptr);
    if (recovered_seq > 0) {
        sequencer_cursor_ = recovered_seq;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: WAL open complete: recovered seq_no={} record_count={}", recovered_seq,
                   wal_.record_count());
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: WAL is fresh");
    }

    wal_snapshot_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.snapshot_interval_seconds));

    // Publish nothing until this instance actually becomes leader (adopt_role flips this).
    set_publisher_role(pubsub_itc_fw_app::Role::unknown);

    if (!config_.ha_enabled) {
        ++epoch_;
        adopt_role(pubsub_itc_fw_app::Role::leader);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: ha_enabled=false -- starting as leader");
    } else {
        // Instance ids are 1 for the primary and 2 for the secondary, so the peer is the other one.
        // The publisher keeps no epoch on disk, so it starts knowing none.
        const int64_t self_id = static_cast<int64_t>(config_.instance_id);
        const int64_t peer_id = self_id == 1 ? 2 : 1;
        lease_agent_.emplace("MepThread", get_logger(), lease_links_, pubsub_itc_fw_app::ComponentGroup::matching_engine_publisher, self_id, peer_id,
                             arbiter_pool_voter_id, "the arbiter", config_.lease, std::chrono::steady_clock::now(), 0,
                             fix_common::PeerStatementsFlag{fix_common::PeerStatementsFlag::PeerAlwaysMayLead});
        lease_tick_timer_id_ = start_recurring_timer(fix_common::LeaseTiming::tick_interval);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: ha_enabled=true -- leading only while the peer or the arbiter grants a lease (period={} ms)", config_.lease.period.count());
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

void MatchingEnginePublisherThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();

    if (svc == "sequencer") {
        sequencer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: sequencer (primary) connection {} established -- sending WalSubscribeRequest",
                   id.get_value());
        pubsub_itc_fw_app::WalSubscribeRequest req{};
        req.subscriber_id = wal_subscriber_id_;
        req.from_seq_no = sequencer_cursor_;
        send_pdu(id, pubsub_itc_fw_app::WalSubscribeRequest::message_pdu_id, 0, req);
    } else if (svc == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: sequencer (secondary) connection {} established -- sending WalSubscribeRequest",
                   id.get_value());
        pubsub_itc_fw_app::WalSubscribeRequest req{};
        req.subscriber_id = wal_subscriber_id_;
        req.from_seq_no = sequencer_cursor_;
        send_pdu(id, pubsub_itc_fw_app::WalSubscribeRequest::message_pdu_id, 0, req);
    } else if (svc == "arbiter_primary") {
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: arbiter-primary connection {} established", id.get_value());
    } else if (svc == "arbiter_secondary") {
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: arbiter-secondary connection {} established", id.get_value());
    } else if (svc == "peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: outbound peer connection {} established", id.get_value());
    } else if (svc == peer_inbound_svc_) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: inbound peer connection {} established", id.get_value());
    } else if (svc == orders_inbound_svc_ || svc == er_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MepThread: topic subscriber connection {} established on {} -- awaiting TopicSubscribeRequest", id.get_value(), svc);
    }
}

void MatchingEnginePublisherThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == sequencer_conn_id_) {
        sequencer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: sequencer (primary) connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: sequencer (secondary) connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: outbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: inbound peer connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
    } else {
        // Topic subscriber connection (or already gone): the owning publisher tears down
        // its data+control pair; the other no-ops on a connection it does not own.
        orders_publisher_.on_connection_lost(id);
        er_publisher_.on_connection_lost(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEnginePublisherThread::on_connection_writable(pubsub_itc_fw::ConnectionID id) {
    // The socket can accept another frame: let the owning publisher stream the next page.
    orders_publisher_.on_connection_writable(id);
    er_publisher_.on_connection_writable(id);
}

void MatchingEnginePublisherThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    // Peer PDUs.
    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // Arbiter PDUs: the active arbiter's answers to this publisher's lease requests.
    if (conn_id == arbiter_primary_conn_id_ || conn_id == arbiter_secondary_conn_id_) {
        if (message.pdu_id() == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
            handle_lease_grant(message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
            handle_lease_refusal(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: unexpected PDU {} from an arbiter -- dropping", message.pdu_id());
        }
        release_pdu_payload(message);
        return;
    }

    // Sequencer WAL follower PDUs.
    if (conn_id == sequencer_conn_id_ || conn_id == sequencer_secondary_conn_id_) {
        if (message.pdu_id() == pubsub_itc_fw_app::WalRecord::message_pdu_id) {
            handle_wal_record_from_sequencer(conn_id, message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::WalSubscribeAck::message_pdu_id) {
            handle_wal_subscribe_ack(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: unexpected PDU {} from sequencer connection {} -- dropping",
                       message.pdu_id(), conn_id.get_value());
        }
        release_pdu_payload(message);
        return;
    }

    // Topic subscriber PDUs.
    if (svc == orders_inbound_svc_ || svc == er_inbound_svc_) {
        if (message.pdu_id() == pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id) {
            handle_topic_subscribe_request(conn_id, message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicAck::message_pdu_id) {
            handle_topic_ack(conn_id, message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: unexpected PDU {} from topic subscriber {} -- dropping", message.pdu_id(),
                       conn_id.get_value());
        }
        release_pdu_payload(message);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: PDU on unrecognised connection {} ({}) -- dropping", conn_id.get_value(), svc);
    release_pdu_payload(message);
}

void MatchingEnginePublisherThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == wal_snapshot_timer_id_) {
        try {
            wal_.take_snapshot();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: WAL snapshot taken: last_seq_no={} record_count={}", wal_.last_seq_no(),
                       wal_.record_count());
        } catch (const std::exception& ex) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "MepThread: WAL snapshot failed: {}", ex.what());
        }
        return;
    }

    if (id == lease_tick_timer_id_) {
        act_on(lease_agent_->on_tick(std::chrono::steady_clock::now()));
        return;
    }
}

void MatchingEnginePublisherThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

pubsub_itc_fw::ConnectionID MatchingEnginePublisherThread::peer_active_conn() const {
    return peer_conn_id_.is_valid() ? peer_conn_id_ : peer_inbound_conn_id_;
}

void MatchingEnginePublisherThread::adopt_role(pubsub_itc_fw_app::Role new_role) {
    if (new_role == role_) {
        return;
    }
    const auto transition_level = (role_ == pubsub_itc_fw_app::Role::unknown) ? pubsub_itc_fw::FwLogLevel::Info : pubsub_itc_fw::FwLogLevel::Warning;
    PUBSUB_LOG(get_logger(), transition_level, "MepThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);
    role_ = new_role;
    set_publisher_role(new_role);
    if (new_role == pubsub_itc_fw_app::Role::follower) {
        // Stop publishing and drop all topic subscribers so they rediscover the new leader.
        orders_publisher_.drop_all_subscribers();
        er_publisher_.drop_all_subscribers();
    }
}

void MatchingEnginePublisherThread::act_on(fix_common::PairLeaseAgent::Change change) {
    epoch_ = lease_agent_->highest_epoch();
    switch (change) {
        case fix_common::PairLeaseAgent::Change::BecameLeader:
            adopt_role(pubsub_itc_fw_app::Role::leader);
            break;
        case fix_common::PairLeaseAgent::Change::StoppedLeading:
        case fix_common::PairLeaseAgent::Change::AgreedPeerLeads:
            adopt_role(pubsub_itc_fw_app::Role::follower);
            break;
        case fix_common::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void MatchingEnginePublisherThread::PublisherLeaseLinks::send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
    }
}

void MatchingEnginePublisherThread::PublisherLeaseLinks::send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) {
    // Both arbiters: only the active one answers, and which one that is may have changed.
    for (const pubsub_itc_fw::ConnectionID& conn : {owner_.arbiter_primary_conn_id_, owner_.arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            owner_.send_pdu(conn, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
        }
    }
}

void MatchingEnginePublisherThread::PublisherLeaseLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseGrant::message_pdu_id, 0, grant);
}

void MatchingEnginePublisherThread::PublisherLeaseLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id,
                                                                      const pubsub_itc_fw_app::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseRefusal::message_pdu_id, 0, refusal);
}

void MatchingEnginePublisherThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::LeaseRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode LeaseRequest -- dropping");
        return;
    }
    if (request.group != pubsub_itc_fw_app::ComponentGroup::matching_engine_publisher || !lease_agent_.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: LeaseRequest for group={} on the peer link -- dropping",
                   pubsub_itc_fw_app::to_string(request.group));
        return;
    }
    act_on(lease_agent_->on_request(
        conn_id, request.candidate_instance_id, request.epoch, request.request_id,
        fix_common::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead},
        std::chrono::steady_clock::now()));
}

void MatchingEnginePublisherThread::handle_lease_grant(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::LeaseGrantView grant{};
    if (!pubsub_itc_fw_app::decode(grant, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode LeaseGrant -- dropping");
        return;
    }
    if (grant.group != pubsub_itc_fw_app::ComponentGroup::matching_engine_publisher || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, std::chrono::steady_clock::now()));
}

void MatchingEnginePublisherThread::handle_lease_refusal(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::LeaseRefusalView refusal{};
    if (!pubsub_itc_fw_app::decode(refusal, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode LeaseRefusal -- dropping");
        return;
    }
    if (refusal.group != pubsub_itc_fw_app::ComponentGroup::matching_engine_publisher || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, std::chrono::steady_clock::now()));
}

void MatchingEnginePublisherThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());
    if (pdu_id == pubsub_itc_fw_app::LeaseRequest::message_pdu_id) {
        handle_lease_request(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
        handle_lease_grant(message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
        handle_lease_refusal(message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: unknown peer PDU {} -- dropping", pdu_id);
    }
}

void MatchingEnginePublisherThread::handle_wal_subscribe_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::WalSubscribeAckView ack{};
    if (!pubsub_itc_fw_app::decode(ack, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode WalSubscribeAck -- dropping");
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: WalSubscribeAck accepted_from_seq_no={}", ack.accepted_from_seq_no);
}

void MatchingEnginePublisherThread::handle_wal_record_from_sequencer(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode WalRecord from sequencer -- dropping");
        return;
    }

    wal_.append(view.seq_no, view.pdu_id, view.payload.data, static_cast<int>(view.payload.size), view.wall_time_ns);
    sequencer_cursor_ = view.seq_no;

    // Ack immediately.
    pubsub_itc_fw_app::WalAck ack{};
    ack.seq_no = view.seq_no;
    send_pdu(conn_id, pubsub_itc_fw_app::WalAck::message_pdu_id, 0, ack);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MepThread: WalRecord seq={} pdu_id={} written to MEP WAL, WalAck sent", view.seq_no,
               view.pdu_id);

    // The record is now in the WAL; wake each publisher so it streams the new record to any
    // caught-up subscriber. Each filters by topic membership and only publishes while leader.
    orders_publisher_.notify_record_appended(view.seq_no, view.pdu_id);
    er_publisher_.notify_record_appended(view.seq_no, view.pdu_id);
}

void MatchingEnginePublisherThread::handle_topic_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::TopicSubscribeRequestView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode TopicSubscribeRequest -- dropping");
        return;
    }

    const std::string subscriber_id(view.subscriber_id);
    const std::string topic_name(view.topic_name);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MepThread: TopicSubscribeRequest subscriber_id={} topic={} from_seq_no={} conn={}",
               subscriber_id, topic_name, view.from_seq_no, conn_id.get_value());

    // Route by topic to the owning publisher. The publisher handles the not-leader reply,
    // orphan pre-emption, the subscribe ack, and socket-paced streaming from the shared WAL;
    // an unknown topic is rejected here. Data and control channels both arrive here (they
    // carry the same topic_name) and route to the same publisher.
    pubsub_itc_fw_app::Topic topic{};
    if (!pubsub_itc_fw_app::topic_from_name(topic_name, topic)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: unknown topic '{}' from subscriber {} -- disconnecting", topic_name,
                   subscriber_id);
        topic_disconnect(conn_id);
        return;
    }
    if (topic == pubsub_itc_fw_app::Topic::orders) {
        orders_publisher_.on_subscribe_request(conn_id, view);
    } else {
        er_publisher_.on_subscribe_request(conn_id, view);
    }
}

void MatchingEnginePublisherThread::handle_topic_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_needed = 0;
    size_t consumed = 0;
    pubsub_itc_fw_app::TopicAckView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MepThread: failed to decode TopicAck -- dropping");
        return;
    }

    // Fan to both publishers; only the one that owns this connection acts (the ack is the
    // F2 truncation cursor + drives the lag policy). The MEP's WAL truncation itself is a
    // no-op (see topic_truncate_wal).
    orders_publisher_.on_ack(conn_id, view);
    er_publisher_.on_ack(conn_id, view);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MepThread: TopicAck conn={} last_seq_no={}", conn_id.get_value(), view.last_seq_no);
}

void MatchingEnginePublisherThread::set_publisher_role(pubsub_itc_fw_app::Role role) {
    const bool is_leader = (role == pubsub_itc_fw_app::Role::leader);
    orders_publisher_.set_leader(is_leader);
    er_publisher_.set_leader(is_leader);
}

// TopicPublisherHost -- each publisher decides what to send; this thread does it

void MatchingEnginePublisherThread::topic_send_subscribe_ack(pubsub_itc_fw::ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) {
    send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeAck::message_pdu_id, 0, ack);
}

void MatchingEnginePublisherThread::topic_send_page(pubsub_itc_fw::ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) {
    send_pdu(connection_id, pubsub_itc_fw_app::TopicPage::message_pdu_id, seq_no, page);
}

void MatchingEnginePublisherThread::topic_send_not_leader(pubsub_itc_fw::ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) {
    send_pdu(connection_id, pubsub_itc_fw_app::TopicNotLeader::message_pdu_id, 0, not_leader);
}

void MatchingEnginePublisherThread::topic_send_lagged(pubsub_itc_fw::ConnectionID control_connection_id, const pubsub_itc_fw_app::TopicLagged& lagged) {
    send_pdu(control_connection_id, pubsub_itc_fw_app::TopicLagged::message_pdu_id, 0, lagged);
}

void MatchingEnginePublisherThread::topic_disconnect(pubsub_itc_fw::ConnectionID connection_id) {
    pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = connection_id;
    get_reactor().enqueue_control_command(cmd);
}

void MatchingEnginePublisherThread::topic_request_writable_notification(pubsub_itc_fw::ConnectionID connection_id) {
    request_writable_notification(connection_id);
}

void MatchingEnginePublisherThread::topic_truncate_wal(int64_t /*safe_seq_no*/) {
    // No-op. This MEP's WAL is shared by both topics and its retention is governed by the
    // periodic wal_snapshot (durability) policy, not by topic acks. Ack-driven reclamation
    // over a shared multi-topic WAL would have to truncate to the global minimum across BOTH
    // publishers' subscriber sets; that is deferred (see docs/pubsub/mep_rewire_and_oar.md,
    // "Wrinkle A"). on_ack still advances each publisher's cursor for the lag policy.
}

} // namespaces
