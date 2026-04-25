// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewaySeqThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sample_fix_gateway_seq {

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
    cfg.pool_name        = "FixGatewaySeqPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

FixGatewaySeqThread::FixGatewaySeqThread(
    pubsub_itc_fw::ApplicationThread::ConstructorToken token,
    pubsub_itc_fw::QuillLogger& logger,
    pubsub_itc_fw::Reactor& reactor,
    const FixGatewaySeqConfiguration& config)
    : ApplicationThread(token, logger, reactor, "FixGatewaySeqThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{}
{}

void FixGatewaySeqThread::on_app_ready_event()
{
    connect_to_service("sequencer_primary");
    connect_to_service("sequencer_secondary");
}

void FixGatewaySeqThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    // TODO: distinguish FIX client connections from sequencer/ME connections
    // using the ServiceRegistry service name associated with this connection.
    // For now log the event.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} established", id.get_value());
}

void FixGatewaySeqThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                              const std::string& reason)
{
    fix_client_sessions_.erase(id);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} lost: {}", id.get_value(), reason);
}

void FixGatewaySeqThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message)
{
    // TODO: parse inbound FIX bytes, build fix_equity_orders PDU,
    // record cl_ord_id -> connection ID mapping, then forward PDU to
    // both sequencer connections.
    //
    // For now consume all bytes so the mirrored buffer does not stall.
    commit_raw_bytes(message.connection_id(),
                     static_cast<int64_t>(message.payload_size()));

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: raw FIX bytes received on connection {} ({} bytes) -- stub",
               message.connection_id().get_value(), message.payload_size());
}

void FixGatewaySeqThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message)
{
    // TODO: decode ExecutionReport PDU from fix_equity_orders.hpp.
    // Look up cl_ord_id in cl_ord_id_to_session_. If absent, log and drop.
    // Otherwise serialise a FIX ExecutionReport and send_raw to the client.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: ER PDU received on connection {} -- stub",
               message.connection_id().get_value());
}

void FixGatewaySeqThread::on_timer_event([[maybe_unused]] const std::string& name)
{
    // TODO: handle logon timeout timers per FIX client session.
}

void FixGatewaySeqThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace sample_fix_gateway_seq
