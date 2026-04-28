// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>

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
    , peer_conn_id_{}
    , arbiter_conn_id_{}
{}

void SequencerThread::on_app_ready_event()
{
    connect_to_service("gateway");
    connect_to_service("sequencer_peer");
    connect_to_service("arbiter");
}

void SequencerThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    if (id.service_name() == "gateway") {
        gateway_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: gateway connection {} established", id.get_value());
    } else if (id.service_name() == "sequencer_peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: peer sequencer connection {} established", id.get_value());
    } else if (id.service_name() == "arbiter") {
        arbiter_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: arbiter connection {} established", id.get_value());
    } else {
        // Inbound connection -- either order PDU from gateway or ER from ME.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: inbound connection {} established", id.get_value());
    }
}

void SequencerThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                          const std::string& reason)
{
    if (id == gateway_conn_id_) {
        gateway_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: gateway connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: peer sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_conn_id_) {
        arbiter_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: arbiter connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
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
#if 0
    // 1. Identify incoming PDU metadata from the EventMessage data (which includes the header).
    const auto* incoming_hdr = reinterpret_cast<const pubsub_itc_fw::PduHeader*>(message.data());
    const uint32_t original_payload_len = ntohl(incoming_hdr->byte_count);

    // 2. Calculate sizes for the SequencedMessage envelope.
    // The envelope body consists of the DSL struct plus the entire original PDU.
    const uint32_t envelope_body_size = sizeof(pubsub_itc_fw_app::SequencedMessage) +
                                       sizeof(pubsub_itc_fw::PduHeader) +
                                       original_payload_len;
    const size_t total_frame_size = sizeof(pubsub_itc_fw::PduHeader) + envelope_body_size;

    auto [slab_id, chunk] = outbound_allocator().allocate(total_frame_size);

    // 4. Construct the outer PduHeader (Topic: SequencedMessage).
    auto* out_hdr = reinterpret_cast<pubsub_itc_fw::PduHeader*>(chunk);
    out_hdr->byte_count = htonl(envelope_body_size);
    out_hdr->pdu_id     = htons(static_cast<int16_t>(pubsub_itc_fw_app::Topics::SequencedMessage));
    out_hdr->version    = 1;
    out_hdr->filler_a   = 0;
    out_hdr->canary     = htonl(pubsub_itc_fw::pdu_canary_value);
    out_hdr->filler_b   = 0;

    // 5. Fill the SequencedMessage metadata.
    auto* seq_msg = reinterpret_cast<pubsub_itc_fw_app::SequencedMessage*>(out_hdr + 1);
    seq_msg->sequence_number = next_sequence_number_++;
    seq_msg->timestamp_ns    = pubsub_itc_fw::HighResolutionClock::now().time_since_epoch().count();

    // 6. Perform the opaque copy.
    // The original PDU (Header + Payload) is copied into the memory following the metadata.
    std::memcpy(seq_msg + 1, message.data(), sizeof(pubsub_itc_fw::PduHeader) + original_payload_len);

    // 7. Dispatch to the Matching Engine via the framework helper.
    // Assumes matching_engine_conn_id_ was captured during on_connection_established.
    enqueue_send_pdu_command(matching_engine_conn_id_, slab_id, chunk, static_cast<uint32_t>(total_frame_size));
#endif

}

void SequencerThread::on_timer_event([[maybe_unused]] const std::string& name)
{
    // TODO: handle leader-follower heartbeat timer.
}

void SequencerThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace sequencer
