// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "WitnessThread.hpp"

#include <algorithm>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace witness {

namespace {

static constexpr int16_t pdu_arbiter_heartbeat = 300;
static constexpr int16_t pdu_arbiter_vote_request = 301;
static constexpr int16_t pdu_arbiter_vote_response = 302;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const WitnessConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "WitnessPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "WitnessPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

WitnessThread::WitnessThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                             pubsub_itc_fw::Reactor& reactor, const WitnessConfiguration& config)
    : ApplicationThread(token, logger, reactor, "WitnessThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(),
                        make_allocator_config(config, logger), pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config) {}

void WitnessThread::on_initial_event() {}

void WitnessThread::on_app_ready_event() {}

void WitnessThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter connection {} established", id.get_value());
}

void WitnessThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter connection {} lost: {}", id.get_value(), reason);

    const auto conn_it = conn_to_instance_id_.find(id.get_value());
    if (conn_it != conn_to_instance_id_.end()) {
        const int64_t instance_id = conn_it->second;
        instance_to_conn_id_.erase(instance_id);
        conn_to_instance_id_.erase(conn_it);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter instance_id={} unregistered", instance_id);
    }
}

void WitnessThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());

    if (pdu_id == pdu_arbiter_heartbeat) {
        handle_arbiter_heartbeat(conn_id, message);
    } else if (pdu_id == pdu_arbiter_vote_request) {
        handle_arbiter_vote_request(conn_id, message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "WitnessThread: unexpected PDU pdu_id={} on connection {} -- dropping (witness only handles arbiter PDUs)", pdu_id, conn_id.get_value());
    }

    release_pdu_payload(message);
}

void WitnessThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void WitnessThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// ---------------------------------------------------------------------------
// Protocol handlers
// ---------------------------------------------------------------------------

void WitnessThread::handle_arbiter_heartbeat(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbiterHeartbeatView hb{};

    if (!pubsub_itc_fw_app::decode(hb, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "WitnessThread: failed to decode ArbiterHeartbeat -- dropping");
        return;
    }

    const bool already_known = conn_to_instance_id_.count(conn_id.get_value()) > 0;
    conn_to_instance_id_[conn_id.get_value()] = hb.instance_id;
    instance_to_conn_id_[hb.instance_id] = conn_id;
    max_observed_epoch_ = std::max(max_observed_epoch_, hb.epoch);

    if (!already_known) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter instance_id={} registered on connection {} (epoch={})",
                   hb.instance_id, conn_id.get_value(), hb.epoch);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "WitnessThread: ArbiterHeartbeat from instance_id={} (epoch={})", hb.instance_id,
                   hb.epoch);
    }
}

void WitnessThread::handle_arbiter_vote_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbiterVoteRequestView req{};

    if (!pubsub_itc_fw_app::decode(req, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "WitnessThread: failed to decode ArbiterVoteRequest -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "WitnessThread: ArbiterVoteRequest from instance_id={} (peer_instance_id={} epoch={})", req.self_instance_id, req.peer_instance_id,
               req.epoch);

    max_observed_epoch_ = std::max(max_observed_epoch_, req.epoch);
    const int32_t new_epoch = max_observed_epoch_ + 1;

    // Grant to requester if peer arbiter is not connected; else to the lower instance_id.
    const bool peer_connected = instance_to_conn_id_.count(req.peer_instance_id) > 0;
    const int64_t granted_to =
        peer_connected ? std::min(req.self_instance_id, req.peer_instance_id) : req.self_instance_id;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: vote granted to instance_id={} epoch={} (peer_connected={})", granted_to,
               new_epoch, peer_connected);

    send_arbiter_vote_response(conn_id, granted_to, new_epoch);
}

void WitnessThread::send_arbiter_vote_response(const pubsub_itc_fw::ConnectionID& conn_id, int64_t granted_to_instance_id, int32_t epoch) {
    pubsub_itc_fw_app::ArbiterVoteResponse resp{};
    resp.granted_to_instance_id = granted_to_instance_id;
    resp.epoch = epoch;
    send_pdu(conn_id, pdu_arbiter_vote_response, 0, resp);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "WitnessThread: ArbiterVoteResponse sent to connection {} (granted_to={} epoch={})", conn_id.get_value(), granted_to_instance_id, epoch);
}

} // namespace witness
