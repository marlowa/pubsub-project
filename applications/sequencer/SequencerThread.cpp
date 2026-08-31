// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

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
    , me_outbound_order_conn_id_{}
    , peer_conn_id_{}
    , peer_inbound_conn_id_{}
    , arbiter_primary_conn_id_{}
    , arbiter_secondary_conn_id_{}
    , epoch_store_(config.wal_directory + "/epoch.state") {}

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
        // Start as leader so that send_pdu paths are active. Assign directly
        // rather than through set_epoch(): replay is an offline tool run against
        // a copy of the WAL, and it shares the WAL directory with the live
        // sequencer. Persisting from here would overwrite the epoch of a node
        // that is entitled to it.
        epoch_ = 1;
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

    // Recover the leadership generation before any election can run. Without
    // this the node starts at zero, and a pair restarted together would agree a
    // generation the venue has already spent -- see EpochStore.
    epoch_ = epoch_store_.load();
    if (epoch_ > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: recovered epoch={} from {}", epoch_, epoch_store_.path());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: no stored epoch at {} -- starting from epoch 0", epoch_store_.path());
    }

    wal_snapshot_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.snapshot_interval_seconds));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL snapshot timer started (interval={}s)", config_.snapshot_interval_seconds);

    if (!config_.ha_enabled) {
        // Single-node mode: start as leader immediately, no election needed.
        set_epoch(epoch_ + 1);
        adopt_role(pubsub_itc_fw_app::Role::leader);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=false -- starting as leader immediately");
    } else {
        // HA mode: arm startup election window. If no peer contact within this
        // window, self-promote. Shorter than heartbeat_timeout_seconds so that
        // single-node HA deployments (peer down) also recover quickly.
        peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.startup_election_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=true -- startup election timeout armed ({}s)",
                   config_.startup_election_timeout_seconds);
    }

    // Registered here rather than at ready, matching the gateways. The handle is a no-op when
    // metrics are disabled, and the application and component tokens come from configuration.
    if (!config_.wal_append_buckets.empty()) {
        wal_append_histogram_ = get_reactor().metrics().register_histogram(
            "sequencer_thread", "wal_append_nanoseconds", "Nanoseconds spent committing one record to the write-ahead log, on the reactor thread",
            config_.wal_append_buckets);
    }

    // Whether the log's next segment is being created ahead of the writer, or the writer is
    // having to do it. The append histogram says what a commit cost; these two say why. Without
    // them a tail that fails to improve cannot be told apart from a helper that never kept up.
    wal_segments_filled_inline_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "wal_segments_filled_inline",
        "Log segments the writer created itself instead of adopting one prepared ahead of it. Expected to be 1, for the first segment");
    wal_segments_waited_for_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "wal_segments_waited_for", "Segment rolls that had to wait for a preparation still in progress. Expected to be 0");
}

void SequencerThread::append_to_wal(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns) {
    // Every append goes through here so there is one place that knows what a commit costs.
    //
    // It is worth measuring because it is the one thing on the order path that touches a
    // disk. On 2026-08-31 this thread was measured in uninterruptible sleep for up to 557 ms
    // at a stretch, with no time on the run queue at all -- so it was waiting for I/O rather
    // than for a cpu. Nothing reported that: the only sign was the reactor's stall watchdog
    // noticing a callback had overrun, and the figure it prints is how long the callback had
    // taken SO FAR, which understates the wait.
    const auto started = std::chrono::steady_clock::now();
    wal_.append(seq_no, pdu_id, payload, size, wall_time_ns);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    wal_append_histogram_.observe(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

void SequencerThread::on_app_ready_event() {
    if (config_.replay_mode) {
        // Replay mode: connect only to the matching engine; skip gateway, HA
        // arbiters, and peer replication.
        connect_to_service("matching_engine");
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay mode -- connecting to matching engine only");
        return;
    }

    for (const auto& endpoint : config_.gateway_endpoints) {
        connect_to_service(endpoint.service_name());
    }
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

    const auto endpoint = std::find_if(config_.gateway_endpoints.begin(), config_.gateway_endpoints.end(),
                                       [&svc](const auto& candidate) { return candidate.service_name() == svc; });
    if (endpoint != config_.gateway_endpoints.end()) {
        gateway_conn_ids_[gateway_key(endpoint->protocol, endpoint->instance)] = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway protocol={} instance={} connection {} established",
                   endpoint->protocol, endpoint->instance, id.get_value());
        // Tell it the acceptance state straight away. A transition-only design gets exactly
        // this case wrong: a gateway starting during an outage would otherwise assume the
        // venue was fine, which is the default that caused BUG-0009 in the first place.
        // refresh_order_acceptance() broadcasts when it changes anything, and this connection
        // is already in the map by then, so sending again here would duplicate it.
        if (!refresh_order_acceptance()) {
            send_order_acceptance(id);
        }
    } else if (svc == "matching_engine") {
        // Claim the order connection only if nothing else currently holds it. An engine that
        // connects here is the one configured as primary, which is not the same thing as the
        // one that leads: after a failover the promoted secondary holds leadership, and a
        // restarting primary that took this slot would be sent orders it discards as a
        // follower. Its RoleAnnouncement decides, not the fact that it dialled in.
        me_order_conn_by_instance_[me_primary_instance_id] = id;
        if (me_announced_leader_instance_ == me_primary_instance_id) {
            // This instance told us it leads, before we had a way of reaching it. Now we do.
            me_outbound_order_conn_id_ = id;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: order connection {} to matching engine instance {}, which had already announced leadership -- routing there",
                       id.get_value(), me_primary_instance_id);
        } else if (!me_outbound_order_conn_id_.is_valid()) {
            me_outbound_order_conn_id_ = id;
        } else {
            me_secondary_standby_conn_id_ = id;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: matching engine connection {} established while connection {} is already active -- held as standby pending its role",
                       id.get_value(), me_outbound_order_conn_id_.get_value());
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: matching engine order connection {} established", id.get_value());
        if (config_.replay_mode) {
            replay_me_order_ready_ = true;
            try_dispatch_replay();
        }
    } else if (svc == "matching_engine_secondary") {
        me_secondary_standby_conn_id_ = id;
        me_order_conn_by_instance_[me_secondary_instance_id] = id;
        if (me_announced_leader_instance_ == me_secondary_instance_id) {
            me_outbound_order_conn_id_ = id;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: order connection {} to matching engine instance {}, which had already announced leadership -- routing there",
                       id.get_value(), me_secondary_instance_id);
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ME-secondary standby connection {} established (pre-warmed for failover)",
                   id.get_value());
    } else if (svc == "arbiter_primary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-primary connection {} established", id.get_value());
        if (first_arbiter) {
            cancel_timer(arbiter_heartbeat_timer_id_);
            arbiter_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds{30});
        }
    } else if (svc == "arbiter_secondary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-secondary connection {} established", id.get_value());
        if (first_arbiter) {
            cancel_timer(arbiter_heartbeat_timer_id_);
            arbiter_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds{30});
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

    // Report the end of an outage when the engine comes BACK, not when the next order happens to
    // arrive. The first version noticed recovery only on the forward path, so a venue that
    // recovered while nothing was trading never said so -- the operator was left with the last
    // warning and silence, which is the shape of the defect this is fixing.
    if (me_outbound_order_conn_id_.is_valid()) {
        note_matching_engine_reachable();
    }
}

void SequencerThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    const auto lost_gateway = std::find_if(gateway_conn_ids_.begin(), gateway_conn_ids_.end(), [&id](const auto& entry) { return entry.second == id; });
    if (lost_gateway != gateway_conn_ids_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway id {} connection {} lost: {}", lost_gateway->first,
                   id.get_value(), reason);
        gateway_conn_ids_.erase(lost_gateway);
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
            cancel_timer(arbiter_heartbeat_timer_id_);
        }
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer(arbiter_heartbeat_timer_id_);
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

    // A matching engine stating which role it holds. Accepted on either ME connection,
    // because which of them the leader is sitting on is exactly what this establishes.
    if (message.pdu_id() == pubsub_itc_fw_app::RoleAnnouncement::message_pdu_id) {
        handle_role_announcement(conn_id, message);
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

    // Session bindings from the gateways: which instance and connection a session identity
    // is reachable at right now. They arrive on the order connection because that is the
    // one a gateway already holds, and they are handled before the order/ER split because
    // they are neither.
    if (message.pdu_id() == pubsub_itc_fw_app::SessionBound::message_pdu_id) {
        handle_session_bound(conn_id, message);
        release_pdu_payload(message);
        return;
    }
    if (message.pdu_id() == pubsub_itc_fw_app::SessionUnbound::message_pdu_id) {
        handle_session_unbound(message);
        release_pdu_payload(message);
        return;
    }
    if (message.pdu_id() == pubsub_itc_fw_app::SessionSequenceUpdate::message_pdu_id) {
        handle_session_sequence_update(message);
        release_pdu_payload(message);
        return;
    }
    if (message.pdu_id() == pubsub_itc_fw_app::SessionReplayRequest::message_pdu_id) {
        handle_session_replay_request(conn_id, message);
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
            // The order is already durably WAL-committed above; we simply cannot forward it right
            // now because no matching engine is connected (during ME failover, before the promoted
            // secondary reconnects). The forward is deferred rather than the order lost -- but only
            // where a PROMOTION follows. A promoting follower reports where its replica reached and
            // is sent everything after it, which recovers this order; an engine that starts COLD
            // never reconciles at all and this order is never applied and never answered. That is
            // BUG-0064, and it is why the reassurance this comment used to give without
            // qualification was wrong.
            //
            // It costs the venue nothing to defer -- the payload is released here and the WAL is
            // the whole mechanism. It costs the MEMBER a great deal: it has been acknowledged, so
            // it believes the order is live, and it cannot cancel it because a cancel needs the
            // same matching engine. That is the asymmetry BUG-0009 is about, and why this is
            // reported by how long it has gone on rather than once per order.
            note_order_deferred(seq);
            release_pdu_payload(message);
            return;
        }

        // Reachable again. Anything deferred is about to be recovered by the promoted engine's
        // WAL replay, and an operator wants one line saying what the outage cost.
        note_matching_engine_reachable();

        // Record seq_no -> the session that placed this order, so its execution reports can
        // be routed back to it. seq_no is globally unique, unlike a ClOrdID.
        //
        // What is stored is the session's identity, not the connection it arrived on. The
        // connection is where the session happens to be *now*, and by the time a report is
        // ready it may be somewhere else entirely -- a different socket, or a different
        // gateway instance after a failover. Resolving that at send time is what makes a
        // report survive the reconnect; see docs/availability/gateway_ha.md.
        if (inbound.has_sender_comp_id && !inbound.sender_comp_id.empty()) {
            OriginSession origin;
            origin.identity = fix_common::SessionIdentity::make(inbound.sender_comp_id,
                                                                inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent);
            // Remembered, not read: the gateway stamped this when it read the order off the
            // client socket, and gets it back on the ER so it can measure the round trip.
            origin.has_ingress_ns = inbound.has_gateway_ingress_ns;
            origin.gateway_ingress_ns = inbound.gateway_ingress_ns;
            seq_no_to_session_[seq] = origin;
        } else if (inbound.has_gateway_session_conn_id) {
            // An order with a connection but no comp id cannot be routed home: the address
            // it arrived on is not an identity, and there is nothing else to file it under.
            // Warned rather than passed over, because it means a gateway stopped stamping
            // the comp id and every report for that order will be dropped later, far from
            // the cause.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: order seq={} arrived with a session connection but no sender_comp_id -- "
                       "its execution reports cannot be routed to any session",
                       seq);
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

        // Which gateway this ER belongs to is not known until the envelope has been
        // decoded and the routing map consulted, so the "is that gateway connected?"
        // check happens at the point of sending rather than here.

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
        // The session this report belongs to, and then -- separately -- where that session
        // can be reached. Keeping the two apart is the whole of step 5: an order is filed
        // under an identity that outlives connections, and the address is resolved at the
        // last possible moment, so a member that reconnected while the report was in flight
        // still receives it.
        fix_common::SessionIdentity routing_identity{};
        bool has_routing_conn = false;
        int32_t routing_conn_id = 0;
        int16_t routing_gateway_id = gateway_ids::default_when_absent;
        int16_t routing_gateway_instance = gateway_ids::first_instance;
        // The originating gateway's ingress stamp, returned to it on the ER so it can
        // measure the round trip. Only the routing-map branch can supply one: an ER that
        // is not tied to a sequenced order never had an originating read to measure from.
        bool has_routing_ingress_ns = false;
        int64_t routing_ingress_ns = 0;
        bool erase_routing_entry = false;
        {
            auto it = seq_no_to_session_.find(er_seq_no);
            if (it != seq_no_to_session_.end()) {
                routing_identity = it->second.identity;
                has_routing_ingress_ns = it->second.has_ingress_ns;
                routing_ingress_ns = it->second.gateway_ingress_ns;

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
            } else if (inbound.has_sender_comp_id && !inbound.sender_comp_id.empty()) {
                // No originating order sequence: the cancel-on-failover reports a promoted
                // matching engine emits. They carry the identity of the session whose order
                // was cancelled, which is exactly what is needed -- and it is why the ME now
                // stores the identity against each resting order rather than the connection
                // that placed it, which by definition no longer exists in this scenario.
                routing_identity = fix_common::SessionIdentity::make(inbound.sender_comp_id, inbound.has_origin_gateway_id ? inbound.origin_gateway_id
                                                                                                                           : gateway_ids::default_when_absent);
            } else {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                           "SequencerThread: ER seq_no={} not in routing map and no comp id on the envelope -- forwarding unaddressed", er_seq_no);
            }

            // Now the address, resolved from the identity rather than remembered with it.
            if (!routing_identity.empty()) {
                const fix_common::SessionDestination* destination = session_destination(routing_identity);
                if (destination != nullptr) {
                    has_routing_conn = true;
                    routing_conn_id = destination->conn_id;
                    routing_gateway_id = routing_identity.protocol;
                    routing_gateway_instance = destination->instance;
                } else {
                    // The session is not connected anywhere. Its reports are dropped, as
                    // they always were -- but now for a reason that names the session
                    // rather than a connection id that stopped meaning anything.
                    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                               "SequencerThread: ER seq_no={} for session comp_id='{}' protocol={} -- session not bound to any instance, dropping", er_seq_no,
                               routing_identity.comp_id_view(), routing_identity.protocol);
                }
            }
        }

        // Sequence the ER into the WAL and deliver it the same three ways an order is
        // (append, replicate to the peer follower, stream to external subscribers) so
        // the MEP -- an external WAL subscriber -- publishes it on the execution_reports
        // topic. Each ER gets its OWN seq_no (an order can emit several ERs -- New, Fill,
        // Canceled -- so they cannot share the order's seq). The stored/replicated/streamed
        // record is the WalRecord-wrapped ER; the routing conn id rides on the envelope
        // (the MEP unwraps and publishes only the inner DD-derived ER). Replay skips ER
        // records (dispatch_replay_records only re-sends NOS/OCR). NOTE: an ER driven by a
        // sequenced order is forwarded to the gateway gated on the *order's* WalAck, not on
        // this ER record's own -- so at a failover instant a just-forwarded ER may be
        // missing from the new leader's WAL (an execution_reports-topic gap at the seam).
        // Full two-tier commit of ordinary ERs is still a follow-up; ERs with no
        // originating order sequence already gate on their own record, see below.
        const int64_t er_wal_seq = next_sequence_number_++;
        const int64_t er_wall_time_ns = config_.wall_clock->now_ns();

        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = er_wal_seq;
        envelope.pdu_id = inbound.pdu_id;
        envelope.payload = inbound.payload;
        envelope.wall_time_ns = er_wall_time_ns;
        envelope.has_gateway_session_conn_id = has_routing_conn;
        envelope.gateway_session_conn_id = routing_conn_id;
        envelope.has_origin_gateway_id = has_routing_conn;
        envelope.origin_gateway_id = routing_gateway_id;
        envelope.has_gateway_instance_id = has_routing_conn;
        envelope.gateway_instance_id = routing_gateway_instance;
        envelope.has_gateway_ingress_ns = has_routing_ingress_ns;
        envelope.gateway_ingress_ns = routing_ingress_ns;
        // The identity travels with the report as well as the address, and outlasts it: the
        // address is only true while the session stays where it is, whereas this says whose
        // report it was. That is what a WAL reader, a topic subscriber, or a replay after a
        // reconnect has to key on -- the connection ids in an old record name sockets that
        // are long gone. The string_view points into routing_identity, which outlives every
        // use of this envelope below.
        envelope.has_sender_comp_id = !routing_identity.empty();
        envelope.sender_comp_id = routing_identity.comp_id_view();

        append_envelope_to_wal(envelope);
        send_wal_record(envelope);
        stream_wal_record_to_external_subscribers(envelope);

        // Which WalAck releases this ER to the gateway.
        //
        // Ordinarily it is the *order's* WAL entry: do not tell a client its order
        // executed until the follower has durably committed the order itself.
        //
        // An ER with no originating order sequence cannot use that rule. The
        // cancel-on-failover ERs a promoted matching engine emits carry seq_no 0,
        // because they are generated on promotion rather than driven by a sequenced
        // order. Gating those on seq_no 0 waited for a WalAck that can never arrive:
        // every one was parked in pending_er_ forever -- never delivered, never
        // dropped, and traced only by the Debug line below. Worse, pending_er_ is
        // keyed on the gate sequence, so all of them collided on key 0 and only the
        // first was even retained; the rest were discarded outright. The whole book
        // was cancelled and no client was ever told.
        //
        // They gate on the ER record's own WAL sequence instead. The follower acks
        // every WalRecord it receives (see handle_peer_wal_record), so er_wal_seq is
        // acked exactly as an order's seq_no is, and it is unique per ER so the keys
        // no longer collide. This is strictly the stronger guarantee -- the client
        // learns of the cancel only once the backup holds the cancel record itself --
        // and it is the "full two-tier commit of ERs" noted above, applied to the one
        // case that had no working gate at all.
        const bool gate_on_own_record = (er_seq_no == 0);
        const int64_t gate_seq_no = gate_on_own_record ? er_wal_seq : er_seq_no;

        if (!needs_wal_ack()) {
            send_er_to_origin_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope);
            note_report_forwarded(routing_identity);
            release_pdu_payload(message);
            if (erase_routing_entry) {
                seq_no_to_session_.erase(er_seq_no);
            }
        } else {
            auto acked_it = wal_acked_seq_nos_.find(gate_seq_no);
            if (acked_it != wal_acked_seq_nos_.end()) {
                // Follower already acked this seq_no; forward immediately.
                wal_acked_seq_nos_.erase(acked_it);
                send_er_to_origin_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope);
                note_report_forwarded(routing_identity);
                release_pdu_payload(message);
                if (erase_routing_entry) {
                    seq_no_to_session_.erase(er_seq_no);
                }
            } else {
                // WalAck not yet received; buffer the inner (unwrapped) ER until it arrives.
                PendingEr pending{};
                pending.pdu_id = inbound.pdu_id;
                pending.seq_no = er_seq_no;
                pending.payload.assign(inbound.payload.data, inbound.payload.data + inbound.payload.size);
                pending.identity = routing_identity;
                pending.has_gateway_ingress_ns = has_routing_ingress_ns;
                pending.gateway_ingress_ns = routing_ingress_ns;
                pending.erase_routing_entry = erase_routing_entry;
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq={} buffered -- awaiting WalAck seq={} from follower",
                           er_seq_no, gate_seq_no);
                pending_er_.emplace(gate_seq_no, std::move(pending));
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

void SequencerThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == wal_snapshot_timer_id_) {
        // Published on the snapshot timer rather than per append: both change at most once per
        // segment, which is seconds apart, so there is nothing to gain from the order path.
        wal_segments_filled_inline_gauge_.set(static_cast<double>(wal_.segments_filled_inline()));
        wal_segments_waited_for_gauge_.set(static_cast<double>(wal_.segments_waited_for()));

        try {
            wal_.take_snapshot();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: WAL snapshot taken: last_seq_no={}, record_count={}, peer acknowledged through {}", wal_.last_seq_no(),
                       wal_.record_count(), peer_acked_through_);

            // Nothing is reclaimed here, and that is deliberate.
            //
            // Taking a snapshot used to delete every segment before the current one, which left
            // the log holding about thirty seconds of history -- less the busier the venue was.
            // A component recovering from a checkpoint asks for everything after the position it
            // holds, and that deletion is what made the answer unavailable.
            //
            // Reclaiming safely needs the lowest position anything may still ask from, and the
            // venue cannot yet establish it: the follower's is known (above), but the matching
            // engine publishes no position, and a member's resend can reach back as far as its
            // session goes. Deleting on a timer instead of on those positions is what the
            // original defect was. So the log grows until they exist, which is a bounded and
            // visible problem where silent loss was neither. See docs/bug_list.md BUG-0048.
            if (wal_.record_count() > wal_growth_warning_records) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: the WAL holds {} records and nothing is reclaiming it -- retention cannot be anchored until "
                           "every component publishes the position it may ask from (docs/bug_list.md BUG-0048)",
                           wal_.record_count());
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: WAL snapshot failed: {}", ex.what());
        }
        return;
    }

    if (id == peer_heartbeat_timer_id_) {
        send_peer_heartbeat();
        return;
    }

    if (id == arbiter_heartbeat_timer_id_) {
        send_arbiter_heartbeat();
        return;
    }

    if (id == peer_heartbeat_timeout_timer_id_) {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return; // already leader, nothing to do
        }

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: peer heartbeat timeout (role={})", pubsub_itc_fw_app::to_string(role_));

        arbitration_attempts_ = 0;
        arbitration_outstanding_ = false;
        if (!request_arbitration()) {
            // No arbiter connected. The peer stopped sending heartbeats, so
            // there is no second claimant to tie-break against: take leadership
            // in a new generation.
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: no arbiter connected -- assuming leadership, peer is not responding (degraded)");
            set_epoch(epoch_ + 1);
            adopt_role(pubsub_itc_fw_app::Role::leader);
        }
        return;
    }

    if (id == arbitration_timeout_timer_id_) {
        if (role_ == pubsub_itc_fw_app::Role::leader) {
            return;
        }
        // An arbiter that started moments ago declines to arbitrate until it has
        // had a chance to learn who leads what, and says so, asking the component
        // to come back. Do that before concluding no arbiter is coming.
        if (arbitration_attempts_ < max_arbitration_attempts && request_arbitration()) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: no arbitration decision yet -- retrying (attempt {} of {})",
                       arbitration_attempts_, max_arbitration_attempts);
            return;
        }
        // Nothing arbitrated and the peer is not answering either, so there is no
        // second claimant to weigh against. Take leadership in a new generation.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: no arbitration decision and peer is not responding -- assuming leadership (degraded)");
        arbitration_outstanding_ = false;
        set_epoch(epoch_ + 1);
        adopt_role(pubsub_itc_fw_app::Role::leader);
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
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), transition_level, "SequencerThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        cancel_timer(peer_heartbeat_timer_id_);
        peer_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.heartbeat_interval_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: now LEADER -- heartbeat timer started ({}s interval)",
                   config_.heartbeat_interval_seconds);
        // Immediately, not at the next heartbeat: the gap between taking
        // leadership and the arbiter hearing about it is exactly the window in
        // which the arbiter could issue a competing generation.
        send_leadership_lease();
        // And tell the gateways where this venue now stands on accepting orders. They may be
        // holding what the previous leader last said, which was true of a process that is no
        // longer running. This instance has deferred nothing, so it accepts -- but that has to
        // be said rather than assumed, because silence here leaves a refusal in place that
        // nothing will ever lift.
        broadcast_order_acceptance();
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        cancel_timer(peer_heartbeat_timer_id_);
        peer_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.heartbeat_interval_seconds));
        // Arm (or re-arm) the heartbeat timeout.
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: now FOLLOWER -- heartbeat timer started, timeout armed ({}s)",
                   config_.heartbeat_timeout_seconds);
        // A follower forwards nothing to a matching engine, so it defers nothing. Clearing the
        // bookkeeping matters for what happens if this instance leads AGAIN: a deferral begun in
        // a previous leadership would otherwise still be open, because the recovery that would
        // have closed it happened while this instance was not the one watching for it. It would
        // then be re-promoted already refusing orders, with an age measured from an outage that
        // ended long ago, and nothing would lift it unless a matching engine happened to
        // reconnect afterwards.
        deferring_orders_ = false;
        deferred_order_count_ = 0;
        accepting_orders_ = true;
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

    // If the peer already holds leadership, adopt follower. Deferring is always
    // safe: it can only avoid creating a second leader, and it needs no new
    // generation because the peer's is adopted rather than one invented.
    //
    // Only a peer that says it is leading gets deferred to. A peer that holds no
    // role does not become entitled to lead by having the higher epoch: the
    // epoch counts generations seen, and a node that has seen more of them is
    // not thereby in charge. Reading it as authority makes both nodes defer --
    // the one with the lower epoch by this branch, the other by the instance-id
    // rule below -- and the pair comes up with two followers and no leader.
    if (peer_current_role == pubsub_itc_fw_app::Role::leader) {
        if (peer_epoch > epoch_) {
            // The peer led in a generation this node has not seen, so this node
            // is the stale one. Take its generation along with its leadership.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: peer (instance_id={}) leads at epoch {} > my epoch {} -- adopting follower in its generation", peer_instance_id,
                       peer_epoch, epoch_);
            set_epoch(peer_epoch);
            adopt_role(pubsub_itc_fw_app::Role::follower);
            return;
        }
        if (peer_epoch < epoch_) {
            // A leader from an older generation than the one this node has
            // already seen. It was leader once and has not learned that it
            // stopped being one. Following it would put the venue back in a
            // generation it has left, so let the arbiter say who leads now.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: peer (instance_id={}) claims leader at epoch {} but this node has seen epoch {} -- not following a stale leader",
                       peer_instance_id, peer_epoch, epoch_);
            resolve_with_visible_peer(peer_instance_id, peer_epoch);
            return;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: peer (instance_id={}) is already leader -- adopting follower",
                   peer_instance_id);
        adopt_role(pubsub_itc_fw_app::Role::follower);
        return;
    }

    // Neither side holds a role. Settle it here rather than at the arbiter.
    //
    // This looks like the arbiter's job and is not, because of who knows what.
    // Both nodes can see each other, so between them they hold every fact the
    // decision needs: both instance ids, both epochs, and the knowledge that
    // neither is leading. The arbiter holds none of that first-hand, and a
    // recently started one says so and refuses. Referring the question from the
    // side that has the facts to the side that has not is the wrong direction.
    //
    // Two leaders cannot come of it. Both sides run this with the same pair of
    // ids and the same pair of epochs and so reach the same answer without
    // needing to agree on anything further.
    //
    // Where the arbiter does earn its place is the opposite case, when the peer
    // cannot be seen at all: that is the one this node cannot settle alone, and
    // on_timer_event refers it.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: neither this node nor peer (instance_id={}) holds a role -- resolving between peers", peer_instance_id);
    resolve_with_visible_peer(peer_instance_id, peer_epoch);
}

void SequencerThread::set_epoch(int32_t new_epoch) {
    if (new_epoch < epoch_) {
        // Nothing should ask for this. Refusing keeps the counter monotonic even
        // if some future caller gets it wrong, because an epoch that moves
        // backwards silently disarms every downstream check at once.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: refusing to move epoch backwards ({} -> {}) -- keeping {}", epoch_,
                   new_epoch, epoch_);
        return;
    }
    if (new_epoch == epoch_) {
        return;
    }

    const int32_t previous = epoch_;
    epoch_ = new_epoch;

    // Written before the new epoch is acted on, so a crash in the gap cannot
    // bring the node back believing it still owns a generation it has spent.
    if (!epoch_store_.store(epoch_)) {
        // Carry on in the new generation regardless. Stopping would take out a
        // sequencer that is otherwise healthy, and the in-memory epoch is still
        // correct for as long as this process lives. What is lost is the
        // guarantee across a restart, and that is worth an alert.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: epoch advanced {} -> {} but could not be written to {} -- a restart from here may reuse a spent generation", previous,
                   epoch_, epoch_store_.path());
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: epoch advanced {} -> {} (recorded)", previous, epoch_);
}

bool SequencerThread::request_arbitration() {
    if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
        return false;
    }
    arbitration_outstanding_ = true;
    ++arbitration_attempts_;
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: requesting arbitration from arbiter pool");
    send_arbitration_report();
    arbitration_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.arbitration_timeout_seconds));
    return true;
}

void SequencerThread::resolve_with_visible_peer(int64_t peer_instance_id, int32_t peer_epoch) {
    // Both sides run this with the same two instance ids and the same two
    // epochs, so both reach the same answer independently and no exchange of
    // agreement is needed.
    //
    // The new generation is one past the higher of the two epochs, which puts it
    // ahead of anything either node has led in before. Taking the higher of the
    // two matters when one node has lost its stored epoch: max is symmetric, so
    // the node that still remembers carries the other one forward, and both
    // still compute the same number.
    set_epoch(std::max(epoch_, peer_epoch) + 1);

    if (static_cast<int64_t>(config_.instance_id) < peer_instance_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: my instance_id={} < peer instance_id={} -- adopting leader (epoch={})",
                   config_.instance_id, peer_instance_id, epoch_);
        adopt_role(pubsub_itc_fw_app::Role::leader);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: my instance_id={} >= peer instance_id={} -- adopting follower (epoch={})",
                   config_.instance_id, peer_instance_id, epoch_);
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

    send_leadership_lease();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: arbiter heartbeat sent (instance_id={} epoch={})", hb.instance_id, hb.epoch);
}

void SequencerThread::send_leadership_lease() {
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        // Leadership is a separate assertion from being alive, and only whoever
        // holds it may make it. Keeping the two apart is what lets a follower
        // send a heartbeat without appearing to claim anything.
        return;
    }

    // Telling the arbiter who leads is what keeps a leadership settled between
    // the two peers from being overturned later by an arbiter that never saw it
    // happen. Holding a confirmed incumbent, the arbiter answers a subsequent
    // report by confirming that incumbent rather than issuing a fresh epoch, so
    // the two never issue generations for the same group at once.
    pubsub_itc_fw_app::LeadershipLease lease{};
    lease.instance_id = static_cast<int64_t>(config_.instance_id);
    lease.group = pubsub_itc_fw_app::ComponentGroup::sequencer;
    lease.epoch = epoch_;
    for (const pubsub_itc_fw::ConnectionID& conn : {arbiter_primary_conn_id_, arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            send_pdu(conn, pubsub_itc_fw_app::LeadershipLease::message_pdu_id, 0, lease);
        }
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: LeadershipLease sent (instance_id={} epoch={})", lease.instance_id,
               lease.epoch);
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
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
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

    cancel_timer(arbitration_timeout_timer_id_);
    arbitration_attempts_ = 0;
    arbitration_outstanding_ = false;

    set_epoch(decision.epoch);

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

    // Deliberately no election here. A StatusQuery carries an instance id and an
    // epoch but not the sender's role, so a query from a node that is already
    // leading looks exactly like one from a node that holds nothing. Electing on
    // it means guessing Role::unknown for the peer, and a node restarting beside
    // a healthy leader then makes itself a second leader before the reply that
    // would have said so arrives.
    //
    // This node sends its own StatusQuery on every peer connection, so it always
    // has a StatusResponse coming, and that one does carry the peer's role. The
    // election runs there, on both sides, from complete information. If no reply
    // ever comes, the startup election timer covers it.
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

    // Follow the leader's generation. Without this a follower keeps whatever
    // epoch it had when it was elected while the leader moves on, the two drift
    // apart, and the gap does damage twice over: promoting the follower produces
    // a generation the venue has already used, and a node comparing epochs on
    // restart is comparing against a number that was never current.
    if (hb.epoch > epoch_ && role_ == pubsub_itc_fw_app::Role::follower) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: leader has moved to epoch {} (this node was at {}) -- following it",
                   hb.epoch, epoch_);
        set_epoch(hb.epoch);
    }

    // Reset the heartbeat timeout whenever we receive a valid heartbeat.
    if (role_ == pubsub_itc_fw_app::Role::follower) {
        cancel_timer(peer_heartbeat_timeout_timer_id_);
        peer_heartbeat_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
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
        envelope.has_origin_gateway_id = view.has_origin_gateway_id;
        envelope.origin_gateway_id = view.origin_gateway_id;
        envelope.has_gateway_instance_id = view.has_gateway_instance_id;
        envelope.gateway_instance_id = view.gateway_instance_id;

        // gateway_ingress_ns is deliberately NOT carried across replay, even though the
        // stored record has one. It records when a client's order was read off a socket,
        // which for a replayed order was minutes or hours ago and has nothing to do with
        // how long this ER took. Forwarding it would put every replayed order in the
        // histogram's overflow bucket and drag the venue's tail latency with it -- an
        // invented stall, reported at exactly the moment a failover makes people look.
        // Leaving it absent costs a handful of observations after promotion and keeps
        // every observation that is recorded a real measurement.

        // Rebuild the routing map as the WAL is replayed, so ERs the matching engine emits
        // for these orders after promotion reach the sessions that placed them.
        //
        // The identity is what is rebuilt, and it is the only part of the old routing entry
        // that a replay could honestly restore. The connection ids in these records name
        // sockets on a process that has since died -- that is why there is a promotion to
        // replay after -- so a rebuilt address would be wrong by construction. The live
        // addresses come from the gateways instead, as SessionBound PDUs, and a session
        // that reconnects after the promotion re-announces itself.
        if (view.has_sender_comp_id && !view.sender_comp_id.empty()) {
            OriginSession origin;
            origin.identity =
                fix_common::SessionIdentity::make(view.sender_comp_id, view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent);
            // has_ingress_ns stays false for the reason above.
            seq_no_to_session_[view.seq_no] = origin;
        }

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
    append_to_wal(envelope.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, wal_encode_buffer_.data(), static_cast<int>(bytes_written),
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
    append_to_wal(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, message.payload(), message.payload_size(), view.wall_time_ns);
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

    // Everything at or below this is on both machines. Acks arrive in the order the records
    // were streamed, so the highest is also the highest contiguous; std::max is belt and
    // braces against a reordering that would otherwise let the floor run ahead of the facts.
    peer_acked_through_ = std::max(peer_acked_through_, view.seq_no);

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
            append_to_wal(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, payload, static_cast<int>(size), view.wall_time_ns);

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

void SequencerThread::send_er_to_origin_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) {
    const pubsub_itc_fw::ConnectionID* connection = gateway_connection(protocol, instance);
    if (connection == nullptr) {
        // Distinguish "configured but not currently connected", which is transient and
        // resolves when the gateway reconnects, from "never configured", which never
        // resolves and means a gateway is submitting orders whose reports can go nowhere.
        // The second is a deployment error worth naming loudly and exactly once per pair,
        // rather than once per report -- a busy gateway would otherwise flood the log with
        // the same misconfiguration.
        const bool configured = std::any_of(config_.gateway_endpoints.begin(), config_.gateway_endpoints.end(), [protocol, instance](const auto& endpoint) {
            return endpoint.protocol == protocol && endpoint.instance == instance;
        });
        if (!configured && unknown_gateways_warned_.insert(gateway_key(protocol, instance)).second) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "SequencerThread: orders received from gateway protocol={} instance={}, which is not in this sequencer's [[gateway]] "
                       "configuration -- its execution reports cannot be delivered anywhere. Check the gateway's instance_id against the "
                       "sequencer's endpoints.",
                       protocol, instance);
        }
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway protocol={} instance={} not connected -- dropping ER seq={}",
                   protocol, instance, er_seq_no);
        return;
    }
    send_pdu(*connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, envelope);
}

const fix_common::SessionDestination* SequencerThread::session_destination(const fix_common::SessionIdentity& identity) const {
    const auto it = session_destinations_.find(identity);
    return it == session_destinations_.end() ? nullptr : &it->second;
}

void SequencerThread::note_order_deferred(int64_t seq_no) {
    const auto now = std::chrono::steady_clock::now();
    ++deferred_order_count_;

    if (!deferring_orders_) {
        deferring_orders_ = true;
        deferral_began_ = now;
        last_deferral_warning_ = now;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: no matching engine reachable -- orders are being accepted and deferred, starting at seq={}. They are WAL-committed and "
                   "recovered on promotion, but every member that placed one believes it is live and cannot cancel it",
                   seq_no);
        refresh_order_acceptance();
        return;
    }

    if (now - last_deferral_warning_ < order_deferral_warning_interval) {
        // Acceptance is still evaluated on every deferred order, because a threshold crossed
        // between two warnings must take effect when it is crossed rather than at the next
        // warning. Only the log is rate-limited; the decision is not.
        refresh_order_acceptance();
        return;
    }
    last_deferral_warning_ = now;
    const auto degraded_for = std::chrono::duration_cast<std::chrono::seconds>(now - deferral_began_);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "SequencerThread: still no matching engine after {}s -- {} order(s) deferred so far, latest seq={}. Members are being told these orders are "
               "live",
               degraded_for.count(), deferred_order_count_, seq_no);

    // Repeat the state on the same interval while the venue is not accepting, so a gateway
    // that missed the transition -- or reconnected without one happening since -- converges on
    // the truth rather than sitting on a stale "fine".
    if (!refresh_order_acceptance() && !accepting_orders_) {
        broadcast_order_acceptance();
    }
}

void SequencerThread::note_matching_engine_reachable() {
    if (!deferring_orders_) {
        return;
    }
    const auto degraded_for = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - deferral_began_);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: a matching engine is reachable again after {}s -- {} order(s) were deferred and are recovered by its WAL replay",
               degraded_for.count(), deferred_order_count_);
    deferring_orders_ = false;
    deferred_order_count_ = 0;

    // Resuming is automatic, and deliberately so. Requiring an operator to re-enable
    // acceptance was considered and rejected: BUG-0009 is precisely a case where nobody was
    // watching, and a design whose recovery depends on the watching that has already failed
    // is not safer. See docs/availability/order_acceptance.md.
    refresh_order_acceptance();
}

bool SequencerThread::refresh_order_acceptance() {
    // Evaluated where it is needed rather than on a timer. The two things that consume this
    // state are an order arriving, which is exactly when refusing matters, and a gateway
    // connecting, which must not be left assuming the venue is fine. A venue with no traffic
    // has nothing to refuse, so there is nothing for a timer to discover.
    bool accepting = true;
    std::chrono::seconds degraded_for{0};
    if (deferring_orders_) {
        degraded_for = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - deferral_began_);
        accepting = degraded_for < order_deferral_refusal_age && deferred_order_count_ < order_deferral_refusal_count;
    }
    if (accepting == accepting_orders_) {
        return false;
    }
    accepting_orders_ = accepting;

    if (accepting) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: accepting orders again -- gateways told the venue can process what they are given");
    } else {
        // Name which threshold spoke. An operator reading this wants to know whether the venue
        // stopped because an outage ran long or because a burst filled it, and those call for
        // different responses.
        const char* const because = degraded_for >= order_deferral_refusal_age ? "the outage has run longer than a failover plausibly takes"
                                                                               : "more orders are deferred than a member should be left wrong about";
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: no longer accepting orders -- {} ({} order(s) deferred over {}s, thresholds {}s / {}). Deferred orders are still "
                   "WAL-committed and recovered on promotion; what stops is taking on new ones the venue cannot act upon",
                   because, deferred_order_count_, degraded_for.count(), order_deferral_refusal_age.count(), order_deferral_refusal_count);
    }
    broadcast_order_acceptance();
    return true;
}

void SequencerThread::send_order_acceptance(const pubsub_itc_fw::ConnectionID& conn_id) {
    // Only the leader has an opinion worth sending. A follower forwards nothing to a matching
    // engine, so it never defers and its state is permanently "accepting" -- and the gateway
    // holds a connection to BOTH sequencers and cannot tell which of them leads.
    //
    // Measured before this guard existed: a gateway restarted during an outage was correctly
    // told "not accepting" by the leader, and two seconds later told "accepting again" by the
    // follower, on a venue with no matching engine running at all. The follower was not lying
    // about itself; it was answering a question that was never about it.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        return;
    }

    pubsub_itc_fw_app::OrderAcceptance state{};
    state.accepting = accepting_orders_;
    state.deferred_order_count = deferring_orders_ ? deferred_order_count_ : 0;
    state.degraded_for_seconds =
        deferring_orders_ ? static_cast<int32_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - deferral_began_).count())
                          : 0;
    send_pdu(conn_id, pubsub_itc_fw_app::OrderAcceptance::message_pdu_id, 0, state);
}

void SequencerThread::broadcast_order_acceptance() {
    for (const auto& [gateway, conn_id] : gateway_conn_ids_) {
        if (conn_id.is_valid()) {
            send_order_acceptance(conn_id);
        }
    }
}

void SequencerThread::handle_session_bound(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::SessionBoundView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode SessionBound -- dropping");
        return;
    }
    if (view.comp_id.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: SessionBound with an empty comp id from protocol={} instance={} -- ignoring, there is no identity to bind",
                   view.gateway_protocol_id, view.gateway_instance_id);
        return;
    }

    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(view.comp_id, view.gateway_protocol_id);
    fix_common::SessionDestination destination{};
    destination.instance = view.gateway_instance_id;
    destination.conn_id = view.gateway_session_conn_id;

    // A binding still present means the previous session never unbound -- its gateway died
    // without saying so. That is the only signal the sequencer gets that the position it
    // remembers for this session is stale rather than exact, and it is what decides whether
    // the resume figure below is used as-is or deliberately raised.
    const bool previous_session_died = session_destinations_.count(identity) != 0;

    const auto existing = session_destinations_.find(identity);
    if (existing != session_destinations_.end()) {
        // The same identity was already bound somewhere. On a reconnect this is the
        // expected case and the whole point -- the address is replaced and the session's
        // reports follow it. But it is also what a comp id logged on twice at once looks
        // like, and that is a venue rule violation ("one comp id may hold a session only
        // once venue-wide") which nothing yet enforces. The sequencer is the only component
        // that can see it happen, so it says so; refusing the second logon is the piece of
        // that decision still to be built, and belongs with the decision, not smuggled in
        // here as a side effect of re-keying.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: session comp_id='{}' protocol={} re-bound from instance={} connection={} to instance={} connection={}",
                   identity.comp_id_view(), identity.protocol, existing->second.instance, existing->second.conn_id, destination.instance, destination.conn_id);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: session comp_id='{}' protocol={} bound to instance={} connection={}",
                   identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id);
    }

    session_destinations_[identity] = destination;

    // Unless the member asked to start again, in which case there is nothing to hand back and
    // keeping it would be worse than useless.
    //
    // ResetSeqNumFlag=Y on a Logon discards the numbering on both sides, so everything remembered
    // here describes a series the member has abandoned: both sequence numbers, and the record of
    // which outbound numbers held execution reports. The numbers alone would be caught by the
    // gateway, which discards what it is handed on that path -- but they would then be stuck,
    // because the updates that follow report the new low numbers and the guards here refuse to
    // lower. And the report ranges would not be caught at all: new ranges from the restarted
    // numbering would be merged into the old ones, and the next gateway to bind the session
    // would be told that numbers 2 to 1001 held reports when in the new numbering they hold a
    // Logon and heartbeats. That is BUG-0051 arriving by a different road.
    //
    // A member may reset at any logon and clients make it easy, so this is an ordinary path.
    if (view.reset_seq_nums) {
        const size_t forgotten = session_sequence_state_.erase(identity);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: session comp_id='{}' protocol={} asked to restart its numbering -- {} remembered sequence state discarded",
                   identity.comp_id_view(), identity.protocol, forgotten > 0 ? "its" : "no");
    }

    // Hand back what the venue remembers of this session's sequence numbers, so the gateway
    // that has just taken it on continues where the last one left off instead of starting
    // at 1. A member whose numbers restarted on every reconnect would see a break it cannot
    // reconcile -- the very thing a failover is supposed to spare it.
    const auto state_it = session_sequence_state_.find(identity);
    const bool known = state_it != session_sequence_state_.end();
    const SessionSequenceState state = known ? state_it->second : SessionSequenceState{};

    // Only the leader answers. The follower tracks bindings too -- it will need them if it
    // is promoted -- but a session has one venue-side view of its numbering, and two replies
    // would have the gateway apply it twice.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        return;
    }

    // Where to resume the member's numbering.
    //
    // After a clean unbind the reported figure is exact and is used as it stands. After an
    // unclean death it is stale by whatever the gateway sent between its last report and its
    // last breath, so the known part of that -- the reports this sequencer forwarded -- is
    // added, plus an allowance for the admin traffic it cannot see.
    //
    // The result is deliberately too HIGH rather than risk being too low. Too high leaves a
    // gap the member closes with a ResendRequest, which the replay then answers. Too low
    // sends a sequence number below what the member expects, which FIX requires it to treat
    // as fatal -- it drops the session, and no amount of replay can help.
    int32_t resume_seq_num = state.outbound_seq_num;
    if (known && previous_session_died) {
        resume_seq_num += state.ers_since_report + unclean_resume_admin_allowance;
    }

    // The inbound number gets NO such allowance, and the asymmetry is deliberate rather than an
    // omission. Above, erring high is safe because it leaves the member a gap it can close with a
    // ResendRequest. Here, erring high means the venue expects a number the member has not reached
    // and will treat its next message as having gone backwards -- fatal, to a member that has done
    // nothing wrong. Erring low leaves the VENUE with the gap, which is the side that can ask.
    //
    // The stored figure is already a lower bound: a member can only have sent more since it was
    // reported, never less. So it is handed back untouched.
    const int32_t resume_inbound_seq_num = state.inbound_seq_num;

    // And which of the session's numbers held a report, which is the half a gateway cannot
    // work out for itself. The instance that sent them may be gone; this is the only surviving
    // record of what its numbering carried, and without it a resend has to guess. See
    // docs/availability/resend_provenance.md.
    std::vector<pubsub_itc_fw_app::SeqNumRange> wire_ranges;
    wire_ranges.reserve(state.report_seq_nums.size());
    for (const fix_common::SeqNumRange& range : state.report_seq_nums) {
        wire_ranges.push_back(pubsub_itc_fw_app::SeqNumRange{range.from_seq_num, range.to_seq_num});
    }

    pubsub_itc_fw_app::SessionBoundAck ack{};
    ack.comp_id = identity.comp_id_view();
    ack.gateway_protocol_id = identity.protocol;
    ack.known = known;
    ack.outbound_seq_num = resume_seq_num;
    ack.inbound_seq_num = resume_inbound_seq_num;
    ack.report_seq_nums = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::SeqNumRange>{wire_ranges.data(), wire_ranges.size()};
    send_pdu(conn_id, pubsub_itc_fw_app::SessionBoundAck::message_pdu_id, 0, ack);

    // The resumed session starts from the figure just handed out, so the running count that
    // fed into it has been spent.
    if (known) {
        SessionSequenceState& stored = session_sequence_state_[identity];
        stored.outbound_seq_num = resume_seq_num;
        stored.ers_since_report = 0;
    }

    if (known && previous_session_died) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: session comp_id='{}' protocol={} previous gateway died without reporting -- "
                   "resuming at outbound={} (reported {} + {} report(s) forwarded since + {} allowance), "
                   "deliberately ahead so the member sees a gap rather than a fatal low sequence; "
                   "inbound={} as reported, deliberately NOT ahead so the member is not treated as having gone backwards",
                   identity.comp_id_view(), identity.protocol, resume_seq_num, state.outbound_seq_num, state.ers_since_report, unclean_resume_admin_allowance,
                   resume_inbound_seq_num);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: session comp_id='{}' protocol={} sequence state {} -- outbound={} inbound={}", identity.comp_id_view(), identity.protocol,
                   known ? "restored" : "is new", resume_seq_num, resume_inbound_seq_num);
    }
}

namespace {

/// The wire's ranges as the ones the merge and query helpers work on.
std::vector<fix_common::SeqNumRange> to_seq_num_ranges(const pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::SeqNumRangeView>& wire) {
    std::vector<fix_common::SeqNumRange> ranges;
    ranges.reserve(wire.size);
    for (size_t index = 0; index < wire.size; ++index) {
        ranges.push_back(fix_common::SeqNumRange{wire.data[index].from_seq_num, wire.data[index].to_seq_num});
    }
    return ranges;
}

} // namespaces

void SequencerThread::handle_session_unbound(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::SessionUnboundView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode SessionUnbound -- dropping");
        return;
    }
    if (view.comp_id.empty()) {
        return;
    }

    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(view.comp_id, view.gateway_protocol_id);
    const auto existing = session_destinations_.find(identity);
    if (existing == session_destinations_.end()) {
        return;
    }

    // Only the connection that is actually bound may unbind the session. A member that
    // reconnects quickly enough for its new SessionBound to arrive before the old
    // connection's SessionUnbound -- two gateways racing, which is precisely what a
    // failover produces -- would otherwise be unbound a moment after binding, and would
    // sit there receiving nothing while appearing perfectly connected.
    if (existing->second.instance != view.gateway_instance_id || existing->second.conn_id != view.gateway_session_conn_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: stale SessionUnbound for comp_id='{}' protocol={} naming instance={} connection={} -- "
                   "it is bound to instance={} connection={}, keeping the newer binding",
                   identity.comp_id_view(), identity.protocol, view.gateway_instance_id, view.gateway_session_conn_id, existing->second.instance,
                   existing->second.conn_id);
        return;
    }

    session_destinations_.erase(existing);

    // Keep the sequence numbers the departing gateway reports. The destination is gone, but
    // the session is not: this is what the next gateway to take it on will be handed, and
    // the only reason a reconnect can continue the member's numbering rather than reset it.
    SessionSequenceState& state = session_sequence_state_[identity];
    state.outbound_seq_num = view.outbound_seq_num;
    if (view.inbound_seq_num > state.inbound_seq_num) {
        state.inbound_seq_num = view.inbound_seq_num;
    }

    // And which of its numbers held a report, for the part the departing gateway had not
    // already reported. Without this a resend served by the next gateway would have to guess
    // which numbers it may replay into, and guessing is BUG-0051.
    fix_common::seq_num_ranges::merge(state.report_seq_nums, to_seq_num_ranges(view.report_seq_nums));
    fix_common::seq_num_ranges::trim(state.report_seq_nums, fix_common::seq_num_ranges::max_remembered);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: session comp_id='{}' protocol={} unbound from instance={} connection={} -- "
               "remembered outbound={} and {} report range(s); its reports have nowhere to go until it binds again",
               identity.comp_id_view(), identity.protocol, view.gateway_instance_id, view.gateway_session_conn_id, state.outbound_seq_num,
               state.report_seq_nums.size());
}

void SequencerThread::note_report_forwarded(const fix_common::SessionIdentity& identity) {
    if (identity.empty()) {
        return;
    }
    // Deliberately counts what was SENT, not what was acknowledged. A report handed to a
    // gateway that then dies still consumed a sequence number there, and counting it makes
    // the resume position too high rather than too low -- the safe direction.
    ++session_sequence_state_[identity].ers_since_report;
}

void SequencerThread::handle_session_sequence_update(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::SessionSequenceUpdateView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode SessionSequenceUpdate -- dropping");
        return;
    }
    if (view.comp_id.empty()) {
        return;
    }

    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(view.comp_id, view.gateway_protocol_id);
    SessionSequenceState& state = session_sequence_state_[identity];

    // Never lowered. Reports can arrive out of order across a reconnect -- a late one from the
    // instance that has just lost the session would otherwise wind the position backwards, and
    // backwards is the direction that kills the member's session.
    if (view.outbound_seq_num > state.outbound_seq_num) {
        state.outbound_seq_num = view.outbound_seq_num;
        // The reported figure now accounts for everything sent so far, so the running count of
        // reports forwarded since the last one starts again.
        state.ers_since_report = 0;
    }

    // Tracked independently of the number above, because the two move for different reasons: a
    // member can send while the venue is quiet, and the venue can send while the member is quiet.
    // Never lowered, for the same reason as the outbound one -- backwards is the direction that
    // breaks a session.
    if (view.inbound_seq_num > state.inbound_seq_num) {
        state.inbound_seq_num = view.inbound_seq_num;
    }

    // Merged unconditionally, and not gated on the number above moving. The ranges describe
    // numbers already sent, so a late update from an instance that has lost the session still
    // carries facts about numbers that really did hold reports; merging is idempotent and
    // cannot wind anything backwards, which is why this is safe where the number is not.
    fix_common::seq_num_ranges::merge(state.report_seq_nums, to_seq_num_ranges(view.report_seq_nums));
    fix_common::seq_num_ranges::trim(state.report_seq_nums, fix_common::seq_num_ranges::max_remembered);
}

void SequencerThread::handle_session_replay_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::SessionReplayRequestView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode SessionReplayRequest -- dropping");
        return;
    }

    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(view.comp_id, view.gateway_protocol_id);
    const int64_t from_seq_no = view.from_seq_no;
    const int32_t max_records = view.max_records > 0 ? view.max_records : default_replay_max_records;

    // How many of the session's newest reports to pass over before collecting.
    //
    // Zero means the member is asking about the tail of its stream, which is the usual case
    // after a disconnect and the only one this used to serve. A member asking about the middle
    // of its history names how far back the range sits, because otherwise it is sent the most
    // recent reports wearing the numbers it asked for -- BUG-0053, which is invisible to it,
    // every other property of the reply being correct.
    const int32_t skip_most_recent = view.skip_most_recent > 0 ? view.skip_most_recent : 0;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: SessionReplayRequest id={} comp_id='{}' protocol={} from_seq_no={} max_records={} skip_most_recent={}", view.request_id,
               identity.comp_id_view(), identity.protocol, from_seq_no, max_records, skip_most_recent);

    // Walk the WAL and hand back this session's execution reports.
    //
    // Nothing is stored twice to make this possible: every report is already in the WAL with
    // the session that originated it on its envelope. What the venue lacked was a way to ask
    // for one session's slice of that stream, and this is it.
    //
    // The cost is a scan from the oldest retained segment on every request, because the WAL
    // is an append-only log with no index -- deliberately, since indexing it would put work
    // on the write path that every order pays for so that a rare reconnect can be quicker.
    // A logon is rare and a resend rarer; if that ever stops being true the answer is a
    // cursor or a per-session index, not a slower hot path. Measured, not assumed: see
    // docs/availability/gateway_ha.md.
    // The MOST RECENT max_records reports, not the first found.
    //
    // What a member has missed is by definition the tail of its stream, so streaming as the
    // scan goes -- oldest first -- fills the gap with the session's ancient history and stops
    // before reaching anything it actually missed. It also makes the reply unbounded: the WAL
    // holds the session's whole retained history, and a member asking for a handful of
    // messages was being sent thousands, which in testing was enough for the client to give
    // up and close the connection mid-answer.
    //
    // So matches are collected into a window of the last max_records and sent afterwards.
    // The window is what bounds the memory: max_records payloads, not the whole slice.
    //
    // With skip_most_recent the window holds that many more, and the newest skip_most_recent
    // of them are dropped at the end -- so what is returned is the max_records reports sitting
    // immediately behind the ones skipped. "Most recent" becomes "most recent below a point the
    // caller names", and the tail case is the same code with the point at zero.
    const size_t window_capacity = static_cast<size_t>(max_records) + static_cast<size_t>(skip_most_recent);

    struct ReplayMatch {
        int64_t record_id{};
        int64_t wall_time_ns{};
        std::vector<uint8_t> payload;
    };
    std::deque<ReplayMatch> window;
    int64_t total_matched = 0;
    int32_t record_count = 0;
    int64_t last_seq_no = from_seq_no;
    bool truncated = false;
    const int64_t started_ns = config_.wall_clock->now_ns();

    [[maybe_unused]] auto end_pos = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, {0, 0}, [&identity, from_seq_no, window_capacity, &window, &total_matched](int64_t record_id, const void* payload, size_t size) {
            if (record_id <= from_seq_no) {
                return;
            }
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return;
            }
            int64_t wall_time_ns{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
            const auto* record_payload = static_cast<const uint8_t*>(payload) + header_size;
            const size_t record_size = size - header_size;

            // Every stored record is a WalRecord envelope; decode it to read who it belonged
            // to and what it was. A separate arena from the caller's: this runs inside the
            // decode of the request itself, whose view is still in use above.
            std::array<uint8_t, 64 * 1024> replay_arena_buffer{};
            pubsub_itc_fw::BumpAllocator replay_arena(replay_arena_buffer.data(), replay_arena_buffer.size());
            size_t replay_consumed = 0;
            size_t replay_needed = 0;
            pubsub_itc_fw_app::WalRecordView stored{};
            if (!pubsub_itc_fw_app::decode(stored, record_payload, record_size, replay_consumed, replay_arena, replay_needed)) {
                return;
            }
            if (stored.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
                return; // orders are not replayed to a member; it already knows what it sent
            }
            if (!stored.has_sender_comp_id ||
                fix_common::SessionIdentity::make(stored.sender_comp_id,
                                                  stored.has_origin_gateway_id ? stored.origin_gateway_id : gateway_ids::default_when_absent) != identity) {
                return;
            }

            ++total_matched;
            ReplayMatch match;
            match.record_id = record_id;
            match.wall_time_ns = wall_time_ns;
            match.payload.assign(stored.payload.data, stored.payload.data + stored.payload.size);
            window.push_back(std::move(match));
            if (window.size() > window_capacity) {
                window.pop_front();
            }
        });

    // Drop the newest skip_most_recent, leaving the ones the caller actually named. The window
    // held them only so that the ones behind them could be found: a scan that discarded them as
    // it went could not know, at the time it saw them, how many more were still coming.
    for (int32_t skipped = 0; skipped < skip_most_recent && !window.empty(); ++skipped) {
        window.pop_back();
    }

    for (const ReplayMatch& match : window) {
        pubsub_itc_fw_app::SessionReplayRecord replay_record{};
        replay_record.request_id = view.request_id;
        replay_record.seq_no = match.record_id;
        replay_record.wall_time_ns = match.wall_time_ns;
        replay_record.payload = pubsub_itc_fw_app::BytesView{match.payload.data(), match.payload.size()};
        send_pdu(conn_id, pubsub_itc_fw_app::SessionReplayRecord::message_pdu_id, match.record_id, replay_record);
        ++record_count;
        last_seq_no = match.record_id;
    }

    // Truncated means the caller was given less than it asked for, so what it wanted reaches
    // further back than the WAL still holds and part of its range cannot be served.
    //
    // It used to mean "the session has history older than this reply", which is true of every
    // resend a session with any history makes -- the reply holds the range asked for and the WAL
    // holds everything the session has ever done. So a warning saying the member had been short
    // changed fired on every healthy resend, and said it right after reporting the resend
    // complete with no gap left. That was BUG-0052.
    truncated = record_count < max_records;

    pubsub_itc_fw_app::SessionReplayComplete complete{};
    complete.request_id = view.request_id;
    complete.record_count = record_count;
    complete.last_seq_no = last_seq_no;
    complete.truncated = truncated;
    send_pdu(conn_id, pubsub_itc_fw_app::SessionReplayComplete::message_pdu_id, 0, complete);

    const int64_t elapsed_ns = config_.wall_clock->now_ns() - started_ns;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: SessionReplayComplete id={} comp_id='{}' records={} last_seq_no={} truncated={} scanned_in_ms={}", view.request_id,
               identity.comp_id_view(), record_count, last_seq_no, truncated, elapsed_ns / 1000000);
}

void SequencerThread::forward_pending_er(const PendingEr& pending) {
    // Wrap the buffered raw ER in a WalRecord envelope and forward it to wherever its
    // session is NOW. The address is resolved here rather than when the ER was buffered,
    // and the difference is the point: this path exists because delivery waited for the
    // follower's ack, and a member can reconnect -- to its backup gateway, after exactly
    // the failure this system is built for -- inside that wait.
    // pending.payload is alive for this call; the envelope's BytesView borrows it.
    const fix_common::SessionDestination* destination = pending.identity.empty() ? nullptr : session_destination(pending.identity);
    if (destination == nullptr && !pending.identity.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: buffered ER seq={} for session comp_id='{}' protocol={} -- session not bound to any instance, dropping", pending.seq_no,
                   pending.identity.comp_id_view(), pending.identity.protocol);
    }

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = pending.seq_no;
    envelope.pdu_id = pending.pdu_id;
    envelope.payload.data = pending.payload.data();
    envelope.payload.size = pending.payload.size();
    envelope.has_gateway_session_conn_id = destination != nullptr;
    envelope.gateway_session_conn_id = destination != nullptr ? destination->conn_id : 0;
    envelope.has_origin_gateway_id = destination != nullptr;
    envelope.origin_gateway_id = pending.identity.protocol;
    envelope.has_gateway_instance_id = destination != nullptr;
    envelope.gateway_instance_id = destination != nullptr ? destination->instance : gateway_ids::first_instance;
    envelope.has_gateway_ingress_ns = pending.has_gateway_ingress_ns;
    envelope.gateway_ingress_ns = pending.gateway_ingress_ns;
    envelope.has_sender_comp_id = !pending.identity.empty();
    envelope.sender_comp_id = pending.identity.comp_id_view();

    if (destination != nullptr) {
        send_er_to_origin_gateway(pending.identity.protocol, destination->instance, pending.seq_no, envelope);
        note_report_forwarded(pending.identity);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: buffered ER seq={} forwarded to protocol={} instance={}", pending.seq_no,
                   pending.identity.protocol, destination->instance);
    }

    if (pending.erase_routing_entry) {
        seq_no_to_session_.erase(pending.seq_no);
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

void SequencerThread::handle_role_announcement(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::RoleAnnouncementView announcement{};

    if (!pubsub_itc_fw_app::decode(announcement, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode RoleAnnouncement -- dropping");
        return;
    }

    if (announcement.group != pubsub_itc_fw_app::ComponentGroup::matching_engine) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: RoleAnnouncement for group={} -- ignoring, only matching_engine is routed",
                   pubsub_itc_fw_app::to_string(announcement.group));
        return;
    }

    // The epoch is what makes a claim safe to believe. An instance whose leadership has been
    // superseded still believes it leads until something tells it otherwise, and it may well
    // announce that on reconnecting; its epoch is behind the one the arbiter has since issued,
    // so the claim is refused without the sequencer having to ask the arbiter anything.
    if (announcement.epoch < me_announced_epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: RoleAnnouncement from instance {} on connection {} quotes epoch {}, behind the {} already accepted -- refusing",
                   announcement.instance_id, conn_id.get_value(), announcement.epoch, me_announced_epoch_);
        return;
    }
    me_announced_epoch_ = announcement.epoch;

    // The announcement arrives on the announcing instance's ER connection, which is not the
    // socket orders travel on. Look up this sequencer's own order connection to that instance:
    // routing orders down the connection an announcement arrived on sends them the wrong way.
    if (announcement.current_role == pubsub_itc_fw_app::Role::leader) {
        me_announced_leader_instance_ = announcement.instance_id;
    }

    const auto order_conn = me_order_conn_by_instance_.find(announcement.instance_id);
    if (order_conn == me_order_conn_by_instance_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: matching engine instance {} announced role at epoch {} but this sequencer holds no order connection to it",
                   announcement.instance_id, announcement.epoch);
        return;
    }

    if (announcement.current_role == pubsub_itc_fw_app::Role::leader) {
        me_announced_leader_instance_ = announcement.instance_id;
        if (order_conn->second != me_outbound_order_conn_id_) {
            // Whatever was in the order slot is not the leader, so it becomes the standby.
            me_secondary_standby_conn_id_ = me_outbound_order_conn_id_;
            me_outbound_order_conn_id_ = order_conn->second;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: matching engine instance {} leads at epoch {} -- orders now route to connection {}", announcement.instance_id,
                       announcement.epoch, order_conn->second.get_value());
        }
        return;
    }

    // A follower must not be sent orders: it drops them. If it is holding the order slot --
    // which is what happens when a restarted primary reconnects on the service it is
    // configured for -- move it out and leave the slot to whichever instance announces
    // leadership. Orders are refused meanwhile rather than sent somewhere they vanish.
    if (order_conn->second == me_outbound_order_conn_id_) {
        me_outbound_order_conn_id_ = pubsub_itc_fw::ConnectionID{};
        me_secondary_standby_conn_id_ = order_conn->second;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: matching engine instance {} on connection {} is a follower at epoch {} -- withdrawn from order routing",
                   announcement.instance_id, order_conn->second.get_value(), announcement.epoch);
    }
}

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
    // The earliest record still held. The log is truncated as it is consumed, so a walk from
    // zero starts wherever truncation left off rather than at the start of the day. An engine
    // that has lost its own record of what it held falls back to this one and has to be able
    // to tell those apart: see R-0123.
    int64_t earliest_retained = 0;
    [[maybe_unused]] auto end_pos = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, {0, 0}, [this, &conn_id, last_seq_no, &streamed, &earliest_retained](int64_t record_id, const void* payload, size_t size) {
            if (earliest_retained == 0) {
                earliest_retained = record_id;
            }
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

    // Signal completion. On receipt the ME considers its book reconciled and becomes leader.
    pubsub_itc_fw_app::MePositionAck ack{};
    ack.last_seq_no = wal_head;
    ack.first_seq_no = earliest_retained;
    send_pdu(conn_id, pubsub_itc_fw_app::MePositionAck::message_pdu_id, 0, ack);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: MePositionAck sent (last_seq_no={}, earliest retained={}) -- ME is now live",
               wal_head, earliest_retained);

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
