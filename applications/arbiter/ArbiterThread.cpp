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

static pubsub_itc_fw::QueueConfiguration make_queue_config()
{
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static pubsub_itc_fw::AllocatorConfiguration make_allocator_config()
{
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name        = "ArbiterPool";
    cfg.objects_per_pool = 16;
    cfg.initial_pools    = 1;
    return cfg;
}

ArbiterThread::ArbiterThread(
    pubsub_itc_fw::ApplicationThread::ConstructorToken token,
    pubsub_itc_fw::QuillLogger& logger,
    pubsub_itc_fw::Reactor& reactor,
    const ArbiterConfiguration& config)
    : ApplicationThread(token, logger, reactor, "ArbiterThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
{}

void ArbiterThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: sequencer connection {} established", id.get_value());
}

void ArbiterThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                        const std::string& reason)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: sequencer connection {} lost: {}", id.get_value(), reason);
}

void ArbiterThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message)
{
    // TODO: decode ArbitrationReport PDU and reply with ArbitrationDecision.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "ArbiterThread: ArbitrationReport PDU received on connection {} -- stub",
               message.connection_id().get_value());
}

void ArbiterThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void ArbiterThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace arbiter
