// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ArbiterThread.hpp>

#include <chrono>
#include <string>

#include <lease_messages.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LeaderEpoch.hpp>
#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

namespace {

// The witness's identity as a voter in deciding which arbiter is active. The arbiters are 1 and 2.
constexpr int64_t witness_voter_id = 3;

// The arbiter pool's identity as a voter in deciding which instance of a component pair leads. The
// instances are 1 and 2. Both arbiters answer as 3: the pool votes through whichever is active.
constexpr int64_t arbiter_pool_voter_id = 3;

// An arbiter's role as a test event names it.
pubsub_itc_fw::LeadershipRole leadership_role(ArbiterRole role) {
    switch (role) {
        case ArbiterRole::leader:
            return pubsub_itc_fw::LeadershipRole::Leader;
        case ArbiterRole::follower:
            return pubsub_itc_fw::LeadershipRole::Follower;
        case ArbiterRole::unknown:
            break;
    }
    return pubsub_itc_fw::LeadershipRole::Unknown;
}

pubsub_itc_fw::LeaseRefusalReason refusal_reason_for(pubsub_itc_fw::LeaseVoter::Verdict verdict) {
    switch (verdict) {
        case pubsub_itc_fw::LeaseVoter::Verdict::RefusedWhileRestarting:
            return pubsub_itc_fw::LeaseRefusalReason::restarting;
        case pubsub_itc_fw::LeaseVoter::Verdict::RefusedPromisedElsewhere:
            return pubsub_itc_fw::LeaseRefusalReason::promised_elsewhere;
        case pubsub_itc_fw::LeaseVoter::Verdict::RefusedEpochBehind:
            return pubsub_itc_fw::LeaseRefusalReason::epoch_behind;
        case pubsub_itc_fw::LeaseVoter::Verdict::RefusedMayNotLead:
            return pubsub_itc_fw::LeaseRefusalReason::may_not_lead;
        case pubsub_itc_fw::LeaseVoter::Verdict::Granted:
            break;
    }
    return pubsub_itc_fw::LeaseRefusalReason::unknown;
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

std::string_view to_string(ArbiterRole role) {
    switch (role) {
        case ArbiterRole::unknown:
            return "unknown";
        case ArbiterRole::leader:
            return "leader";
        case ArbiterRole::follower:
            return "follower";
    }
    return "unknown";
}

ArbiterThread::ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                             const ArbiterConfiguration& config)
    : ApplicationThread(token, logger, reactor, "ArbiterThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , peer_instance_id_{static_cast<int64_t>(config.peer_instance_id)}
    , component_voters_(config.lease.period)
    , lease_promise_store_(config.lease_promise_file, pubsub_itc_fw::LeasePromiseStore::current_boot_id())
    , component_statement_store_(config.lease_promise_file.empty() ? std::string{} : config.lease_promise_file + ".component_statements") {}

void ArbiterThread::on_initial_event() {
    // Only when a test started this arbiter (docs/availability/test_events.md). Made before the lease agent, which is given it.
    if (config_.test_events.enabled) {
        test_events_.emplace(test_event_links_, config_.test_events.component, static_cast<int64_t>(config_.instance_id), "ArbiterThread");
    }
    // An arbiter keeps no epoch on disk, so it starts knowing none. It does keep its promise in
    // deciding which arbiter is active, so that a restart by its supervisor does not forget it.
    pool_lease_.emplace("ArbiterThread", get_logger(), pool_links_, config_.own_pair.pair_id, static_cast<int64_t>(config_.instance_id), peer_instance_id_,
                        witness_voter_id, "the witness", config_.lease, std::chrono::steady_clock::now(), 0,
                        pubsub_itc_fw::PeerStatementsFlag{pubsub_itc_fw::PeerStatementsFlag::PeerAlwaysMayLead});
    if (test_events_.has_value()) {
        pool_lease_->report_test_events_to(*test_events_);
    }
    background_promise_recorder_.emplace(lease_promise_store_);
    pool_lease_->keep_promises_in(*background_promise_recorder_, lease_promise_store_.load(), lease_promise_store_.load_statement(),
                                  std::chrono::steady_clock::now());
    for (const auto& [pair_id, statement] : component_statement_store_.load()) {
        // A statement for a pair this arbiter is no longer configured to vote in is of no use to it.
        if (!votes_in(pair_id)) {
            continue;
        }
        component_voters_.learn_statement(pair_id, statement);
        if (statement.instance_that_may_not_lead() != 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "ArbiterThread: group={} -- the recorded statement from instance {} at epoch {} says instance {} may not lead; no lease is granted "
                       "to it until its leader says otherwise",
                       pair_name(pair_id), statement.leader_id, statement.epoch, statement.instance_that_may_not_lead());
        }
    }
    lease_tick_timer_id_ = start_recurring_timer(pubsub_itc_fw::LeaseTiming::tick_interval);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: deciding the active arbiter by leases (period={} ms, drift allowance={} ms, renewal every {} ms); no vote is granted or asked "
               "for during the first period",
               config_.lease.period.count(), config_.lease.drift_allowance.count(), config_.lease.renewal_interval.count());
}

void ArbiterThread::on_app_ready_event() {
    if (test_events_.has_value()) {
        connect_to_service(std::string(pubsub_itc_fw::TestEventChannel::service_name));
    }
    connect_to_service("peer");
    connect_to_service("witness");
}

void ArbiterThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (test_events_.has_value() && test_events_->on_connection_established(id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: connection {} to the test's event listener established", id.get_value());
        // A role taken before the connection was made is reported now, so that the test learns it whenever it connects.
        if (role_ != ArbiterRole::unknown) {
            test_events_->role_changed("ArbiterThread", static_cast<int64_t>(config_.instance_id), pubsub_itc_fw::LeadershipRole::Unknown,
                                       leadership_role(role_), epoch_);
        }
        return;
    }
    const std::string& svc = id.service_name();
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    if (svc == "peer" || svc == peer_inbound_svc) {
        (svc == "peer" ? peer_conn_id_ : peer_inbound_conn_id_) = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: {} peer connection {} established", svc == "peer" ? "outbound" : "inbound",
                   id.get_value());
        // A peer that has just restarted knows no epochs at all. Telling it the highest granted in
        // each group means it will not grant a lower one if it becomes active; telling it each
        // statement means it will not grant a lease to an instance a leader has said may not lead.
        std::map<int32_t, bool> groups;
        for (const auto& entry : component_voters_.highest_epochs()) {
            groups[entry.first] = true;
        }
        for (const auto& entry : component_voters_.statements()) {
            groups[entry.first] = true;
        }
        for (const auto& entry : groups) {
            send_group_state_to_peer(id, entry.first);
        }
    } else if (svc == "witness") {
        witness_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: witness connection {} established", id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: component connection {} established ({})", id.get_value(), svc);
    }
}

void ArbiterThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (test_events_.has_value() && test_events_->on_connection_lost(id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: the connection to the test's event listener is lost ({}) -- no more test events are sent", reason);
        return;
    }
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
    if (test_events_.has_value() && test_events_->on_framework_pdu_message(message)) {
        release_pdu_payload(message);
        return;
    }
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const auto pdu_id = message.pdu_id();

    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
    } else if (conn_id == witness_conn_id_) {
        if (pdu_id == pubsub_itc_fw::LeaseGrant::message_pdu_id) {
            handle_lease_grant(message);
        } else if (pdu_id == pubsub_itc_fw::LeaseRefusal::message_pdu_id) {
            handle_lease_refusal(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: unexpected PDU pdu_id={} from witness -- dropping", pdu_id);
        }
    } else if (pdu_id == pubsub_itc_fw::LeaseRequest::message_pdu_id) {
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

bool ArbiterThread::votes_in(int32_t pair_id) const {
    for (const VotingPair& pair : config_.voting_pairs) {
        if (pair.pair_id == pair_id) {
            return true;
        }
    }
    return false;
}

std::string ArbiterThread::pair_name(int32_t pair_id) const {
    for (const VotingPair& pair : config_.voting_pairs) {
        if (pair.pair_id == pair_id) {
            return pair.name;
        }
    }
    return pair_id == config_.own_pair.pair_id ? config_.own_pair.name : std::string{"unknown"};
}

pubsub_itc_fw::ConnectionID ArbiterThread::peer_active_conn() const {
    if (peer_conn_id_.is_valid()) {
        return peer_conn_id_;
    }
    return peer_inbound_conn_id_;
}

void ArbiterThread::adopt_role(ArbiterRole new_role) {
    if (new_role == role_) {
        return;
    }
    const auto transition_level = (role_ == ArbiterRole::unknown) ? pubsub_itc_fw::FwLogLevel::Info : pubsub_itc_fw::FwLogLevel::Warning;
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), transition_level, "ArbiterThread: role transition {} -> {} (epoch={})", to_string(role_), to_string(new_role), epoch_);
    if (test_events_.has_value()) {
        test_events_->role_changed("ArbiterThread", static_cast<int64_t>(config_.instance_id), leadership_role(role_), leadership_role(new_role), epoch_);
    }
    role_ = new_role;

    if (new_role == ArbiterRole::leader) {
        component_voters_.became_active(std::chrono::steady_clock::now());
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "ArbiterThread: now ACTIVE -- granting no component a lease for {} ms, because what the previously active arbiter promised is not known",
                   config_.lease.period.count());
    } else {
        component_voters_.became_passive();
    }
}

void ArbiterThread::act_on(pubsub_itc_fw::PairLeaseAgent::Change change) {
    switch (change) {
        case pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader:
            epoch_ = pool_lease_->epoch();
            adopt_role(ArbiterRole::leader);
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::StoppedLeading:
        case pubsub_itc_fw::PairLeaseAgent::Change::AgreedPeerLeads:
            epoch_ = pool_lease_->highest_epoch();
            adopt_role(ArbiterRole::follower);
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void ArbiterThread::ArbiterTestEventLinks::send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) {
    owner_.send_pdu_payload(connection, pdu_id, 0, bytes, size);
}

void ArbiterThread::PoolLinks::send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
    }
}

void ArbiterThread::PoolLinks::send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) {
    if (owner_.witness_conn_id_.is_valid()) {
        owner_.send_pdu(owner_.witness_conn_id_, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
    }
}

void ArbiterThread::PoolLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseGrant::message_pdu_id, 0, grant);
}

void ArbiterThread::PoolLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseRefusal::message_pdu_id, 0, refusal);
}

void ArbiterThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseRequestView request{};
    if (!pubsub_itc_fw::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeaseRequest from peer -- dropping");
        return;
    }
    if (request.pair_id != config_.own_pair.pair_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: LeaseRequest for pair {} on the peer link -- dropping", request.pair_id);
        return;
    }
    act_on(pool_lease_->on_request(
        conn_id, request.candidate_instance_id, request.epoch, request.request_id,
        pubsub_itc_fw::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead},
        std::chrono::steady_clock::now()));
}

void ArbiterThread::handle_lease_grant(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseGrantView grant{};
    if (!pubsub_itc_fw::decode(grant, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode LeaseGrant -- dropping");
        return;
    }
    act_on(pool_lease_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, std::chrono::steady_clock::now()));
}

void ArbiterThread::handle_lease_refusal(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseRefusalView refusal{};
    if (!pubsub_itc_fw::decode(refusal, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
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
    pubsub_itc_fw::LeaseRequestView request{};
    if (!pubsub_itc_fw::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode a component's LeaseRequest -- dropping");
        return;
    }
    if (!votes_in(request.pair_id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterThread: component LeaseRequest for pair {}, which this arbiter is not configured to vote in -- dropping", request.pair_id);
        return;
    }

    // A passive arbiter stays silent rather than refusing. The component sent the same request to
    // the active arbiter, with the same id, and a refusal from here would cancel it.
    if (!component_voters_.active()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "ArbiterThread: not active -- leaving group={} instance {}'s LeaseRequest to the active arbiter", pair_name(request.pair_id),
                   request.candidate_instance_id);
        return;
    }

    const int32_t highest_before = component_voters_.highest_epoch(request.pair_id);
    const pubsub_itc_fw::LeaseVoter::Answer answer =
        component_voters_.consider(request.pair_id, request.candidate_instance_id, request.epoch, std::chrono::steady_clock::now());

    if (answer.verdict == pubsub_itc_fw::LeaseVoter::Verdict::Granted) {
        // A leader's statement is recorded, and on disk, before the grant echoes it: the leader may
        // rely on the echo to act on a command its peer lacks.
        int64_t echoed = 0;
        bool statement_changed = false;
        const pubsub_itc_fw::LeaderStatement statement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead};
        if (statement.leader_id != 0) {
            const pubsub_itc_fw::LeaderStatement before = component_voters_.statement(request.pair_id);
            echoed = component_voters_.record_statement(request.pair_id, statement);
            statement_changed = !component_voters_.statement(request.pair_id).same_as(before);
            if (statement_changed && !save_component_statements()) {
                echoed = 0;
            }
        }
        pubsub_itc_fw::LeaseGrant grant{};
        grant.voter_instance_id = arbiter_pool_voter_id;
        grant.pair_id = request.pair_id;
        grant.epoch = request.epoch;
        grant.request_id = request.request_id;
        grant.echoed_statement_number = echoed;
        send_pdu(conn_id, pubsub_itc_fw::LeaseGrant::message_pdu_id, 0, grant);
        if (statement_changed) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: group={} instance {} says at epoch {} (statement {}) that its peer {}",
                       pair_name(request.pair_id), statement.leader_id, statement.epoch, statement.number,
                       statement.peer_may_lead ? "may lead" : "may not lead");
        }

        int64_t& last = last_granted_to_[request.pair_id];
        if (last != request.candidate_instance_id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: group={} lease granted to instance {} at epoch {}",
                       pair_name(request.pair_id), request.candidate_instance_id, request.epoch);
            if (test_events_.has_value()) {
                test_events_->lease_granted_to(request.pair_id, request.candidate_instance_id, request.epoch);
            }
            last = request.candidate_instance_id;
        }
        if (answer.highest_epoch > highest_before || statement_changed) {
            const pubsub_itc_fw::ConnectionID peer = peer_active_conn();
            if (peer.is_valid()) {
                send_group_state_to_peer(peer, request.pair_id);
            }
        }
        return;
    }

    pubsub_itc_fw::LeaseRefusal refusal{};
    refusal.voter_instance_id = arbiter_pool_voter_id;
    refusal.pair_id = request.pair_id;
    refusal.highest_epoch = answer.highest_epoch;
    refusal.request_id = request.request_id;
    refusal.reason = refusal_reason_for(answer.verdict);
    send_pdu(conn_id, pubsub_itc_fw::LeaseRefusal::message_pdu_id, 0, refusal);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: group={} refused instance {} at epoch {} ({})", pair_name(request.pair_id),
               request.candidate_instance_id, request.epoch, pubsub_itc_fw::to_string(refusal.reason));
}

void ArbiterThread::send_group_state_to_peer(const pubsub_itc_fw::ConnectionID& conn_id, int32_t pair_id) {
    pubsub_itc_fw::ArbiterStateRecord record{};
    record.pair_id = pair_id;
    record.epoch = component_voters_.highest_epoch(pair_id);
    // The epoch records which instance leads in it: its remainder on division by 4 is that instance's id.
    record.leader_instance_id = record.epoch % pubsub_itc_fw::LeaderEpoch::epoch_stride;
    record.component_instance_id = record.leader_instance_id;
    const pubsub_itc_fw::LeaderStatement statement = component_voters_.statement(pair_id);
    record.statement_leader_id = statement.leader_id;
    record.statement_epoch = statement.epoch;
    record.statement_number = statement.number;
    record.peer_may_lead = statement.peer_may_lead;
    send_pdu(conn_id, pubsub_itc_fw::ArbiterStateRecord::message_pdu_id, 0, record);
}

bool ArbiterThread::save_component_statements() {
    if (component_statement_store_.save(component_voters_.statements())) {
        return true;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "ArbiterThread: could not write the component statements to '{}' -- the newest statement is held in memory only, and is not echoed",
               component_statement_store_.path());
    return false;
}

void ArbiterThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();
    if (pdu_id == pubsub_itc_fw::LeaseRequest::message_pdu_id) {
        handle_lease_request(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw::LeaseGrant::message_pdu_id) {
        handle_lease_grant(message);
    } else if (pdu_id == pubsub_itc_fw::LeaseRefusal::message_pdu_id) {
        handle_lease_refusal(message);
    } else if (pdu_id == pubsub_itc_fw::ArbiterStateRecord::message_pdu_id) {
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
    pubsub_itc_fw::ArbiterStateRecordView record{};
    if (!pubsub_itc_fw::decode(record, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "ArbiterThread: failed to decode ArbiterStateRecord -- dropping");
        return;
    }
    component_voters_.learn_epoch(record.pair_id, record.epoch);
    const pubsub_itc_fw::LeaderStatement statement{record.statement_leader_id, record.statement_epoch, record.statement_number, record.peer_may_lead};
    if (statement.leader_id != 0 && component_voters_.learn_statement(record.pair_id, statement)) {
        const bool saved = save_component_statements();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: peer reports group={} instance {} said at epoch {} that its peer {}{}",
                   pair_name(record.pair_id), statement.leader_id, statement.epoch, statement.peer_may_lead ? "may lead" : "may not lead",
                   saved ? "" : " -- held in memory only");
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "ArbiterThread: peer reports group={} has been granted epoch {}", pair_name(record.pair_id),
               record.epoch);
}

} // namespaces
