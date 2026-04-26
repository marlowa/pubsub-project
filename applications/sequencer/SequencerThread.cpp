// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sequencer {

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
    cfg.pool_name        = "SequencerPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

SequencerThread::SequencerThread(
    pubsub_itc_fw::ApplicationThread::ConstructorToken token,
    pubsub_itc_fw::QuillLogger& logger,
    pubsub_itc_fw::Reactor& reactor,
    const SequencerConfiguration& config)
    : ApplicationThread(token, logger, reactor, "SequencerThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , gateway_conn_id_{}
{}

void SequencerThread::on_app_ready_event()
{
    connect_to_service("gateway");
    connect_to_service("sequencer_peer");
    connect_to_service("arbiter");
}

void SequencerThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    // TODO: use ServiceRegistry to identify whether this is the ME connection,
    // the peer sequencer connection, or the arbiter connection, and update
    // the leader-follower state machine accordingly.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: connection {} established", id.get_value());
}

void SequencerThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                          const std::string& reason)
{
    // TODO: trigger leader-follower state machine transition on relevant
    // connection losses (peer, arbiter).
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: connection {} lost: {}", id.get_value(), reason);
}

void SequencerThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message)
{
    // Two PDU types arrive here:
    //   - Order PDUs (NewOrderSingle, OrderCancelRequest) from gateways on listen_port.
    //     If leader: wrap in SequencedMessage with next_sequence_number_++, encode,
    //     and send_pdu to matching_engine inbound (ME connects to er_listen_port).
    //     If follower: discard (already in sync via receipt).
    //   - ExecutionReport PDUs from the ME on er_listen_port.
    //     Forward to the originating gateway via gateway_conn_id_.
    // TODO: distinguish by connection ID once IDs are tracked per connection type.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: PDU received on connection {} seq={} -- stub",
               message.connection_id().get_value(), next_sequence_number_);
}

void SequencerThread::on_timer_event([[maybe_unused]] const std::string& name)
{
    // TODO: handle leader-follower heartbeat timer.
}

void SequencerThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace sequencer
