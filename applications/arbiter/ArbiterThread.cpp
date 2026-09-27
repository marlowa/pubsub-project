// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterThread.hpp"

#include <chrono>

#include <LeaderEpoch.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace arbiter {

namespace {

// The witness's identity as a voter in deciding which arbiter is active. The arbiters are 1 and 2.
constexpr int64_t witness_voter_id = 3;

// The arbiter pool's identity as a voter in deciding which instance of a component pair leads. The
// instances are 1 and 2. Both arbiters answer as 3: the pool votes through whichever is active.
constexpr int64_t arbiter_pool_voter_id = 3;

pubsub_itc_fw_app::LeaseRefusalReason refusal_reason_for(fix_common::LeaseVoter::Verdict verdict) {
    switch (verdict) {
        case fix_common::LeaseVoter::Verdict::RefusedWhileRestarting:
            return pubsub_itc_fw_app::LeaseRefusalReason::restarting;
        case fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere:
            return pubsub_itc_fw_app::LeaseRefusalReason::promised_elsewhere;
        case fix_common::LeaseVoter::Verdict::RefusedEpochBehind:
            return pubsub_itc_fw_app::LeaseRefusalReason::epoch_behind;
        case fix_common::LeaseVoter::Verdict::Granted:
            break;
    }
    return pubsub_itc_fw_app::LeaseRefusalReason::unknown;
}

bool is_component_group(pubsub_itc_fw_app::ComponentGroup group) {
    return group == pubsub_itc_fw_app::ComponentGroup::sequencer || group == pubsub_itc_fw_app::ComponentGroup::matching_engine ||
           group == pubsub_itc_fw_app::ComponentGroup::matching_engine_publisher;
}

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
    , peer_instance_id_{static_cast<int64_t>(config.peer_instance_id)}
    , component_voters_(config.lease.period)
    , lease_promise_store_(config.lease_promise_file, fix_common::LeasePromiseStore::current_boot_id()) {}

void ArbiterThread::on_initial_event() {
    // An arbiter keeps no epoch on disk, so it starts knowing none. It does keep its promise in
    // deciding which arbiter is active, so that a restart by its supervisor does not forget it.
    pool_lease_.emplace("ArbiterThread", get_logger(), pool_links_, pubsub_itc_fw_app::ComponentGroup::arbiter, static_cast<int64_t>(config_.instance_id),
                        peer_instance_id_, witness_voter_id, "the witness", config_.lease, std::chrono::steady_clock::now(), 0);
    pool_lease_->keep_promises_in(lease_promise_store_, lease_promise_store_.load(), std::chrono::steady_clock::now());
    lease_tick_timer_id_ = start_recurring_timer(fix_common::LeaseTiming::tick_interval);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: deciding the active arbiter by leases (period={} ms, drift allowance={} ms, renewal every {} ms); no vote is granted or asked "
               "for during the first period",
               config_.lease.period.count(), config_.lease.drift_allowance.count(), config_.lease.renewal_interval.count());
}

void ArbiterThread::on_app_ready_event() {
    connect_to_service("peer");
    connect_to_service("witness");
}

void ArbiterThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    if (svc == "peer" || svc == peer_inbound_svc) {
        (svc == "peer" ? peer_conn_id_ : peer_inbound_conn_id_) = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: {} peer connection {} established", svc == "peer" ? "outbound" : "inbound",
                   id.get_value());
        // A peer that has just restarted knows no epochs at all. Telling it the highest granted in
        // each group means it will not grant a lower one if it becomes active.
        for (const auto& entry : component_voters_.highest_epochs()) {
            send_highest_epoch_to_peer(id, entry.first, entry.second);
        }
    } else if (svc == "witness") {
        witness_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: witness connection {} established", id.get_value());
    } else {
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: witness connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component connection {} lost: {}", id.get_value(), reason);
    }
}

void ArbiterThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const auto pdu_id = message.pdu_id();

    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
    } else if (conn_id == witness_conn_id_) {
        if (pdu_id == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
            handle_lease_grant(message);
        } else if (pdu_id == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
            handle_lease_refusal(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from witness -- dropping", pdu_id);
        }
    } else if (pdu_id == pubsub_itc_fw_app::LeaseRequest::message_pdu_id) {
        handle_component_lease_request(conn_id, message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from component on connection {} -- dropping",
                   pdu_id, conn_id.get_value());
    }
    release_pdu_payload(message);
}

void ArbiterThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == lease_tick_timer_id_) {
        act_on(pool_lease_->on_tick(std::chrono::steady_clock::now()));
    }
}

void ArbiterThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

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
        component_voters_.became_active(std::chrono::steady_clock::now());
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "ArbiterThread: now ACTIVE -- granting no component a lease for {} ms, because what the previously active arbiter promised is not known",
                   config_.lease.period.count());
    } else {
        component_voters_.became_passive();
    }
}

void ArbiterThread::act_on(fix_common::PairLeaseAgent::Change change) {
    switch (change) {
        case fix_common::PairLeaseAgent::Change::BecameLeader:
            epoch_ = pool_lease_->epoch();
            adopt_role(pubsub_itc_fw_app::Role::leader);
            break;
        case fix_common::PairLeaseAgent::Change::StoppedLeading:
        case fix_common::PairLeaseAgent::Change::AgreedPeerLeads:
            epoch_ = pool_lease_->highest_epoch();
            adopt_role(pubsub_itc_fw_app::Role::follower);
            break;
        case fix_common::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void ArbiterThread::PoolLinks::send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
    }
}

void ArbiterThread::PoolLinks::send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) {
    if (owner_.witness_conn_id_.is_valid()) {
        owner_.send_pdu(owner_.witness_conn_id_, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
    }
}

void ArbiterThread::PoolLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseGrant::message_pdu_id, 0, grant);
}

void ArbiterThread::PoolLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseRefusal::message_pdu_id, 0, refusal);
}

void ArbiterThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeaseRequest from peer -- dropping");
        return;
    }
    if (request.group != pubsub_itc_fw_app::ComponentGroup::arbiter) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: LeaseRequest for group={} on the peer link -- dropping",
                   pubsub_itc_fw_app::to_string(request.group));
        return;
    }
    act_on(pool_lease_->on_request(conn_id, request.candidate_instance_id, request.epoch, request.request_id, std::chrono::steady_clock::now()));
}

void ArbiterThread::handle_lease_grant(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseGrantView grant{};
    if (!pubsub_itc_fw_app::decode(grant, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeaseGrant -- dropping");
        return;
    }
    act_on(pool_lease_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, std::chrono::steady_clock::now()));
}

void ArbiterThread::handle_lease_refusal(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseRefusalView refusal{};
    if (!pubsub_itc_fw_app::decode(refusal, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeaseRefusal -- dropping");
        return;
    }
    act_on(pool_lease_->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, std::chrono::steady_clock::now()));
}

void ArbiterThread::handle_component_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode a component's LeaseRequest -- dropping");
        return;
    }
    if (!is_component_group(request.group)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: component LeaseRequest for group={} -- dropping",
                   pubsub_itc_fw_app::to_string(request.group));
        return;
    }

    // A passive arbiter stays silent rather than refusing. The component sent the same request to
    // the active arbiter, with the same id, and a refusal from here would cancel it.
    if (!component_voters_.active()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "ArbiterThread: not active -- leaving group={} instance {}'s LeaseRequest to the active arbiter",
                   pubsub_itc_fw_app::to_string(request.group), request.candidate_instance_id);
        return;
    }

    const int32_t highest_before = component_voters_.highest_epoch(request.group);
    const fix_common::LeaseVoter::Answer answer =
        component_voters_.consider(request.group, request.candidate_instance_id, request.epoch, std::chrono::steady_clock::now());

    if (answer.verdict == fix_common::LeaseVoter::Verdict::Granted) {
        pubsub_itc_fw_app::LeaseGrant grant{};
        grant.voter_instance_id = arbiter_pool_voter_id;
        grant.group = request.group;
        grant.epoch = request.epoch;
        grant.request_id = request.request_id;
        send_pdu(conn_id, pubsub_itc_fw_app::LeaseGrant::message_pdu_id, 0, grant);

        int64_t& last = last_granted_to_[request.group];
        if (last != request.candidate_instance_id) {
            // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: group={} lease granted to instance {} at epoch {}",
                       pubsub_itc_fw_app::to_string(request.group), request.candidate_instance_id, request.epoch);
            last = request.candidate_instance_id;
        }
        if (answer.highest_epoch > highest_before) {
            const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
            if (peer.is_valid()) {
                send_highest_epoch_to_peer(peer, request.group, answer.highest_epoch);
            }
        }
        return;
    }

    pubsub_itc_fw_app::LeaseRefusal refusal{};
    refusal.voter_instance_id = arbiter_pool_voter_id;
    refusal.group = request.group;
    refusal.highest_epoch = answer.highest_epoch;
    refusal.request_id = request.request_id;
    refusal.reason = refusal_reason_for(answer.verdict);
    send_pdu(conn_id, pubsub_itc_fw_app::LeaseRefusal::message_pdu_id, 0, refusal);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: group={} refused instance {} at epoch {} ({})",
               pubsub_itc_fw_app::to_string(request.group), request.candidate_instance_id, request.epoch, pubsub_itc_fw_app::to_string(refusal.reason));
}

void ArbiterThread::send_highest_epoch_to_peer(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::ComponentGroup group, int32_t epoch) {
    pubsub_itc_fw_app::ArbiterStateRecord record{};
    record.group = group;
    record.epoch = epoch;
    // The epoch records which instance leads in it: its remainder on division by 4 is that instance's id.
    record.leader_instance_id = epoch % fix_common::LeaderEpoch::epoch_stride;
    record.component_instance_id = record.leader_instance_id;
    send_pdu(conn_id, pubsub_itc_fw_app::ArbiterStateRecord::message_pdu_id, 0, record);
}

void ArbiterThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();
    if (pdu_id == pubsub_itc_fw_app::LeaseRequest::message_pdu_id) {
        handle_lease_request(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
        handle_lease_grant(message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
        handle_lease_refusal(message);
    } else if (pdu_id == pubsub_itc_fw_app::ArbiterStateRecord::message_pdu_id) {
        handle_arbiter_state_record(message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unknown peer PDU id {} -- dropping", pdu_id);
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
    component_voters_.learn_epoch(record.group, record.epoch);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: peer reports group={} has been granted epoch {}",
               pubsub_itc_fw_app::to_string(record.group), record.epoch);
}

} // namespaces
