// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace matching_engine {

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
    cfg.pool_name        = "MatchingEnginePool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

MatchingEngineThread::MatchingEngineThread(
    pubsub_itc_fw::ApplicationThread::ConstructorToken token,
    pubsub_itc_fw::QuillLogger& logger,
    pubsub_itc_fw::Reactor& reactor,
    const MatchingEngineConfiguration& config)
    : ApplicationThread(token, logger, reactor, "MatchingEngineThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , sequencer_er_conn_id_{}
{}

void MatchingEngineThread::on_app_ready_event()
{
    connect_to_service("sequencer_er");
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    if (id.service_name() == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: sequencer ER connection {} established", id.get_value());
    } else {
        // Inbound connection from sequencer carrying sequenced order PDUs.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: sequencer order connection {} established", id.get_value());
    }
}

void MatchingEngineThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                               const std::string& reason)
{
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: sequencer order connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEngineThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message)
{
    // TODO: unwrap SequencedMessage envelope, decode inner PDU
    // (NewOrderSingle or OrderCancelRequest), run matching logic,
    // encode ExecutionReport PDU, and send_pdu to sequencer_er_conn_id_.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: sequenced PDU received on connection {} -- stub",
               message.connection_id().get_value());
}

void MatchingEngineThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void MatchingEngineThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace matching_engine
