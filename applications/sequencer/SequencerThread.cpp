// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sequencer {

namespace {

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const SequencerConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "SequencerPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "SequencerPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

SequencerThread::SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                                 const SequencerConfiguration& config)
    : ApplicationThread(token, logger, reactor, "SequencerThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , order_inbound_svc_("inbound:" + std::to_string(config.listen_port))
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , gateway_conn_id_{}
    , me_outbound_order_conn_id_{}
    , peer_conn_id_{}
    , peer_inbound_conn_id_{}
    , arbiter_primary_conn_id_{}
    , arbiter_secondary_conn_id_{} {}

void SequencerThread::on_initial_event() {
    if (config_.replay_mode) {
        // Replay mode: open WAL with a buffering callback that accumulates all
        // records into replay_buffer_.  dispatch_replay_records() sends them to
        // the ME once the ME connection is established.
        const int64_t recovered_seq = wal_.open(
            config_.wal_directory, config_.wal_segment_size,
            [this](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size, int64_t wall_time_ns) {
                ReplayRecord rec;
                rec.seq_no = seq_no;
                rec.pdu_id = pdu_id;
                rec.wall_time_ns = wall_time_ns;
                rec.payload.assign(payload, payload + payload_size);
                replay_buffer_.push_back(std::move(rec));
            },
            WalOpenMode{WalOpenMode::IgnoreSnapshot});
        next_sequence_number_ = recovered_seq > 0 ? recovered_seq + 1 : 1;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: replay mode -- WAL read complete: {} record(s), last seq_no={}, "
                   "next_sequence_number={}",
                   replay_buffer_.size(), recovered_seq, next_sequence_number_);
        // Start as leader so that send_pdu paths are active.
        ++epoch_;
        adopt_role(pubsub_itc_fw_app::Role::leader);
        return;
    }

    // Normal mode: WAL replay is used only to recover next_sequence_number_.
    // The routing map is intentionally not rebuilt: by the time the sequencer
    // restarts, the ME has almost certainly already sent ERs for any in-flight
    // orders from the previous run, and those ERs will not be re-sent.
    // Populating the routing map from WAL replay would leave entries that are
    // never erased, causing unbounded heap growth under high throughput.
    // ERs for unroutable seq_nos are handled gracefully by the "not in
    // routing map" fallback in on_framework_pdu_message().
    const int64_t recovered_seq = wal_.open(config_.wal_directory, config_.wal_segment_size, nullptr);
    if (recovered_seq > 0) {
        next_sequence_number_ = recovered_seq + 1;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WAL open complete: recovered seq_no={}, record_count={}, "
                   "next_sequence_number={}",
                   recovered_seq, wal_.record_count(), next_sequence_number_);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL is fresh (no prior records), starting from seq_no=1");
    }

    start_recurring_timer("wal_snapshot", std::chrono::seconds(config_.snapshot_interval_seconds));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL snapshot timer started (interval={}s)", config_.snapshot_interval_seconds);

    if (!config_.ha_enabled) {
        // Single-node mode: start as leader immediately, no election needed.
        ++epoch_;
        adopt_role(pubsub_itc_fw_app::Role::leader);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=false -- starting as leader immediately");
    } else {
        // HA mode: arm startup election window. If no peer contact within this
        // window, self-promote. Shorter than heartbeat_timeout_seconds so that
        // single-node HA deployments (peer down) also recover quickly.
        start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.startup_election_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=true -- startup election timeout armed ({}s)",
                   config_.startup_election_timeout_seconds);
    }
}

void SequencerThread::on_app_ready_event() {
    if (config_.replay_mode) {
        // Replay mode: connect only to the matching engine; skip gateway, HA
        // arbiters, and peer replication.
        connect_to_service("matching_engine");
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay mode -- connecting to matching engine only");
        return;
    }

    connect_to_service("gateway");
    connect_to_service("matching_engine");
    if (config_.ha_enabled) {
        connect_to_service("arbiter_primary");
        connect_to_service("arbiter_secondary");
        connect_to_service("peer");
    }
}

void SequencerThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    if (svc == "gateway") {
        gateway_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway connection {} established", id.get_value());
    } else if (svc == "matching_engine") {
        me_outbound_order_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: matching engine order connection {} established", id.get_value());
        if (config_.replay_mode) {
            replay_me_order_ready_ = true;
            try_dispatch_replay();
        }
    } else if (svc == "arbiter_primary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-primary connection {} established", id.get_value());
        if (first_arbiter) {
            start_recurring_timer("arbiter_heartbeat", std::chrono::seconds{30});
        }
    } else if (svc == "arbiter_secondary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-secondary connection {} established", id.get_value());
        if (first_arbiter) {
            start_recurring_timer("arbiter_heartbeat", std::chrono::seconds{30});
        }
    } else if (svc == "peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: outbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
    } else {
        // Inbound connection identified by listener port:
        //   inbound:7001 / inbound:7002 -- gateway sending order PDUs
        //   inbound:7021 / inbound:7022 -- matching engine sending ER PDUs
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} established ({})", id.get_value(), svc);
        if (config_.replay_mode && svc == er_inbound_svc_) {
            // The ME has connected to our ER port and can now receive ER PDUs
            // back from the ME after we dispatch. Safe to send orders now.
            replay_me_er_ready_ = true;
            try_dispatch_replay();
        }
    }
}

void SequencerThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == gateway_conn_id_) {
        gateway_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway connection {} lost: {}", id.get_value(), reason);
    } else if (id == me_outbound_order_conn_id_) {
        me_outbound_order_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: matching engine order connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer("arbiter_heartbeat");
        }
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer("arbiter_heartbeat");
        }
    } else if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: outbound peer connection {} lost: {}", id.get_value(), reason);
        flush_pending_er();
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: inbound peer connection {} lost: {}", id.get_value(), reason);
        flush_pending_er();
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
}

void SequencerThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "TRACE on_framework_pdu_message: msg.connection_id value={} service_name=[{}]",
               conn_id.get_value(), svc);

    // Peer PDUs arrive on the outbound peer connection or from the inbound peer listener.

    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // Arbiter PDUs: only ArbitrationDecision (pdu_id=201) is expected from either arbiter.
    if (conn_id == arbiter_primary_conn_id_ || conn_id == arbiter_secondary_conn_id_) {
        handle_arbitration_decision(message);
        release_pdu_payload(message);
        return;
    }

    const bool is_order_pdu = (svc == order_inbound_svc_);
    const bool is_er_pdu = (svc == er_inbound_svc_);

    if (is_order_pdu) {
        // Order PDU from a gateway instance. As leader, stamp a sequence number
        // and forward to the matching engine.
        //
        // seq_no is carried in the PDU transport header (third arg to send_pdu).
        // The ME reads it via message.seq_no() and echoes it on the ER reply.
        const int64_t seq = next_sequence_number_++;
        const int64_t wall_time_ns = config_.wall_clock->now_ns();

        // WAL commit: only the leader appends from the direct gateway PDU.
        // Followers write their WAL exclusively via WalRecord from the leader,
        // which ensures WALs are identical byte-for-byte.  Unknown role (during
        // election startup) appends locally because it may become the leader.
        if (role_ != pubsub_itc_fw_app::Role::follower) {
            wal_.append(seq, message.pdu_id(), message.payload(), message.payload_size(), wall_time_ns);
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order PDU on connection {} pdu_id={} seq={} -- WAL append ok (wal_size={}) role={}",
                       message.connection_id().get_value(), message.pdu_id(), seq, wal_.record_count(), pubsub_itc_fw_app::to_string(role_));
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order PDU on connection {} pdu_id={} seq={} -- follower, WAL written via WalRecord",
                       message.connection_id().get_value(), message.pdu_id(), seq);
        }

        if (role_ != pubsub_itc_fw_app::Role::leader) {
            // Follower/unknown: do not forward to ME.
            release_pdu_payload(message);
            return;
        }

        if (!me_outbound_order_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: matching engine not connected -- dropping order PDU");
            release_pdu_payload(message);
            return;
        }

        // Forward the raw PDU payload to the ME by re-encoding it.
        // We use the pdu_id from the incoming message so the ME sees the
        // correct topic tag (NewOrderSingle=1000, OrderCancelRequest=1001).
        const auto pdu_id = message.pdu_id();
        bool forwarded_to_me = false;

        if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle)) {
            auto& arena_buf = decode_arena_buffer();
            pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
            arena.reset();
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::NewOrderSingleView view{};

            if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
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
            nos.has_sender_comp_id = view.has_sender_comp_id;
            nos.sender_comp_id = view.sender_comp_id;

            // Stamp wall time so the ME can use the original sequencing time
            // for transact_time during replay instead of calling system_clock::now().
            nos.has_sequenced_at = true;
            nos.sequenced_at = wall_time_ns;

            // Record seq_no → gateway_session_conn_id for ER routing back to the
            // exact FIX session.  seq_no is globally unique; gateway_session_conn_id
            // identifies the specific connection within the gateway.
            if (view.has_gateway_session_conn_id) {
                seq_no_to_session_conn_id_[seq] = view.gateway_session_conn_id;
            }

            send_pdu(me_outbound_order_conn_id_, pdu_id, seq, nos);
            forwarded_to_me = true;

        } else if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest)) {
            auto& arena_buf = decode_arena_buffer();
            pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
            arena.reset();
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::OrderCancelRequestView view{};

            if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
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
            ocr.has_sender_comp_id = view.has_sender_comp_id;
            ocr.sender_comp_id = view.sender_comp_id;

            ocr.has_sequenced_at = true;
            ocr.sequenced_at = wall_time_ns;

            // Record seq_no → gateway_session_conn_id for cancel-ER routing.
            if (view.has_gateway_session_conn_id) {
                seq_no_to_session_conn_id_[seq] = view.gateway_session_conn_id;
            }

            send_pdu(me_outbound_order_conn_id_, pdu_id, seq, ocr);
            forwarded_to_me = true;

        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unknown order PDU id {} -- dropping", pdu_id);
        }

        // Replicate to follower before releasing the slab so the payload pointer stays valid.
        if (forwarded_to_me && config_.ha_enabled) {
            send_wal_record(seq, pdu_id, message, wall_time_ns);
        }

        release_pdu_payload(message);

    } else if (is_er_pdu) {
        // ExecutionReport from the ME. Leader forwards to gateway; follower drops.
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER PDU on connection {} -- follower, discarding",
                       message.connection_id().get_value());
            release_pdu_payload(message);
            return;
        }

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER PDU on connection {} pdu_id={} seq={} -- forwarding to gateway",
                   message.connection_id().get_value(), message.pdu_id(), message.seq_no());

        if (!gateway_conn_id_.is_valid()) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway not connected -- dropping ER PDU");
            release_pdu_payload(message);
            return;
        }

        const auto pdu_id = message.pdu_id();
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::ExecutionReportView view{};

        if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
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

        // Look up the originating FIX session by the ER's seq_no.  The ME echoes
        // the NOS's seq_no back in the ER transport envelope, so message.seq_no()
        // here is the same value that was stored in seq_no_to_session_conn_id_
        // when the NOS was forwarded.  seq_no is globally unique, eliminating the
        // ClOrdID-collision problem that occurred when multiple FIX sessions
        // submitted orders with identical ClOrdID sequences.
        const int64_t er_seq_no = message.seq_no();
        bool erase_routing_entry = false;
        {
            auto it = seq_no_to_session_conn_id_.find(er_seq_no);
            if (it != seq_no_to_session_conn_id_.end()) {
                er.has_gateway_session_conn_id = true;
                er.gateway_session_conn_id = it->second;

                switch (view.ord_status) {
                    case pubsub_itc_fw_app::OrdStatus::Filled:
                    case pubsub_itc_fw_app::OrdStatus::Canceled:
                    case pubsub_itc_fw_app::OrdStatus::Rejected:
                    case pubsub_itc_fw_app::OrdStatus::Expired:
                    case pubsub_itc_fw_app::OrdStatus::DoneForDay:
                    case pubsub_itc_fw_app::OrdStatus::Replaced:
                        erase_routing_entry = true;
                        break;
                    default:
                        break;
                }
            } else {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: ER seq_no={} not in routing map -- forwarding without gateway_session_conn_id", er_seq_no);
            }
        }

        if (!needs_wal_ack()) {
            send_pdu(gateway_conn_id_, pdu_id, er_seq_no, er);
            release_pdu_payload(message);
            if (erase_routing_entry) {
                seq_no_to_session_conn_id_.erase(er_seq_no);
            }
        } else {
            auto acked_it = wal_acked_seq_nos_.find(er_seq_no);
            if (acked_it != wal_acked_seq_nos_.end()) {
                // Follower already acked this seq_no; forward immediately.
                wal_acked_seq_nos_.erase(acked_it);
                send_pdu(gateway_conn_id_, pdu_id, er_seq_no, er);
                release_pdu_payload(message);
                if (erase_routing_entry) {
                    seq_no_to_session_conn_id_.erase(er_seq_no);
                }
            } else {
                // WalAck not yet received; buffer the ER until it arrives.
                PendingEr pending{};
                pending.pdu_id = pdu_id;
                pending.seq_no = er_seq_no;
                pending.payload.assign(message.payload(), message.payload() + static_cast<size_t>(message.payload_size()));
                pending.has_gateway_session_conn_id = er.has_gateway_session_conn_id;
                pending.gateway_session_conn_id = er.gateway_session_conn_id;
                pending.erase_routing_entry = erase_routing_entry;
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq={} buffered -- awaiting WalAck from follower", er_seq_no);
                pending_er_.emplace(er_seq_no, std::move(pending));
                release_pdu_payload(message);
            }
        }

    } else {
        // Unknown source -- log and discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: PDU on unexpected connection {} ({}) -- dropping",
                   message.connection_id().get_value(), svc);
        release_pdu_payload(message);
    }
}

void SequencerThread::on_timer_event(const std::string& name) {
    if (name == "wal_snapshot") {
        try {
            wal_.take_snapshot();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL snapshot taken: last_seq_no={}, record_count={}",
                       wal_.last_seq_no(), wal_.record_count());
        } catch (const std::exception& ex) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: WAL snapshot failed: {}", ex.what());
        }
        return;
    }

    if (name == "peer_heartbeat") {
        send_peer_heartbeat();
        return;
    }

    if (name == "arbiter_heartbeat") {
        send_arbiter_heartbeat();
        return;
    }

    if (name == "peer_heartbeat_timeout") {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return; // already leader, nothing to do
        }

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: peer heartbeat timeout (role={})", pubsub_itc_fw_app::to_string(role_));

        if (arbiter_primary_conn_id_.is_valid() || arbiter_secondary_conn_id_.is_valid()) {
            // Ask the active arbiter to break the tie.  Arm a fallback timer so we
            // self-promote if no arbiter responds in time.
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: requesting arbitration from arbiter pool");
            send_arbitration_report();
            start_one_off_timer("arbitration_timeout", std::chrono::seconds(config_.arbitration_timeout_seconds));
        } else {
            // No arbiter connected — degrade to local instance-id rule.
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: no arbiter connected -- self-promoting using instance-id rule (degraded)");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }

    if (name == "arbitration_timeout") {
        // Witness did not reply in time. Fall back to local instance-id rule.
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: arbitration timeout -- arbiter unreachable, self-promoting using instance-id rule (degraded)");
            ++epoch_;
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }
}

void SequencerThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// ---------------------------------------------------------------------------
// Leader-follower state machine helpers
// ---------------------------------------------------------------------------

// PDU IDs for the leader-follower protocol (plain integers, not Topics enum).
static constexpr int16_t pdu_status_query = 100;
static constexpr int16_t pdu_status_response = 101;
static constexpr int16_t pdu_heartbeat = 102;
static constexpr int16_t pdu_wal_record = 103;
static constexpr int16_t pdu_wal_ack = 104;
static constexpr int16_t pdu_arbitration_report = 200;
static constexpr int16_t pdu_arbitration_decision = 201;

pubsub_itc_fw::ConnectionID SequencerThread::peer_active_conn() const {
    if (peer_conn_id_.is_valid()) {
        return peer_conn_id_;
    }
    return peer_inbound_conn_id_;
}

void SequencerThread::adopt_role(pubsub_itc_fw_app::Role new_role) {
    if (new_role == role_) {
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        write_fence_file();
        cancel_timer("peer_heartbeat_timeout");
        start_recurring_timer("peer_heartbeat", std::chrono::seconds(config_.heartbeat_interval_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: now LEADER -- heartbeat timer started ({}s interval)",
                   config_.heartbeat_interval_seconds);
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        start_recurring_timer("peer_heartbeat", std::chrono::seconds(config_.heartbeat_interval_seconds));
        // Arm (or re-arm) the heartbeat timeout.
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.heartbeat_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: now FOLLOWER -- heartbeat timer started, timeout armed ({}s)",
                   config_.heartbeat_timeout_seconds);
    }
}

void SequencerThread::elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role) {
    if (role_ == pubsub_itc_fw_app::Role::leader || role_ == pubsub_itc_fw_app::Role::follower) {
        // Already elected: just update epoch knowledge if the peer is ahead.
        if (peer_epoch > epoch_) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: peer epoch {} > my epoch {} -- unexpected (already elected as {})",
                       peer_epoch, epoch_, pubsub_itc_fw_app::to_string(role_));
        }
        return;
    }

    // Stale check: if the peer has a higher epoch it was leader in a previous
    // generation and we are a restarting stale node.
    if (peer_epoch > epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: peer epoch {} > my epoch {} -- adopting follower (peer is newer generation)", peer_epoch, epoch_);
        epoch_ = peer_epoch;
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }

    // If the peer already elected itself as leader, adopt follower.
    if (peer_current_role == pubsub_itc_fw_app::Role::leader) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: peer (instance_id={}) is already leader -- adopting follower",
                   peer_instance_id);
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }

    // Both unknown: lowest instance_id wins.
    if (static_cast<int64_t>(config_.instance_id) < peer_instance_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: my instance_id={} < peer instance_id={} -- adopting leader",
                   config_.instance_id, peer_instance_id);
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: my instance_id={} >= peer instance_id={} -- adopting follower",
                   config_.instance_id, peer_instance_id);
        adopt_role(pubsub_itc_fw_app::Role::follower);
    }
}

void SequencerThread::send_status_query(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusQuery sq{};
    sq.instance_id = static_cast<int64_t>(config_.instance_id);
    sq.epoch = epoch_;
    send_pdu(conn_id, pdu_status_query, 0, sq);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusQuery sent on connection {} (instance_id={} epoch={})",
               conn_id.get_value(), sq.instance_id, sq.epoch);
}

void SequencerThread::send_status_response(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusResponse sr{};
    sr.self_instance_id = static_cast<int64_t>(config_.instance_id);
    sr.peer_instance_id = 0; // we don't know the peer's ID here; it's in the query
    sr.epoch = epoch_;
    sr.current_role = role_;
    sr.next_sequence_number = next_sequence_number_;
    send_pdu(conn_id, pdu_status_response, 0, sr);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusResponse sent on connection {} (role={} epoch={} next_seq={})",
               conn_id.get_value(), pubsub_itc_fw_app::to_string(role_), epoch_, next_sequence_number_);
}

void SequencerThread::send_peer_heartbeat() {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: heartbeat timer fired but no peer connection -- skipping");
        return;
    }
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    send_pdu(target, pdu_heartbeat, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: Heartbeat sent to peer (epoch={})", epoch_);
}

void SequencerThread::send_arbiter_heartbeat() {
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_heartbeat, 0, hb);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_heartbeat, 0, hb);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: arbiter heartbeat sent (instance_id={} epoch={})", hb.instance_id, hb.epoch);
}

void SequencerThread::send_arbitration_report() {
    pubsub_itc_fw_app::ArbitrationReport report{};
    report.self_instance_id = static_cast<int64_t>(config_.instance_id);
    report.peer_instance_id = peer_instance_id_;
    report.epoch = epoch_;
    report.proposed_role = pubsub_itc_fw_app::Role::leader;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_arbitration_report, 0, report);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_arbitration_report, 0, report);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ArbitrationReport sent to arbiter pool (self_instance_id={} peer_instance_id={} epoch={})", report.self_instance_id,
               report.peer_instance_id, report.epoch);
}

void SequencerThread::handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message) {
    cancel_timer("arbitration_timeout");

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbitrationDecisionView decision{};

    if (!pubsub_itc_fw_app::decode(decision, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ArbitrationDecision -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ArbitrationDecision received (leader={} follower={} epoch={})",
               decision.leader_instance_id, decision.follower_instance_id, decision.epoch);

    epoch_ = decision.epoch;

    if (decision.leader_instance_id == static_cast<int64_t>(config_.instance_id)) {
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else if (decision.follower_instance_id == static_cast<int64_t>(config_.instance_id)) {
        adopt_role(pubsub_itc_fw_app::Role::follower);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: ArbitrationDecision does not mention this instance (instance_id={}) -- ignoring", config_.instance_id);
    }
}

void SequencerThread::write_fence_file() const {
    const std::string& path = config_.fence_file_path;
    const std::string content = std::to_string(config_.instance_id) + "\n";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to write fence file '{}': {}", path, std::strerror(errno));
        return;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: fence file written: {}", path);
}

void SequencerThread::handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::StatusQueryView sq{};

    if (!pubsub_itc_fw_app::decode(sq, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode StatusQuery -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusQuery received from peer (instance_id={} epoch={})", sq.instance_id,
               sq.epoch);

    peer_instance_id_ = sq.instance_id;

    // Reply immediately so the peer can run election logic on our response.
    send_status_response(conn_id);

    // Run our own election based on the peer's identity.
    elect_role(sq.instance_id, sq.epoch, pubsub_itc_fw_app::Role::unknown);
}

void SequencerThread::handle_peer_status_response(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::StatusResponseView sr{};

    if (!pubsub_itc_fw_app::decode(sr, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode StatusResponse -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusResponse received from peer (self_id={} epoch={} role={} next_seq={})",
               sr.self_instance_id, sr.epoch, pubsub_itc_fw_app::to_string(sr.current_role), sr.next_sequence_number);

    peer_instance_id_ = sr.self_instance_id;

    elect_role(sr.self_instance_id, sr.epoch, sr.current_role);

    // Sync next_sequence_number_ if the peer is ahead. This covers WAL recovery:
    // a restarting node reads its WAL and gets next_sequence_number_=N, but the
    // peer (leader) may have advanced to M > N while this node was down. Without
    // this sync the restarted follower would stamp wrong seq numbers and corrupt
    // the sequence when it is later promoted to leader.
    if (sr.next_sequence_number > next_sequence_number_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: advancing next_sequence_number_ from {} to {} (peer is ahead)",
                   next_sequence_number_, sr.next_sequence_number);
        next_sequence_number_ = sr.next_sequence_number;
    }
}

void SequencerThread::handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::HeartbeatView hb{};

    if (!pubsub_itc_fw_app::decode(hb, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode Heartbeat -- dropping");
        return;
    }

    if (hb.epoch < epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: Heartbeat from stale peer (peer epoch={} < my epoch={}) -- ignoring",
                   hb.epoch, epoch_);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: Heartbeat received from peer (instance_id={} epoch={})", hb.instance_id,
               hb.epoch);

    // Reset the heartbeat timeout whenever we receive a valid heartbeat.
    if (role_ == pubsub_itc_fw_app::Role::follower) {
        cancel_timer("peer_heartbeat_timeout");
        start_one_off_timer("peer_heartbeat_timeout", std::chrono::seconds(config_.heartbeat_timeout_seconds));
    }
}

void SequencerThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());

    if (pdu_id == pdu_status_query) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pdu_status_response) {
        handle_peer_status_response(message);
    } else if (pdu_id == pdu_heartbeat) {
        handle_peer_heartbeat(message);
    } else if (pdu_id == pdu_wal_record) {
        handle_wal_record(conn_id, message);
    } else if (pdu_id == pdu_wal_ack) {
        handle_wal_ack(message);
    } else if (pdu_id == pdu_arbitration_decision) {
        // Should not arrive on the peer channel — decisions come from the arbiter.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: ArbitrationDecision received on peer channel (unexpected) -- dropping");
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unknown peer PDU id {} -- dropping", pdu_id);
    }
}

// ---------------------------------------------------------------------------
// Replay mode dispatch
// ---------------------------------------------------------------------------

void SequencerThread::try_dispatch_replay() {
    if (replay_me_order_ready_ && replay_me_er_ready_) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay -- both ME connections ready, starting dispatch");
        dispatch_replay_records();
    }
}

void SequencerThread::dispatch_replay_records() {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay -- dispatching {} record(s) to matching engine", replay_buffer_.size());

    size_t dispatched = 0;
    for (const auto& record : replay_buffer_) {
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;

        if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle)) {
            pubsub_itc_fw_app::NewOrderSingleView view{};
            if (!pubsub_itc_fw_app::decode(view, record.payload.data(), record.payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: replay -- failed to decode NOS seq={} -- skipping",
                           record.seq_no);
                continue;
            }

            pubsub_itc_fw_app::NewOrderSingle nos{};
            nos.cl_ord_id = view.cl_ord_id;
            nos.side = view.side;
            nos.symbol = view.symbol;
            nos.ord_type = view.ord_type;
            nos.transact_time = view.transact_time;
            nos.order_qty = view.order_qty;
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
            nos.has_sender_comp_id = view.has_sender_comp_id;
            nos.sender_comp_id = view.sender_comp_id;
            nos.has_gateway_session_conn_id = view.has_gateway_session_conn_id;
            nos.gateway_session_conn_id = view.gateway_session_conn_id;
            // Stamp the original sequencing time so the ME uses it for transact_time.
            nos.has_sequenced_at = true;
            nos.sequenced_at = record.wall_time_ns;

            send_pdu(me_outbound_order_conn_id_, record.pdu_id, record.seq_no, nos);

        } else if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest)) {
            pubsub_itc_fw_app::OrderCancelRequestView view{};
            if (!pubsub_itc_fw_app::decode(view, record.payload.data(), record.payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: replay -- failed to decode OCR seq={} -- skipping",
                           record.seq_no);
                continue;
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
            ocr.has_sender_comp_id = view.has_sender_comp_id;
            ocr.sender_comp_id = view.sender_comp_id;
            ocr.has_gateway_session_conn_id = view.has_gateway_session_conn_id;
            ocr.gateway_session_conn_id = view.gateway_session_conn_id;
            ocr.has_sequenced_at = true;
            ocr.sequenced_at = record.wall_time_ns;

            send_pdu(me_outbound_order_conn_id_, record.pdu_id, record.seq_no, ocr);

        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: replay -- unknown pdu_id={} seq={} -- skipping", record.pdu_id,
                       record.seq_no);
            continue;
        }
        ++dispatched;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay complete -- {}/{} record(s) dispatched to matching engine", dispatched,
               replay_buffer_.size());

    replay_buffer_.clear();
    replay_buffer_.shrink_to_fit();
}

// ---------------------------------------------------------------------------
// WAL replication helpers (Slice 7)
// ---------------------------------------------------------------------------

bool SequencerThread::needs_wal_ack() const {
    return config_.ha_enabled && peer_active_conn().is_valid();
}

void SequencerThread::send_wal_record(int64_t seq, int16_t wal_pdu_id, const pubsub_itc_fw::EventMessage& message, int64_t wall_time_ns) {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::WalRecord wal_record{};
    wal_record.seq_no = seq;
    wal_record.pdu_id = wal_pdu_id;
    wal_record.payload.data = message.payload();
    wal_record.payload.size = static_cast<size_t>(message.payload_size());
    wal_record.wall_time_ns = wall_time_ns;
    send_pdu(target, pdu_wal_record, seq, wal_record);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: WalRecord sent to follower seq={} pdu_id={}", seq, wal_pdu_id);
}

void SequencerThread::handle_wal_record(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode WalRecord -- dropping");
        return;
    }

    wal_.append(view.seq_no, view.pdu_id, view.payload.data, static_cast<int>(view.payload.size), view.wall_time_ns);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "SequencerThread: WalRecord seq={} pdu_id={} written to follower WAL (wal_size={}) -- sending WalAck", view.seq_no, view.pdu_id,
               wal_.record_count());

    pubsub_itc_fw_app::WalAck wal_ack{};
    wal_ack.seq_no = view.seq_no;
    send_pdu(conn_id, pdu_wal_ack, 0, wal_ack);
}

void SequencerThread::handle_wal_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalAckView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode WalAck -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: WalAck received seq={}", view.seq_no);

    auto it = pending_er_.find(view.seq_no);
    if (it != pending_er_.end()) {
        forward_pending_er(it->second);
        pending_er_.erase(it);
    } else {
        // ER hasn't arrived yet; record the ack so the ER path can forward immediately on arrival.
        wal_acked_seq_nos_.insert(view.seq_no);
    }
}

void SequencerThread::flush_pending_er() {
    if (!pending_er_.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: peer lost -- flushing {} buffered ERs to gateway (degraded mode)",
                   pending_er_.size());
        for (auto& [seq_no, pending] : pending_er_) {
            forward_pending_er(pending);
        }
        pending_er_.clear();
    }
    wal_acked_seq_nos_.clear();
}

void SequencerThread::forward_pending_er(const PendingEr& pending) {
    if (!gateway_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway not connected -- dropping buffered ER");
        return;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ExecutionReportView view{};

    if (!pubsub_itc_fw_app::decode(view, pending.payload.data(), pending.payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode buffered ExecutionReport -- dropping");
        return;
    }

    // pending.payload is alive for the lifetime of this call; string_view fields
    // in both view and er borrow from it and remain valid through send_pdu.
    pubsub_itc_fw_app::ExecutionReport er{};
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

    if (pending.has_gateway_session_conn_id) {
        er.has_gateway_session_conn_id = true;
        er.gateway_session_conn_id = pending.gateway_session_conn_id;
    }

    send_pdu(gateway_conn_id_, pending.pdu_id, pending.seq_no, er);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: buffered ER seq={} forwarded to gateway", pending.seq_no);

    if (pending.erase_routing_entry) {
        seq_no_to_session_conn_id_.erase(pending.seq_no);
    }
}

} // namespace sequencer
