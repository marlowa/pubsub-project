// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
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
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning,
                   "ArbiterPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

ArbiterThread::ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                             const ArbiterConfiguration& config)
    : ApplicationThread(token, logger, reactor, "ArbiterThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config) {}

void ArbiterThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: sequencer connection {} established", id.get_value());
}

void ArbiterThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "ArbiterThread: sequencer connection {} lost: {}", id.get_value(), reason);
}

void ArbiterThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    // Currently the only PDU the sequencer sends here is a periodic Heartbeat
    // (pdu_id=102) to keep the inbound inactivity timeout from firing under
    // heavy order load.  Receiving any data resets the timer; no reply needed.
    // TODO: decode ArbitrationReport PDU (pdu_id=200) and reply with ArbitrationDecision.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "ArbiterThread: PDU pdu_id={} received on connection {} -- heartbeat acknowledged",
               message.pdu_id(), message.connection_id().get_value());
    release_pdu_payload(message);
}

void ArbiterThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void ArbiterThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

} // namespace arbiter
