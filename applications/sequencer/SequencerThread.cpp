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

static pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static pubsub_itc_fw::AllocatorConfiguration make_allocator_config() {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name = "SequencerPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools = 1;
    return cfg;
}

SequencerThread::SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                                 const SequencerConfiguration& config)
    : ApplicationThread(token, logger, reactor, "SequencerThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , gateway_conn_id_{}
    , me_outbound_order_conn_id_{}
    , arbiter_conn_id_{} {}

void SequencerThread::on_app_ready_event() {
    connect_to_service("gateway");
    connect_to_service("matching_engine");
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

void SequencerThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "gateway") {
        gateway_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway connection {} established", id.get_value());
    } else if (id.service_name() == "matching_engine") {
        me_outbound_order_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: matching engine order connection {} established", id.get_value());
    } else if (id.service_name() == "arbiter") {
        arbiter_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter connection {} established", id.get_value());
    } else {
        // Inbound connection identified by listener port:
        //   inbound:7001 / inbound:7002 -- gateway sending order PDUs
        //   inbound:7021 / inbound:7022 -- matching engine sending ER PDUs
        const std::string& svc = id.service_name();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} established ({})", id.get_value(), svc);
    }
}

void SequencerThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == gateway_conn_id_) {
        gateway_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway connection {} lost: {}", id.get_value(), reason);
    } else if (id == me_outbound_order_conn_id_) {
        me_outbound_order_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: matching engine order connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_conn_id_) {
        arbiter_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
}

void SequencerThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "TRACE on_framework_pdu_message: msg.connection_id value={} service_name=[{}]",
               conn_id.get_value(), svc);

    // Order PDUs arrive from the gateway on port 7001 (primary) or 7002 (secondary).
    // ER PDUs arrive from the ME on port 7021 (primary ER listener) or 7022 (secondary).
    const bool is_order_pdu = (svc == "inbound:7001" || svc == "inbound:7002");
    const bool is_er_pdu = (svc == "inbound:7021" || svc == "inbound:7022");

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

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: order PDU on connection {} pdu_id={} seq={} -- forwarding to ME",
                   message.connection_id().get_value(), message.pdu_id(), seq);

        if (!me_outbound_order_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: matching engine not connected -- dropping order PDU");
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
            std::size_t bytes_consumed = 0;
            pubsub_itc_fw_app::NewOrderSingleView view{};

            if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<std::size_t>(message.payload_size()), bytes_consumed, arena,
                                           arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode NewOrderSingle -- dropping");
                release_pdu_payload(message);
                return;
            }

            // Re-encode into an owning struct and send to ME.
            //
            // The string fields below are std::string_view, not std::string.
            // We assign view.X directly: those views point into the slab
            // payload owned by the inbound EventMessage, which is alive until
            // release_pdu_payload(message) is called below (after send_pdu
            // returns). Wrapping the assignment in std::string(view.X) would
            // create a temporary that dies at the semicolon, leaving the
            // string_view dangling -- visible at runtime as small-string
            // optimisation bookkeeping bytes leaking into the encoded wire
            // payload (e.g. cl_ord_id encoded as 0x0F 0x00 0x00 0x00).
            //
            // Optional fields: every has_* flag is copied along with its
            // value so anything the gateway populated reaches the ME.
            pubsub_itc_fw_app::NewOrderSingle nos{};

            // Required fields.
            nos.cl_ord_id = view.cl_ord_id;
            nos.side = view.side;
            nos.symbol = view.symbol;
            nos.ord_type = view.ord_type;
            nos.transact_time = view.transact_time;
            nos.order_qty = view.order_qty;

            // Optional fields.
            nos.has_price = view.has_price;
            nos.price = view.price;
            nos.has_stop_px = view.has_stop_px;
            nos.stop_px = view.stop_px;
            nos.has_time_in_force = view.has_time_in_force;
            nos.time_in_force = view.time_in_force;
            nos.has_account = view.has_account;
            nos.account = view.account;
            nos.has_ex_destination = view.has_ex_destination;
            nos.ex_destination = view.ex_destination;
            nos.has_exec_inst = view.has_exec_inst;
            nos.exec_inst = view.exec_inst;
            nos.has_min_qty = view.has_min_qty;
            nos.min_qty = view.min_qty;
            nos.has_max_floor = view.has_max_floor;
            nos.max_floor = view.max_floor;
            nos.has_expire_time = view.has_expire_time;
            nos.expire_time = view.expire_time;
            nos.has_text = view.has_text;
            nos.text = view.text;

            send_pdu(me_outbound_order_conn_id_, pdu_id, seq, nos);

        } else if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest)) {
            auto& arena_buf = decode_arena_buffer();
            pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
            arena.reset();
            std::size_t arena_bytes_needed = 0;
            std::size_t bytes_consumed = 0;
            pubsub_itc_fw_app::OrderCancelRequestView view{};

            if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<std::size_t>(message.payload_size()), bytes_consumed, arena,
                                           arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode OrderCancelRequest -- dropping");
                release_pdu_payload(message);
                return;
            }

            pubsub_itc_fw_app::OrderCancelRequest ocr{};
            ocr.orig_cl_ord_id = view.orig_cl_ord_id;
            ocr.cl_ord_id = view.cl_ord_id;
            ocr.symbol = view.symbol;
            ocr.side = view.side;
            ocr.transact_time = view.transact_time;
            ocr.order_qty = view.order_qty;
            ocr.has_account = view.has_account;
            ocr.account = view.account;
            ocr.has_text = view.has_text;
            ocr.text = view.text;

            send_pdu(me_outbound_order_conn_id_, pdu_id, seq, ocr);

        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unknown order PDU id {} -- dropping", pdu_id);
        }

        release_pdu_payload(message);

    } else if (is_er_pdu) {
        // ExecutionReport from the ME. Forward to the gateway.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ER PDU on connection {} pdu_id={} seq={} -- forwarding to gateway",
                   message.connection_id().get_value(), message.pdu_id(), message.seq_no());

        if (!gateway_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway not connected -- dropping ER PDU");
            release_pdu_payload(message);
            return;
        }

        const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        std::size_t arena_bytes_needed = 0;
        std::size_t bytes_consumed = 0;
        pubsub_itc_fw_app::ExecutionReportView view{};

        if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<std::size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ExecutionReport -- dropping");
            release_pdu_payload(message);
            return;
        }

        // Re-encode into an owning struct and forward to the gateway.
        // String fields are std::string_view assigned directly from the
        // view; the slab payload backing the view is alive until
        // release_pdu_payload(message) below.
        //
        // The ExecutionReport schema has many optional fields. Each is
        // gated by a has_* flag; the value field is meaningful only when
        // the flag is set. The forward-as-is policy is to copy every
        // has_* flag from view to er, and copy each value unconditionally
        // (when the flag is false the value is the schema default and the
        // encoder will not emit the value field).
        pubsub_itc_fw_app::ExecutionReport er{};

        // Required fields.
        er.order_id = view.order_id;
        er.exec_id = view.exec_id;
        er.exec_type = view.exec_type;
        er.ord_status = view.ord_status;
        er.symbol = view.symbol;
        er.side = view.side;
        er.leaves_qty = view.leaves_qty;
        er.cum_qty = view.cum_qty;
        er.avg_px = view.avg_px;
        er.transact_time = view.transact_time;

        // Optional fields -- copy has_* flag and value together.
        er.has_cl_ord_id = view.has_cl_ord_id;
        er.cl_ord_id = view.cl_ord_id;
        er.has_orig_cl_ord_id = view.has_orig_cl_ord_id;
        er.orig_cl_ord_id = view.orig_cl_ord_id;
        er.has_ord_type = view.has_ord_type;
        er.ord_type = view.ord_type;
        er.has_price = view.has_price;
        er.price = view.price;
        er.has_stop_px = view.has_stop_px;
        er.stop_px = view.stop_px;
        er.has_order_qty = view.has_order_qty;
        er.order_qty = view.order_qty;
        er.has_time_in_force = view.has_time_in_force;
        er.time_in_force = view.time_in_force;
        er.has_account = view.has_account;
        er.account = view.account;
        er.has_ex_destination = view.has_ex_destination;
        er.ex_destination = view.ex_destination;
        er.has_exec_inst = view.has_exec_inst;
        er.exec_inst = view.exec_inst;
        er.has_last_qty = view.has_last_qty;
        er.last_qty = view.last_qty;
        er.has_last_px = view.has_last_px;
        er.last_px = view.last_px;
        er.has_trade_date = view.has_trade_date;
        er.trade_date = view.trade_date;
        er.has_exec_ref_id = view.has_exec_ref_id;
        er.exec_ref_id = view.exec_ref_id;
        er.has_ord_rej_reason = view.has_ord_rej_reason;
        er.ord_rej_reason = view.ord_rej_reason;
        er.has_cxl_rej_reason = view.has_cxl_rej_reason;
        er.cxl_rej_reason = view.cxl_rej_reason;
        er.has_text = view.has_text;
        er.text = view.text;
        er.has_min_qty = view.has_min_qty;
        er.min_qty = view.min_qty;
        er.has_max_floor = view.has_max_floor;
        er.max_floor = view.max_floor;
        er.has_expire_time = view.has_expire_time;
        er.expire_time = view.expire_time;

        send_pdu(gateway_conn_id_, pdu_id, message.seq_no(), er);
        release_pdu_payload(message);

    } else {
        // Unknown source -- log and discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: PDU on unexpected connection {} ({}) -- dropping",
                   message.connection_id().get_value(), svc);
        release_pdu_payload(message);
    }
}

void SequencerThread::on_timer_event([[maybe_unused]] const std::string& name) {
    // TODO: handle leader-follower heartbeat timer.
}

void SequencerThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

} // namespace sequencer
