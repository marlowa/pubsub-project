// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "WitnessThread.hpp"

#include <chrono>

#include <lease_messages.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/TechnicalEventsSubscription.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace witness {

namespace {

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
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Error, "WitnessPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

// The witness's identity as a voter. The arbiters are 1 and 2.
constexpr int64_t witness_voter_id = 3;

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

} // namespaces

WitnessThread::WitnessThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                             const WitnessConfiguration& config)
    : TopicSubscriberThread(token, logger, reactor, "WitnessThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                            pubsub_itc_fw::ApplicationThreadConfiguration{}, config.technical_events.publisher_services(), std::string("witness"),
                            std::string(pubsub_itc_fw::TechnicalEventsSubscription::topic_name), 0)
    , config_(config) {}

void WitnessThread::on_started() {
    voter_.emplace(config_.lease.period, std::chrono::steady_clock::now(), 0);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "WitnessThread: voting on which arbiter is active (lease period={} ms); no vote is granted during the first period",
               config_.lease.period.count());
}

void WitnessThread::on_app_ready_event() {}

void WitnessThread::on_other_connection_established(pubsub_itc_fw::ConnectionID id) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter connection {} established", id.get_value());
}

void WitnessThread::on_other_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: arbiter connection {} lost: {}", id.get_value(), reason);
}

void WitnessThread::on_other_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const auto pdu_id = message.pdu_id();

    if (pdu_id == pubsub_itc_fw::LeaseRequest::message_pdu_id) {
        handle_lease_request(conn_id, message);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "WitnessThread: unexpected PDU pdu_id={} on connection {} -- dropping (the witness answers only lease requests from arbiters)", pdu_id,
                   conn_id.get_value());
    }

    release_pdu_payload(message);
}

void WitnessThread::on_timer_event([[maybe_unused]] pubsub_itc_fw::TimerID id) {}

void WitnessThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void WitnessThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseRequestView request{};

    if (!pubsub_itc_fw::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "WitnessThread: failed to decode LeaseRequest -- dropping");
        return;
    }
    if (request.pair_id != config_.voting_pair.pair_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "WitnessThread: LeaseRequest for pair {} -- dropping, the witness votes only in pair {} ({})", request.pair_id, config_.voting_pair.pair_id,
                   config_.voting_pair.name);
        return;
    }

    const pubsub_itc_fw::LeaseVoter::Answer answer = voter_->consider(request.candidate_instance_id, request.epoch, std::chrono::steady_clock::now());

    if (answer.verdict == pubsub_itc_fw::LeaseVoter::Verdict::Granted) {
        pubsub_itc_fw::LeaseGrant grant{};
        grant.voter_instance_id = witness_voter_id;
        grant.pair_id = config_.voting_pair.pair_id;
        grant.epoch = request.epoch;
        grant.request_id = request.request_id;
        send_pdu(conn_id, pubsub_itc_fw::LeaseGrant::message_pdu_id, 0, grant);
        // Logged when the arbiter holding the vote changes, not at every renewal.
        if (request.candidate_instance_id != last_granted_to_) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "WitnessThread: vote granted to arbiter {} at epoch {}", request.candidate_instance_id,
                       request.epoch);
            last_granted_to_ = request.candidate_instance_id;
        }
        return;
    }

    pubsub_itc_fw::LeaseRefusal refusal{};
    refusal.voter_instance_id = witness_voter_id;
    refusal.pair_id = config_.voting_pair.pair_id;
    refusal.highest_epoch = answer.highest_epoch;
    refusal.request_id = request.request_id;
    refusal.reason = refusal_reason_for(answer.verdict);
    send_pdu(conn_id, pubsub_itc_fw::LeaseRefusal::message_pdu_id, 0, refusal);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "WitnessThread: refused arbiter {} at epoch {} ({})", request.candidate_instance_id,
               request.epoch, pubsub_itc_fw::to_string(refusal.reason));
}

void WitnessThread::on_pubsub_message(const pubsub_itc_fw::EventMessage& message) {
    if (!pubsub_itc_fw::TechnicalEventsSubscription::is_end_of_day_end(message, config_.trading_day)) {
        return;
    }
    pubsub_itc_fw::TechnicalEventsSubscription::record_the_end(config_.last_state_file, config_.trading_day);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "WitnessThread: the end-of-day end of trading day {} at seq={} arrived on the technical_events topic -- stopping for the end of day",
               config_.trading_day, message.seq_no());
    stop_for_end_of_day(fmt::format("the end-of-day end of trading day {} at seq={}, from the technical_events topic", config_.trading_day, message.seq_no()));
}

} // namespaces
