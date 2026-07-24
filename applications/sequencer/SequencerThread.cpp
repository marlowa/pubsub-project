// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <cstring>

#include <array>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

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

} // namespaces

// PDU IDs for the leader-follower and external WAL subscriber protocols.

SequencerThread::SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                                 const SequencerConfiguration& config)
    : ApplicationThread(token, logger, reactor, "SequencerThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , order_inbound_svc_("inbound:" + std::to_string(config.listen_port))
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , wal_subscriber_inbound_svc_("inbound:" + std::to_string(config.wal_subscriber_listen_port))
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
            pubsub_itc_fw::WalOpenMode{pubsub_itc_fw::WalOpenMode::IgnoreSnapshot});
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
        connect_to_service("matching_engine_secondary");
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
    } else if (svc == "matching_engine_secondary") {
        me_secondary_standby_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ME-secondary standby connection {} established (pre-warmed for failover)",
                   id.get_value());
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
        install_peer_wal_inline_handler(id);
        send_status_query(id);
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        install_peer_wal_inline_handler(id);
        send_status_query(id);
    } else if (svc == wal_subscriber_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: external WAL subscriber connection {} established -- awaiting WalSubscribeRequest", id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} established ({})", id.get_value(), svc);
        if (config_.replay_mode && svc == er_inbound_svc_) {
            // The ME has connected to our ER port and can now receive ER PDUs
            // back from the ME after we dispatch. Safe to send orders now.
            replay_me_er_ready_ = true;
            try_dispatch_replay();
        }
    }
}

void SequencerThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == gateway_conn_id_) {
        gateway_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway connection {} lost: {}", id.get_value(), reason);
    } else if (id == me_outbound_order_conn_id_) {
        me_outbound_order_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: matching engine order connection {} lost: {} -- ME-secondary may promote and reconnect", id.get_value(), reason);
    } else if (id == me_secondary_standby_conn_id_) {
        me_secondary_standby_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: ME-secondary standby connection {} lost: {}", id.get_value(), reason);
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
    } else if (id.service_name() == wal_subscriber_inbound_svc_) {
        wal_subscriber_conn_ids_.erase(id);
        external_wal_subscriber_registry_.remove_subscriber(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: external WAL subscriber connection {} lost: {} (remaining={})",
                   id.get_value(), reason, external_wal_subscriber_registry_.subscriber_count());
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

    // ME failover reconciliation (Slice D): a promoted ME-secondary sends
    // MePositionRequest over the sequencer's connection to trigger WAL catch-up.
    // In normal operation the request arrives on the pre-warmed standby
    // connection (me_secondary_standby_conn_id_); accept it on either the active
    // or standby ME connection for robustness.
    if (message.pdu_id() == pubsub_itc_fw_app::MePositionRequest::message_pdu_id &&
        (conn_id == me_outbound_order_conn_id_ || conn_id == me_secondary_standby_conn_id_)) {
        handle_me_position_request(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // External WAL subscriber PDUs (WalSubscribeRequest and WalAck from MEP).
    if (svc == wal_subscriber_inbound_svc_) {
        if (message.pdu_id() == pubsub_itc_fw_app::WalSubscribeRequest::message_pdu_id) {
            handle_wal_subscribe_request(conn_id, message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::WalAck::message_pdu_id) {
            handle_external_wal_ack(conn_id, message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: unexpected PDU {} on external WAL subscriber connection {} -- dropping", message.pdu_id(), conn_id.get_value());
        }
        release_pdu_payload(message);
        return;
    }

    const bool is_order_pdu = (svc == order_inbound_svc_);
    const bool is_er_pdu = (svc == er_inbound_svc_);

    if (is_order_pdu) {
        // Order arrives from the gateway wrapped in a WalRecord envelope: the routing
        // metadata (gateway_session_conn_id, sender_comp_id) rides on the envelope so
        // the DD-derived FIX PDU stays pure. The FIX payload is opaque here -- no field
        // hand-copy. Decode only the envelope, stamp seq_no + wall_time_ns, then
        // persist / replicate / stream / forward the SAME stamped envelope.
        //
        // seq_no is carried in the PDU transport header (third arg to send_pdu); the ME
        // reads it via message.seq_no() and echoes it on the ER reply.
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::WalRecordView inbound{};
        if (!pubsub_itc_fw_app::decode(inbound, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode order envelope -- dropping");
            release_pdu_payload(message);
            return;
        }

        const int16_t inner_pdu_id = inbound.pdu_id;
        if (inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
            inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unknown order envelope pdu_id {} -- dropping", inner_pdu_id);
            release_pdu_payload(message);
            return;
        }

        const int64_t seq = next_sequence_number_++;
        const int64_t wall_time_ns = config_.wall_clock->now_ns();

        // Stamp the envelope with the assigned seq_no and sequencing wall time. The
        // inner FIX payload (a BytesView into the inbound slab) stays valid until
        // release_pdu_payload(message) below, after every send has copied it.
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = seq;
        envelope.pdu_id = inner_pdu_id;
        envelope.payload = inbound.payload;
        envelope.wall_time_ns = wall_time_ns;
        envelope.has_gateway_session_conn_id = inbound.has_gateway_session_conn_id;
        envelope.gateway_session_conn_id = inbound.gateway_session_conn_id;
        envelope.has_sender_comp_id = inbound.has_sender_comp_id;
        envelope.sender_comp_id = inbound.sender_comp_id;

        // WAL commit: only the leader appends from the direct gateway PDU. Followers
        // write their WAL exclusively via WalRecord from the leader, keeping WALs
        // byte-identical. Unknown role (election startup) appends locally because it
        // may become the leader.
        if (role_ != pubsub_itc_fw_app::Role::follower) {
            append_envelope_to_wal(envelope);
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order envelope on connection {} inner_pdu_id={} seq={} -- WAL append ok (wal_size={}) role={}",
                       message.connection_id().get_value(), inner_pdu_id, seq, wal_.record_count(), pubsub_itc_fw_app::to_string(role_));
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order envelope on connection {} inner_pdu_id={} seq={} -- follower, WAL written via WalRecord",
                       message.connection_id().get_value(), inner_pdu_id, seq);
        }

        if (role_ != pubsub_itc_fw_app::Role::leader) {
            // Follower/unknown: do not forward to ME.
            release_pdu_payload(message);
            return;
        }

        if (!me_outbound_order_conn_id_.is_valid()) {
            // The order is already durably WAL-committed above; we simply cannot
            // forward it right now because no matching engine is connected (e.g.
            // during ME failover, before the promoted secondary reconnects). The
            // forward is deferred, NOT the order lost: on promotion the ME replays
            // the WAL from its last-applied seq up to the head, so this order is
            // recovered. Hence Info, not an alarming "dropped".
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: no matching engine connected -- order seq={} WAL-committed, "
                       "forward deferred until an ME reconnects (recovered via WAL replay on ME promotion)",
                       seq);
            release_pdu_payload(message);
            return;
        }

        // Record seq_no -> gateway_session_conn_id for ER routing back to the exact
        // FIX session. seq_no is globally unique; gateway_session_conn_id identifies
        // the specific connection within the gateway.
        if (inbound.has_gateway_session_conn_id) {
            seq_no_to_session_conn_id_[seq] = inbound.gateway_session_conn_id;
        }

        // Forward the stamped envelope to the ME. The ME unwraps it, reads
        // wall_time_ns as the sequencing time (transact_time during replay), and
        // decodes the inner FIX PDU. The FIX payload is passed through opaque -- no
        // field hand-copy.
        send_pdu(me_outbound_order_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq, envelope);

        // Replicate to the peer follower before releasing the slab so the inner
        // payload pointer stays valid.
        if (config_.ha_enabled) {
            send_wal_record(envelope);
        }

        // Stream to external WAL subscribers (MEP primary and secondary), which
        // unwrap the envelope and publish the inner DD-derived PDU on its topic.
        stream_wal_record_to_external_subscribers(envelope);

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

        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        // The ER arrives wrapped in a WalRecord envelope from the ME. Unwrap it; the
        // inner ER is decoded only to read ord_status (for routing-map eviction) -- its
        // payload is forwarded opaque.
        pubsub_itc_fw_app::WalRecordView inbound{};
        if (!pubsub_itc_fw_app::decode(inbound, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ER envelope -- dropping");
            release_pdu_payload(message);
            return;
        }
        if (inbound.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: ER envelope carries unexpected pdu_id {} -- dropping",
                       inbound.pdu_id);
            release_pdu_payload(message);
            return;
        }
        pubsub_itc_fw_app::ExecutionReportView view{};
        if (!pubsub_itc_fw_app::decode(view, inbound.payload.data, inbound.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ExecutionReport -- dropping");
            release_pdu_payload(message);
            return;
        }

        // Route the ER back to the originating FIX session. The ME echoes the order's
        // seq_no in the transport header, so message.seq_no() resolves via the map for
        // ordinary ERs. ERs not tied to a sequenced order (the seq_no==0 cancel-on-failover
        // ERs) instead carry the conn id on the inbound envelope. The conn id rides on the
        // envelope, never inside the DD-derived ER.
        const int64_t er_seq_no = message.seq_no();
        bool has_routing_conn = false;
        int32_t routing_conn_id = 0;
        bool erase_routing_entry = false;
        {
            auto it = seq_no_to_session_conn_id_.find(er_seq_no);
            if (it != seq_no_to_session_conn_id_.end()) {
                has_routing_conn = true;
                routing_conn_id = it->second;

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
            } else if (inbound.has_gateway_session_conn_id) {
                has_routing_conn = true;
                routing_conn_id = inbound.gateway_session_conn_id;
            } else {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                           "SequencerThread: ER seq_no={} not in routing map and no envelope conn id -- forwarding without gateway_session_conn_id", er_seq_no);
            }
        }

        // Sequence the ER into the WAL and deliver it the same three ways an order is
        // (append, replicate to the peer follower, stream to external subscribers) so
        // the MEP -- an external WAL subscriber -- publishes it on the execution_reports
        // topic. Each ER gets its OWN seq_no (an order can emit several ERs -- New, Fill,
        // Canceled -- so they cannot share the order's seq). The stored/replicated/streamed
        // record is the WalRecord-wrapped ER; the routing conn id rides on the envelope
        // (the MEP unwraps and publishes only the inner DD-derived ER). Replay skips ER
        // records (dispatch_replay_records only re-sends NOS/OCR). NOTE: the ER is
        // forwarded to the gateway gated on the *order's* WalAck, not this ER record's own
        // -- so at a failover instant a just-forwarded ER may be missing from the new
        // leader's WAL (an execution_reports-topic gap at the seam). Full two-tier commit
        // of ERs is a follow-up.
        const int64_t er_wal_seq = next_sequence_number_++;
        const int64_t er_wall_time_ns = config_.wall_clock->now_ns();

        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = er_wal_seq;
        envelope.pdu_id = inbound.pdu_id;
        envelope.payload = inbound.payload;
        envelope.wall_time_ns = er_wall_time_ns;
        envelope.has_gateway_session_conn_id = has_routing_conn;
        envelope.gateway_session_conn_id = routing_conn_id;

        append_envelope_to_wal(envelope);
        send_wal_record(envelope);
        stream_wal_record_to_external_subscribers(envelope);

        // Forward the envelope-wrapped ER to the gateway, gated on the follower having
        // durably committed the *order's* WAL entry (er_seq_no). The gateway unwraps the
        // envelope, reads gateway_session_conn_id, and routes to the FIX session.
        if (!needs_wal_ack()) {
            send_pdu(gateway_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, envelope);
            release_pdu_payload(message);
            if (erase_routing_entry) {
                seq_no_to_session_conn_id_.erase(er_seq_no);
            }
        } else {
            auto acked_it = wal_acked_seq_nos_.find(er_seq_no);
            if (acked_it != wal_acked_seq_nos_.end()) {
                // Follower already acked this seq_no; forward immediately.
                wal_acked_seq_nos_.erase(acked_it);
                send_pdu(gateway_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, envelope);
                release_pdu_payload(message);
                if (erase_routing_entry) {
                    seq_no_to_session_conn_id_.erase(er_seq_no);
                }
            } else {
                // WalAck not yet received; buffer the inner (unwrapped) ER until it arrives.
                PendingEr pending{};
                pending.pdu_id = inbound.pdu_id;
                pending.seq_no = er_seq_no;
                pending.payload.assign(inbound.payload.data, inbound.payload.data + inbound.payload.size);
                pending.has_gateway_session_conn_id = has_routing_conn;
                pending.gateway_session_conn_id = routing_conn_id;
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
            // No arbiter connected -- degrade to local instance-id rule.
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

// Leader-follower state machine helpers

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

    const auto transition_level = (role_ == pubsub_itc_fw_app::Role::unknown) ? pubsub_itc_fw::FwLogLevel::Info : pubsub_itc_fw::FwLogLevel::Warning;
    PUBSUB_LOG(get_logger(), transition_level, "SequencerThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
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
    send_pdu(conn_id, pubsub_itc_fw_app::StatusQuery::message_pdu_id, 0, sq);
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
    send_pdu(conn_id, pubsub_itc_fw_app::StatusResponse::message_pdu_id, 0, sr);
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
    hb.group = pubsub_itc_fw_app::ComponentGroup::sequencer;
    send_pdu(target, pubsub_itc_fw_app::Heartbeat::message_pdu_id, 0, hb);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: Heartbeat sent to peer (epoch={})", epoch_);
}

void SequencerThread::send_arbiter_heartbeat() {
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    hb.group = pubsub_itc_fw_app::ComponentGroup::sequencer;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pubsub_itc_fw_app::Heartbeat::message_pdu_id, 0, hb);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pubsub_itc_fw_app::Heartbeat::message_pdu_id, 0, hb);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: arbiter heartbeat sent (instance_id={} epoch={})", hb.instance_id, hb.epoch);
}

void SequencerThread::send_arbitration_report() {
    pubsub_itc_fw_app::ArbitrationReport report{};
    report.self_instance_id = static_cast<int64_t>(config_.instance_id);
    report.peer_instance_id = peer_instance_id_;
    report.epoch = epoch_;
    report.proposed_role = pubsub_itc_fw_app::Role::leader;
    report.group = pubsub_itc_fw_app::ComponentGroup::sequencer;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pubsub_itc_fw_app::ArbitrationReport::message_pdu_id, 0, report);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pubsub_itc_fw_app::ArbitrationReport::message_pdu_id, 0, report);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ArbitrationReport sent to arbiter pool (self_instance_id={} peer_instance_id={} epoch={})", report.self_instance_id,
               report.peer_instance_id, report.epoch);
}

void SequencerThread::handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message) {
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

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ArbitrationDecision received (group={} leader={} follower={} epoch={})",
               pubsub_itc_fw_app::to_string(decision.group), decision.leader_instance_id, decision.follower_instance_id, decision.epoch);

    // Defence in depth: the arbiter keys decisions by (group, instance_id), but
    // reject any decision not addressed to the sequencer group so a routing
    // mistake can never drive a spurious sequencer promotion or cancel our own
    // arbitration timeout. Validate before touching any state.
    if (decision.group != pubsub_itc_fw_app::ComponentGroup::sequencer) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: ArbitrationDecision addressed to group={} (not sequencer) -- ignoring",
                   pubsub_itc_fw_app::to_string(decision.group));
        return;
    }

    cancel_timer("arbitration_timeout");

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

    if (pdu_id == pubsub_itc_fw_app::StatusQuery::message_pdu_id) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::StatusResponse::message_pdu_id) {
        handle_peer_status_response(message);
    } else if (pdu_id == pubsub_itc_fw_app::Heartbeat::message_pdu_id) {
        handle_peer_heartbeat(message);
    } else if (pdu_id == pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        handle_wal_record(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::WalAck::message_pdu_id) {
        handle_wal_ack(message);
    } else if (pdu_id == pubsub_itc_fw_app::ArbitrationDecision::message_pdu_id) {
        // Should not arrive on the peer channel -- decisions come from the arbiter.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: ArbitrationDecision received on peer channel (unexpected) -- dropping");
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unknown peer PDU id {} -- dropping", pdu_id);
    }
}

// Replay mode dispatch

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
        // Each stored record is a WalRecord envelope (Option B). Decode it, then
        // re-send the envelope to the ME for NOS/OCR (the ME unwraps it and reads
        // wall_time_ns as the sequencing time); ER envelopes are outputs, not
        // inputs, and are not replayed to the ME.
        if (record.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: replay -- record seq={} is not a WalRecord (pdu_id={}) -- skipping",
                       record.seq_no, record.pdu_id);
            continue;
        }

        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::WalRecordView view{};
        if (!pubsub_itc_fw_app::decode(view, record.payload.data(), record.payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: replay -- failed to decode envelope seq={} -- skipping",
                       record.seq_no);
            continue;
        }

        if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
            view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
            continue;
        }

        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = view.seq_no;
        envelope.pdu_id = view.pdu_id;
        envelope.payload = view.payload;
        envelope.wall_time_ns = view.wall_time_ns;
        envelope.has_gateway_session_conn_id = view.has_gateway_session_conn_id;
        envelope.gateway_session_conn_id = view.gateway_session_conn_id;
        envelope.has_sender_comp_id = view.has_sender_comp_id;
        envelope.sender_comp_id = view.sender_comp_id;

        send_pdu(me_outbound_order_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, record.seq_no, envelope);
        ++dispatched;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay complete -- {}/{} record(s) dispatched to matching engine", dispatched,
               replay_buffer_.size());

    replay_buffer_.clear();
    replay_buffer_.shrink_to_fit();
}

// WAL replication helpers (Slice 7)

bool SequencerThread::needs_wal_ack() const {
    return config_.ha_enabled && peer_active_conn().is_valid();
}

void SequencerThread::append_envelope_to_wal(const pubsub_itc_fw_app::WalRecord& envelope) {
    // Option B: the WAL stores the WalRecord envelope itself (record pdu_id =
    // WalRecord), so the persisted bytes are byte-identical to the replication and
    // external-subscriber streams. Encode the envelope into a scratch buffer, then
    // hand it to the framework WAL under its own pdu_id. Measure then fit: a zero-size
    // out buffer makes encode report bytes_needed, then the reusable buffer is grown to
    // hold it -- no fixed cap that could silently fail to persist an over-large record,
    // and no per-record allocation after the buffer reaches its high-water mark.
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    [[maybe_unused]] const bool measured = pubsub_itc_fw_app::encode(envelope, nullptr, 0, bytes_written, bytes_needed);
    if (wal_encode_buffer_.size() < bytes_needed) {
        wal_encode_buffer_.resize(bytes_needed);
    }
    if (!pubsub_itc_fw_app::encode(envelope, wal_encode_buffer_.data(), wal_encode_buffer_.size(), bytes_written, bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: failed to encode envelope for seq={} ({} bytes needed) -- record NOT persisted", envelope.seq_no, bytes_needed);
        return;
    }
    wal_.append(envelope.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, wal_encode_buffer_.data(), static_cast<int>(bytes_written),
                envelope.wall_time_ns);
}

void SequencerThread::send_wal_record(const pubsub_itc_fw_app::WalRecord& envelope) {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid()) {
        return;
    }
    send_pdu(target, pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope.seq_no, envelope);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: WalRecord sent to follower seq={} inner_pdu_id={}", envelope.seq_no,
               envelope.pdu_id);
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

    // Option B: store the received WalRecord bytes verbatim under the WalRecord pdu
    // so the follower WAL is byte-identical to the leader's. (view is decoded only to
    // read seq_no + wall_time_ns for the append header and the WalAck.)
    wal_.append(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, message.payload(), message.payload_size(), view.wall_time_ns);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "SequencerThread: WalRecord seq={} inner_pdu_id={} written to follower WAL (wal_size={}) -- sending WalAck", view.seq_no, view.pdu_id,
               wal_.record_count());

    pubsub_itc_fw_app::WalAck wal_ack{};
    wal_ack.seq_no = view.seq_no;
    send_pdu(conn_id, pubsub_itc_fw_app::WalAck::message_pdu_id, 0, wal_ack);
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

void SequencerThread::install_peer_wal_inline_handler(const pubsub_itc_fw::ConnectionID& conn_id) {
    if (!config_.ha_enabled) {
        return;
    }

    install_inline_pdu_handler(conn_id, [this](pubsub_itc_fw::PduParser* parser, pubsub_itc_fw::PduFramer* framer) {
        parser->set_inline_handler([this, framer](int16_t pdu_id, int64_t /*seq_no*/, const uint8_t* payload, size_t size) -> bool {
            if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
                return false;
            }
            if (framer->has_pending_data()) {
                return false;
            }

            std::array<uint8_t, 4096> arena_buffer;
            pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::WalRecordView view{};

            if (!pubsub_itc_fw_app::decode(view, payload, size, bytes_consumed, arena, arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread (inline): failed to decode WalRecord -- falling back to ITC");
                return false;
            }

            // Option B: persist the received WalRecord bytes verbatim (record pdu_id =
            // WalRecord) so leader and follower WALs stay byte-identical.
            wal_.append(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, payload, static_cast<int>(size), view.wall_time_ns);

            pubsub_itc_fw_app::WalAck wal_ack{};
            wal_ack.seq_no = view.seq_no;
            std::array<uint8_t, 8> ack_buffer;
            pubsub_itc_fw_app::encode_fast(wal_ack, ack_buffer.data());

            auto [ok, err] = framer->send(pubsub_itc_fw_app::WalAck::message_pdu_id, 0, 0, ack_buffer.data(), static_cast<uint32_t>(ack_buffer.size()));
            if (!ok) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread (inline): WalAck send failed for seq={}: {}", view.seq_no, err);
            } else {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread (inline): WalRecord seq={} written and WalAck sent", view.seq_no);
            }

            return true;
        });
    });

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL inline handler installation requested for peer connection {}",
               conn_id.get_value());
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

    // Wrap the buffered raw ER in a WalRecord envelope carrying the routing conn id
    // and forward it to the gateway, which unwraps and routes to the FIX session.
    // pending.payload is alive for this call; the envelope's BytesView borrows it.
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = pending.seq_no;
    envelope.pdu_id = pending.pdu_id;
    envelope.payload.data = pending.payload.data();
    envelope.payload.size = pending.payload.size();
    envelope.has_gateway_session_conn_id = pending.has_gateway_session_conn_id;
    envelope.gateway_session_conn_id = pending.gateway_session_conn_id;

    send_pdu(gateway_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, pending.seq_no, envelope);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: buffered ER seq={} forwarded to gateway", pending.seq_no);

    if (pending.erase_routing_entry) {
        seq_no_to_session_conn_id_.erase(pending.seq_no);
    }
}

// External WAL subscriber helpers

void SequencerThread::handle_wal_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalSubscribeRequestView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode WalSubscribeRequest -- dropping");
        return;
    }

    const std::string subscriber_id(view.subscriber_id);
    const int64_t from_seq_no = view.from_seq_no;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WalSubscribeRequest subscriber_id={} from_seq_no={} conn={}", subscriber_id,
               from_seq_no, conn_id.get_value());

    const pubsub_itc_fw::ConnectionID orphan = external_wal_subscriber_registry_.register_subscriber(conn_id, subscriber_id, from_seq_no);
    if (orphan.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: displacing orphan connection {} for subscriber_id={}",
                   orphan.get_value(), subscriber_id);
        wal_subscriber_conn_ids_.erase(orphan);
        pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = orphan;
        get_reactor().enqueue_control_command(cmd);
    }

    wal_subscriber_conn_ids_.insert(conn_id);

    const int64_t accepted_from_seq_no = (from_seq_no == -1) ? wal_.last_seq_no() : from_seq_no;

    pubsub_itc_fw_app::WalSubscribeAck ack{};
    ack.accepted_from_seq_no = accepted_from_seq_no;
    send_pdu(conn_id, pubsub_itc_fw_app::WalSubscribeAck::message_pdu_id, 0, ack);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WalSubscribeAck sent subscriber_id={} accepted_from_seq_no={}", subscriber_id,
               accepted_from_seq_no);

    if (from_seq_no == -1) {
        return;
    }

    // Replay WAL records with seq_no > from_seq_no, unwrapping the payload format.
    [[maybe_unused]] auto end_pos =
        pubsub_itc_fw::WalReader::replay(config_.wal_directory, {0, 0}, [this, &conn_id, from_seq_no](int64_t record_id, const void* payload, size_t size) {
            if (record_id <= from_seq_no) {
                return;
            }
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return;
            }
            int64_t wall_time_ns{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
            int16_t pdu_id{};
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));
            const auto* pdu_payload = static_cast<const uint8_t*>(payload) + header_size;
            const size_t pdu_size = size - header_size;

            pubsub_itc_fw_app::WalRecord wal_record{};
            wal_record.seq_no = record_id;
            wal_record.pdu_id = pdu_id;
            wal_record.payload.data = pdu_payload;
            wal_record.payload.size = pdu_size;
            wal_record.wall_time_ns = wall_time_ns;
            send_pdu(conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, wal_record);
        });

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL replay complete for subscriber_id={}", subscriber_id);
}

void SequencerThread::handle_external_wal_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalAckView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode external WalAck -- dropping");
        return;
    }

    external_wal_subscriber_registry_.update_cursor(conn_id, view.seq_no);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: external WalAck conn={} seq={} min_cursor={}", conn_id.get_value(),
               view.seq_no, external_wal_subscriber_registry_.min_cursor());
}

void SequencerThread::stream_wal_record_to_external_subscribers(const pubsub_itc_fw_app::WalRecord& envelope) {
    if (wal_subscriber_conn_ids_.empty()) {
        return;
    }
    for (const auto& subscriber_conn_id : wal_subscriber_conn_ids_) {
        send_pdu(subscriber_conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope.seq_no, envelope);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: WalRecord seq={} inner_pdu_id={} streamed to {} external subscriber(s)",
               envelope.seq_no, envelope.pdu_id, wal_subscriber_conn_ids_.size());
}

// ME failover reconciliation (Slice D)

void SequencerThread::handle_me_position_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::MePositionRequestView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode MePositionRequest -- dropping");
        return;
    }

    const int64_t last_seq_no = view.last_seq_no;

    // Only the leader may serve WAL catch-up. The promoted ME asks every sequencer
    // it holds a pre-warmed order connection to (it cannot tell which one is the
    // leader); if a follower also streamed, the ME would replay the WAL twice and
    // process the second replay as live traffic once promoted. The follower instead
    // re-points its own ME order connection to the standby -- so that if it later
    // wins a sequencer election it forwards to the promoted ME -- and drops the
    // request without streaming or acking.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        if (conn_id == me_secondary_standby_conn_id_) {
            me_secondary_standby_conn_id_ = pubsub_itc_fw::ConnectionID{};
            me_outbound_order_conn_id_ = conn_id;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: MePositionRequest on connection {} but I am follower -- re-pointed ME order connection, not serving catch-up",
                   conn_id.get_value());
        return;
    }

    const int64_t wal_head = wal_.last_seq_no();
    me_catchup_conn_id_ = conn_id;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: MePositionRequest from connection {} last_seq_no={} -- streaming WAL catch-up up to head={}", conn_id.get_value(), last_seq_no,
               wal_head);

    // Walk the WAL from last_seq_no+1 to the head, unwrapping each stored record
    // and streaming the underlying NOS/OCR PDU directly to the ME connection.
    size_t streamed = 0;
    [[maybe_unused]] auto end_pos = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, {0, 0}, [this, &conn_id, last_seq_no, &streamed](int64_t record_id, const void* payload, size_t size) {
            if (record_id <= last_seq_no) {
                return;
            }
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return;
            }
            int64_t wall_time_ns{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
            int16_t pdu_id{};
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));
            const auto* pdu_payload = static_cast<const uint8_t*>(payload) + header_size;
            const size_t pdu_size = size - header_size;
            stream_wal_record_to_me(conn_id, record_id, pdu_id, pdu_payload, pdu_size, wall_time_ns);
            ++streamed;
        });

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL catch-up complete -- {} record(s) streamed to ME connection {}", streamed,
               conn_id.get_value());

    // Signal completion. On receipt the ME cancels its book and becomes leader.
    pubsub_itc_fw_app::MePositionAck ack{};
    ack.last_seq_no = wal_head;
    send_pdu(conn_id, pubsub_itc_fw_app::MePositionAck::message_pdu_id, 0, ack);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: MePositionAck sent (last_seq_no={}) -- ME is now live", wal_head);

    // Promote this connection to the active ME order connection so subsequent
    // sequenced orders flow to the newly-promoted ME. If the request arrived on
    // the standby connection (the normal failover case), this swaps the active
    // ME from the dead primary to the caught-up secondary. The old standby slot
    // is cleared; a future ME-primary restart would arrive on it again.
    if (conn_id == me_secondary_standby_conn_id_) {
        me_secondary_standby_conn_id_ = pubsub_itc_fw::ConnectionID{};
    }
    me_outbound_order_conn_id_ = conn_id;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ME order connection promoted to {} -- sequenced orders now route to the caught-up ME", conn_id.get_value());

    me_catchup_conn_id_ = pubsub_itc_fw::ConnectionID{};
}

void SequencerThread::stream_wal_record_to_me(const pubsub_itc_fw::ConnectionID& conn_id, int64_t record_id, int16_t pdu_id, const uint8_t* pdu_payload,
                                              size_t pdu_size, int64_t wall_time_ns) {
    // Each stored WAL record is a WalRecord envelope (Option B). Decode it and
    // re-send the envelope to the ME for NOS/OCR (the ME unwraps it and reads
    // wall_time_ns as the sequencing time); ER envelopes are outputs, not inputs,
    // and are not streamed to the ME during catch-up.
    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- record seq={} is not a WalRecord (pdu_id={}) -- skipping",
                   record_id, pdu_id);
        return;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, pdu_payload, pdu_size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- failed to decode envelope seq={} -- skipping", record_id);
        return;
    }

    if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        return;
    }

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = view.seq_no;
    envelope.pdu_id = view.pdu_id;
    envelope.payload = view.payload;
    envelope.wall_time_ns = view.wall_time_ns;
    envelope.has_gateway_session_conn_id = view.has_gateway_session_conn_id;
    envelope.gateway_session_conn_id = view.gateway_session_conn_id;
    envelope.has_sender_comp_id = view.has_sender_comp_id;
    envelope.sender_comp_id = view.sender_comp_id;

    send_pdu(conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, envelope);
    (void)wall_time_ns;
}

} // namespaces
