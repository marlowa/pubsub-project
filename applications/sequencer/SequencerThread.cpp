// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
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
    , matching_engine_conn_id_{}
    , arbiter_conn_id_{}
{}

void SequencerThread::on_app_ready_event()
{
    connect_to_service("gateway");
    connect_to_service("arbiter");
    /*
     * The peer-to-peer connection is part of the leader-follower protocol
     * (StatusQuery / StatusResponse / Heartbeat). The protocol is defined
     * in DSL but not yet implemented, so issuing a Connect for the peer
     * here just produces a perpetual retry loop against a port that no
     * sequencer instance currently binds. Re-enable this when the
     * leader-follower protocol implementation lands.
     */
}

void SequencerThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    if (id.service_name() == "gateway") {
        gateway_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: gateway connection {} established", id.get_value());
    } else if (id.service_name() == "arbiter") {
        arbiter_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: arbiter connection {} established", id.get_value());
    } else {
        // Inbound connection -- identify by port.
        // port 7001 / 7002: gateway connects inbound with order PDUs
        // port 7021 / 7022: matching engine connects inbound to send ERs back
        const std::string& svc = id.service_name();
        if (svc == "inbound:7021" || svc == "inbound:7022") {
            matching_engine_conn_id_ = id;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: matching engine ER connection {} established ({})",
                       id.get_value(), svc);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: inbound connection {} established ({})",
                       id.get_value(), svc);
        }
    }
}

void SequencerThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                          const std::string& reason)
{
    if (id == gateway_conn_id_) {
        gateway_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: gateway connection {} lost: {}", id.get_value(), reason);
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
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "TRACE on_framework_pdu_message: msg.connection_id value={} service_name=[{}]",
               conn_id.get_value(), svc);

    // Order PDUs arrive from the gateway on port 7001 (primary) or 7002 (secondary).
    // ER PDUs arrive from the ME on port 7021 (primary ER listener) or 7022 (secondary).
    const bool is_order_pdu = (svc == "inbound:7001" || svc == "inbound:7002");
    const bool is_er_pdu    = (svc == "inbound:7021" || svc == "inbound:7022");

    if (is_order_pdu) {
        // Order PDU from a gateway instance. As leader, stamp a sequence number
        // and forward to the matching engine.
        //
        // TODO: leader-follower -- followers should discard rather than forward.
        // For now the stub behaves as unconditional leader.
        //
        // TODO: SequencedMessage envelope -- currently we forward the raw PDU
        // bytes directly to the ME without wrapping. The SequencedMessage
        // wrapper will be added once the ME decode path is implemented.
        const int64_t seq = next_sequence_number_++;

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: order PDU on connection {} pdu_id={} seq={} -- forwarding to ME",
                   message.connection_id().get_value(), message.pdu_id(), seq);

        if (!matching_engine_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                "SequencerThread: matching engine not connected -- dropping order PDU");
            release_pdu_payload(message);
            return;
        }

        // Forward the raw PDU payload to the ME by re-encoding it.
        // We use the pdu_id from the incoming message so the ME sees the
        // correct topic tag (NewOrderSingle=1000, OrderCancelRequest=1001).
        const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());

        if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle)) {
            auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
            std::size_t arena_bytes_needed = 0;
            std::size_t bytes_consumed     = 0;
            pubsub_itc_fw_app::NewOrderSingleView view{};

            if (!pubsub_itc_fw_app::decode(view,
                                            message.payload(),
                                            static_cast<std::size_t>(message.payload_size()),
                                            bytes_consumed,
                                            arena,
                                            arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                    "SequencerThread: failed to decode NewOrderSingle -- dropping");
                release_pdu_payload(message);
                return;
            }

            // Re-encode into an owning struct and send to ME.
            pubsub_itc_fw_app::NewOrderSingle nos{};
            nos.cl_ord_id     = std::string(view.cl_ord_id);
            nos.symbol        = std::string(view.symbol);
            nos.side          = view.side;
            nos.ord_type      = view.ord_type;
            nos.transact_time = view.transact_time;
            nos.order_qty     = std::string(view.order_qty);
            nos.has_price          = view.has_price;
            nos.price              = std::string(view.price);
            nos.has_time_in_force  = view.has_time_in_force;
            nos.time_in_force      = view.time_in_force;

            send_pdu(matching_engine_conn_id_, pdu_id, nos);

        } else if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest)) {
            auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
            std::size_t arena_bytes_needed = 0;
            std::size_t bytes_consumed     = 0;
            pubsub_itc_fw_app::OrderCancelRequestView view{};

            if (!pubsub_itc_fw_app::decode(view,
                                            message.payload(),
                                            static_cast<std::size_t>(message.payload_size()),
                                            bytes_consumed,
                                            arena,
                                            arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                    "SequencerThread: failed to decode OrderCancelRequest -- dropping");
                release_pdu_payload(message);
                return;
            }

            pubsub_itc_fw_app::OrderCancelRequest ocr{};
            ocr.orig_cl_ord_id = std::string(view.orig_cl_ord_id);
            ocr.cl_ord_id      = std::string(view.cl_ord_id);
            ocr.symbol         = std::string(view.symbol);
            ocr.side           = view.side;
            ocr.transact_time  = view.transact_time;
            ocr.order_qty      = std::string(view.order_qty);

            send_pdu(matching_engine_conn_id_, pdu_id, ocr);

        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                "SequencerThread: unknown order PDU id {} -- dropping", pdu_id);
        }

        release_pdu_payload(message);

    } else if (is_er_pdu) {
        // ExecutionReport from the ME. Forward to the gateway.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: ER PDU on connection {} -- forwarding to gateway",
                   message.connection_id().get_value());

        if (!gateway_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                "SequencerThread: gateway not connected -- dropping ER PDU");
            release_pdu_payload(message);
            return;
        }

        const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        std::size_t arena_bytes_needed = 0;
        std::size_t bytes_consumed     = 0;
        pubsub_itc_fw_app::ExecutionReportView view{};

        if (!pubsub_itc_fw_app::decode(view,
                                        message.payload(),
                                        static_cast<std::size_t>(message.payload_size()),
                                        bytes_consumed,
                                        arena,
                                        arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                "SequencerThread: failed to decode ExecutionReport -- dropping");
            release_pdu_payload(message);
            return;
        }

        pubsub_itc_fw_app::ExecutionReport er{};
        er.cl_ord_id   = std::string(view.cl_ord_id);
        er.exec_id     = std::string(view.exec_id);
        er.exec_type   = view.exec_type;
        er.ord_status  = view.ord_status;
        er.symbol      = std::string(view.symbol);
        er.side        = view.side;
        er.order_qty   = std::string(view.order_qty);
        er.last_qty    = std::string(view.last_qty);
        er.last_px     = std::string(view.last_px);
        er.leaves_qty  = std::string(view.leaves_qty);
        er.cum_qty     = std::string(view.cum_qty);
        er.avg_px      = std::string(view.avg_px);
        er.transact_time = view.transact_time;

        send_pdu(gateway_conn_id_, pdu_id, er);
        release_pdu_payload(message);

    } else {
        // Unknown source -- log and discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: PDU on unexpected connection {} ({}) -- dropping",
                   message.connection_id().get_value(), svc);
        release_pdu_payload(message);
    }
}

void SequencerThread::on_timer_event([[maybe_unused]] const std::string& name)
{
    // TODO: handle leader-follower heartbeat timer.
}

void SequencerThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

} // namespace sequencer
