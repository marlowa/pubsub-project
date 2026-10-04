// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include <cstring>
#include <limits>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

#include <LeaderEpoch.hpp>
#include <LeaderStatement.hpp>
#include <OrderPathMetrics.hpp>
#include <PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FileSystemUtils.hpp>
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

// The arbiter pool's identity as a voter in deciding which sequencer leads. The sequencers are 1 and 2.
constexpr int64_t arbiter_pool_voter_id = 3;

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

// A named helper rather than a designated initialiser at the call site: this project builds
// as C++17, where designated initialisers are a C++20 feature and -Werror rejects them.
//
// Naming the scope is what opts this thread into the framework's per-thread metrics, and it
// matters more here than anywhere else on the order path. The sequencer sits between the two
// ends of the round trip -- every order reaches the matching engine through it and every
// report comes back through it -- so a round trip that is slow for want of this thread being
// scheduled is indistinguishable, from outside, from one that is slow anywhere else.
pubsub_itc_fw::ApplicationThreadConfiguration make_thread_config() {
    pubsub_itc_fw::ApplicationThreadConfiguration configuration;
    configuration.metrics_scope = "sequencer_thread";
    return configuration;
}

} // namespaces

// PDU IDs for the leader-follower and external WAL subscriber protocols.

SequencerThread::SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                                 const SequencerConfiguration& config)
    : ApplicationThread(token, logger, reactor, "SequencerThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        make_thread_config())
    , config_(config)
    , order_inbound_svc_("inbound:" + std::to_string(config.listen_port))
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , wal_subscriber_inbound_svc_("inbound:" + std::to_string(config.wal_subscriber_listen_port))
    , peer_conn_id_{}
    , peer_inbound_conn_id_{}
    , arbiter_primary_conn_id_{}
    , arbiter_secondary_conn_id_{}
    , epoch_store_(config.wal_directory + "/epoch.state")
    , lease_promise_store_(config.wal_directory + "/lease_promise.state", fix_common::LeasePromiseStore::current_boot_id()) {}

void SequencerThread::on_initial_event() {
    // The log starts a helper thread to prepare its next segment, and a new thread inherits the
    // processor mask of whichever thread created it. This one is created from here, on the
    // application thread, by then pinned to a processor reserved for the order path -- so
    // without being told otherwise the helper does its file opening and memory mapping on the
    // processor this thread sequences every order on. Told before the log is opened, because
    // opening it is what starts the helper. See BUG-0093.
    pubsub_itc_fw::CpuLayout helper_layout;
    const auto [helper_layout_loaded, helper_layout_error] = helper_layout.load(config_.cpu_layout_file, config_.cpu_layout_component);
    if (helper_layout_loaded) {
        wal_.set_helper_cores(helper_layout.background_cores());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: could not read the CPU layout, so the log's helper thread stays where it lands: {}", helper_layout_error);
    }

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
    reserve_record_of_identifiers();
    open_wal_trusting_it_up_to_any_gap();
    const int64_t recovered_seq = wal_.last_seq_no();
    if (recovered_seq > 0) {
        next_sequence_number_ = recovered_seq + 1;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WAL open complete: recovered seq_no={}, record_count={}, "
                   "next_sequence_number={}",
                   recovered_seq, wal_.record_count(), next_sequence_number_);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL is fresh (no prior records), starting from seq_no=1");
    }

    // Recover the leadership generation before any lease is asked for. Without
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
        set_epoch(fix_common::LeaderEpoch::next_for(epoch_, static_cast<int64_t>(config_.instance_id)));
        adopt_role(pubsub_itc_fw_app::Role::leader);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=false -- starting as leader immediately");
    } else {
        // Instance ids are 1 for the primary and 2 for the secondary, so the peer is the other one.
        const int64_t self_id = static_cast<int64_t>(config_.instance_id);
        const int64_t peer_id = self_id == 1 ? 2 : 1;
        lease_agent_.emplace("SequencerThread", get_logger(), lease_links_, pubsub_itc_fw_app::ComponentGroup::sequencer, self_id, peer_id,
                             arbiter_pool_voter_id, "the arbiter", config_.lease, std::chrono::steady_clock::now(), epoch_,
                             fix_common::PeerStatementsFlag{fix_common::PeerStatementsFlag::SayWhetherPeerMayLead});
        background_promise_recorder_.emplace(lease_promise_store_);
        lease_agent_->keep_promises_in(*background_promise_recorder_, lease_promise_store_.load(), lease_promise_store_.load_statement(),
                                       std::chrono::steady_clock::now());
        lease_tick_timer_id_ = start_recurring_timer(fix_common::LeaseTiming::tick_interval);
        acknowledgement_watch_timer_id_ = start_recurring_timer(acknowledgement_watch_interval);
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: ha_enabled=true -- leading only while the peer or the arbiter grants a lease (period={} ms); nothing is asked for "
                   "during the first period",
                   config_.lease.period.count());
    }

    // Registered here rather than at ready, matching the gateways. The handle is a no-op when
    // metrics are disabled, and the application and component tokens come from configuration.
    if (!config_.wal_append_buckets.empty()) {
        wal_append_histogram_ = get_reactor().metrics().register_histogram(
            "sequencer_thread", "wal_append_nanoseconds", "Nanoseconds spent committing one record to the write-ahead log, on the reactor thread",
            config_.wal_append_buckets);
    }

    // One family, four children, told apart by scope. Registered together so that a
    // deployment cannot end up with some checkpoints of the path and not others, which would
    // read as a stage taking no time rather than as a stage not being measured.
    if (!config_.order_path_elapsed_buckets.empty()) {
        order_in_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::order_in_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
        order_out_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::order_out_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
        er_in_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::er_in_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
        er_out_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::er_out_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
    }

    // A mount option decides whether this component meets its latency requirement, and nothing
    // in the configuration reveals it, so it is checked and said out loud at startup.
    //
    // Every writeback of a dirty mapped page also dirties the inode's timestamps, and an inode
    // change is journalled metadata -- so each flush drags the log into an ext4 journal
    // transaction, and waiting for one to commit is uninterruptible. `lazytime` keeps timestamp
    // updates in memory instead. Measured on 2026-08-31 over 9.3 million records, same load and
    // same hardware, with only this option changed:
    //
    //     relatime   5 appends over 100 ms, 1 over 500 ms, worst reactor stall 845 ms
    //     lazytime   0 appends over  10 ms,                worst reactor stall   0 ms
    //
    // A machine rebuilt without it will look as though the venue has regressed for no reason,
    // which is why this warns rather than staying silent. It is a warning and not a refusal:
    // the venue works correctly without it, only slower in the tail.
    const std::string wal_mount_options = pubsub_itc_fw::FileSystemUtils::mount_options(config_.wal_directory);
    if (wal_mount_options.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: cannot tell how the filesystem holding the log at {} is mounted, so cannot confirm it uses lazytime. See "
                   "docs/operations/filesystem_requirements.md",
                   config_.wal_directory);
    } else if (wal_mount_options.find("lazytime") == std::string::npos) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the filesystem holding the log at {} is mounted [{}] and does NOT use lazytime. Commit latency will show "
                   "occasional stalls of hundreds of milliseconds. Remount with lazytime. See docs/operations/filesystem_requirements.md",
                   config_.wal_directory, wal_mount_options);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: the log at {} is on a filesystem mounted [{}]", config_.wal_directory,
                   wal_mount_options);
    }

    // Whether the log's next segment is being created ahead of the writer, or the writer is
    // having to do it. The append histogram says what a commit cost; these two say why. Without
    // them a tail that fails to improve cannot be told apart from a helper that never kept up.
    wal_segments_filled_inline_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "wal_segments_filled_inline",
        "Log segments the writer created itself instead of adopting one prepared ahead of it. Expected to be 1, for the first segment");
    wal_segments_waited_for_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "wal_segments_waited_for", "Segment rolls that had to wait for a preparation still in progress. Expected to be 0");
    running_alone_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_running_alone",
        "1 while a follower is connected but too far behind and the leader sends orders to the matching engine without waiting for it; a loss of "
        "resilience. Expected to be 0");
    kept_reports_lost_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_kept_reports_lost",
        "Reports from the matching engine, kept in case this instance takes the lead, that were overwritten while still needed because the store "
        "was full. Expected to be 0");
}

void SequencerThread::append_to_wal(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns, int32_t leader_epoch) {
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

    const std::lock_guard<std::mutex> lock(log_epochs_mutex_);
    if (seq_no != log_epochs_.last_seq_no() + 1) {
        // Records are numbered without a break; one that is not means the table of epochs no longer
        // describes the log, and a rejoining follower could be told the logs agree when they do not.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: record {} was written after record {} -- the log has a gap, and the table of epochs is not extended past it", seq_no,
                   log_epochs_.last_seq_no());
        return;
    }
    log_epochs_.note_record(seq_no, leader_epoch);
}

void SequencerThread::open_wal_trusting_it_up_to_any_gap() {
    // The whole log is read, rather than from the snapshot onwards, because the table of epochs needs
    // every record's epoch, and the check for a gap needs every record's number.
    int64_t expected = 1;
    int64_t last_before_gap = -1;
    int64_t first_after_gap = 0;
    const int64_t recovered_seq = wal_.open(
        config_.wal_directory, config_.wal_segment_size,
        [this, &expected, &last_before_gap, &first_after_gap](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size,
                                                              int64_t /*wall_time_ns*/) {
            if (last_before_gap >= 0) {
                return;
            }
            if (seq_no != expected) {
                last_before_gap = expected - 1;
                first_after_gap = seq_no;
                return;
            }
            int32_t epoch = 0;
            if (pdu_id == pubsub_itc_fw_app::WalRecord::message_pdu_id) {
                auto& arena_buf = decode_arena_buffer();
                pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
                size_t arena_bytes_needed = 0;
                size_t bytes_consumed = 0;
                pubsub_itc_fw_app::WalRecordView view{};
                if (pubsub_itc_fw_app::decode(view, payload, payload_size, bytes_consumed, arena, arena_bytes_needed)) {
                    if (view.has_leader_epoch) {
                        epoch = view.leader_epoch;
                    }
                    note_logged_command(view);
                }
            }
            log_epochs_.note_record(seq_no, epoch);
            ++expected;
        },
        pubsub_itc_fw::WalOpenMode{pubsub_itc_fw::WalOpenMode::IgnoreSnapshot});

    if (last_before_gap >= 0) {
        // Records after a gap were sent by a leader, which still holds them and sends them again once
        // this instance's log and its leader's agree (docs/availability/follower_log_repair.md, 4.2).
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the write-ahead log has a gap -- record {} follows record {}. Discarding every record from {} to {}; the leader "
                   "sends them again",
                   first_after_gap, last_before_gap, last_before_gap + 1, recovered_seq);
        wal_.truncate_after(last_before_gap);
    }
    highest_replicated_seq_no_.store(wal_.last_seq_no(), std::memory_order_release);
    // Everything the log holds has been read; a follower reads on from where the log now ends.
    identifiers_read_position_ = wal_.scan_start_for(wal_.last_seq_no() + 1);
    if (logged_identifiers_.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the record of command identifiers holds {} identifier(s) from the log, of {} reserved", logged_identifiers_->size(),
                   logged_identifiers_->capacity());
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
        engine_routing_.connected(me_primary_instance_id, id, ClaimIfNothingRoutedFlag{ClaimIfNothingRoutedFlag::ClaimIfNothingRouted});
        if (engine_routing_.active() == id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: order connection {} to matching engine instance {} carries orders{}",
                       id.get_value(), me_primary_instance_id,
                       engine_routing_.announced_leader() == me_primary_instance_id ? ", because it had already announced that it leads" : "");
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: matching engine connection {} established while connection {} is already active -- held as standby pending its role",
                       id.get_value(), engine_routing_.active().get_value());
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: matching engine order connection {} established", id.get_value());
        if (config_.replay_mode) {
            replay_me_order_ready_ = true;
            try_dispatch_replay();
        }
    } else if (svc == "matching_engine_secondary") {
        engine_routing_.connected(me_secondary_instance_id, id, ClaimIfNothingRoutedFlag{ClaimIfNothingRoutedFlag::DoNotClaimIfNothingRouted});
        if (engine_routing_.active() == id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: order connection {} to matching engine instance {}, which had already announced leadership -- routing there",
                       id.get_value(), me_secondary_instance_id);
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ME-secondary standby connection {} established (pre-warmed for failover)",
                   id.get_value());
    } else if (svc == "arbiter_primary") {
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-primary connection {} established", id.get_value());
    } else if (svc == "arbiter_secondary") {
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: arbiter-secondary connection {} established", id.get_value());
    } else if (svc == "peer") {
        peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: outbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        install_peer_wal_inline_handler(id);
        send_status_query(id);
        forget_log_agreement();
        send_log_position_request();
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        install_peer_wal_inline_handler(id);
        send_status_query(id);
        forget_log_agreement();
        send_log_position_request();
    } else if (svc == "inbound:" + std::to_string(config_.listen_port)) {
        // A gateway's connection for orders. While reading from the gateways is paused, a gateway that
        // connects is paused too.
        order_connection_ids_.push_back(id);
        if (order_reading_paused_) {
            pause_reading(id);
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway order connection {} established{}", id.get_value(),
                   order_reading_paused_ ? " -- reading from it is paused while orders wait" : "");
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
    if (engine_routing_.active().is_valid()) {
        note_matching_engine_reachable();
    }
}

void SequencerThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    const auto lost_gateway = std::find_if(gateway_conn_ids_.begin(), gateway_conn_ids_.end(), [&id](const auto& entry) { return entry.second == id; });
    if (lost_gateway != gateway_conn_ids_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway id {} connection {} lost: {}", lost_gateway->first,
                   id.get_value(), reason);
        gateway_conn_ids_.erase(lost_gateway);
    } else if (engine_routing_.is_engine_connection(id)) {
        const bool carried_orders = engine_routing_.active() == id;
        engine_routing_.lost(id);
        if (carried_orders) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: matching engine order connection {} lost: {} -- the other engine may promote and reconnect", id.get_value(), reason);
            if (awaiting_engine_position_) {
                // The engine that was asked is gone. Whichever engine acts next catches up from this
                // log before it acts, which sends it everything the asked one lacked, so nothing more
                // is waited for.
                stop_awaiting_engine_position("the matching engine it asked has disconnected; whichever engine acts next catches up from this log");
            }
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: standby matching engine connection {} lost: {}", id.get_value(),
                       reason);
        }
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
    } else if (id == peer_conn_id_) {
        peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: outbound peer connection {} lost: {}", id.get_value(), reason);
        forget_log_agreement();
        flush_pending_er();
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: inbound peer connection {} lost: {}", id.get_value(), reason);
        forget_log_agreement();
        flush_pending_er();
    } else if (const auto order_connection = std::find(order_connection_ids_.begin(), order_connection_ids_.end(), id);
               order_connection != order_connection_ids_.end()) {
        order_connection_ids_.erase(order_connection);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway order connection {} lost: {}", id.get_value(), reason);
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

    // Arbiter PDUs: the active arbiter's answers to this sequencer's lease requests.
    if (conn_id == arbiter_primary_conn_id_ || conn_id == arbiter_secondary_conn_id_) {
        if (message.pdu_id() == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
            handle_lease_grant(message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
            handle_lease_refusal(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unexpected PDU {} from an arbiter -- dropping", message.pdu_id());
        }
        release_pdu_payload(message);
        return;
    }

    // A matching engine asking to be brought up to date before it acts. Accepted on this
    // sequencer's order connection to either engine, whichever carries orders at the moment: an
    // engine that is about to lead may be on either, and a request dropped because routing pointed
    // elsewhere left the venue with no engine while one was leading and asking (BUG-0108).
    if (message.pdu_id() == pubsub_itc_fw_app::MePositionRequest::message_pdu_id && engine_routing_.is_engine_connection(conn_id)) {
        handle_me_position_request(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // The leading matching engine's answer to the question a new leader asks on taking the lead.
    if (message.pdu_id() == pubsub_itc_fw_app::EnginePosition::message_pdu_id && engine_routing_.is_engine_connection(conn_id)) {
        handle_engine_position(message);
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

        // A follower writes its log only from its leader's records, so that the two logs stay
        // identical, and numbers nothing itself. It discards the gateway's copy here, before a
        // number is taken: counting these copies in next_sequence_number_ is what left a newly
        // promoted follower numbering below records its log already held (BUG-0105).
        if (role_ == pubsub_itc_fw_app::Role::follower) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order envelope on connection {} inner_pdu_id={} -- follower, discarding the gateway's copy; the log is "
                       "written from the leader's records",
                       message.connection_id().get_value(), inner_pdu_id);
            release_pdu_payload(message);
            return;
        }

        // A command a gateway sends again after a change of leader, because nothing had answered it.
        // If this log already holds it, it must not be sequenced twice: its answer comes from the
        // engine, by one of the routes in docs/availability/commands_during_a_change_of_leader.md,
        // section 3.6. If not, nothing ever acted on it, and it is sequenced as a new command.
        if (role_ == pubsub_itc_fw_app::Role::leader && inbound.has_sent_again && inbound.sent_again && command_already_logged(inbound)) {
            ++sent_again_already_logged_;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: command ClOrdID={} from comp_id='{}' sent again -- this log already holds it, not sequencing it twice",
                       inbound.cl_ord_id, inbound.sender_comp_id);
            release_pdu_payload(message);
            return;
        }
        if (inbound.has_sent_again && inbound.sent_again) {
            ++sent_again_sequenced_;
        }

        const int64_t seq = next_sequence_number_++;
        const int64_t wall_time_ns = config_.wall_clock->now_ns();

        // The first checkpoint this process contributes, reusing the sequencing stamp rather
        // than reading the clock again: they describe the same instant, and two readings would
        // differ by the cost of taking them.
        //
        // NewOrderSingle only, which is the population the round-trip histogram measures. A
        // cancel travels the same path and would otherwise be mixed into the same series,
        // leaving a profile whose stages cannot be added up against a round trip drawn from
        // orders alone.
        if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
            order_path_metrics::observe_checkpoint(order_in_elapsed_histogram_, inbound.has_gateway_ingress_ns, inbound.gateway_ingress_ns, wall_time_ns);
        }

        // Stamp the envelope with the assigned seq_no and sequencing wall time. The
        // inner FIX payload (a BytesView into the inbound slab) stays valid until
        // release_pdu_payload(message) below, after every send has copied it.
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = seq;
        envelope.pdu_id = inner_pdu_id;
        envelope.payload = inbound.payload;
        envelope.wall_time_ns = wall_time_ns;
        envelope.has_leader_epoch = true;
        envelope.leader_epoch = epoch_;
        envelope.has_gateway_session_conn_id = inbound.has_gateway_session_conn_id;
        envelope.gateway_session_conn_id = inbound.gateway_session_conn_id;
        envelope.has_sender_comp_id = inbound.has_sender_comp_id;
        envelope.sender_comp_id = inbound.sender_comp_id;
        // The session is its comp id AND its gateway protocol, so the protocol and the instance are
        // logged with the order and travel on to the matching engine. Without them the engine files a
        // binary gateway member's order under the default protocol, and its reports name the wrong
        // session -- which matters wherever a report is routed by the identity it carries, such as the
        // reports a new leader forwards after a change of leader.
        envelope.has_origin_gateway_id = inbound.has_origin_gateway_id;
        envelope.origin_gateway_id = inbound.origin_gateway_id;
        envelope.has_gateway_instance_id = inbound.has_gateway_instance_id;
        envelope.gateway_instance_id = inbound.gateway_instance_id;
        // Logged so that the record of identifiers can be rebuilt from the log, and kept up to date by
        // a follower, without decoding the command itself.
        envelope.has_cl_ord_id = inbound.has_cl_ord_id;
        envelope.cl_ord_id = inbound.cl_ord_id;

        // The time the gateway read this order off the client connection travels on to the
        // matching engine as well as being remembered above. The sequencer does not need it
        // there -- it keeps its own copy for the report coming back -- but the matching
        // engine cannot time its own part of the journey without it, because every timing on
        // the path is counted from this one moment. See OrderPathMetrics.hpp.
        envelope.has_gateway_ingress_ns = inbound.has_gateway_ingress_ns;
        envelope.gateway_ingress_ns = inbound.gateway_ingress_ns;

        // WAL commit: the leader appends from the direct gateway PDU. A follower has already
        // returned above. An instance that has not yet learnt its role appends locally because
        // it may become the leader.
        append_envelope_to_wal(envelope);
        note_logged_command(envelope.has_sender_comp_id ? envelope.sender_comp_id : std::string_view{},
                            envelope.has_origin_gateway_id ? envelope.origin_gateway_id : gateway_ids::default_when_absent, inner_pdu_id,
                            envelope.has_cl_ord_id ? envelope.cl_ord_id : std::string_view{});
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: order envelope on connection {} inner_pdu_id={} seq={} -- WAL append ok (wal_size={}) role={}",
                   message.connection_id().get_value(), inner_pdu_id, seq, wal_.record_count(), pubsub_itc_fw_app::to_string(role_));

        if (role_ != pubsub_itc_fw_app::Role::leader) {
            // Follower/unknown: do not forward to ME.
            release_pdu_payload(message);
            return;
        }

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

        // With a follower connected and keeping up, the order goes to the matching engine only once
        // the follower has acknowledged its record, so the engine never acts on an order the follower
        // does not hold (docs/availability/change_of_sequencer_leader.md, part 4.2). If the storage
        // for held orders is full the follower is too far behind, and the leader runs as if alone.
        if (needs_wal_ack() && (held_orders_.full() || first_unheld_seq_ != 0)) {
            start_running_alone("its storage for orders waiting on the follower is full");
        }

        // Without a follower that keeps up, the leader acts on an order its follower lacks only once a
        // voter has confirmed its statement that the follower may not lead (rule 11,
        // docs/availability/a_follower_behind_does_not_lead.md), and never ahead of an order already
        // waiting. Until then the order is replicated and published at once and waits: in the storage
        // for held orders, or in the log once that is full.
        if (config_.ha_enabled &&
            (needs_wal_ack() || !may_act_without_follower() || !held_orders_.empty() || first_unheld_seq_ != 0 || awaiting_engine_position_)) {
            if (!needs_wal_ack()) {
                say_follower_may_not_lead();
            }
            send_wal_record(envelope);
            stream_wal_record_to_external_subscribers(envelope);
            hold_until_acknowledged(envelope, message);
            pause_or_resume_order_reading();
            release_if_confirmed();
            return; // The payload is released when the held order is sent on, or at once if it waits in the log.
        }

        // Sent at once: HA is off, or no follower keeps up and a voter has confirmed that it may not lead.
        if (engine_routing_.active().is_valid()) {
            // Reachable again. Anything deferred is recovered by the catch-up the arriving engine
            // performs before it acts, and an operator wants one line saying what the outage cost.
            note_matching_engine_reachable();

            // A member is waiting for this: it is the order on its way to be matched. The
            // replication and subscriber sends below are deliberately not marked -- nobody is
            // waiting on a client connection for any of them.
            send_pdu(engine_routing_.active(), pubsub_itc_fw_app::WalRecord::message_pdu_id, seq, envelope,
                     pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});

            // Immediately after the send rather than before it, so that order_in to order_out
            // covers everything this component did with the order, the write-ahead log commit
            // included.
            if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
                order_path_metrics::observe_checkpoint(order_out_elapsed_histogram_, inbound.has_gateway_ingress_ns, inbound.gateway_ingress_ns,
                                                       config_.wall_clock->now_ns());
            }
        } else {
            // The order is already durably WAL-committed above; we simply cannot forward it right
            // now because no matching engine is connected. The forward is deferred rather than the
            // order lost: whichever engine acts next reports the position it has reached and is
            // sent everything after it, which includes this order.
            //
            // That holds however the next engine arrives. A promoting follower knows its position
            // from the replica it was maintaining and a starting one from the region it recovered,
            // and both catch up before they act. A start used to skip the catch-up, so an order
            // deferred while every engine was down was applied by nobody and answered to nobody --
            // BUG-0064, and the reason this comment once carried a warning instead of a
            // reassurance.
            //
            // It costs the venue nothing to defer -- the payload is released here and the WAL is
            // the whole mechanism. It costs the MEMBER a great deal: it has been acknowledged, so
            // it believes the order is live, and it cannot cancel it because a cancel needs the
            // same matching engine. That is the asymmetry BUG-0009 is about, and why this is
            // reported by how long it has gone on rather than once per order.
            note_order_deferred(seq);
        }

        // Replicated whether or not an engine took it: a deferred order must reach the follower's log
        // as much as any other.
        if (config_.ha_enabled) {
            send_wal_record(envelope);
        }

        // Stream to external WAL subscribers (MEP primary and secondary), which
        // unwrap the envelope and publish the inner DD-derived PDU on its topic.
        stream_wal_record_to_external_subscribers(envelope);

        release_pdu_payload(message);

    } else if (is_er_pdu) {
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            // Kept in case this instance takes the lead before the leader has forwarded it.
            keep_report_from_engine(message);
            release_pdu_payload(message);
            return;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER PDU on connection {} pdu_id={} seq={} -- forwarding to gateway",
                   message.connection_id().get_value(), message.pdu_id(), message.seq_no());
        forward_report_from_engine(message.payload(), static_cast<size_t>(message.payload_size()), message.seq_no(), ReportSource::matching_engine);
        release_pdu_payload(message);
    } else {
        // Unknown source -- log and discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: PDU on unexpected connection {} ({}) -- dropping",
                   message.connection_id().get_value(), svc);
        release_pdu_payload(message);
    }
}

void SequencerThread::forward_report_from_engine(const uint8_t* bytes, size_t size, int64_t er_seq_no, ReportSource source) {

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
    if (!pubsub_itc_fw_app::decode(inbound, bytes, size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ER envelope -- dropping");
        return;
    }
    if (inbound.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: ER envelope carries unexpected pdu_id {} -- dropping", inbound.pdu_id);
        return;
    }
    pubsub_itc_fw_app::ExecutionReportView view{};
    if (!pubsub_itc_fw_app::decode(view, inbound.payload.data, inbound.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode ExecutionReport -- dropping");
        return;
    }

    // The report that acknowledges a new order is the only one the order-path checkpoints
    // record, matching the population the gateway's round-trip histogram measures. Every
    // report for an order carries the same ingress stamp, so a Canceled report would be
    // recorded as though the path had taken as long as the order rested on the book --
    // which would swamp the distribution rather than merely widen it.
    //
    // ord_status is already decoded here for the routing map, so the restriction costs
    // nothing beyond the comparison.
    //
    // A kept report is not measured at all: it waited in the store for a change of leader, which
    // has nothing to do with how long the path takes.
    const bool is_new_order_ack = (source == ReportSource::matching_engine) && (view.ord_status == pubsub_itc_fw_app::OrdStatus::New);

    // Whether the member may already hold this report. The matching engine says so of a report it
    // repeats. A kept report may have been forwarded by the leader before it died, and nothing here
    // can tell whether it was, so it is always marked. See R-0122.
    const bool possible_repeat = inbound.poss_resend || (source == ReportSource::kept_while_not_leading);

    // The report has arrived from the matching engine. Against the matching engine's own
    // er_out this gives the hop between the two processes; against er_out below it gives
    // what this component costs the report, which includes sequencing it into the log and
    // any wait for the follower to acknowledge it.
    if (is_new_order_ack) {
        order_path_metrics::observe_checkpoint(er_in_elapsed_histogram_, inbound.has_gateway_ingress_ns, inbound.gateway_ingress_ns,
                                               config_.wall_clock->now_ns());
    }

    // Route the ER back to the originating FIX session. The ME echoes the order's
    // seq_no in the transport header, so er_seq_no resolves via the map for
    // ordinary ERs. ERs not tied to a sequenced order (the seq_no==0 cancel-on-failover
    // ERs) instead carry the conn id on the inbound envelope. The conn id rides on the
    // envelope, never inside the DD-derived ER.
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
            // The order's sequence is not in the map. Either the report has no originating
            // order sequence, as with the cancel-on-failover reports a promoted matching
            // engine emits, or it was kept while this instance was not leading, and only a
            // leader fills the map. Every report the engine sends carries the identity of the
            // session whose order it is about, which is exactly what is needed -- and it is
            // why the ME stores the identity against each resting order rather than the
            // connection that placed it, which by then may no longer exist.
            routing_identity = fix_common::SessionIdentity::make(inbound.sender_comp_id,
                                                                 inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent);
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
    envelope.has_leader_epoch = true;
    envelope.leader_epoch = epoch_;
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
    // The ClOrdID of the command the report answers, so that a gateway can tell which command it
    // holds unanswered is now answered without decoding the report itself, which the binary gateway
    // relays as it arrives (docs/availability/commands_during_a_change_of_leader.md, section 3.1).
    envelope.has_cl_ord_id = view.has_cl_ord_id;
    envelope.cl_ord_id = view.cl_ord_id;
    // Whether the member may already hold the report, worked out above. The gateway writes it
    // as PossResend. See R-0122.
    envelope.poss_resend = possible_repeat;

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
    //
    // A report that repeats one already sent needs the same treatment, for the same reason
    // arrived at from the other direction. It names the sequence of an order the venue took
    // earlier, and that order's WalAck came and was consumed at the time -- so gating on it
    // waits for an acknowledgement that is in the past and never comes again. Every report
    // a matching engine sends out of a catch-up is such a report, which is what the mark on
    // the envelope says. Without this the deferred order that catch-up exists to answer is
    // applied, reported, and then parked in pending_er_ -- answered everywhere except at
    // the member.
    const bool gate_on_own_record = (er_seq_no == 0) || possible_repeat;
    const int64_t gate_seq_no = gate_on_own_record ? er_wal_seq : er_seq_no;

    if (!needs_wal_ack()) {
        send_er_to_origin_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope, is_new_order_ack);
        note_report_forwarded(routing_identity);
        if (erase_routing_entry) {
            seq_no_to_session_.erase(er_seq_no);
        }
    } else {
        if (gate_seq_no <= peer_acked_through_) {
            // The follower has already acknowledged the record this report depends on.
            send_er_to_origin_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope, is_new_order_ack);
            note_report_forwarded(routing_identity);
            if (erase_routing_entry) {
                seq_no_to_session_.erase(er_seq_no);
            }
        } else {
            // WalAck not yet received; buffer the inner (unwrapped) ER until it arrives.
            PendingEr pending{};
            pending.pdu_id = inbound.pdu_id;
            pending.seq_no = er_seq_no;
            pending.payload.assign(inbound.payload.data, inbound.payload.data + inbound.payload.size);
            if (view.has_cl_ord_id) {
                pending.cl_ord_id.assign(view.cl_ord_id);
            }
            pending.identity = routing_identity;
            pending.has_gateway_ingress_ns = has_routing_ingress_ns;
            pending.gateway_ingress_ns = routing_ingress_ns;
            pending.poss_resend = possible_repeat;
            pending.is_new_order_ack = is_new_order_ack;
            pending.erase_routing_entry = erase_routing_entry;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq={} buffered -- awaiting WalAck seq={} from follower", er_seq_no,
                       gate_seq_no);
            pending_er_.emplace(gate_seq_no, std::move(pending));
        }
    }
}

std::chrono::steady_clock::time_point SequencerThread::kept_reports_needed_from() const {
    // Before this instance has granted any request, no leader has been seen forwarding anything, so
    // every kept report may be needed.
    if (last_granted_to_leader_ == std::chrono::steady_clock::time_point{}) {
        return std::chrono::steady_clock::time_point::min();
    }
    return last_granted_to_leader_ - config_.lease.period - config_.lease.drift_allowance;
}

void SequencerThread::keep_report_from_engine(const pubsub_itc_fw::EventMessage& message) {
    const std::chrono::steady_clock::time_point needed_from = kept_reports_needed_from();
    kept_reports_.discard_received_before(needed_from);
    kept_reports_.keep(message.seq_no(), message.payload(), static_cast<size_t>(message.payload_size()), std::chrono::steady_clock::now(), needed_from);

    if (kept_reports_.lost() != kept_reports_lost_reported_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the store of reports kept in case this instance takes the lead is full, and {} report(s) still needed have "
                   "been overwritten. If this instance takes the lead now, those reports reach their members only if the leader forwarded them "
                   "before it stopped",
                   kept_reports_.lost() - kept_reports_lost_reported_);
        kept_reports_lost_reported_ = kept_reports_.lost();
        kept_reports_lost_gauge_.set(static_cast<double>(kept_reports_.lost()));
    }
}

void SequencerThread::forward_kept_reports() {
    kept_reports_.discard_received_before(kept_reports_needed_from());
    const size_t count = kept_reports_.count();
    const size_t bytes = kept_reports_.bytes_used();
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: taking the lead -- forwarding {} kept report(s) ({} bytes), each marked as a possible repeat", count, bytes);
    kept_reports_.take_all([this](int64_t er_seq_no, const uint8_t* report, size_t size) {
        forward_report_from_engine(report, size, er_seq_no, ReportSource::kept_while_not_leading);
    });
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

    if (id == lease_tick_timer_id_) {
        const auto now = std::chrono::steady_clock::now();
        act_on(lease_agent_->on_tick(now));
        release_if_confirmed();
        say_follower_may_lead_if_it_holds_everything();
        if (!follower_log_agreed_.load(std::memory_order_acquire) && now - last_position_request_at_ >= std::chrono::seconds{1}) {
            send_log_position_request();
        }
        return;
    }

    if (id == acknowledgement_watch_timer_id_) {
        check_follower_acknowledgements();
        const auto now = std::chrono::steady_clock::now();
        if (awaiting_engine_position_ && now - engine_position_asked_at_ >= engine_position_ask_interval) {
            ask_engine_for_position();
        }
        if (role_ != pubsub_itc_fw_app::Role::leader && now - identifiers_read_at_ >= identifiers_read_interval) {
            read_identifiers_from_log();
        }
        if (log_tail_index_.has_value() && now - log_tail_index_used_at_ >= log_tail_index_idle_limit) {
            discard_log_tail_index("no command has been sent again for a minute");
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
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), transition_level, "SequencerThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch_);

    role_ = new_role;
    forget_log_agreement();

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        // The record of identifiers must hold every command in the log before a command sent again
        // is checked against it. A follower reads the records it was sent once a second; this reads
        // the rest. From here this instance writes every record itself and notes each as it writes it.
        read_identifiers_from_log();

        // Number new records above every record the log holds. Records replicated while this
        // instance followed were written under its leader's numbers and did not move
        // next_sequence_number_, so without this the new leader would give new records numbers
        // its log already holds (BUG-0105).
        const int64_t highest_replicated = highest_replicated_seq_no_.load(std::memory_order_acquire);
        if (highest_replicated >= next_sequence_number_) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: taking the lead -- numbering new records from {}, above the highest replicated record {} (was {})",
                       highest_replicated + 1, highest_replicated, next_sequence_number_);
            next_sequence_number_ = highest_replicated + 1;
        }

        // The previous leader may have died holding reports it had not forwarded. This instance
        // kept every report the engine sent it, so it forwards them all now
        // (docs/availability/change_of_sequencer_leader.md, section 4.4).
        forward_kept_reports();

        // After the kept reports, and on the same connections, so that a gateway has the answers to
        // the commands it holds before it is told to send them again, and sends again only those still
        // unanswered (docs/availability/commands_during_a_change_of_leader.md, section 3.3). The
        // message carries this leadership's epoch, which is what tells a gateway that a new instance
        // leads.
        //
        // Tell the gateways where this venue now stands on accepting orders. They may be holding
        // what the previous leader last said, which was true of a process that is no longer
        // running. This instance has deferred nothing, so it accepts -- but that has to be said
        // rather than assumed, because silence here leaves a refusal in place that nothing will
        // ever lift.
        broadcast_order_acceptance();

        // This instance's log may hold orders the engine never received: the follower wrote and
        // acknowledged them, and the previous leader died before the acknowledgement reached it.
        // Ask the engine how far it has got, and send it the rest before anything new
        // (docs/availability/commands_during_a_change_of_leader.md, section 3.5).
        if (config_.ha_enabled) {
            ask_engine_for_position();
        }
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        awaiting_engine_position_ = false;
        if (log_tail_index_.has_value()) {
            discard_log_tail_index("this instance has stopped leading");
        }
        // Every record the log holds now was noted as it was written or read. The reactor thread
        // writes nothing until this log and the leader's are found to agree, so the end of the log
        // does not move under this read of it.
        identifiers_read_position_ = wal_.scan_start_for(wal_.last_seq_no() + 1);
        discard_held_orders();
        first_unheld_seq_ = 0;
        pause_or_resume_order_reading();
        send_log_position_request();
        if (running_alone_) {
            running_alone_ = false;
            running_alone_gauge_.set(0.0);
        }
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

void SequencerThread::act_on(fix_common::PairLeaseAgent::Change change) {
    // Whatever happened, the highest epoch this instance knows may have risen, and it is kept on disk.
    set_epoch(lease_agent_->highest_epoch());
    switch (change) {
        case fix_common::PairLeaseAgent::Change::BecameLeader:
            adopt_role(pubsub_itc_fw_app::Role::leader);
            break;
        case fix_common::PairLeaseAgent::Change::StoppedLeading:
        case fix_common::PairLeaseAgent::Change::AgreedPeerLeads:
            adopt_role(pubsub_itc_fw_app::Role::follower);
            break;
        case fix_common::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void SequencerThread::SequencerLeaseLinks::send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
    }
}

void SequencerThread::SequencerLeaseLinks::send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) {
    // Both arbiters: only the active one answers, and which one that is may have changed.
    for (const pubsub_itc_fw::ConnectionID& conn : {owner_.arbiter_primary_conn_id_, owner_.arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            owner_.send_pdu(conn, pubsub_itc_fw_app::LeaseRequest::message_pdu_id, 0, request);
        }
    }
}

void SequencerThread::SequencerLeaseLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseGrant::message_pdu_id, 0, grant);
}

void SequencerThread::SequencerLeaseLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw_app::LeaseRefusal::message_pdu_id, 0, refusal);
}

void SequencerThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode LeaseRequest -- dropping");
        return;
    }
    if (request.group != pubsub_itc_fw_app::ComponentGroup::sequencer || !lease_agent_.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: LeaseRequest for group={} on the peer link -- dropping",
                   pubsub_itc_fw_app::to_string(request.group));
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const fix_common::PairLeaseAgent::Change change = lease_agent_->on_request(
        conn_id, request.candidate_instance_id, request.epoch, request.request_id,
        fix_common::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead}, now);
    if (change == fix_common::PairLeaseAgent::Change::AgreedPeerLeads) {
        // The peer now leads, and forwards the engine's reports, until at least a lease period from now.
        last_granted_to_leader_ = now;
    }
    act_on(change);
}

void SequencerThread::handle_lease_grant(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseGrantView grant{};
    if (!pubsub_itc_fw_app::decode(grant, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode LeaseGrant -- dropping");
        return;
    }
    if (grant.group != pubsub_itc_fw_app::ComponentGroup::sequencer || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, std::chrono::steady_clock::now()));
    // A grant may carry the echo that confirms the leader's statement, which releases what waits on it.
    release_if_confirmed();
}

void SequencerThread::handle_lease_refusal(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LeaseRefusalView refusal{};
    if (!pubsub_itc_fw_app::decode(refusal, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode LeaseRefusal -- dropping");
        return;
    }
    if (refusal.group != pubsub_itc_fw_app::ComponentGroup::sequencer || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, std::chrono::steady_clock::now()));
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

    // The exchange tells a restarting peer how far the sequence has reached. Which of the two leads
    // is not settled here: that is decided by leases, which this exchange plays no part in.
    send_status_response(conn_id);
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

    // This instance's next sequence number is not moved up to the peer's. A rejoining follower is
    // sent the records it missed (docs/availability/follower_log_repair.md), and an instance that
    // takes the lead numbers above every record it then holds.
}

void SequencerThread::handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());

    if (pdu_id == pubsub_itc_fw_app::StatusQuery::message_pdu_id) {
        handle_peer_status_query(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::StatusResponse::message_pdu_id) {
        handle_peer_status_response(message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseRequest::message_pdu_id) {
        handle_lease_request(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseGrant::message_pdu_id) {
        handle_lease_grant(message);
    } else if (pdu_id == pubsub_itc_fw_app::LeaseRefusal::message_pdu_id) {
        handle_lease_refusal(message);
    } else if (pdu_id == pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        handle_wal_record(conn_id, message);
    } else if (pdu_id == pubsub_itc_fw_app::WalAck::message_pdu_id) {
        handle_wal_ack(message);
    } else if (pdu_id == pubsub_itc_fw_app::LogPositionRequest::message_pdu_id) {
        handle_log_position_request(message);
    } else if (pdu_id == pubsub_itc_fw_app::LogPositionReply::message_pdu_id) {
        handle_log_position_reply(message);
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

        send_pdu(engine_routing_.active(), pubsub_itc_fw_app::WalRecord::message_pdu_id, record.seq_no, envelope);
        ++dispatched;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay complete -- {}/{} record(s) dispatched to matching engine", dispatched,
               replay_buffer_.size());

    replay_buffer_.clear();
    replay_buffer_.shrink_to_fit();
}

// WAL replication helpers (Slice 7)

bool SequencerThread::needs_wal_ack() const {
    return config_.ha_enabled && peer_active_conn().is_valid() && follower_log_matches_ && !running_alone_;
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
                  envelope.wall_time_ns, envelope.has_leader_epoch ? envelope.leader_epoch : 0);
}

void SequencerThread::send_wal_record(const pubsub_itc_fw_app::WalRecord& envelope) {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!target.is_valid() || !follower_log_matches_) {
        return;
    }
    send_pdu(target, pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope.seq_no, envelope);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: WalRecord sent to follower seq={} inner_pdu_id={}", envelope.seq_no,
               envelope.pdu_id);
}

void SequencerThread::handle_wal_record(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    // Every record reaching this thread was passed on by the inline handler, which counted it, or
    // arrived before the handler was installed. Uncounting it after it is written or discarded is
    // what lets the inline handler write again, once nothing is left here to write before it.
    struct UncountOnExit {
        std::atomic<int64_t>& queued;
        ~UncountOnExit() {
            int64_t current = queued.load(std::memory_order_acquire);
            while (current > 0 && !queued.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
        }
    } uncount{replicated_records_queued_};

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

    if (role_ == pubsub_itc_fw_app::Role::leader || !follower_log_agreed_.load(std::memory_order_acquire)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: WalRecord seq={} discarded -- this log and the leader's are not yet known to agree", view.seq_no);
        return;
    }
    // A record this log already holds, sent again after the logs were found to agree, is acknowledged
    // and not written twice; one that would leave a gap is not written at all.
    if (!replicated_record_is_next(view.seq_no)) {
        return;
    }

    // Option B: store the received WalRecord bytes verbatim under the WalRecord pdu
    // so the follower WAL is byte-identical to the leader's. (view is decoded only to
    // read seq_no + wall_time_ns for the append header and the WalAck.)
    append_to_wal(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, message.payload(), message.payload_size(), view.wall_time_ns,
                  view.has_leader_epoch ? view.leader_epoch : 0);
    note_replicated_record(view.seq_no);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "SequencerThread: WalRecord seq={} inner_pdu_id={} written to follower WAL (wal_size={}) -- sending WalAck", view.seq_no, view.pdu_id,
               wal_.record_count());

    pubsub_itc_fw_app::WalAck wal_ack{};
    wal_ack.seq_no = view.seq_no;
    send_pdu(conn_id, pubsub_itc_fw_app::WalAck::message_pdu_id, 0, wal_ack);
}

void SequencerThread::forget_log_agreement() {
    follower_log_agreed_.store(false, std::memory_order_release);
    follower_log_matches_ = false;
    // A leader that stops sending live records says so, or its follower, still believing the logs
    // agree, would wait for records that never come.
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (config_.ha_enabled && role_ == pubsub_itc_fw_app::Role::leader && target.is_valid()) {
        pubsub_itc_fw_app::LogPositionReply reply{};
        reply.ask_again = true;
        send_pdu(target, pubsub_itc_fw_app::LogPositionReply::message_pdu_id, 0, reply);
    }
}

bool SequencerThread::replicated_record_is_next(int64_t seq_no) {
    int64_t last = 0;
    {
        const std::lock_guard<std::mutex> lock(log_epochs_mutex_);
        last = log_epochs_.last_seq_no();
    }
    if (seq_no == last + 1) {
        return true;
    }
    if (seq_no > last + 1) {
        // A record is missing. Writing this one would leave a gap, so nothing more is written until
        // the leader has been asked again where the logs agree, which the lease timer does.
        follower_log_agreed_.store(false, std::memory_order_release);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: replicated record {} arrived after record {} -- one is missing, so this log and the leader's are no longer "
                   "known to agree; asking again",
                   seq_no, last);
    }
    return false;
}

void SequencerThread::send_log_position_request() {
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (!config_.ha_enabled || role_ == pubsub_itc_fw_app::Role::leader || !target.is_valid() || follower_log_agreed_.load(std::memory_order_acquire)) {
        return;
    }
    pubsub_itc_fw_app::LogPositionRequest request{};
    {
        const std::lock_guard<std::mutex> lock(log_epochs_mutex_);
        request.last_seq_no = log_epochs_.last_seq_no();
        request.last_epoch = log_epochs_.last_epoch();
    }
    last_position_request_at_ = std::chrono::steady_clock::now();
    send_pdu(target, pubsub_itc_fw_app::LogPositionRequest::message_pdu_id, 0, request);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "SequencerThread: asking the leader where its log and this one agree (last record {} from epoch {})", request.last_seq_no, request.last_epoch);
}

void SequencerThread::handle_log_position_request(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LogPositionRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode LogPositionRequest -- dropping");
        return;
    }
    const pubsub_itc_fw::ConnectionID target = peer_active_conn();
    if (role_ != pubsub_itc_fw_app::Role::leader || !target.is_valid()) {
        return;
    }

    LogEpochTable::PositionAnswer answer{};
    {
        const std::lock_guard<std::mutex> lock(log_epochs_mutex_);
        answer = log_epochs_.answer_position(request.last_seq_no, request.last_epoch);
    }
    pubsub_itc_fw_app::LogPositionReply reply{};
    reply.seq_no = answer.seq_no;
    reply.epoch = answer.epoch;
    reply.agreed = answer.seq_no == request.last_seq_no && (answer.seq_no == 0 || answer.epoch == request.last_epoch);
    // On the connection the records go on, so that the follower has the answer before the records
    // that follow it.
    send_pdu(target, pubsub_itc_fw_app::LogPositionReply::message_pdu_id, 0, reply);
    if (!reply.agreed) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the follower's log ends at record {} from epoch {}; it agrees with this one at most through record {} -- told to "
                   "discard the rest",
                   request.last_seq_no, request.last_epoch, answer.seq_no);
        return;
    }

    // The logs agree through the follower's last record. Send it every record after that, then live
    // records. Nothing is appended while this runs, because this thread is the one that appends, so
    // the live records follow without a gap.
    int64_t sent = 0;
    const int64_t after = request.last_seq_no;
    [[maybe_unused]] const auto end_position = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, wal_.scan_start_for(after + 1), [this, &target, after, &sent](int64_t record_id, const void* payload, size_t size) {
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (record_id <= after || size <= header_size) {
                return;
            }
            const auto* envelope = static_cast<const uint8_t*>(payload) + header_size;
            send_pdu_payload(target, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, envelope, size - header_size);
            ++sent;
        });
    peer_acked_through_ = after;
    follower_log_matches_ = true;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: the follower's log agrees with this one through record {} -- sent it the {} record(s) after that; live records follow", after,
               sent);
}

void SequencerThread::handle_log_position_reply(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::LogPositionReplyView reply{};
    if (!pubsub_itc_fw_app::decode(reply, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode LogPositionReply -- dropping");
        return;
    }
    if (role_ == pubsub_itc_fw_app::Role::leader) {
        return;
    }
    if (reply.ask_again) {
        follower_log_agreed_.store(false, std::memory_order_release);
        send_log_position_request();
        return;
    }
    if (follower_log_agreed_.load(std::memory_order_acquire)) {
        return;
    }

    // The inline handler is passing every record on while the logs are not known to agree, so this
    // thread is the only one touching the log here.
    int64_t last_before = 0;
    int64_t keep_through = 0;
    bool agreed = false;
    {
        const std::lock_guard<std::mutex> lock(log_epochs_mutex_);
        last_before = log_epochs_.last_seq_no();
        const LogEpochTable::FollowerStep step = log_epochs_.follower_step(LogEpochTable::PositionAnswer{reply.seq_no, reply.epoch});
        keep_through = step.keep_through;
        agreed = reply.agreed && step.agreed && keep_through == last_before;
        if (keep_through < last_before) {
            wal_.truncate_after(keep_through);
            log_epochs_.truncate_after(keep_through);
            // The records after keep_through are gone, and the leader sends others under the same
            // numbers, so the record of identifiers reads on from where the log now ends. Identifiers
            // of the discarded records stay in it; a command that matches one of them is checked
            // against the log exactly, and not found, so it is sequenced as new.
            identifiers_read_position_ = wal_.scan_start_for(keep_through + 1);
        }
    }
    if (keep_through < last_before) {
        highest_replicated_seq_no_.store(keep_through, std::memory_order_release);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: discarded records {} to {} from this instance's log -- the leader does not hold them. They were never acted on, "
                   "because the matching engine is sent an order only once both logs hold it",
                   keep_through + 1, last_before);
    }
    if (agreed) {
        follower_log_agreed_.store(true, std::memory_order_release);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: this log agrees with the leader's through record {} -- writing the records it sends from here", keep_through);
        return;
    }
    send_log_position_request();
}

void SequencerThread::note_replicated_record(int64_t seq_no) {
    int64_t highest = highest_replicated_seq_no_.load(std::memory_order_relaxed);
    while (seq_no > highest && !highest_replicated_seq_no_.compare_exchange_weak(highest, seq_no, std::memory_order_release, std::memory_order_relaxed)) {
        // compare_exchange_weak has reloaded highest; try again while seq_no is still the larger.
    }
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
    last_acknowledgement_at_ = std::chrono::steady_clock::now();

    // The orders this acknowledgement covers are now on both machines, so they may be acted on.
    release_held_orders_through(view.seq_no);

    // A follower that has acknowledged every record this leader has written has caught up.
    if (running_alone_ && peer_acked_through_ >= next_sequence_number_ - 1) {
        stop_running_alone("it has acknowledged every record this leader has written");
    }

    // Every report waiting on a record this acknowledgement covers may now go.
    const auto covered_end = pending_er_.upper_bound(peer_acked_through_);
    for (auto it = pending_er_.begin(); it != covered_end; ++it) {
        forward_pending_er(it->second);
    }
    pending_er_.erase(pending_er_.begin(), covered_end);
    say_follower_may_lead_if_it_holds_everything();
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
            // A record this handler does not write is passed to the sequencer thread, and counted,
            // so that this handler writes nothing until every record passed on has been written or
            // discarded there: records are then written in order, and by one thread at a time. While
            // this log and the leader's are not known to agree, every record is passed on, and the
            // sequencer thread discards it (docs/availability/follower_log_repair.md, 4.3).
            if (framer->has_pending_data() || !follower_log_agreed_.load(std::memory_order_acquire) ||
                replicated_records_queued_.load(std::memory_order_acquire) > 0) {
                replicated_records_queued_.fetch_add(1, std::memory_order_acq_rel);
                return false;
            }

            std::array<uint8_t, 4096> arena_buffer;
            pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::WalRecordView view{};

            if (!pubsub_itc_fw_app::decode(view, payload, size, bytes_consumed, arena, arena_bytes_needed)) {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread (inline): failed to decode WalRecord -- falling back to ITC");
                replicated_records_queued_.fetch_add(1, std::memory_order_acq_rel);
                return false;
            }

            // A record already held is not written twice; one that would leave a gap is passed to the
            // sequencer thread, which discards it, the logs no longer being known to agree.
            if (!replicated_record_is_next(view.seq_no)) {
                replicated_records_queued_.fetch_add(1, std::memory_order_acq_rel);
                return false;
            }

            // Option B: persist the received WalRecord bytes verbatim (record pdu_id =
            // WalRecord) so leader and follower WALs stay byte-identical.
            append_to_wal(view.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, payload, static_cast<int>(size), view.wall_time_ns,
                          view.has_leader_epoch ? view.leader_epoch : 0);
            note_replicated_record(view.seq_no);

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

void SequencerThread::hold_until_acknowledged(const pubsub_itc_fw_app::WalRecord& envelope, const pubsub_itc_fw::EventMessage& message) {
    // Once one order waits in the log, every later one does too, so that orders are sent in order.
    if (first_unheld_seq_ == 0 && held_orders_.push_back(HeldOrder{envelope, message.slab_id(), message.payload(), std::chrono::steady_clock::now()})) {
        return;
    }
    if (first_unheld_seq_ == 0) {
        first_unheld_seq_ = envelope.seq_no;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the storage for waiting orders is full -- orders from seq={} wait in the write-ahead log, and are sent to the matching "
                   "engine from there when they are released",
                   envelope.seq_no);
    }
    release_pdu_payload(message);
}

void SequencerThread::send_held_order_to_matching_engine(const HeldOrder& held) {
    if (!engine_routing_.active().is_valid()) {
        // Recovered by the catch-up whichever engine acts next performs before it acts, exactly as
        // an order deferred on arrival is.
        note_order_deferred(held.envelope.seq_no);
        return;
    }
    note_matching_engine_reachable();
    send_pdu(engine_routing_.active(), pubsub_itc_fw_app::WalRecord::message_pdu_id, held.envelope.seq_no, held.envelope,
             pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
    if (held.envelope.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        order_path_metrics::observe_checkpoint(order_out_elapsed_histogram_, held.envelope.has_gateway_ingress_ns, held.envelope.gateway_ingress_ns,
                                               config_.wall_clock->now_ns());
    }
}

void SequencerThread::release_held_orders_through(int64_t acknowledged_seq_no) {
    // A new leader sends nothing new until the engine has been sent the orders it lacks, or the
    // engine would act on a later order before an earlier one.
    if (awaiting_engine_position_) {
        pause_or_resume_order_reading();
        return;
    }
    while (!held_orders_.empty() && held_orders_.front().envelope.seq_no <= acknowledged_seq_no) {
        const HeldOrder& held = held_orders_.front();
        send_held_order_to_matching_engine(held);
        release_pdu_payload(held.slab_id, held.buffer);
        held_orders_.pop_front();
    }
    // Every held order is numbered below the first that waits in the log, so these follow them.
    if (first_unheld_seq_ != 0 && first_unheld_seq_ <= acknowledged_seq_no) {
        send_logged_orders(first_unheld_seq_, acknowledged_seq_no);
        first_unheld_seq_ = acknowledged_seq_no >= next_sequence_number_ - 1 ? 0 : acknowledged_seq_no + 1;
    }
    pause_or_resume_order_reading();
}

void SequencerThread::release_all_held_orders() {
    release_held_orders_through(next_sequence_number_ - 1);
}

bool SequencerThread::may_act_without_follower() const {
    return !config_.ha_enabled || !lease_agent_.has_value() || lease_agent_->may_act_without_peer();
}

void SequencerThread::say_follower_may_not_lead() {
    const auto now = std::chrono::steady_clock::now();
    if (!lease_agent_.has_value() || role_ != pubsub_itc_fw_app::Role::leader || !lease_agent_->acting(now) || !lease_agent_->says_peer_may_lead()) {
        return;
    }
    lease_agent_->begin_running_without_peer(now);
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: saying the follower may not lead -- orders it does not hold wait until a voter confirms that statement");
}

void SequencerThread::say_follower_may_lead_if_it_holds_everything() {
    const auto now = std::chrono::steady_clock::now();
    if (!lease_agent_.has_value() || role_ != pubsub_itc_fw_app::Role::leader || !lease_agent_->acting(now) || lease_agent_->says_peer_may_lead()) {
        return;
    }
    // In this order: the leader waits for the follower's acknowledgements again, nothing waits, and the
    // follower has acknowledged every record; only then is it true that the follower holds everything
    // the engine has acted on.
    if (!needs_wal_ack() || !held_orders_.empty() || first_unheld_seq_ != 0 || peer_acked_through_ < next_sequence_number_ - 1) {
        return;
    }
    lease_agent_->peer_holds_everything(now);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: the follower holds every record through {} -- saying it may lead again",
               peer_acked_through_);
}

void SequencerThread::release_if_confirmed() {
    if (role_ != pubsub_itc_fw_app::Role::leader || needs_wal_ack() || !may_act_without_follower()) {
        return;
    }
    if (!held_orders_.empty() || first_unheld_seq_ != 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: a voter has confirmed that the follower may not lead -- sending the {} held order(s){} to the matching engine",
                   held_orders_.size(), first_unheld_seq_ != 0 ? " and the orders waiting in the log" : "");
        release_all_held_orders();
    }
    if (!pending_er_.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: no follower keeps up -- forwarding {} buffered ERs to their gateways without waiting for it", pending_er_.size());
        forward_all_pending_er();
    }
    pause_or_resume_order_reading();
}

void SequencerThread::send_logged_orders(int64_t first, int64_t through) {
    const pubsub_itc_fw::ConnectionID engine = engine_routing_.active();
    int64_t sent = 0;
    [[maybe_unused]] const auto end_position = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, wal_.scan_start_for(first), [this, &engine, first, through, &sent](int64_t record_id, const void* payload, size_t size) {
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (record_id < first || record_id > through || size <= header_size) {
                return;
            }
            int64_t wall_time_ns{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
            int16_t pdu_id{};
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));
            const auto* record = static_cast<const uint8_t*>(payload) + header_size;
            if (engine.is_valid()) {
                if (stream_wal_record_to_me(engine, record_id, pdu_id, record, size - header_size, wall_time_ns)) {
                    ++sent;
                }
                return;
            }
            // No engine: an order is deferred, as one sent from the storage would be, and recovered
            // by the catch-up whichever engine acts next performs.
            auto& arena_buf = decode_arena_buffer();
            pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::WalRecordView view{};
            if (pdu_id == pubsub_itc_fw_app::WalRecord::message_pdu_id &&
                pubsub_itc_fw_app::decode(view, record, size - header_size, bytes_consumed, arena, arena_bytes_needed) &&
                (view.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) ||
                 view.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest))) {
                note_order_deferred(record_id);
            }
        });
    if (engine.is_valid()) {
        note_matching_engine_reachable();
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: sent {} order(s) numbered {} to {} to the matching engine from the log", sent,
               first, through);
}

void SequencerThread::reserve_record_of_identifiers() {
    if (!config_.ha_enabled) {
        return;
    }
    identifiers_growth_reporter_.report_threshold_bytes = 1;
    identifiers_growth_reporter_.on_large_allocation = [this](size_t bytes, size_t /*largest*/) { identifiers_table_bytes_ = bytes; };
    const auto started = std::chrono::steady_clock::now();
    logged_identifiers_.emplace(config_.identifiers_reserved, &identifiers_growth_reporter_);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: reserved the record of command identifiers for {} identifier(s): {} slots, {} MiB, in {} ms", config_.identifiers_reserved,
               logged_identifiers_->slot_count(), identifiers_table_bytes_ / (1024 * 1024), took.count());
}

void SequencerThread::note_logged_command(const pubsub_itc_fw_app::WalRecordView& view) {
    note_logged_command(view.has_sender_comp_id ? view.sender_comp_id : std::string_view{},
                        view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent, view.pdu_id,
                        view.has_cl_ord_id ? view.cl_ord_id : std::string_view{});
}

void SequencerThread::note_logged_command(std::string_view comp_id, int16_t protocol, int16_t inner_pdu_id, std::string_view cl_ord_id) {
    if (!logged_identifiers_.has_value() || cl_ord_id.empty() || comp_id.empty()) {
        return;
    }
    if (inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        return;
    }
    if (logged_identifiers_->add(LoggedCommandIdentifiers::identifier(comp_id, protocol, cl_ord_id)) == LoggedCommandIdentifiers::Added::full &&
        !identifiers_full_reported_) {
        identifiers_full_reported_ = true;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: the record of command identifiers is full at {} -- later commands are not recorded, so every command a gateway "
                   "sends again after a change of leader is checked against the log itself. Raise commands.identifiers_reserved",
                   logged_identifiers_->capacity());
    }
}

bool SequencerThread::command_already_logged(const pubsub_itc_fw_app::WalRecordView& inbound) {
    if (!logged_identifiers_.has_value() || !inbound.has_cl_ord_id || inbound.cl_ord_id.empty() || !inbound.has_sender_comp_id ||
        inbound.sender_comp_id.empty()) {
        // Without HA there is no change of leader, and so nothing is ever sent again; a command that
        // does not say whose it is cannot be looked for.
        return false;
    }
    const int16_t protocol = inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent;
    const uint64_t id = LoggedCommandIdentifiers::identifier(inbound.sender_comp_id, protocol, inbound.cl_ord_id);
    // Not in the record means certainly not in the log -- unless the record is full, when a command
    // logged after it filled would not be in it either.
    if (!logged_identifiers_->full() && !logged_identifiers_->may_hold(id)) {
        return false;
    }
    if (!log_tail_index_.has_value()) {
        log_tail_index_.emplace(config_.wal_directory);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: commands are being sent again after the change of leader -- reading the end of the log to check them exactly");
    }
    log_tail_index_used_at_ = std::chrono::steady_clock::now();
    const int64_t first_sent_no_earlier_than = inbound.has_gateway_ingress_ns ? inbound.gateway_ingress_ns : std::numeric_limits<int64_t>::min();
    return log_tail_index_->holds(inbound.sender_comp_id, protocol, inbound.cl_ord_id, first_sent_no_earlier_than);
}

void SequencerThread::discard_log_tail_index(const char* reason) {
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: commands sent again since the change of leader: {} already in this log and not sequenced twice, {} sequenced as new; "
               "{} log record(s) read to check them -- {}",
               sent_again_already_logged_, sent_again_sequenced_, log_tail_index_->records_read(), reason);
    log_tail_index_.reset();
    sent_again_already_logged_ = 0;
    sent_again_sequenced_ = 0;
}

void SequencerThread::read_identifiers_from_log() {
    identifiers_read_at_ = std::chrono::steady_clock::now();
    if (!logged_identifiers_.has_value()) {
        return;
    }
    // Only records whose checksum is complete are read, so a record the reactor thread is part way
    // through writing is not read until the next time. Records already noted are noted again
    // harmlessly.
    identifiers_read_position_ =
        pubsub_itc_fw::WalReader::replay(config_.wal_directory, identifiers_read_position_, [this](int64_t /*record_id*/, const void* payload, size_t size) {
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size <= header_size) {
                return;
            }
            int16_t pdu_id{};
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));
            if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
                return;
            }
            auto& arena_buf = decode_arena_buffer();
            pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
            size_t arena_bytes_needed = 0;
            size_t bytes_consumed = 0;
            pubsub_itc_fw_app::WalRecordView view{};
            if (pubsub_itc_fw_app::decode(view, static_cast<const uint8_t*>(payload) + header_size, size - header_size, bytes_consumed, arena,
                                          arena_bytes_needed)) {
                note_logged_command(view);
            }
        });
}

void SequencerThread::ask_engine_for_position() {
    const pubsub_itc_fw::ConnectionID engine = engine_routing_.active();
    if (!engine.is_valid()) {
        // No engine to ask. Whichever engine acts next catches up from this log before it acts, and
        // is sent everything after its own position, the orders asked about here included.
        if (awaiting_engine_position_) {
            stop_awaiting_engine_position("no matching engine is connected; whichever engine acts next catches up from this log");
        }
        return;
    }
    if (!awaiting_engine_position_) {
        awaiting_engine_position_ = true;
        held_at_takeover_ = next_sequence_number_ - 1;
        engine_position_asks_ = 0;
    }
    ++engine_position_request_id_;
    ++engine_position_asks_;
    engine_position_asked_at_ = std::chrono::steady_clock::now();
    pubsub_itc_fw_app::EnginePositionQuery query{};
    query.request_id = engine_position_request_id_;
    send_pdu(engine, pubsub_itc_fw_app::EnginePositionQuery::message_pdu_id, 0, query);
    // A few asks are routine: when the venue starts, the engine is still catching up as the first
    // leader takes the lead, and answers only once it has finished, a second or two later. Five asks
    // means it has not answered for four seconds while new orders wait, which is worth a Warning, once.
    PUBSUB_LOG(get_logger(),
               engine_position_asks_ == engine_position_asks_before_warning ? pubsub_itc_fw::FwLogLevel::Warning : pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: asking the matching engine on connection {} for the highest order it has acted on (ask {}, request {}) -- this log "
               "holds records through {}, and new orders wait until the engine has every one it lacks",
               engine.get_value(), engine_position_asks_, engine_position_request_id_, held_at_takeover_);
}

void SequencerThread::handle_engine_position(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::EnginePositionView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode EnginePosition -- dropping");
        return;
    }
    if (!awaiting_engine_position_ || role_ != pubsub_itc_fw_app::Role::leader || view.request_id != engine_position_request_id_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: EnginePosition for request {} not awaited (awaiting request {}) -- ignoring", view.request_id,
                   awaiting_engine_position_ ? engine_position_request_id_ : 0);
        return;
    }

    awaiting_engine_position_ = false;
    if (view.highest_applied < held_at_takeover_) {
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the matching engine has acted on orders through {} -- sending it the orders this log holds from {} to {}, which it "
                   "never received",
                   view.highest_applied, view.highest_applied + 1, held_at_takeover_);
        send_logged_orders(view.highest_applied + 1, held_at_takeover_);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the matching engine has acted on orders through {}, and this log holds nothing after that it has not been sent",
                   view.highest_applied);
    }
    release_orders_that_waited_for_the_engine();
}

void SequencerThread::stop_awaiting_engine_position(const char* reason) {
    awaiting_engine_position_ = false;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: no longer waiting for the matching engine's position -- {}", reason);
    release_orders_that_waited_for_the_engine();
}

void SequencerThread::release_orders_that_waited_for_the_engine() {
    // The orders that arrived meanwhile now go the way they would have gone: once the follower
    // acknowledges them, or once a voter has confirmed that the follower may not lead.
    if (needs_wal_ack()) {
        release_held_orders_through(peer_acked_through_);
    } else {
        release_if_confirmed();
    }
    pause_or_resume_order_reading();
}

int64_t SequencerThread::released_through() const {
    if (!held_orders_.empty()) {
        return held_orders_.front().envelope.seq_no - 1;
    }
    if (first_unheld_seq_ != 0) {
        return first_unheld_seq_ - 1;
    }
    return next_sequence_number_ - 1;
}

void SequencerThread::pause_or_resume_order_reading() {
    const bool waiting_for_confirmation =
        role_ == pubsub_itc_fw_app::Role::leader && ((!needs_wal_ack() && !may_act_without_follower()) || awaiting_engine_position_);
    if (!order_reading_paused_) {
        if (waiting_for_confirmation && (held_orders_.size() >= pause_order_reading_at || first_unheld_seq_ != 0)) {
            for (const pubsub_itc_fw::ConnectionID& id : order_connection_ids_) {
                pause_reading(id);
            }
            order_reading_paused_ = true;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: {} orders wait for a voter to confirm that the follower may not lead -- reading from the {} gateway connection(s) "
                       "is paused until they are released",
                       held_orders_.size(), order_connection_ids_.size());
        }
        return;
    }
    if (waiting_for_confirmation && (held_orders_.size() > resume_order_reading_at || first_unheld_seq_ != 0)) {
        return;
    }
    for (const pubsub_itc_fw::ConnectionID& id : order_connection_ids_) {
        resume_reading(id);
    }
    order_reading_paused_ = false;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: reading from the gateways resumed -- {} order(s) still held",
               held_orders_.size());
}

void SequencerThread::discard_held_orders() {
    // An instance that has stopped leading must not act on what it holds: whichever instance leads
    // now decides what the matching engine sees. The orders are in this instance's log and were
    // never acted on or answered.
    if (!held_orders_.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: stopped leading while holding {} order(s) the follower had not acknowledged -- they were not sent to the matching "
                   "engine and are discarded",
                   held_orders_.size());
    }
    while (!held_orders_.empty()) {
        release_pdu_payload(held_orders_.front().slab_id, held_orders_.front().buffer);
        held_orders_.pop_front();
    }
}

void SequencerThread::start_running_alone(const char* reason) {
    if (running_alone_) {
        return;
    }
    running_alone_ = true;
    running_alone_since_ = std::chrono::steady_clock::now();
    running_alone_gauge_.set(1.0);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "SequencerThread: the follower is too far behind -- {}. Running as if alone: once a voter confirms that the follower may not lead, "
               "orders go to the matching engine without waiting for it and reports are forwarded, so resilience is reduced until the follower "
               "catches up. {} order(s) held; the follower has acknowledged through {} of {}",
               reason, held_orders_.size(), peer_acked_through_, next_sequence_number_ - 1);
    say_follower_may_not_lead();
    release_if_confirmed();
}

void SequencerThread::stop_running_alone(const char* reason) {
    if (!running_alone_) {
        return;
    }
    running_alone_ = false;
    running_alone_gauge_.set(0.0);
    const auto alone_for = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - running_alone_since_);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: the follower is keeping up again -- {}. Waiting for its acknowledgements again after {} ms running as if alone", reason,
               alone_for.count());
}

void SequencerThread::check_follower_acknowledgements() {
    if (held_orders_.empty() || !needs_wal_ack()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto waiting_since = std::max(last_acknowledgement_at_, held_orders_.front().held_at);
    if (now - waiting_since >= follower_acknowledgement_timeout) {
        start_running_alone("no acknowledgement from it for at least 100 ms while orders were waiting");
    }
}

void SequencerThread::forward_all_pending_er() {
    for (auto& [seq_no, pending] : pending_er_) {
        forward_pending_er(pending);
    }
    pending_er_.clear();
}

void SequencerThread::flush_pending_er() {
    if (running_alone_) {
        stop_running_alone("it has disconnected, and the leader runs alone as it does with no follower");
    }
    // Held orders and buffered reports go once a voter confirms that the follower, now gone, may not
    // lead; with HA off they go at once.
    if (!config_.ha_enabled) {
        release_all_held_orders();
        forward_all_pending_er();
        return;
    }
    say_follower_may_not_lead();
    release_if_confirmed();
}

void SequencerThread::send_er_to_origin_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope,
                                                bool is_new_order_ack) {
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
    // A member is waiting for this: it is the report on its way back to them.
    send_pdu(*connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, envelope,
             pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});

    // The last checkpoint this component contributes, recorded here rather than at each of
    // the three call sites because this is the one place a report leaves for a gateway. Two
    // of those sites are the immediate path and the wait-for-follower path, and the third is
    // a report released long after the fact; a checkpoint written out at each would be three
    // chances to record a different thing.
    //
    // After the send, so that the stage it closes includes handing the report to the reactor.
    // Nothing is recorded when the report never left: the early returns above are a gateway
    // that is not connected, and a report that was dropped is not a stage that was fast.
    if (is_new_order_ack) {
        order_path_metrics::observe_checkpoint(er_out_elapsed_histogram_, envelope.has_gateway_ingress_ns, envelope.gateway_ingress_ns,
                                               config_.wall_clock->now_ns());
    }
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
               "SequencerThread: a matching engine is reachable again after {}s -- {} order(s) were deferred and are sent to it by the catch-up it performs "
               "before it acts",
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
    state.has_leader_epoch = true;
    state.leader_epoch = epoch_;
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
    envelope.has_cl_ord_id = !pending.cl_ord_id.empty();
    envelope.cl_ord_id = pending.cl_ord_id;
    envelope.poss_resend = pending.poss_resend;

    if (destination != nullptr) {
        send_er_to_origin_gateway(pending.identity.protocol, destination->instance, pending.seq_no, envelope, pending.is_new_order_ack);
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

    // The epoch is what makes a claim safe to believe: an instance whose leadership has been
    // superseded may still announce that it leads, and its epoch is behind the one already accepted.
    // The announcement arrives on the announcing instance's ER connection, which is not the socket
    // orders travel on; EngineOrderRouting maps the instance to this sequencer's own order connection.
    const int32_t accepted_epoch = engine_routing_.announced_epoch();
    switch (engine_routing_.announced(announcement.instance_id, announcement.current_role, announcement.epoch)) {
        case EngineOrderRouting::AnnouncementOutcome::RefusedAsBehind:
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: RoleAnnouncement from instance {} on connection {} quotes epoch {}, behind the {} already accepted -- refusing",
                       announcement.instance_id, conn_id.get_value(), announcement.epoch, accepted_epoch);
            break;
        case EngineOrderRouting::AnnouncementOutcome::NoOrderConnection:
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: matching engine instance {} announced role at epoch {} but this sequencer holds no order connection to it",
                       announcement.instance_id, announcement.epoch);
            break;
        case EngineOrderRouting::AnnouncementOutcome::RoutedToLeader:
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: matching engine instance {} leads at epoch {} -- orders now route to connection {}", announcement.instance_id,
                       announcement.epoch, engine_routing_.active().get_value());
            break;
        case EngineOrderRouting::AnnouncementOutcome::AlreadyRoutedToLeader:
        case EngineOrderRouting::AnnouncementOutcome::FollowerNotCarryingOrders:
            break;
        case EngineOrderRouting::AnnouncementOutcome::FollowerHandedToLeader:
            // Handed to the instance last known to lead rather than left empty: a leader announces
            // only on a change of role or a new connection, and neither follows a follower's
            // announcement, so an emptied slot would stay empty beside a healthy leader (BUG-0077).
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: matching engine instance {} on connection {} is a follower at epoch {} -- order routing handed to instance {} on "
                       "connection {}, which is the leader this sequencer knows of",
                       announcement.instance_id, conn_id.get_value(), announcement.epoch, engine_routing_.announced_leader(),
                       engine_routing_.active().get_value());
            break;
        case EngineOrderRouting::AnnouncementOutcome::FollowerWithdrawnNoLeader:
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: matching engine instance {} on connection {} is a follower at epoch {} -- withdrawn from order routing, and this "
                       "sequencer knows of no leader to hand it to. Orders are deferred until an engine leads or asks to catch up",
                       announcement.instance_id, conn_id.get_value(), announcement.epoch);
            break;
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
    const bool asking_to_lead = view.asking_to_lead;

    // Only the leader may serve WAL catch-up. The promoted ME asks every sequencer
    // it holds a pre-warmed order connection to (it cannot tell which one is the
    // leader); if a follower also streamed, the ME would replay the WAL twice and
    // process the second replay as live traffic once promoted. The follower instead
    // re-points its own ME order connection to the standby -- so that if it later
    // wins a sequencer election it forwards to the promoted ME -- and drops the
    // request without streaming or acking.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        engine_routing_.asked_to_catch_up_while_following(conn_id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: MePositionRequest on connection {} but I am follower -- re-pointed ME order connection, not serving catch-up",
                   conn_id.get_value());
        return;
    }

    // Only orders already released to the engine: one still waiting for the follower's
    // acknowledgement, or for a voter's confirmation, is sent live when it is released, and sending it
    // here as well would act on it early.
    const int64_t wal_head = released_through();
    me_catchup_conn_id_ = conn_id;

    // A negative position is an engine saying it holds nothing and has applied nothing of this
    // venue's record -- a process that found no region where a predecessor would have left one.
    // It is not behind by the whole log; it is new. Sending it the retained history would
    // rebuild a book out of orders it never held, and issue a report to a member for every one,
    // so it is placed at the head and told nothing.
    //
    // The cost is stated rather than hidden: an order this venue took and no engine ever
    // applied is not recovered by an instance that starts this way. Recovering it needs an
    // engine that can say where it had got to, which is what the region is for.
    if (last_seq_no < 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: MePositionRequest from connection {} carries no position -- the engine holds nothing and is placed at head={} without "
                   "catch-up. Any order taken and not yet applied is not recovered by this instance",
                   conn_id.get_value(), wal_head);
        pubsub_itc_fw_app::MePositionAck head_ack{};
        head_ack.last_seq_no = wal_head;
        head_ack.first_seq_no = wal_head;
        head_ack.records_sent = 0;
        send_pdu(conn_id, pubsub_itc_fw_app::MePositionAck::message_pdu_id, 0, head_ack);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: MePositionRequest from connection {} last_seq_no={} ({}) -- streaming WAL catch-up up to head={}", conn_id.get_value(),
               last_seq_no, asking_to_lead ? "asking to lead" : "starting, not asking to lead", wal_head);

    // Walk the WAL from last_seq_no+1 to the head, unwrapping each stored record
    // and streaming the underlying NOS/OCR PDU directly to the ME connection.
    size_t streamed = 0;
    // The earliest record still held. The log is truncated as it is consumed, so a walk from
    // zero starts wherever truncation left off rather than at the start of the day. An engine
    // that has lost its own record of what it held falls back to this one and has to be able
    // to tell those apart: see R-0123.
    int64_t earliest_retained = 0;
    [[maybe_unused]] auto end_pos = pubsub_itc_fw::WalReader::replay(
        config_.wal_directory, {0, 0},
        [this, &conn_id, last_seq_no, wal_head, &streamed, &earliest_retained](int64_t record_id, const void* payload, size_t size) {
            if (earliest_retained == 0) {
                earliest_retained = record_id;
            }
            if (record_id <= last_seq_no || record_id > wal_head) {
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
            if (stream_wal_record_to_me(conn_id, record_id, pdu_id, pdu_payload, pdu_size, wall_time_ns)) {
                ++streamed;
            }
        });

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL catch-up complete -- {} record(s) streamed to ME connection {}", streamed,
               conn_id.get_value());

    // Signal completion. On receipt the ME considers its book reconciled and becomes leader.
    pubsub_itc_fw_app::MePositionAck ack{};
    ack.last_seq_no = wal_head;
    ack.first_seq_no = earliest_retained;
    // What was sent, not what was walked past. The engine checks it received exactly this many
    // and will not act if it did not, which is R-0101.
    ack.records_sent = static_cast<int64_t>(streamed);
    send_pdu(conn_id, pubsub_itc_fw_app::MePositionAck::message_pdu_id, 0, ack);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: MePositionAck sent (last_seq_no={}, earliest retained={}, records_sent={}) -- ME is now live", wal_head, earliest_retained,
               streamed);

    // Promote this connection to the active ME order connection so subsequent sequenced orders
    // flow to the newly-promoted ME. If the request arrived on the standby connection (the normal
    // failover case), this swaps the active ME from the dead primary to the caught-up secondary.
    // The old standby slot is cleared; a future ME-primary restart would arrive on it again.
    //
    // Only for a promotion. An instance that is merely starting gets the records and no routing:
    // it does not yet know whether its peer leads, and it learns that from the peer over a
    // different channel and on its own schedule. Moving the order connection here on the strength
    // of the ask took the routing off a working leader in 400 milliseconds, and nothing put it
    // back -- measured by ha_test.py scenario 54, and it stopped the venue matching. The asker is
    // the only party that knows which case this is, which is why the request carries it rather
    // than this side inferring it. See BUG-0077.
    if (!asking_to_lead) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: connection {} caught up but is starting rather than being promoted -- order routing left where it is",
                   conn_id.get_value());
        me_catchup_conn_id_ = pubsub_itc_fw::ConnectionID{};
        return;
    }

    engine_routing_.caught_up_to_lead(conn_id);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ME order connection promoted to {} -- sequenced orders now route to the caught-up ME", conn_id.get_value());

    me_catchup_conn_id_ = pubsub_itc_fw::ConnectionID{};
}

bool SequencerThread::stream_wal_record_to_me(const pubsub_itc_fw::ConnectionID& conn_id, int64_t record_id, int16_t pdu_id, const uint8_t* pdu_payload,
                                              size_t pdu_size, int64_t wall_time_ns) {
    // Each stored WAL record is a WalRecord envelope (Option B). Decode it and
    // re-send the envelope to the ME for NOS/OCR (the ME unwraps it and reads
    // wall_time_ns as the sequencing time); ER envelopes are outputs, not inputs,
    // and are not streamed to the ME during catch-up.
    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- record seq={} is not a WalRecord (pdu_id={}) -- skipping",
                   record_id, pdu_id);
        return false;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, pdu_payload, pdu_size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- failed to decode envelope seq={} -- skipping", record_id);
        return false;
    }

    if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        return false;
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
    // The engine files an order under the session's comp id AND its gateway protocol, so the
    // protocol must travel too: without it a binary gateway's member is filed under the default
    // protocol, and the engine's reports and its check for a repeated ClOrdID name the wrong session.
    envelope.has_origin_gateway_id = view.has_origin_gateway_id;
    envelope.origin_gateway_id = view.origin_gateway_id;
    envelope.has_gateway_instance_id = view.has_gateway_instance_id;
    envelope.gateway_instance_id = view.gateway_instance_id;

    send_pdu(conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, envelope);
    (void)wall_time_ns;
    return true;
}

} // namespaces
