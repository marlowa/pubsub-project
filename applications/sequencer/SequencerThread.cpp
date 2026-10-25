// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include "PublisherRecordFromWal.hpp"

#include <cstring>
#include <limits>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <vector>

#include <LeasePairNumbers.hpp>
#include <OrderPathMetrics.hpp>
#include <lease_messages.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FileSystemUtils.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
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
                                 const SequencerConfiguration& config, pubsub_itc_fw::WalFollowingPositions& wal_positions)
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
    , wal_positions_(wal_positions) {}

void SequencerThread::on_initial_event() {
    // The log starts a helper thread to prepare its next segment, and a new thread inherits the
    // processor mask of whichever thread created it. This one is created from here, on the
    // application thread, by then pinned to a processor reserved for the order path -- so
    // without being told otherwise the helper does its file opening and memory mapping on the
    // processor this thread sequences every order on. Told before the log is opened, because
    // opening it is what starts the helper. See BUG-0093.
    pubsub_itc_fw::CpuLayout helper_layout;
    const auto [helper_layout_loaded, helper_layout_error] = helper_layout.load(config_.cpu_layout_file, config_.cpu_layout_component);
    if (!helper_layout_loaded) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: could not read the CPU layout, so the log's helper thread stays where it lands: {}", helper_layout_error);
    }

    if (config_.replay_mode) {
        // Replay mode: read every record of the WAL into replay_buffer_, and write nothing.
        // dispatch_replay_records() sends them to the ME once the ME connection is established.
        int64_t recovered_seq = 0;
        static_cast<void>(pubsub_itc_fw::WalReader::replay(
            config_.wal_directory, pubsub_itc_fw::WalPosition{0, 0}, [this, &recovered_seq](int64_t seq_no, const void* payload, size_t size) {
                const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
                if (!contents.has_value()) {
                    return;
                }
                ReplayRecord rec;
                rec.seq_no = seq_no;
                rec.pdu_id = contents->pdu_id;
                rec.wall_time_ns = contents->wall_time_ns;
                rec.payload.assign(contents->bytes, contents->bytes + contents->size);
                replay_buffer_.push_back(std::move(rec));
                recovered_seq = seq_no;
            }));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay mode -- WAL read complete: {} record(s), last seq_no={}",
                   replay_buffer_.size(), recovered_seq);
        // Start as leader so that send_pdu paths are active. Replay is an offline tool run against a
        // copy of the WAL, sharing the WAL directory with the live sequencer, so it records no epoch: it
        // uses epoch 1, and has no replicated WAL.
        on_role_changed(pubsub_itc_fw_app::Role::leader, current_epoch());
        return;
    }

    // Registered before the replicated WAL is made, because it is given the handles. Each is a no-op
    // when metrics are disabled, and the application and component tokens come from configuration.
    if (!config_.wal_append_buckets.empty()) {
        wal_append_histogram_ = get_reactor().metrics().register_histogram(
            "sequencer_thread", "wal_append_nanoseconds", "Nanoseconds spent committing one record to the write-ahead log", config_.wal_append_buckets);
    }
    running_alone_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_running_alone",
        "1 while a follower is connected but too far behind and the leader sends orders to the matching engine without waiting for it; a loss of "
        "resilience. Expected to be 0");

    // The WAL, kept on both instances of the pair, and the deciding of which instance leads. Opening it
    // reads every record, discards any after a gap, and recovers the epoch; with HA off, this instance
    // leads before open() returns. The routing map is intentionally not rebuilt from the WAL: by the
    // time the sequencer restarts, the ME has almost certainly already sent ERs for any in-flight orders
    // from the previous run, and those ERs will not be re-sent. ERs for unroutable seq_nos are handled
    // by the "not in routing map" fallback in on_framework_pdu_message().
    pubsub_itc_fw::ReplicatedWalConfiguration wal_configuration;
    wal_configuration.owner_name = "SequencerThread";
    wal_configuration.wal_directory = config_.wal_directory;
    wal_configuration.wal_segment_size = config_.wal_segment_size;
    if (helper_layout_loaded) {
        wal_configuration.helper_cores = helper_layout.background_cores();
        background_cores_ = helper_layout.background_cores();
    }
    wal_configuration.ha_enabled = config_.ha_enabled;
    wal_configuration.pair_id = fix_common::pair_id_of(pubsub_itc_fw_app::ComponentGroup::sequencer);
    wal_configuration.instance_id = static_cast<int64_t>(config_.instance_id);
    wal_configuration.third_voter_id = arbiter_pool_voter_id;
    wal_configuration.third_voter_name = "the arbiter";
    wal_configuration.lease = config_.lease;
    wal_configuration.wal_append_histogram = wal_append_histogram_;
    wal_configuration.running_alone_gauge = running_alone_gauge_;
    replicated_wal_.emplace(std::move(wal_configuration), get_logger(), replication_links_, lease_links_, replicated_wal_events_);
    // Only when a test started this instance (docs/availability/test_events.md).
    if (config_.test_events.enabled) {
        test_events_.emplace(test_event_links_, config_.test_events.component, static_cast<int64_t>(config_.instance_id), "SequencerThread");
        replicated_wal_->report_test_events_to(*test_events_);
    }
    // The reading thread reads only before the position published here, and sends a follower nothing
    // above the limit published here (docs/availability/following_the_wal.md, section 3.2).
    replicated_wal_->publish_positions_to(wal_positions_);
    handed_over_publishers_.emplace(replicated_wal_->wal(), config_.wal_directory, make_publisher_converter(64 * 1024), publisher_links_,
                                    pubsub_itc_fw::WalKeptRecordsCapacity{config_.records_kept_for_publishers, config_.bytes_kept_for_publishers});
    if (config_.ha_enabled) {
        wal_follower_service_.emplace(get_logger(), replicated_wal_->wal(), wal_positions_, wal_following_, wal_following_);
        replicated_wal_->use_peer_catch_up(wal_following_);
    }
    const int64_t recovered_seq = replicated_wal_->open(std::chrono::steady_clock::now());
    start_building_record_of_identifiers();
    if (recovered_seq > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WAL open complete: recovered seq_no={}, record_count={}, next_sequence_number={}", recovered_seq,
                   replicated_wal_->wal().record_count(), replicated_wal_->next_seq_no());
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL is fresh (no prior records), starting from seq_no=1");
    }

    wal_snapshot_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.snapshot_interval_seconds));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL snapshot timer started (interval={}s)", config_.snapshot_interval_seconds);

    if (!config_.ha_enabled) {
        // TEST CONTRACT -- ha_test.py matches this text (scenario 47), because what it checks is that an operator is told why both instances lead.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: ha_enabled=false -- starting as leader immediately");
    } else {
        lease_tick_timer_id_ = start_recurring_timer(pubsub_itc_fw::LeaseTiming::tick_interval);
        acknowledgement_watch_timer_id_ = start_recurring_timer(pubsub_itc_fw::ReplicatedWal::acknowledgement_watch_interval);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: ha_enabled=true -- leading only while the peer or the arbiter grants a lease (period={} ms); nothing is asked for "
                   "during the first period",
                   config_.lease.period.count());
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
    damaged_record_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_wal_damaged_record",
        "The number of a record in the write-ahead log that could not be read when a follower was being caught up, so that the follower "
        "was not caught up past it and does not act; 0 when there is none. Expected to be 0");
    kept_reports_lost_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_kept_reports_lost",
        "Reports from the matching engine, kept in case this instance takes the lead, that were overwritten while still needed because the store "
        "was full. Expected to be 0");
    reports_dropped_unbound_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_reports_dropped_unbound",
        "Execution reports dropped because the member's session was connected to no gateway. A member that is away has its reports dropped "
        "and sent when it logs on again, so this is not always 0; a report dropped for a member that is connected means the member was "
        "never told");
    held_reports_dropped_gauge_ = get_reactor().metrics().register_gauge(
        "sequencer_thread", "sequencer_held_reports_dropped_full",
        "Execution reports held until the gateways announced their sessions that were dropped because the room for them was full. "
        "Expected to be 0");
}

void SequencerThread::on_app_ready_event() {
    if (config_.replay_mode) {
        // Replay mode: connect only to the matching engine; skip gateway, HA
        // arbiters, and peer replication.
        connect_to_service("matching_engine");
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: replay mode -- connecting to matching engine only");
        return;
    }

    if (test_events_.has_value()) {
        connect_to_service(std::string(pubsub_itc_fw::TestEventChannel::service_name));
    }
    for (const auto& endpoint : config_.gateway_endpoints) {
        connect_to_service(endpoint.service_name());
        gateway_announcements_[gateway_key(endpoint.protocol, endpoint.instance)] = GatewayAnnouncement{};
        reports_held_for_gateways_.try_emplace(gateway_key(endpoint.protocol, endpoint.instance), held_report_capacity_bytes, held_report_capacity_reports);
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
    if (test_events_.has_value() && test_events_->on_connection_established(id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: connection {} to the test's event listener established", id.get_value());
        // A change of role made before the connection was is reported now, so that the test learns the role whenever
        // it connects.
        replicated_wal_->report_role_to_test_events();
        return;
    }
    const std::string& svc = id.service_name();
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    const auto endpoint = std::find_if(config_.gateway_endpoints.begin(), config_.gateway_endpoints.end(),
                                       [&svc](const auto& candidate) { return candidate.service_name() == svc; });
    if (endpoint != config_.gateway_endpoints.end()) {
        gateway_conn_ids_[gateway_key(endpoint->protocol, endpoint->instance)] = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway protocol={} instance={} connection {} established",
                   endpoint->protocol, endpoint->instance, id.get_value());
        send_reports_held_for_gateway(endpoint->protocol, endpoint->instance, id);
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
        send_status_query(id);
        if (replicated_wal_.has_value()) {
            replicated_wal_->on_peer_connection_established(std::chrono::steady_clock::now());
        }
    } else if (svc == peer_inbound_svc) {
        peer_inbound_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound peer connection {} established -- sending StatusQuery",
                   id.get_value());
        send_status_query(id);
        if (replicated_wal_.has_value()) {
            replicated_wal_->on_peer_connection_established(std::chrono::steady_clock::now());
        }
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: publisher connection {} established -- awaiting WalFollowRequest",
                   id.get_value());
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
    if (test_events_.has_value() && test_events_->on_connection_lost(id, reason)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the connection to the test's event listener is lost ({}) -- no more test events are sent", reason);
        return;
    }
    forget_command_connection(id);
    // A gateway whose link was lost may hold sessions this instance has not been told of by the time it comes up again.
    for (auto& [gateway, announcement] : gateway_announcements_) {
        if (announcement.announced && announcement.link == id) {
            announcement.announced = false;
            announcement.link = pubsub_itc_fw::ConnectionID{};
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: gateway id {} lost the link it announced its sessions on -- its sessions are not known until it announces again",
                       gateway);
        }
    }
    const auto lost_gateway = std::find_if(gateway_conn_ids_.begin(), gateway_conn_ids_.end(), [&id](const auto& entry) { return entry.second == id; });
    if (lost_gateway != gateway_conn_ids_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway id {} connection {} lost: {}", lost_gateway->first,
                   id.get_value(), reason);
        gateway_conn_ids_.erase(lost_gateway);
    } else if (engine_routing_.is_engine_connection(id)) {
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_connection_lost(id);
        }
        engine_servings_.erase(id.get_value());
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
        if (replicated_wal_.has_value()) {
            replicated_wal_->on_peer_connection_lost(std::chrono::steady_clock::now());
        }
    } else if (id == peer_inbound_conn_id_) {
        peer_inbound_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: inbound peer connection {} lost: {}", id.get_value(), reason);
        if (replicated_wal_.has_value()) {
            replicated_wal_->on_peer_connection_lost(std::chrono::steady_clock::now());
        }
    } else if (const auto order_connection = std::find(order_connection_ids_.begin(), order_connection_ids_.end(), id);
               order_connection != order_connection_ids_.end()) {
        order_connection_ids_.erase(order_connection);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway order connection {} lost: {}", id.get_value(), reason);
    } else if (is_publisher_connection(id)) {
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_connection_lost(id);
        }
        handed_over_publishers_->remove(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: publisher connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
}

void SequencerThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const std::string& svc = conn_id.service_name();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "TRACE on_framework_pdu_message: msg.connection_id value={} service_name=[{}]",
               conn_id.get_value(), svc);

    if (test_events_.has_value() && test_events_->on_framework_pdu_message(message)) {
        release_pdu_payload(message);
        return;
    }

    // Peer PDUs arrive on the outbound peer connection or from the inbound peer listener.

    if (conn_id == peer_conn_id_ || conn_id == peer_inbound_conn_id_) {
        handle_peer_pdu(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    // Arbiter PDUs: the active arbiter's answers to this sequencer's lease requests.
    if (conn_id == arbiter_primary_conn_id_ || conn_id == arbiter_secondary_conn_id_) {
        const auto pdu_id = static_cast<int16_t>(message.pdu_id());
        const bool lease_answer = pdu_id == pubsub_itc_fw::LeaseGrant::message_pdu_id || pdu_id == pubsub_itc_fw::LeaseRefusal::message_pdu_id;
        if (lease_answer && replicated_wal_.has_value()) {
            static_cast<void>(replicated_wal_->handle_message(conn_id, pdu_id, message.payload(), static_cast<size_t>(message.payload_size()),
                                                              std::chrono::steady_clock::now()));
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
    if (message.pdu_id() == pubsub_itc_fw::WalFollowRequest::message_pdu_id && engine_routing_.is_engine_connection(conn_id)) {
        handle_wal_follow_request(conn_id, message);
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

    // The matching engine publishers' requests to follow the WAL.
    if (svc == wal_subscriber_inbound_svc_) {
        if (message.pdu_id() == pubsub_itc_fw::WalFollowRequest::message_pdu_id) {
            handle_publisher_follow_request(conn_id, message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: unexpected PDU {} on publisher connection {} -- dropping",
                       message.pdu_id(), conn_id.get_value());
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
    if (message.pdu_id() == pubsub_itc_fw_app::SessionsAnnounced::message_pdu_id) {
        handle_sessions_announced(conn_id, message);
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
    if (message.pdu_id() == pubsub_itc_fw_app::UndeliveredReportsRequest::message_pdu_id) {
        handle_undelivered_reports_request(message);
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

        // Noted whatever this instance's role, because a follower can be promoted while a gateway still
        // has two connections open here.
        const bool gateway_has_two_connections = note_command_connection(message.connection_id(), inbound);

        // A follower writes its log only from its leader's records, so that the two logs stay
        // identical, and numbers nothing itself. So does an instance that has not yet learnt its role:
        // it may already be receiving its leader's records, and a record it numbered itself would
        // take a place in its log that the leader's record of that number needs (BUG-0147). If it
        // turns out to lead, the gateways send again every command still unanswered when it announces
        // its epoch. It discards the gateway's copy here, before a
        // number is taken: counting these copies in the numbering is what left a newly
        // promoted follower numbering below records its log already held (BUG-0105).
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: order envelope on connection {} inner_pdu_id={} -- {}, discarding the gateway's copy; the log is "
                       "written from the leader's records",
                       message.connection_id().get_value(), inner_pdu_id, pubsub_itc_fw_app::to_string(role_));
            release_pdu_payload(message);
            return;
        }

        // A command a gateway sends again after a change of leader, or after its connection to this
        // sequencer was closed and opened again, because nothing had answered it. If this log already
        // holds it, it must not be sequenced twice: its answer comes from the engine, by one of the
        // routes in docs/availability/commands_during_a_change_of_leader.md, section 3.6. If not,
        // nothing ever acted on it, and it is sequenced as a new command.
        //
        // An unmarked command is checked too while its gateway has two connections open here: it may
        // be an original read from the old connection after its marked copy came in on the new one.
        const bool sent_again = inbound.has_sent_again && inbound.sent_again;
        if (role_ == pubsub_itc_fw_app::Role::leader && (sent_again || gateway_has_two_connections) && command_already_logged(inbound)) {
            if (sent_again) {
                ++sent_again_already_logged_;
            } else {
                ++command_connections_[command_connection_gateway_[message.connection_id().get_value()]].already_logged;
            }
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                       "SequencerThread: command ClOrdID={} from comp_id='{}' {} -- this log already holds it, not sequencing it twice", inbound.cl_ord_id,
                       inbound.sender_comp_id, sent_again ? "sent again" : "arrived while its gateway had two connections open");
            release_pdu_payload(message);
            return;
        }
        if (inbound.has_sent_again && inbound.sent_again) {
            ++sent_again_sequenced_;
        }

        const int64_t seq = replicated_wal_->next_seq_no();
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
        envelope.leader_epoch = current_epoch();
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
        stamp_reports_forwarded_through(envelope);

        // The time the gateway read this order off the client connection travels on to the
        // matching engine as well as being remembered above. The sequencer does not need it
        // there -- it keeps its own copy for the report coming back -- but the matching
        // engine cannot time its own part of the journey without it, because every timing on
        // the path is counted from this one moment. See OrderPathMetrics.hpp.
        envelope.has_gateway_ingress_ns = inbound.has_gateway_ingress_ns;
        envelope.gateway_ingress_ns = inbound.gateway_ingress_ns;

        // WAL commit: the leader appends from the direct gateway PDU. Every other instance has
        // already returned above.
        append_envelope_to_wal(envelope);
        note_logged_command(envelope.has_sender_comp_id ? envelope.sender_comp_id : std::string_view{},
                            envelope.has_origin_gateway_id ? envelope.origin_gateway_id : gateway_ids::default_when_absent, inner_pdu_id,
                            envelope.has_cl_ord_id ? envelope.cl_ord_id : std::string_view{});
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: order envelope on connection {} inner_pdu_id={} seq={} -- WAL append ok (wal_size={}) role={}",
                   message.connection_id().get_value(), inner_pdu_id, seq, replicated_wal_->wal().record_count(), pubsub_itc_fw_app::to_string(role_));

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

        // The order goes to the matching engine once its record is committed: once the follower has
        // acknowledged it, with a follower connected and keeping up, so that the engine never acts on an
        // order the follower does not hold (docs/availability/change_of_sequencer_leader.md, part 4.2); or,
        // with no follower keeping up, once a voter has confirmed that the follower may not lead (rule 11,
        // docs/availability/a_follower_behind_does_not_lead.md); or at once with HA off. It never goes
        // ahead of an order already waiting. Until then it waits, in the storage for held orders, or in
        // the log once that is full, in which case the follower is too far behind and the leader runs as
        // if alone.
        if (replicated_wal_->waits_for_follower() && (held_orders_.full() || first_unheld_seq_ != 0)) {
            replicated_wal_->follower_too_far_behind("its storage for orders waiting on the follower is full", std::chrono::steady_clock::now());
        }

        hold_until_acknowledged(envelope, message);
        release_held_orders_through(replicated_wal_->committed_through());
        return; // The payload is released when the held order is sent on, or at once if it waits in the log.

    } else if (is_er_pdu) {
        if (!replicated_wal_.has_value()) {
            // Replay mode: the matching engine's reports to the replayed orders are not recorded, because
            // replay writes nothing to the WAL it reads.
            release_pdu_payload(message);
            return;
        }
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

    // Where the report stands in the engine's reports, when the engine says.
    std::optional<EngineReportPosition> position;
    if (inbound.has_report_engine_epoch && inbound.has_report_number) {
        position = EngineReportPosition{inbound.report_engine_epoch, inbound.report_number};
    }

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
                // The session is not connected anywhere, so the report has nowhere to go. It is
                // counted as dropped where it would have been sent, below, or when it is taken
                // from pending_er_; by then the session may be connected again.
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
    const int64_t er_wal_seq = replicated_wal_->next_seq_no();
    const int64_t er_wall_time_ns = config_.wall_clock->now_ns();

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = er_wal_seq;
    envelope.pdu_id = inbound.pdu_id;
    envelope.payload = inbound.payload;
    envelope.wall_time_ns = er_wall_time_ns;
    envelope.has_leader_epoch = true;
    envelope.leader_epoch = current_epoch();
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
    envelope.has_report_engine_epoch = position.has_value();
    envelope.report_engine_epoch = position.has_value() ? position->epoch : 0;
    envelope.has_report_number = position.has_value();
    envelope.report_number = position.has_value() ? position->number : 0;
    // The command the report answers, which its transport header carried; zero for a report no command produced.
    envelope.has_answers_seq_no = er_seq_no > 0;
    envelope.answers_seq_no = er_seq_no;
    stamp_reports_forwarded_through(envelope);

    append_envelope_to_wal(envelope);

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

    // The report as it would wait, for the follower or for its session to be announced.
    auto as_pending = [&]() {
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
        pending.position = position;
        pending.log_seq_no = er_wal_seq;
        return pending;
    };
    // Sent now, unless its session is not known: then it is held until the session is announced, or dropped.
    auto send_now = [&]() {
        if (!has_routing_conn && !routing_identity.empty()) {
            if (hold_report_if_sessions_unknown(as_pending())) {
                if (erase_routing_entry) {
                    seq_no_to_session_.erase(er_seq_no);
                }
                return;
            }
            note_report_dropped_unbound(routing_identity, er_seq_no);
        }
        send_er_to_origin_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope, is_new_order_ack);
        note_report_forwarded(routing_identity);
        if (erase_routing_entry) {
            seq_no_to_session_.erase(er_seq_no);
        }
    };

    // With no follower keeping up, a report is forwarded at once, as it always has been; otherwise once
    // the record it depends on is committed.
    if (!replicated_wal_->waits_for_follower() || gate_seq_no <= replicated_wal_->committed_through()) {
        send_now();
    } else {
        // WalAck not yet received; buffer the inner (unwrapped) ER until it arrives.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq={} buffered -- awaiting WalAck seq={} from follower", er_seq_no,
                   gate_seq_no);
        pending_er_.emplace(gate_seq_no, as_pending());
    }

    // Handled, whether forwarded, waiting, or dropped: reports_forwarded_through() accounts for the
    // ones still waiting.
    if (position.has_value() && (!latest_report_seen_.has_value() || position->is_after(*latest_report_seen_))) {
        latest_report_seen_ = position;
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
    // Where the report stands in the engine's reports, so that it can be discarded once the leader
    // has said it forwarded it.
    std::optional<EngineReportPosition> position;
    {
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::WalRecordView view{};
        if (pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed) &&
            view.has_report_engine_epoch && view.has_report_number) {
            position = EngineReportPosition{view.report_engine_epoch, view.report_number};
        }
    }
    kept_reports_.keep(message.seq_no(), message.payload(), static_cast<size_t>(message.payload_size()), std::chrono::steady_clock::now(), needed_from,
                       position);

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
    // on_role_changed, just before this on taking the lead, discarded what the old leader said it
    // forwarded; this says how many.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: taking the lead -- {} kept report(s) discarded because the old leader said it had forwarded them",
               kept_reports_discarded_as_forwarded_);
    const size_t count = kept_reports_.count();
    const size_t bytes = kept_reports_.bytes_used();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: taking the lead -- forwarding {} kept report(s) ({} bytes), each marked as a possible repeat", count, bytes);
    report(pubsub_itc_fw_app::KeptReportsForwarded{static_cast<int64_t>(kept_reports_discarded_as_forwarded_), static_cast<int64_t>(count),
                                                   static_cast<int64_t>(bytes)});
    kept_reports_discarded_as_forwarded_ = 0;
    kept_reports_.take_all([this](int64_t er_seq_no, const uint8_t* report, size_t size) {
        forward_report_from_engine(report, size, er_seq_no, ReportSource::kept_while_not_leading);
    });
}

void SequencerThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == wal_snapshot_timer_id_) {
        // Published on the snapshot timer rather than per append: both change at most once per
        // segment, which is seconds apart, so there is nothing to gain from the order path.
        if (!replicated_wal_.has_value()) {
            return;
        }
        const pubsub_itc_fw::Wal& wal = replicated_wal_->wal();
        wal_segments_filled_inline_gauge_.set(static_cast<double>(wal.segments_filled_inline()));
        wal_segments_waited_for_gauge_.set(static_cast<double>(wal.segments_waited_for()));

        try {
            replicated_wal_->take_snapshot();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: WAL snapshot taken: last_seq_no={}, record_count={}, committed through {}", wal.last_seq_no(), wal.record_count(),
                       replicated_wal_->committed_through());

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
            if (wal.record_count() > wal_growth_warning_records) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "SequencerThread: the WAL holds {} records and nothing is reclaiming it -- retention cannot be anchored until "
                           "every component publishes the position it may ask from (docs/bug_list.md BUG-0048)",
                           wal.record_count());
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: WAL snapshot failed: {}", ex.what());
        }
        return;
    }

    if (id == lease_tick_timer_id_) {
        replicated_wal_->on_lease_tick(std::chrono::steady_clock::now());
        return;
    }

    if (id == acknowledgement_watch_timer_id_) {
        const auto now = std::chrono::steady_clock::now();
        replicated_wal_->on_acknowledgement_watch(now);
        if (awaiting_engine_position_ && now - engine_position_asked_at_ >= engine_position_ask_interval) {
            ask_engine_for_position();
        }
        if (role_ != pubsub_itc_fw_app::Role::leader && now - kept_reports_discarded_at_ >= kept_reports_discard_interval) {
            kept_reports_discarded_at_ = now;
            discard_kept_reports_the_leader_forwarded();
        }
        take_record_of_identifiers_if_built();
        if (log_tail_index_.has_value() && now - log_tail_index_used_at_ >= log_tail_index_idle_limit) {
            discard_log_tail_index("no command has been sent again for a minute");
        }
        return;
    }
}

void SequencerThread::on_itc_message(const pubsub_itc_fw::EventMessage& message) {
    // The only messages between threads this thread receives are the reading thread's reports. The last
    // order released is published first, so that a matching engine is handed over only if the position
    // the reading thread reached is the limit as it is now, never as it was.
    publish_released_position();
    if (wal_follower_service_.has_value() && wal_follower_service_->on_inter_thread_message(message)) {
        return;
    }
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: an inter-thread message that is not a report from the reading thread -- ignored");
}

// Following the WAL (docs/availability/following_the_wal.md)

void SequencerThread::SequencerWalFollowing::start_peer_catch_up(int64_t after_seq_no) {
    // On the connection the replicated WAL sends its records on, so that the records sent by the reading
    // thread and the new records sent from here after the hand-over travel on one connection, in order.
    catch_up_connection_ = owner_.peer_active_conn();
    if (!catch_up_connection_.is_valid() || !owner_.wal_follower_service_.has_value()) {
        return;
    }
    owner_.wal_follower_service_->serve(catch_up_connection_, after_seq_no, pubsub_itc_fw::FollowerLimit::LastWritten, peer_converter,
                                        pubsub_itc_fw::CaughtUpMessage::DoNotSend);
}

void SequencerThread::SequencerWalFollowing::stop_peer_catch_up() {
    if (!owner_.wal_follower_service_.has_value()) {
        return;
    }
    // Only the follower sequencer's serving stops, with the reading thread's confirmation, so that
    // anything sent to it afterwards follows every record the reading thread sent it. A matching engine
    // being served goes on being served. Everything stops when this instance stops leading
    // (on_role_changed), which is what keeps a WAL about to be cut back from being read.
    if (catch_up_connection_.is_valid()) {
        owner_.wal_follower_service_->stop_serving(catch_up_connection_);
        catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
    }
    owner_.end_pause_for_hand_over();
}

bool SequencerThread::SequencerWalFollowing::reading_has_stopped() const {
    return !owner_.wal_follower_service_.has_value() || owner_.wal_follower_service_->reading_has_stopped();
}

void SequencerThread::SequencerWalFollowing::send_to_reading_thread(std::unique_ptr<pubsub_itc_fw::WalReadingCommand> command) {
    owner_.post_message(wal_reading_thread_id, pubsub_itc_fw::carry(owner_.get_thread_id(), std::move(command)));
}

void SequencerThread::SequencerWalFollowing::send_declined(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowDeclined& declined) {
    owner_.send_pdu(to, pubsub_itc_fw::WalFollowDeclined::message_pdu_id, 0, declined);
}

void SequencerThread::SequencerWalFollowing::send_caught_up(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowCaughtUp& caught_up) {
    owner_.send_pdu(to, pubsub_itc_fw::WalFollowCaughtUp::message_pdu_id, 0, caught_up);
}

void SequencerThread::SequencerWalFollowing::send_leader_changed(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalLeaderChanged& changed) {
    owner_.send_pdu(to, pubsub_itc_fw::WalLeaderChanged::message_pdu_id, 0, changed);
}

void SequencerThread::SequencerWalFollowing::follower_handed_over(const pubsub_itc_fw::ConnectionID& follower, int64_t through, int64_t records_sent,
                                                                  pubsub_itc_fw::WalPosition resume_at) {
    if (owner_.is_publisher_connection(follower)) {
        // Told it has caught up already, by the service; from here it is sent each record as it is committed,
        // read on from where the reading thread stopped.
        owner_.note_handed_over_through(through);
        owner_.handed_over_publishers_->add(follower, through, resume_at);
        PUBSUB_LOG(owner_.get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the matching engine publisher on connection {} holds every record through {} ({} sent by the reading thread) -- "
                   "each record from here is sent as it is committed",
                   follower.get_value(), through, records_sent);
        return;
    }
    if (follower == catch_up_connection_ && owner_.replicated_wal_.has_value()) {
        catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
        // The replicated WAL sends the follower sequencer its records from here; the service forgets it,
        // so that it is never told of a change of leader meant for the followers that ask.
        owner_.wal_follower_service_->on_connection_lost(follower);
        owner_.note_handed_over_through(through);
        owner_.replicated_wal_->on_peer_caught_up(through, std::chrono::steady_clock::now());
        owner_.end_pause_for_hand_over();
        return;
    }
    const auto engine = owner_.engine_servings_.find(follower.get_value());
    if (engine != owner_.engine_servings_.end()) {
        const EngineServing serving = engine->second;
        owner_.engine_servings_.erase(engine);
        owner_.engine_handed_over(follower, through, records_sent, serving);
        owner_.end_pause_for_hand_over();
    }
}

void SequencerThread::SequencerWalFollowing::hand_over_falling_behind(const pubsub_itc_fw::ConnectionID& /*follower*/) {
    owner_.pause_order_reading_for_hand_over();
}

void SequencerThread::SequencerWalFollowing::follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no) {
    std::string which_follower = "the matching engine";
    if (follower == catch_up_connection_) {
        catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
        which_follower = "the follower sequencer";
    } else if (owner_.is_publisher_connection(follower)) {
        which_follower = "a matching engine publisher";
    } else {
        owner_.engine_servings_.erase(follower.get_value());
    }
    owner_.report_follower_cannot_be_served(follower, damaged_seq_no, which_follower);
    owner_.end_pause_for_hand_over();
}

void SequencerThread::SequencerWalFollowing::declined_while_not_leading(const pubsub_itc_fw::ConnectionID& /*follower*/) {}

void SequencerThread::SequencerWalFollowing::reading_stopped() {
    // The follower sequencer's request, if one waited for the reading thread to stop, is answered now.
    if (owner_.replicated_wal_.has_value()) {
        owner_.replicated_wal_->on_peer_catch_up_stopped(std::chrono::steady_clock::now());
    }
}

void SequencerThread::report_follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no, const std::string& which_follower) {
    damaged_record_standing_ = damaged_seq_no;
    damaged_record_gauge_.set(static_cast<double>(damaged_seq_no));
    const bool first_time = damaged_records_reported_.insert(which_follower + " " + std::to_string(damaged_seq_no)).second;
    PUBSUB_LOG(get_logger(), first_time ? pubsub_itc_fw::FwLogLevel::Error : pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: record {} in the write-ahead log cannot be read, so {} on connection {} is not caught up past it and is not handed "
               "over -- it does not act on what it holds until a person deals with the record{}",
               damaged_seq_no, which_follower, follower.get_value(), first_time ? "" : " (asked again, and refused again)");
}

void SequencerThread::note_handed_over_through(int64_t through) {
    if (damaged_record_standing_ != 0 && through >= damaged_record_standing_) {
        damaged_record_standing_ = 0;
        damaged_record_gauge_.set(0.0);
    }
}

void SequencerThread::pause_order_reading_for_hand_over() {
    if (paused_for_hand_over_) {
        return;
    }
    paused_for_hand_over_ = true;
    for (const pubsub_itc_fw::ConnectionID& id : order_connection_ids_) {
        pause_reading(id);
    }
    order_reading_paused_ = true;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "SequencerThread: reading from the {} gateway connection(s) is paused until the follower the reading thread is catching up is handed "
               "over to new records -- orders were sequenced faster than one round between this thread and the reading thread",
               order_connection_ids_.size());
}

void SequencerThread::end_pause_for_hand_over() {
    if (!paused_for_hand_over_) {
        return;
    }
    paused_for_hand_over_ = false;
    // Resumed unless something else still needs reading paused.
    pause_or_resume_order_reading();
}

// Leader-follower state machine helpers

pubsub_itc_fw::ConnectionID SequencerThread::peer_active_conn() const {
    if (peer_conn_id_.is_valid()) {
        return peer_conn_id_;
    }
    return peer_inbound_conn_id_;
}

void SequencerThread::on_role_changed(pubsub_itc_fw_app::Role new_role, int32_t epoch) {
    if (new_role == role_) {
        return;
    }

    const auto transition_level = (role_ == pubsub_itc_fw_app::Role::unknown) ? pubsub_itc_fw::FwLogLevel::Info : pubsub_itc_fw::FwLogLevel::Warning;
    PUBSUB_LOG(get_logger(), transition_level, "SequencerThread: role transition {} -> {} (epoch={})", pubsub_itc_fw_app::to_string(role_),
               pubsub_itc_fw_app::to_string(new_role), epoch);

    const bool stops_leading = role_ == pubsub_itc_fw_app::Role::leader;
    role_ = new_role;

    // Only the leader serves followers. Every one being served stops, with the reading thread's
    // confirmation, so that a WAL about to be cut back is not being read; each asks again of whichever
    // instance leads next.
    if (stops_leading && wal_follower_service_.has_value()) {
        wal_follower_service_->stop_all();
        engine_servings_.clear();
        end_pause_for_hand_over();
    }
    // A publisher handed over is sent nothing more from here either; it asks again when told of the new leader.
    if (stops_leading && handed_over_publishers_.has_value()) {
        handed_over_publishers_->remove_all();
    }
    // Reports held for sessions not yet announced are the new leader's to send: it forwards what it kept while following.
    if (stops_leading) {
        held_reports_.discard_all();
        reports_held_since_sessions_known_ = false;
        // Likewise the reports held for a gateway with no connection: they are in the WAL, and the new leader sends them.
        for (auto& [gateway, held] : reports_held_for_gateways_) {
            held.discard_all();
        }
        gateways_holding_reports_.clear();
    }

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        // Every record this instance wrote while it followed was noted as it was written, so the record
        // of identifiers already holds every command in the log. What the old leader said it forwarded
        // is acted on now, so that the kept reports forwarded below are only those it may not have.
        discard_kept_reports_the_leader_forwarded();

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
        if (config_.ha_enabled && replicated_wal_.has_value()) {
            ask_engine_for_position();
        }

        // Every engine this instance declined while it followed is told it leads, and asks again at once
        // rather than when its own timer next fires.
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_became_leader(epoch);
        }
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        awaiting_engine_position_ = false;
        if (log_tail_index_.has_value()) {
            discard_log_tail_index("this instance has stopped leading");
        }
        discard_held_orders();
        first_unheld_seq_ = 0;
        pause_or_resume_order_reading();
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

void SequencerThread::SequencerLeaseLinks::send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
    }
}

void SequencerThread::SequencerLeaseLinks::send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) {
    // Both arbiters: only the active one answers, and which one that is may have changed.
    for (const pubsub_itc_fw::ConnectionID& conn : {owner_.arbiter_primary_conn_id_, owner_.arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            owner_.send_pdu(conn, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
        }
    }
}

void SequencerThread::SequencerLeaseLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseGrant::message_pdu_id, 0, grant);
}

void SequencerThread::SequencerLeaseLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseRefusal::message_pdu_id, 0, refusal);
}

bool SequencerThread::SequencerReplicationLinks::peer_connected() const {
    return owner_.peer_active_conn().is_valid();
}

void SequencerThread::SequencerReplicationLinks::send_record_to_peer(const pubsub_itc_fw::ReplicatedRecord& record) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::ReplicatedRecord::message_pdu_id, record.seq_no, record);
    }
}

void SequencerThread::SequencerReplicationLinks::send_ack_to_peer(const pubsub_itc_fw::ConnectionID& on, const pubsub_itc_fw::WalAck& ack) {
    if (on.is_valid()) {
        owner_.send_pdu(on, pubsub_itc_fw::WalAck::message_pdu_id, 0, ack);
    }
}

void SequencerThread::SequencerReplicationLinks::send_position_request_to_peer(const pubsub_itc_fw::LogPositionRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::LogPositionRequest::message_pdu_id, 0, request);
    }
}

void SequencerThread::SequencerReplicationLinks::send_position_reply_to_peer(const pubsub_itc_fw::LogPositionReply& reply) {
    const pubsub_itc_fw::ConnectionID peer = owner_.peer_active_conn();
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::LogPositionReply::message_pdu_id, 0, reply);
    }
}

void SequencerThread::SequencerReplicatedWalEvents::role_changed(pubsub_itc_fw::ReplicaRole /*from*/, pubsub_itc_fw::ReplicaRole to, int32_t epoch) {
    switch (to) {
        case pubsub_itc_fw::ReplicaRole::leader:
            owner_.on_role_changed(pubsub_itc_fw_app::Role::leader, epoch);
            break;
        case pubsub_itc_fw::ReplicaRole::follower:
            owner_.on_role_changed(pubsub_itc_fw_app::Role::follower, epoch);
            break;
        case pubsub_itc_fw::ReplicaRole::unknown:
            owner_.on_role_changed(pubsub_itc_fw_app::Role::unknown, epoch);
            break;
    }
}

void SequencerThread::SequencerReplicatedWalEvents::committed_through(int64_t seq_no) {
    // The orders and the reports waiting for these records may now go: the orders to the matching
    // engine, in order, and the reports to their gateways.
    owner_.release_held_orders_through(seq_no);
    owner_.forward_pending_er_through(seq_no);
    // Last, after the order path's own work: the publishers are sent the records just committed.
    owner_.send_committed_records_to_publishers(seq_no);
}

void SequencerThread::SequencerReplicatedWalEvents::written_as_follower(int64_t /*seq_no*/, const pubsub_itc_fw::WalEntryContents& entry) {
    // Noted as it is written, as the leader notes the records it writes: the command it holds, so that
    // a command sent again after a change of leader is checked against it, and the position up to which
    // the leader had forwarded the engine's reports, so that the kept copies of those reports can be
    // discarded.
    if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return;
    }
    auto& arena_buf = owner_.decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, entry.bytes, entry.size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(owner_.get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: a replicated record did not decode as a WalRecord");
        return;
    }
    owner_.note_logged_command(view);
    owner_.note_forwarded_sample(entry.wall_time_ns, view);
}

void SequencerThread::SequencerReplicatedWalEvents::discarded_after(int64_t /*seq_no*/) {
    // Identifiers of the discarded records stay in the record of identifiers; a command that matches one
    // of them is checked against the log exactly, and not found, so it is sequenced as new.
}

void SequencerThread::SequencerReplicatedWalEvents::granted_lease_to_peer(std::chrono::steady_clock::time_point at) {
    // The peer now leads, and forwards the engine's reports, until at least a lease period from now.
    owner_.last_granted_to_leader_ = at;
}

void SequencerThread::send_status_query(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusQuery sq{};
    sq.instance_id = static_cast<int64_t>(config_.instance_id);
    sq.epoch = current_epoch();
    send_pdu(conn_id, pubsub_itc_fw_app::StatusQuery::message_pdu_id, 0, sq);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusQuery sent on connection {} (instance_id={} epoch={})",
               conn_id.get_value(), sq.instance_id, sq.epoch);
}

void SequencerThread::send_status_response(const pubsub_itc_fw::ConnectionID& conn_id) {
    pubsub_itc_fw_app::StatusResponse sr{};
    sr.self_instance_id = static_cast<int64_t>(config_.instance_id);
    sr.peer_instance_id = 0; // we don't know the peer's ID here; it's in the query
    sr.epoch = current_epoch();
    sr.current_role = role_;
    sr.next_sequence_number = replicated_wal_.has_value() ? replicated_wal_->next_seq_no() : 0;
    send_pdu(conn_id, pubsub_itc_fw_app::StatusResponse::message_pdu_id, 0, sr);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: StatusResponse sent on connection {} (role={} epoch={} next_seq={})",
               conn_id.get_value(), pubsub_itc_fw_app::to_string(role_), sr.epoch, sr.next_sequence_number);
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
    } else if (replicated_wal_.has_value() && replicated_wal_->handle_message(conn_id, pdu_id, message.payload(), static_cast<size_t>(message.payload_size()),
                                                                              std::chrono::steady_clock::now())) {
        // The lease rules and the replicated WAL: handled there.
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
    // The replicated WAL numbers the record itself, with the number the envelope was stamped with, writes
    // it, sends it to the follower, and commits it when it may.
    const int64_t written_as = replicated_wal_->append(pubsub_itc_fw_app::WalRecord::message_pdu_id, wal_encode_buffer_.data(), bytes_written,
                                                       envelope.wall_time_ns, std::chrono::steady_clock::now());
    // A copy for the publishers, so that they are sent it when it is committed without its being read back from
    // the WAL. A publisher is sent a record's envelope byte for byte as it lies in the WAL (publisher_record_from_wal),
    // which is these bytes.
    if (handed_over_publishers_.has_value()) {
        handed_over_publishers_->written(written_as, pubsub_itc_fw_app::WalRecord::message_pdu_id, wal_encode_buffer_.data(), bytes_written);
    }
    if (written_as != envelope.seq_no) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: the envelope was stamped seq={} but the replicated WAL wrote the record as {} -- the two numberings differ",
                   envelope.seq_no, written_as);
    }
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
        report(pubsub_itc_fw_app::HeldOrdersFull{envelope.seq_no});
    }
    release_pdu_payload(message);
}

void SequencerThread::send_held_order_to_matching_engine(const HeldOrder& held) {
    if (wal_follower_service_.has_value() && wal_follower_service_->is_being_caught_up(engine_routing_.active())) {
        // The reading thread is catching the engine up, and sends it this order: it is released, so at or
        // below the limit the reading thread serves up to.
        return;
    }
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
        send_orders_that_waited_in_the_log(first_unheld_seq_, acknowledged_seq_no);
        first_unheld_seq_ = acknowledged_seq_no >= replicated_wal_->last_seq_no() ? 0 : acknowledged_seq_no + 1;
    }
    publish_released_position();
    pause_or_resume_order_reading();
}

void SequencerThread::release_all_held_orders() {
    release_held_orders_through(replicated_wal_->last_seq_no());
}

void SequencerThread::send_orders_that_waited_in_the_log(int64_t first, int64_t through) {
    // With a reading thread and an engine to send to, the reading thread serves the engine from the first
    // of them, and this thread reads nothing. Every order sent from the storage is numbered below the
    // first, and is queued already. Once released, each later order is at or below the limit the reading
    // thread serves up to, so it sends those too, until the engine is handed over. An engine already being
    // served needs nothing more: its limit rises as orders are released.
    const pubsub_itc_fw::ConnectionID engine = engine_routing_.active();
    if (wal_follower_service_.has_value() && engine.is_valid()) {
        if (!wal_follower_service_->is_being_caught_up(engine)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: orders from {} waited in the log -- the reading thread sends them to the matching engine on connection {}", first,
                       engine.get_value());
            report(pubsub_itc_fw_app::EngineServedFromWal{engine.get_value(), first - 1});
            serve_engine(engine, first - 1, EngineServing{});
        }
        return;
    }
    // With no engine, they are counted as deferred; with no reading thread (high availability off), they
    // are read here.
    send_logged_orders(first, through);
}

void SequencerThread::send_logged_orders(int64_t first, int64_t through) {
    const pubsub_itc_fw::ConnectionID engine = engine_routing_.active();
    if (wal_follower_service_.has_value() && wal_follower_service_->is_being_caught_up(engine)) {
        // The reading thread is catching the engine up, and sends it these orders, as it does one sent from
        // the storage.
        return;
    }
    int64_t sent = 0;
    [[maybe_unused]] const auto end_position =
        pubsub_itc_fw::WalReader::replay(config_.wal_directory, replicated_wal_->wal().scan_start_for(first),
                                         [this, &engine, first, through, &sent](int64_t record_id, const void* payload, size_t size) {
                                             const std::optional<pubsub_itc_fw::WalEntryContents> contents =
                                                 pubsub_itc_fw::read_wal_entry_contents(payload, size);
                                             if (record_id < first || record_id > through || !contents.has_value() || contents->size == 0) {
                                                 return;
                                             }
                                             const int64_t wall_time_ns = contents->wall_time_ns;
                                             const int16_t pdu_id = contents->pdu_id;
                                             const uint8_t* record = contents->bytes;
                                             if (engine.is_valid()) {
                                                 if (stream_wal_record_to_me(engine, record_id, pdu_id, record, contents->size, wall_time_ns)) {
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
                                                 pubsub_itc_fw_app::decode(view, record, contents->size, bytes_consumed, arena, arena_bytes_needed) &&
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

void SequencerThread::start_building_record_of_identifiers() {
    if (!config_.ha_enabled) {
        return;
    }
    std::vector<std::string> segment_paths;
    for (const uint64_t segment : replicated_wal_->wal().segments_on_disk()) {
        segment_paths.push_back(replicated_wal_->wal().segment_path(segment));
    }
    identifiers_building_ = true;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: building the record of command identifiers in the background, for {} identifier(s), from {} log segment(s) -- "
               "until it is complete, a command sent again is checked against the log itself",
               config_.identifiers_reserved, segment_paths.size());
    identifiers_builder_.start(config_.identifiers_reserved, std::move(segment_paths), background_cores_);
}

void SequencerThread::take_record_of_identifiers_if_built() {
    if (!identifiers_building_ || !identifiers_builder_.finished()) {
        return;
    }
    logged_identifiers_.emplace(identifiers_builder_.take());
    identifiers_building_ = false;
    const size_t noted = identifiers_noted_while_building_.size();
    for (const uint64_t id : identifiers_noted_while_building_) {
        record_identifier(id);
    }
    identifiers_noted_while_building_.clear();
    identifiers_noted_while_building_.shrink_to_fit();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: the record of command identifiers is complete -- {} MiB reserved in {} ms and {} log record(s) read in {} ms in "
               "the background, holding {} identifier(s) of {} reserved, {} of them noted while it was built",
               identifiers_builder_.table_bytes() / (1024 * 1024), identifiers_builder_.reserve_time().count(), identifiers_builder_.records_read(),
               identifiers_builder_.read_time().count(), logged_identifiers_->size(), logged_identifiers_->capacity(), noted);
    if (identifiers_builder_.segments_not_read() > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: {} log segment(s) could not be read while the record of command identifiers was built, and were passed over "
                   "(the last because: {}). This is expected only when a repair cut the log back meanwhile, which the Quill log shows above",
                   identifiers_builder_.segments_not_read(), identifiers_builder_.last_segment_error());
    }
}

void SequencerThread::note_logged_command(const pubsub_itc_fw_app::WalRecordView& view) {
    const std::optional<uint64_t> id = command_identifier(view);
    if (id.has_value()) {
        note_logged_identifier(*id);
    }
}

void SequencerThread::note_logged_command(std::string_view comp_id, int16_t protocol, int16_t inner_pdu_id, std::string_view cl_ord_id) {
    const std::optional<uint64_t> id = command_identifier(comp_id, protocol, inner_pdu_id, cl_ord_id);
    if (id.has_value()) {
        note_logged_identifier(*id);
    }
}

void SequencerThread::note_logged_identifier(uint64_t id) {
    if (identifiers_building_) {
        identifiers_noted_while_building_.push_back(id);
        return;
    }
    record_identifier(id);
}

void SequencerThread::record_identifier(uint64_t id) {
    if (!logged_identifiers_.has_value()) {
        return;
    }
    if (logged_identifiers_->add(id) == LoggedCommandIdentifiers::Added::full && !identifiers_full_reported_) {
        identifiers_full_reported_ = true;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: the record of command identifiers is full at {} -- later commands are not recorded, so every command a gateway "
                   "sends again after a change of leader is checked against the log itself. Raise commands.identifiers_reserved",
                   logged_identifiers_->capacity());
    }
}

bool SequencerThread::command_already_logged(const pubsub_itc_fw_app::WalRecordView& inbound) {
    if (!config_.ha_enabled || !inbound.has_cl_ord_id || inbound.cl_ord_id.empty() || !inbound.has_sender_comp_id || inbound.sender_comp_id.empty()) {
        // Without HA there is no change of leader, and so nothing is ever sent again; a command that
        // does not say whose it is cannot be looked for.
        return false;
    }
    take_record_of_identifiers_if_built();
    const int16_t protocol = inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent;
    const uint64_t id = LoggedCommandIdentifiers::identifier(inbound.sender_comp_id, protocol, inbound.cl_ord_id);
    // Not in the record means certainly not in the log -- unless the record is full, when a command
    // logged after it filled would not be in it either, or is still being built, when the command may
    // be in the part of the log not yet read into it. In both cases the log itself is checked.
    if (logged_identifiers_.has_value() && !logged_identifiers_->full() && !logged_identifiers_->may_hold(id)) {
        return false;
    }
    if (!log_tail_index_.has_value()) {
        log_tail_index_.emplace(config_.wal_directory);
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: commands that may already be in the log are arriving -- reading the end of the log to check them exactly");
        report(pubsub_itc_fw_app::CheckingLogForRepeats{replicated_wal_.has_value() ? replicated_wal_->next_seq_no() - 1 : 0});
    }
    log_tail_index_used_at_ = std::chrono::steady_clock::now();
    const int64_t first_sent_no_earlier_than = inbound.has_gateway_ingress_ns ? inbound.gateway_ingress_ns : std::numeric_limits<int64_t>::min();
    return log_tail_index_->holds(inbound.sender_comp_id, protocol, inbound.cl_ord_id, first_sent_no_earlier_than);
}

bool SequencerThread::note_command_connection(const pubsub_itc_fw::ConnectionID& id, const pubsub_itc_fw_app::WalRecordView& inbound) {
    auto known = command_connection_gateway_.find(id.get_value());
    if (known == command_connection_gateway_.end()) {
        if (!inbound.has_gateway_instance_id) {
            // A command that does not say which gateway instance sent it cannot be matched with others.
            return false;
        }
        const int16_t protocol = inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent;
        const GatewayKey gateway = gateway_key(protocol, inbound.gateway_instance_id);
        known = command_connection_gateway_.emplace(id.get_value(), gateway).first;
        CommandConnections& connections = command_connections_[gateway];
        ++connections.open;
        if (connections.open == 2) {
            connections.already_logged = 0;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: gateway protocol={} instance={} is sending commands on a second connection, {}, while its earlier one is still "
                       "open -- checking every command from it against the log until the earlier one ends",
                       protocol, inbound.gateway_instance_id, id.get_value());
        }
    }
    return command_connections_[known->second].open > 1;
}

void SequencerThread::forget_command_connection(const pubsub_itc_fw::ConnectionID& id) {
    const auto known = command_connection_gateway_.find(id.get_value());
    if (known == command_connection_gateway_.end()) {
        return;
    }
    const GatewayKey gateway = known->second;
    command_connection_gateway_.erase(known);
    CommandConnections& connections = command_connections_[gateway];
    --connections.open;
    if (connections.open == 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: gateway protocol={} instance={} has one connection open again -- {} command(s) that arrived while it had two "
                   "were already in this log and were not sequenced twice; checking only commands marked as sent again from now on",
                   gateway >> 16, static_cast<int16_t>(gateway & 0xFFFF), connections.already_logged);
    }
    if (connections.open == 0) {
        command_connections_.erase(gateway);
    }
}

void SequencerThread::discard_log_tail_index(const char* reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: commands sent again since the change of leader: {} already in this log and not sequenced twice, {} sequenced as new; "
               "{} log record(s) read to check them -- {}",
               sent_again_already_logged_, sent_again_sequenced_, log_tail_index_->records_read(), reason);
    log_tail_index_.reset();
    sent_again_already_logged_ = 0;
    sent_again_sequenced_ = 0;
}

std::optional<EngineReportPosition> SequencerThread::reports_forwarded_through() const {
    if (!latest_report_seen_.has_value()) {
        return std::nullopt;
    }
    // A report still waiting for an acknowledgement has not been forwarded, and neither has anything
    // after it as far as a follower can tell, so the position stops just before the oldest waiting.
    std::optional<EngineReportPosition> oldest_waiting;
    for (const auto& [gate, pending] : pending_er_) {
        if (pending.position.has_value() && (!oldest_waiting.has_value() || oldest_waiting->is_after(*pending.position))) {
            oldest_waiting = pending.position;
        }
    }
    // A report held until its session is announced has not been forwarded either. Normally nothing is held, so this
    // reads nothing.
    held_reports_.for_each_held([&oldest_waiting](const fix_common::SessionIdentity&, const uint8_t* bytes, size_t size) {
        const PendingEr held = decode_held_report(bytes, size);
        if (held.position.has_value() && (!oldest_waiting.has_value() || oldest_waiting->is_after(*held.position))) {
            oldest_waiting = held.position;
        }
    });
    return oldest_waiting.has_value() ? oldest_waiting->just_before() : latest_report_seen_;
}

void SequencerThread::stamp_reports_forwarded_through(pubsub_itc_fw_app::WalRecord& envelope) const {
    const std::optional<EngineReportPosition> through = reports_forwarded_through();
    envelope.has_reports_forwarded_through_epoch = through.has_value();
    envelope.reports_forwarded_through_epoch = through.has_value() ? through->epoch : 0;
    envelope.has_reports_forwarded_through_number = through.has_value();
    envelope.reports_forwarded_through_number = through.has_value() ? through->number : 0;
}

void SequencerThread::note_forwarded_sample(int64_t written_ns, const pubsub_itc_fw_app::WalRecordView& view) {
    latest_record_written_ns_ = std::max(latest_record_written_ns_, written_ns);
    if (!view.has_reports_forwarded_through_epoch || !view.has_reports_forwarded_through_number) {
        return;
    }
    const EngineReportPosition through{view.reports_forwarded_through_epoch, view.reports_forwarded_through_number};
    // Only a change is worth keeping: the samples say when the leader's position moved.
    if (forwarded_samples_count_ > 0) {
        const ForwardedSample& newest = forwarded_samples_[(forwarded_samples_head_ + forwarded_samples_count_ - 1) % forwarded_sample_capacity];
        if (!through.is_after(newest.through)) {
            return;
        }
    }
    if (forwarded_samples_count_ == forwarded_sample_capacity) {
        forwarded_samples_head_ = (forwarded_samples_head_ + 1) % forwarded_sample_capacity;
        --forwarded_samples_count_;
    }
    forwarded_samples_[(forwarded_samples_head_ + forwarded_samples_count_) % forwarded_sample_capacity] = ForwardedSample{written_ns, through};
    ++forwarded_samples_count_;
}

void SequencerThread::discard_kept_reports_the_leader_forwarded() {
    const int64_t cutoff_ns = latest_record_written_ns_ - std::chrono::duration_cast<std::chrono::nanoseconds>(reports_forwarded_delay).count();
    // The newest sample written at or before the cutoff is the one that applies; older ones are no
    // longer needed.
    while (forwarded_samples_count_ >= 2 && forwarded_samples_[(forwarded_samples_head_ + 1) % forwarded_sample_capacity].written_ns <= cutoff_ns) {
        forwarded_samples_head_ = (forwarded_samples_head_ + 1) % forwarded_sample_capacity;
        --forwarded_samples_count_;
    }
    if (forwarded_samples_count_ == 0 || forwarded_samples_[forwarded_samples_head_].written_ns > cutoff_ns) {
        return;
    }
    kept_reports_discarded_as_forwarded_ += static_cast<int64_t>(kept_reports_.discard_through(forwarded_samples_[forwarded_samples_head_].through));
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
        held_at_takeover_ = replicated_wal_->last_seq_no();
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the matching engine has acted on orders through {} -- sending it the orders this log holds from {} to {}, which it "
                   "never received",
                   view.highest_applied, view.highest_applied + 1, held_at_takeover_);
        if (wal_follower_service_.has_value() && engine_routing_.active().is_valid()) {
            // The reading thread sends them, followed by the orders that waited for the engine's answer,
            // which are released below and are at or below the limit it serves up to.
            serve_engine(engine_routing_.active(), view.highest_applied, EngineServing{});
        } else {
            send_logged_orders(view.highest_applied + 1, held_at_takeover_);
        }
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
    // The orders that arrived meanwhile now go the way they would have gone: once their records are
    // committed, which may already be the case.
    release_held_orders_through(replicated_wal_->committed_through());
}

void SequencerThread::publish_released_position() {
    if (replicated_wal_.has_value()) {
        wal_positions_.publish_released(released_through());
    }
}

int64_t SequencerThread::released_through() const {
    if (!held_orders_.empty()) {
        return held_orders_.front().envelope.seq_no - 1;
    }
    if (first_unheld_seq_ != 0) {
        return first_unheld_seq_ - 1;
    }
    return replicated_wal_.has_value() ? replicated_wal_->last_seq_no() : 0;
}

void SequencerThread::pause_or_resume_order_reading() {
    if (paused_for_hand_over_) {
        return; // stays paused until the follower sequencer is handed over (end_pause_for_hand_over)
    }
    const bool waiting_for_confirmation =
        role_ == pubsub_itc_fw_app::Role::leader && ((replicated_wal_.has_value() && replicated_wal_->waits_for_confirmation()) || awaiting_engine_position_);
    if (!order_reading_paused_) {
        if (waiting_for_confirmation && (held_orders_.size() >= pause_order_reading_at() || first_unheld_seq_ != 0)) {
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
    if (waiting_for_confirmation && (held_orders_.size() > resume_order_reading_at() || first_unheld_seq_ != 0)) {
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

void SequencerThread::forward_pending_er_through(int64_t seq_no) {
    // Every report waiting on a record committed through seq_no may now go, in the order of the records.
    const auto covered_end = pending_er_.upper_bound(seq_no);
    for (auto it = pending_er_.begin(); it != covered_end; ++it) {
        forward_pending_er(it->second);
    }
    pending_er_.erase(pending_er_.begin(), covered_end);
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
        if (configured && hold_report_for_gateway(protocol, instance, er_seq_no, envelope)) {
            return;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway protocol={} instance={} not connected -- dropping ER seq={}",
                   protocol, instance, er_seq_no);
        report(pubsub_itc_fw_app::ReportDroppedGatewayNotConnected{protocol, instance, er_seq_no});
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

bool SequencerThread::hold_report_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) {
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        return false;
    }
    const GatewayKey gateway = gateway_key(protocol, instance);
    const auto store = reports_held_for_gateways_.find(gateway);
    if (store == reports_held_for_gateways_.end()) {
        return false;
    }
    // Encoded as it would have been sent, after the sequence number its transport header carries, so that sending it
    // later is a send of these bytes. Measure, then fit, as the WAL's encoding does.
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    [[maybe_unused]] const bool measured = pubsub_itc_fw_app::encode(envelope, nullptr, 0, bytes_written, bytes_needed);
    if (held_envelope_encoding_.size() < sizeof(er_seq_no) + bytes_needed) {
        held_envelope_encoding_.resize(sizeof(er_seq_no) + bytes_needed);
    }
    std::memcpy(held_envelope_encoding_.data(), &er_seq_no, sizeof(er_seq_no));
    if (!pubsub_itc_fw_app::encode(envelope, held_envelope_encoding_.data() + sizeof(er_seq_no), held_envelope_encoding_.size() - sizeof(er_seq_no),
                                   bytes_written, bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: failed to encode ER seq={} for gateway protocol={} instance={} to hold it ({} bytes needed) -- dropping it", er_seq_no,
                   protocol, instance, bytes_needed);
        return false;
    }
    const fix_common::SessionIdentity identity =
        fix_common::SessionIdentity::make(envelope.has_sender_comp_id ? envelope.sender_comp_id : std::string_view{}, protocol);
    store->second.hold(identity, held_envelope_encoding_.data(), sizeof(er_seq_no) + bytes_written);
    if (gateways_holding_reports_.insert(gateway).second) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: gateway protocol={} instance={} has no connection for reports from this instance -- ER seq={} held, and every report "
                   "for it after this, until the connection is made",
                   protocol, instance, er_seq_no);
        report(pubsub_itc_fw_app::ReportsHeldForGateway{protocol, instance, er_seq_no});
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq={} for gateway protocol={} instance={} held", er_seq_no, protocol,
                   instance);
    }
    return true;
}

void SequencerThread::send_reports_held_for_gateway(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& connection) {
    const GatewayKey gateway = gateway_key(protocol, instance);
    gateways_holding_reports_.erase(gateway);
    const auto store = reports_held_for_gateways_.find(gateway);
    if (store == reports_held_for_gateways_.end() || store->second.count() == 0) {
        return;
    }
    // Sent as they were encoded, oldest first. Not timed: a report that waited for a connection says nothing about how
    // long the path takes.
    const size_t sent = store->second.take_all([this, &connection](const fix_common::SessionIdentity&, const uint8_t* bytes, size_t size) {
        int64_t er_seq_no = 0;
        std::memcpy(&er_seq_no, bytes, sizeof(er_seq_no));
        send_pdu_payload(connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, bytes + sizeof(er_seq_no), size - sizeof(er_seq_no));
    });
    int64_t& dropped_reported = gateway_held_reports_dropped_reported_[gateway];
    const int64_t dropped = store->second.dropped_for_room() - dropped_reported;
    dropped_reported = store->second.dropped_for_room();
    if (dropped > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: {} report(s) held for gateway protocol={} instance={} were dropped to make room while it had no connection for "
                   "reports; their members are not told of them",
                   dropped, protocol, instance);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: gateway protocol={} instance={} connection for reports made -- sent it {} report(s) held until it was", protocol, instance,
               sent);
    report(pubsub_itc_fw_app::ReportsSentToGateway{protocol, instance, static_cast<int64_t>(sent), dropped});
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
    state.leader_epoch = current_epoch();
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

    // Connected again, so the next report dropped for it, if any, is logged as a Warning again.
    sessions_with_reports_dropped_.erase(identity);

    // A session established before, announced again because the gateway's link to this instance has
    // come up: this instance may have restarted, and then knows of no session. Only where it is
    // connected is recorded. Its numbering is settled, so there is nothing to send back; and it is
    // not a logon, so a binding already held is not a sign that the previous connection died, and
    // the number the session resumes from is left alone.
    if (view.still_connected) {
        const auto existing = session_destinations_.find(identity);
        if (existing == session_destinations_.end()) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: session comp_id='{}' protocol={} announced again, still connected to instance={} connection={} -- recorded",
                       identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id);
        } else if (existing->second.instance != destination.instance || existing->second.conn_id != destination.conn_id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: session comp_id='{}' protocol={} announced again, still connected to instance={} connection={} -- "
                       "it was bound to instance={} connection={}, now replaced",
                       identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id, existing->second.instance,
                       existing->second.conn_id);
        }
        session_destinations_[identity] = destination;
        send_held_reports(identity);
        report(pubsub_itc_fw_app::SessionAnnouncedAgain{identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id,
                                                        role_ == pubsub_itc_fw_app::Role::leader});
        return;
    }

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

    // Reports held for this session until it was announced. A FIX member that logs on asks, once its numbering is
    // settled, for every report produced since the last it was delivered (UndeliveredReportsRequest), and those are
    // sent from the WAL; the held ones are among them, so sending them here as well would give the member each one
    // twice. The binary gateway asks for no such thing, so a binary member is sent them here.
    if (identity.protocol == gateway_ids::fix_order_gateway) {
        const size_t discarded = held_reports_.release(identity, [](const uint8_t*, size_t) {});
        if (discarded > 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "SequencerThread: session comp_id='{}' protocol={} logged on -- {} report(s) held for it are left to the reports it asks for once "
                       "established",
                       identity.comp_id_view(), identity.protocol, discarded);
            report(pubsub_itc_fw_app::HeldReportsReleased{identity.comp_id_view(), identity.protocol, 0, static_cast<int64_t>(discarded)});
        }
    } else {
        send_held_reports(identity);
    }

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
        // Reports logged from here on are forwarded live to the new binding; those logged before,
        // after the last one the member was delivered, are what its gateway asks for once the
        // session is established.
        stored.bound_at_log_seq_no = replicated_wal_->last_seq_no();
        stored.delivered_figure_may_be_behind = previous_session_died;
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

    // How far the member was served, exactly: the reports logged for it after this one, while it is
    // away, are delivered when it binds again.
    if (view.has_last_report_delivered && view.last_report_delivered > state.last_report_delivered) {
        state.last_report_delivered = view.last_report_delivered;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: session comp_id='{}' protocol={} unbound from instance={} connection={} -- "
               "remembered outbound={} and {} report range(s); its reports have nowhere to go until it binds again",
               identity.comp_id_view(), identity.protocol, view.gateway_instance_id, view.gateway_session_conn_id, state.outbound_seq_num,
               state.report_seq_nums.size());
    report(pubsub_itc_fw_app::SessionUnboundRecorded{identity.comp_id_view(), identity.protocol, view.gateway_instance_id, view.gateway_session_conn_id});
}

void SequencerThread::handle_undelivered_reports_request(const pubsub_itc_fw::EventMessage& message) {
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        return;
    }
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::UndeliveredReportsRequestView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode UndeliveredReportsRequest -- dropping");
        return;
    }
    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(view.comp_id, view.gateway_protocol_id);
    const auto state_it = session_sequence_state_.find(identity);
    const fix_common::SessionDestination* destination = session_destination(identity);
    if (state_it == session_sequence_state_.end() || destination == nullptr || state_it->second.bound_at_log_seq_no == 0) {
        return;
    }
    SessionSequenceState& state = state_it->second;
    const int64_t after = state.last_report_delivered;
    const int64_t through = state.bound_at_log_seq_no;
    const bool possible_repeat = state.delivered_figure_may_be_behind;
    // Answered once for each binding.
    state.bound_at_log_seq_no = 0;
    if (through <= after) {
        return;
    }

    // The member's reports logged after the last it was delivered and up to when it bound again,
    // read from the newest segment backwards, stopping at the first segment that begins at or before
    // the last delivered, and sent oldest first. A member away for a long time on a busy day reaches
    // further back, and so is read further back.
    struct Undelivered {
        int64_t log_seq_no{};
        bool poss_resend{false};
        std::vector<uint8_t> payload;
    };
    std::vector<Undelivered> found;
    std::vector<Undelivered> in_segment;
    const std::vector<uint64_t> segments = replicated_wal_->wal().segments_on_disk();
    for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment) {
        in_segment.clear();
        int64_t first_in_segment = 0;
        static_cast<void>(pubsub_itc_fw::WalReader::replay_segment(
            replicated_wal_->wal().segment_path(*segment), 0,
            [&identity, after, through, &in_segment, &first_in_segment](int64_t record_id, const void* payload, size_t size) {
                if (first_in_segment == 0) {
                    first_in_segment = record_id;
                }
                const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
                if (record_id <= after || record_id > through || !contents.has_value() || contents->size == 0) {
                    return;
                }
                std::array<uint8_t, 64 * 1024> record_arena_buffer{};
                pubsub_itc_fw::BumpAllocator record_arena(record_arena_buffer.data(), record_arena_buffer.size());
                size_t record_consumed = 0;
                size_t record_needed = 0;
                pubsub_itc_fw_app::WalRecordView stored{};
                if (!pubsub_itc_fw_app::decode(stored, contents->bytes, contents->size, record_consumed, record_arena, record_needed)) {
                    return;
                }
                if (stored.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) || !stored.has_sender_comp_id ||
                    fix_common::SessionIdentity::make(stored.sender_comp_id,
                                                      stored.has_origin_gateway_id ? stored.origin_gateway_id : gateway_ids::default_when_absent) != identity) {
                    return;
                }
                in_segment.push_back(
                    Undelivered{record_id, stored.poss_resend, std::vector<uint8_t>(stored.payload.data, stored.payload.data + stored.payload.size)});
            }));
        found.insert(found.begin(), std::make_move_iterator(in_segment.begin()), std::make_move_iterator(in_segment.end()));
        if (first_in_segment != 0 && first_in_segment <= after + 1) {
            break;
        }
    }

    for (const Undelivered& report : found) {
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = report.log_seq_no;
        envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
        envelope.payload = pubsub_itc_fw_app::BytesView{report.payload.data(), report.payload.size()};
        envelope.has_gateway_session_conn_id = true;
        envelope.gateway_session_conn_id = destination->conn_id;
        envelope.has_origin_gateway_id = true;
        envelope.origin_gateway_id = identity.protocol;
        envelope.has_gateway_instance_id = true;
        envelope.gateway_instance_id = destination->instance;
        envelope.has_sender_comp_id = true;
        envelope.sender_comp_id = identity.comp_id_view();
        // Repeats only if the gateway that last served the member died without saying how far.
        envelope.poss_resend = report.poss_resend || possible_repeat;
        // Not timed: a report waiting while its member was away says nothing about how long the path takes.
        const bool is_new_order_ack = false;
        send_er_to_origin_gateway(identity.protocol, destination->instance, 0, envelope, is_new_order_ack);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: session comp_id='{}' protocol={} bound again -- sent it {} report(s) produced while it had no connection (log records {} to "
               "{}){}",
               identity.comp_id_view(), identity.protocol, found.size(), after + 1, through, possible_repeat ? ", marked as possible repeats" : "");
    report(pubsub_itc_fw_app::UndeliveredReportsSent{identity.comp_id_view(), identity.protocol, static_cast<int64_t>(found.size())});
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

namespace {

// The fixed part of a held report, written before its payload and its ClOrdID.
struct HeldReportHeader {
    int64_t seq_no{0};
    int64_t log_seq_no{0};
    int64_t gateway_ingress_ns{0};
    int64_t position_number{0};
    int32_t position_epoch{0};
    uint32_t payload_size{0};
    uint32_t cl_ord_id_size{0};
    int16_t pdu_id{0};
    uint8_t has_position{0};
    uint8_t has_gateway_ingress_ns{0};
    uint8_t is_new_order_ack{0};
    uint8_t poss_resend{0};
};

} // un-named namespace

SequencerThread::PendingEr SequencerThread::decode_held_report(const uint8_t* bytes, size_t size) {
    HeldReportHeader header{};
    if (size < sizeof(header)) {
        throw pubsub_itc_fw::PreconditionAssertion("SequencerThread: a held report is shorter than its header", __FILE__, __LINE__);
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (sizeof(header) + header.payload_size + header.cl_ord_id_size != size) {
        throw pubsub_itc_fw::PreconditionAssertion("SequencerThread: a held report's sizes do not add up", __FILE__, __LINE__);
    }
    PendingEr pending{};
    pending.pdu_id = header.pdu_id;
    pending.seq_no = header.seq_no;
    pending.log_seq_no = header.log_seq_no;
    pending.has_gateway_ingress_ns = header.has_gateway_ingress_ns != 0;
    pending.gateway_ingress_ns = header.gateway_ingress_ns;
    pending.is_new_order_ack = header.is_new_order_ack != 0;
    pending.poss_resend = header.poss_resend != 0;
    if (header.has_position != 0) {
        pending.position = EngineReportPosition{header.position_epoch, header.position_number};
    }
    const uint8_t* payload = bytes + sizeof(header);
    pending.payload.assign(payload, payload + header.payload_size);
    pending.cl_ord_id.assign(reinterpret_cast<const char*>(payload + header.payload_size), header.cl_ord_id_size);
    // The routing entry was dealt with when the report was held.
    pending.erase_routing_entry = false;
    return pending;
}

int32_t SequencerThread::gateways_not_announced() const {
    int32_t not_announced = 0;
    for (const auto& [gateway, announcement] : gateway_announcements_) {
        if (!announcement.announced) {
            ++not_announced;
        }
    }
    return not_announced;
}

bool SequencerThread::hold_report_if_sessions_unknown(const PendingEr& pending) {
    if (role_ != pubsub_itc_fw_app::Role::leader || config_.test_events.expects_no_session_for(pending.identity.comp_id_view())) {
        return false;
    }
    const int32_t not_announced = gateways_not_announced();
    if (not_announced == 0) {
        return false;
    }
    HeldReportHeader header{};
    header.seq_no = pending.seq_no;
    header.log_seq_no = pending.log_seq_no;
    header.gateway_ingress_ns = pending.gateway_ingress_ns;
    header.has_gateway_ingress_ns = pending.has_gateway_ingress_ns ? 1 : 0;
    header.has_position = pending.position.has_value() ? 1 : 0;
    header.position_epoch = pending.position.has_value() ? pending.position->epoch : 0;
    header.position_number = pending.position.has_value() ? pending.position->number : 0;
    header.payload_size = static_cast<uint32_t>(pending.payload.size());
    header.cl_ord_id_size = static_cast<uint32_t>(pending.cl_ord_id.size());
    header.pdu_id = pending.pdu_id;
    header.is_new_order_ack = pending.is_new_order_ack ? 1 : 0;
    header.poss_resend = pending.poss_resend ? 1 : 0;
    held_report_encoding_.resize(sizeof(header) + pending.payload.size() + pending.cl_ord_id.size());
    std::memcpy(held_report_encoding_.data(), &header, sizeof(header));
    std::memcpy(held_report_encoding_.data() + sizeof(header), pending.payload.data(), pending.payload.size());
    std::memcpy(held_report_encoding_.data() + sizeof(header) + pending.payload.size(), pending.cl_ord_id.data(), pending.cl_ord_id.size());
    held_reports_.hold(pending.identity, held_report_encoding_.data(), held_report_encoding_.size());

    if (!reports_held_since_sessions_known_) {
        reports_held_since_sessions_known_ = true;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: ER seq_no={} for session comp_id='{}' protocol={} held -- {} gateway instance(s) have not yet announced their "
                   "sessions, so it may be connected after all. Held reports are sent when their session is announced, and dropped once every "
                   "gateway instance has announced",
                   pending.seq_no, pending.identity.comp_id_view(), pending.identity.protocol, not_announced);
        report(pubsub_itc_fw_app::ReportsHeld{pending.identity.comp_id_view(), pending.identity.protocol, not_announced});
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "SequencerThread: ER seq_no={} for session comp_id='{}' protocol={} held", pending.seq_no,
                   pending.identity.comp_id_view(), pending.identity.protocol);
    }
    if (held_reports_.dropped_for_room() != held_reports_dropped_reported_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the room for reports held until their sessions are announced is full, and {} held report(s) have been "
                   "dropped to make room; their members are not told of them",
                   held_reports_.dropped_for_room() - held_reports_dropped_reported_);
        held_reports_dropped_reported_ = held_reports_.dropped_for_room();
        held_reports_dropped_gauge_.set(static_cast<double>(held_reports_dropped_reported_));
    }
    return true;
}

void SequencerThread::send_held_reports(const fix_common::SessionIdentity& identity) {
    if (held_reports_.count() == 0) {
        return;
    }
    // Decoded into a list first: forward_pending_er must not be called while the store is being walked.
    std::vector<PendingEr> to_send;
    held_reports_.release(identity, [&to_send](const uint8_t* bytes, size_t size) { to_send.push_back(decode_held_report(bytes, size)); });
    if (to_send.empty()) {
        return;
    }
    for (PendingEr& pending : to_send) {
        pending.identity = identity;
        forward_pending_er(pending);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: session comp_id='{}' protocol={} announced -- sent it {} report(s) held until it was", identity.comp_id_view(),
               identity.protocol, to_send.size());
    report(pubsub_itc_fw_app::HeldReportsReleased{identity.comp_id_view(), identity.protocol, static_cast<int64_t>(to_send.size()), 0});
}

void SequencerThread::drop_held_reports_for_sessions_connected_nowhere() {
    reports_held_since_sessions_known_ = false;
    if (held_reports_.count() == 0) {
        return;
    }
    std::vector<std::pair<fix_common::SessionIdentity, int64_t>> dropped;
    held_reports_.take_all([&dropped](const fix_common::SessionIdentity& identity, const uint8_t* bytes, size_t size) {
        dropped.emplace_back(identity, decode_held_report(bytes, size).seq_no);
    });
    for (const auto& [identity, seq_no] : dropped) {
        note_report_dropped_unbound(identity, seq_no);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: every gateway instance has announced its sessions -- {} held report(s) were for sessions connected nowhere, and are "
               "dropped",
               dropped.size());
    report(pubsub_itc_fw_app::HeldReportsReleased{std::string_view{}, 0, 0, static_cast<int64_t>(dropped.size())});
}

void SequencerThread::handle_sessions_announced(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::SessionsAnnouncedView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode SessionsAnnounced -- dropping");
        return;
    }
    const auto found = gateway_announcements_.find(gateway_key(view.gateway_protocol_id, view.gateway_instance_id));
    if (found == gateway_announcements_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: SessionsAnnounced from gateway protocol={} instance={}, which is not in this instance's configuration -- ignored",
                   view.gateway_protocol_id, view.gateway_instance_id);
        return;
    }
    found->second.announced = true;
    found->second.link = conn_id;
    const int32_t not_announced = gateways_not_announced();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: gateway protocol={} instance={} has announced its {} session(s) on connection {}; {} configured gateway instance(s) "
               "have not",
               view.gateway_protocol_id, view.gateway_instance_id, view.sessions_announced, conn_id.get_value(), not_announced);
    if (not_announced == 0) {
        drop_held_reports_for_sessions_connected_nowhere();
    }
}

void SequencerThread::note_report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no) {
    ++reports_dropped_unbound_;
    reports_dropped_unbound_gauge_.set(static_cast<double>(reports_dropped_unbound_));
    // A test sends commands under such a comp id straight to this instance, with no gateway holding a session for it.
    if (config_.test_events.expects_no_session_for(identity.comp_id_view())) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: ER seq_no={} for comp_id='{}' protocol={} dropped -- the test expects no session for it", er_seq_no,
                   identity.comp_id_view(), identity.protocol);
        return;
    }
    // A member that is away is told of these reports when it logs on again, so a dropped report is not
    // always a fault. A member that is connected, and whose gateway has not told this instance so, is
    // never told of them. The Warning names the session once, on its first report dropped since it was
    // last connected; the count says how many.
    if (sessions_with_reports_dropped_.insert(identity).second) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: ER seq_no={} for session comp_id='{}' protocol={} dropped -- the session is connected to no gateway that has "
                   "told this instance so. Its later reports are dropped too, logged at Debug, until it is connected again; "
                   "{} report(s) dropped for this reason in all",
                   er_seq_no, identity.comp_id_view(), identity.protocol, reports_dropped_unbound_);
        report(pubsub_itc_fw_app::ReportDroppedSessionNotConnected{identity.comp_id_view(), identity.protocol, er_seq_no});
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: ER seq_no={} for session comp_id='{}' protocol={} -- session not bound to any instance, dropping", er_seq_no,
                   identity.comp_id_view(), identity.protocol);
    }
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
    // Never lowered, for the same reason.
    if (view.has_last_report_delivered && view.last_report_delivered > state.last_report_delivered) {
        state.last_report_delivered = view.last_report_delivered;
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

    // Only the leader answers. The gateway sends every request to both sequencers, because it does
    // not know which one leads. A follower's WAL may not yet hold the newest reports, so counting
    // back from its newest would find the wrong ones; and when both answered, the gateway placed the
    // two answers' records on the member's numbers in the order they arrived, so the member was sent
    // the wrong orders under the right numbers (BUG-0148). An instance whose role is not yet known
    // declines too. The decline is sent rather than left unsaid so that the gateway can tell it
    // from an answer still coming; a gateway that is only declined asks again when a new leader
    // announces itself.
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        pubsub_itc_fw_app::SessionReplayComplete declined{};
        declined.request_id = view.request_id;
        declined.record_count = 0;
        declined.last_seq_no = from_seq_no;
        declined.truncated = false;
        declined.declined = true;
        send_pdu(conn_id, pubsub_itc_fw_app::SessionReplayComplete::message_pdu_id, 0, declined);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: SessionReplayRequest id={} comp_id='{}' declined -- this instance is {}, and only the leader answers", view.request_id,
                   identity.comp_id_view(), pubsub_itc_fw_app::to_string(role_));
        report(pubsub_itc_fw_app::ReplayDeclined{static_cast<int64_t>(view.request_id), identity.comp_id_view()});
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: SessionReplayRequest id={} comp_id='{}' protocol={} from_seq_no={} max_records={} skip_most_recent={}", view.request_id,
               identity.comp_id_view(), identity.protocol, from_seq_no, max_records, skip_most_recent);

    // Walk the WAL and hand back this session's execution reports.
    //
    // Nothing is stored twice to make this possible: every report is already in the WAL with
    // the session that originated it on its envelope. What the venue lacked was a way to ask
    // for one session's slice of that stream, and this is it.
    //
    // The log has no index of each member's reports, so the segments are read from the newest
    // backwards, and the reading stops as soon as the window below is full or the segment reached
    // begins at or before from_seq_no. What a member has missed is the end of its stream, so an
    // ordinary resend reads the last few segments, not the whole day. Reading the whole log from
    // its first record on every request, on this thread, stopped the venue sequencing orders for
    // as long as that took (docs/bug_list.md, BUG-0113). A request reaching far back still reads
    // far back; serving resends from a separate recovery service is the longer-term answer.
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

    // The matches in one segment, in the order they were written; added to the front of the window,
    // newest first, once the segment has been read.
    std::vector<ReplayMatch> in_segment;
    size_t segments_read = 0;
    const std::vector<uint64_t> segments = replicated_wal_->wal().segments_on_disk();
    for (auto segment = segments.rbegin(); segment != segments.rend() && window.size() < window_capacity; ++segment) {
        in_segment.clear();
        int64_t first_in_segment = 0;
        static_cast<void>(pubsub_itc_fw::WalReader::replay_segment(
            replicated_wal_->wal().segment_path(*segment), 0,
            [&identity, from_seq_no, &in_segment, &total_matched, &first_in_segment](int64_t record_id, const void* payload, size_t size) {
                if (first_in_segment == 0) {
                    first_in_segment = record_id;
                }
                if (record_id <= from_seq_no) {
                    return;
                }
                const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
                if (!contents.has_value()) {
                    return;
                }
                const int64_t wall_time_ns = contents->wall_time_ns;
                const uint8_t* record_payload = contents->bytes;
                const size_t record_size = contents->size;

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
                in_segment.push_back(std::move(match));
            }));
        ++segments_read;
        for (auto match = in_segment.rbegin(); match != in_segment.rend() && window.size() < window_capacity; ++match) {
            window.push_front(std::move(*match));
        }
        // Every older segment holds only records at or before from_seq_no.
        if (first_in_segment != 0 && first_in_segment <= from_seq_no + 1) {
            break;
        }
    }

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
               "SequencerThread: SessionReplayComplete id={} comp_id='{}' records={} last_seq_no={} truncated={} scanned_in_ms={} segments_read={} of {}",
               view.request_id, identity.comp_id_view(), record_count, last_seq_no, truncated, elapsed_ns / 1000000, segments_read, segments.size());
    report(pubsub_itc_fw_app::ReplayAnswered{static_cast<int64_t>(view.request_id), identity.comp_id_view(), static_cast<int64_t>(record_count), truncated});
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
        if (hold_report_if_sessions_unknown(pending)) {
            if (pending.erase_routing_entry) {
                seq_no_to_session_.erase(pending.seq_no);
            }
            return;
        }
        note_report_dropped_unbound(pending.identity, pending.seq_no);
    }

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = pending.log_seq_no;
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

// Matching engine publishers (docs/availability/following_the_wal.md, section 4.3)

void SequencerThread::SequencerTestEventLinks::send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes,
                                                               size_t size) {
    owner_.send_pdu_payload(connection, pdu_id, 0, bytes, size);
}

void SequencerThread::SequencerPublisherLinks::send_record(const pubsub_itc_fw::ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes,
                                                           size_t size) {
    owner_.send_pdu_payload(follower, pdu_id, seq_no, bytes, size);
}

void SequencerThread::handle_publisher_follow_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::WalFollowRequestView view{};
    if (!pubsub_itc_fw::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode a publisher's WalFollowRequest -- dropping");
        return;
    }
    // Whatever this connection was sent before stops: the publisher has asked afresh, from the last record it holds.
    handed_over_publishers_->remove(conn_id);

    // A publisher asks both sequencers, since it cannot tell which leads. Only the leader serves it: if both
    // did, it would be sent every record twice (BUG-0153). One that does not lead declines, and tells the
    // publisher if it takes the lead, when the publisher asks it again.
    const pubsub_itc_fw::Leading leading = role_ == pubsub_itc_fw_app::Role::leader ? pubsub_itc_fw::Leading::Leads : pubsub_itc_fw::Leading::DoesNotLead;
    if (wal_follower_service_.has_value()) {
        // The reading thread sends it every record after its position, up to the last committed, and the
        // service hands it over (SequencerWalFollowing::follower_handed_over), after which it is sent each
        // record from here, as it is committed.
        wal_follower_service_->on_follow_request(conn_id, view, leading, pubsub_itc_fw::FollowerLimit::LastCommitted, publisher_converter);
        return;
    }
    if (leading == pubsub_itc_fw::Leading::DoesNotLead) {
        pubsub_itc_fw::WalFollowDeclined declined{};
        declined.last_seq_no = view.last_seq_no;
        send_pdu(conn_id, pubsub_itc_fw::WalFollowDeclined::message_pdu_id, 0, declined);
        return;
    }

    // Without a reading thread (high availability off): caught up here, from the segment holding the first
    // record the publisher lacks, and then sent each record as it is committed.
    const int64_t committed = replicated_wal_->committed_through();
    const int64_t holds_through = view.last_seq_no < 0 ? committed : view.last_seq_no;
    handed_over_publishers_->add(conn_id, holds_through, replicated_wal_->wal().scan_start_for(holds_through + 1));
    const pubsub_itc_fw::WalHandedOverFollowers::Outcome outcome = handed_over_publishers_->send_to(conn_id, committed);
    if (outcome.damaged_seq_no != 0) {
        report_follower_cannot_be_served(conn_id, outcome.damaged_seq_no, "a matching engine publisher");
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: {} on connection {} held records through {} -- {} record(s) sent, through {}, and each record from here is sent as it "
               "is committed",
               view.follower_name, conn_id.get_value(), view.last_seq_no, outcome.records_sent, committed);
    pubsub_itc_fw::WalFollowCaughtUp caught_up{};
    caught_up.last_seq_no = std::max(committed, holds_through);
    caught_up.records_sent = outcome.records_sent;
    caught_up.first_seq_no_held = replicated_wal_->wal().first_seq_no();
    send_pdu(conn_id, pubsub_itc_fw::WalFollowCaughtUp::message_pdu_id, 0, caught_up);
}

void SequencerThread::send_committed_records_to_publishers(int64_t seq_no) {
    if (!handed_over_publishers_.has_value() || handed_over_publishers_->empty()) {
        return;
    }
    for (const auto& [publisher, damaged_seq_no] : handed_over_publishers_->send_to_all(seq_no)) {
        report_follower_cannot_be_served(publisher, damaged_seq_no, "a matching engine publisher");
    }
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

void SequencerThread::handle_wal_follow_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::WalFollowRequestView view{};
    if (!pubsub_itc_fw::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode WalFollowRequest -- dropping");
        return;
    }
    const int64_t last_seq_no = view.last_seq_no;
    const bool asking_to_lead = view.wants_new_records;

    // Only the leader may serve a catch-up. The promoted engine asks every sequencer it holds an order
    // connection to, since it cannot tell which leads; if a follower also served, the engine would be
    // sent the WAL twice and act on the second as live traffic once promoted. The follower instead
    // re-points its own order connection to the asker -- so that if it later takes the lead it sends
    // orders to the promoted engine -- and declines, remembering the engine, so that it tells it to ask
    // again the moment it takes the lead (WalLeaderChanged).
    if (role_ != pubsub_itc_fw_app::Role::leader) {
        engine_routing_.asked_to_catch_up_while_following(conn_id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WalFollowRequest on connection {} but I am follower -- re-pointed ME order connection, declined, and will say when "
                   "this instance leads",
                   conn_id.get_value());
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_follow_request(conn_id, view, pubsub_itc_fw::Leading::DoesNotLead, pubsub_itc_fw::FollowerLimit::LastReleased,
                                                     engine_converter);
        } else {
            pubsub_itc_fw::WalFollowDeclined declined{};
            declined.last_seq_no = last_seq_no;
            send_pdu(conn_id, pubsub_itc_fw::WalFollowDeclined::message_pdu_id, 0, declined);
        }
        return;
    }

    // Only orders already released to the engine: one still waiting for the follower's
    // acknowledgement, or for a voter's confirmation, is sent live when it is released, and sending it
    // here as well would act on it early.
    const int64_t wal_head = released_through();

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
        // Anything the reading thread was still sending this connection stops first.
        if (wal_follower_service_.has_value() && wal_follower_service_->is_being_caught_up(conn_id)) {
            wal_follower_service_->on_connection_lost(conn_id);
            engine_servings_.erase(conn_id.get_value());
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: WalFollowRequest from connection {} carries no position -- the engine holds nothing and is placed at head={} without "
                   "catch-up. Any order taken and not yet applied is not recovered by this instance",
                   conn_id.get_value(), wal_head);
        pubsub_itc_fw::WalFollowCaughtUp head{};
        head.last_seq_no = wal_head;
        head.first_seq_no_held = wal_head;
        head.records_sent = 0;
        send_pdu(conn_id, pubsub_itc_fw::WalFollowCaughtUp::message_pdu_id, 0, head);
        return;
    }

    // With a reading thread, it reads the WAL and sends the orders, and this thread carries on; the engine
    // is answered when it is handed over (engine_handed_over). Orders released meanwhile reach it too.
    if (wal_follower_service_.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WalFollowRequest from connection {} last_seq_no={} ({}) -- the reading thread sends it the orders after that, up "
                   "to the last released, now {}",
                   conn_id.get_value(), last_seq_no, asking_to_lead ? "asking to lead" : "starting, not asking to lead", wal_head);
        EngineServing serving{};
        serving.answers_catch_up_request = true;
        serving.asking_to_lead = asking_to_lead;
        serve_engine(conn_id, last_seq_no, serving);
        return;
    }

    // Without a reading thread (high availability off): read here, from the segment holding the first
    // record the engine lacks.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: WalFollowRequest from connection {} last_seq_no={} ({}) -- streaming WAL catch-up up to head={}", conn_id.get_value(),
               last_seq_no, asking_to_lead ? "asking to lead" : "starting, not asking to lead", wal_head);
    const EngineCatchUpFromWal caught_up =
        engine_catch_up_from_wal(config_.wal_directory, replicated_wal_->wal().scan_start_for(last_seq_no + 1), last_seq_no, wal_head, decode_arena_buffer(),
                                 [this, &conn_id](int64_t record_id, const pubsub_itc_fw_app::WalRecord& envelope) {
                                     send_pdu(conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, envelope);
                                 });
    if (caught_up.damaged_seq_no != 0) {
        // The engine is not told it has caught up: what it holds would be incomplete without anything showing it.
        report_follower_cannot_be_served(conn_id, caught_up.damaged_seq_no, "the matching engine");
        return;
    }
    const int64_t streamed = caught_up.records_sent;
    EngineServing serving{};
    serving.answers_catch_up_request = true;
    serving.asking_to_lead = asking_to_lead;
    engine_handed_over(conn_id, wal_head, streamed, serving);
}

void SequencerThread::serve_engine(const pubsub_itc_fw::ConnectionID& engine, int64_t after_seq_no, EngineServing serving) {
    publish_released_position();
    engine_servings_[engine.get_value()] = serving;
    wal_follower_service_->serve(engine, after_seq_no, pubsub_itc_fw::FollowerLimit::LastReleased, engine_converter, pubsub_itc_fw::CaughtUpMessage::DoNotSend);
}

void SequencerThread::engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent, EngineServing serving) {
    note_handed_over_through(through);
    report(pubsub_itc_fw_app::EngineHandedOver{engine.get_value(), through, records_sent, serving.answers_catch_up_request});
    if (!serving.answers_catch_up_request) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: the reading thread has sent the matching engine on connection {} the orders it lacked, through {} ({} sent) -- "
                   "orders go to it from here again",
                   engine.get_value(), through, records_sent);
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL catch-up complete -- {} record(s) streamed to ME connection {}",
               records_sent, engine.get_value());

    // Signal completion. On receipt the ME considers its book reconciled and becomes leader. What was
    // sent, not what was walked past: the engine checks it received exactly this many and will not act if
    // it did not, which is R-0101. The earliest record the WAL holds tells the engine whether the WAL
    // reaches back to the first record of the day (R-0123).
    const int64_t earliest_retained = replicated_wal_->wal().first_seq_no();
    pubsub_itc_fw::WalFollowCaughtUp caught_up{};
    caught_up.last_seq_no = through;
    caught_up.first_seq_no_held = earliest_retained;
    caught_up.records_sent = records_sent;
    send_pdu(engine, pubsub_itc_fw::WalFollowCaughtUp::message_pdu_id, 0, caught_up);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: WalFollowCaughtUp sent (last_seq_no={}, earliest retained={}, records_sent={}) -- ME is now live", through, earliest_retained,
               records_sent);

    // Only for a promotion. An instance that is merely starting gets the records and no routing: it does
    // not yet know whether its peer leads, and it learns that from the peer over a different channel and
    // on its own schedule. Moving the order connection here on the strength of the ask took the routing
    // off a working leader in 400 milliseconds, and nothing put it back -- measured by ha_test.py
    // scenario 54, and it stopped the venue matching. The asker is the only party that knows which case
    // this is, which is why the request carries it rather than this side inferring it. See BUG-0077.
    if (!serving.asking_to_lead) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: connection {} caught up but is starting rather than being promoted -- order routing left where it is", engine.get_value());
        return;
    }
    engine_routing_.caught_up_to_lead(engine);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ME order connection promoted to {} -- sequenced orders now route to the caught-up ME", engine.get_value());
}

bool SequencerThread::stream_wal_record_to_me(const pubsub_itc_fw::ConnectionID& conn_id, int64_t record_id, int16_t pdu_id, const uint8_t* pdu_payload,
                                              size_t pdu_size, int64_t /*wall_time_ns*/) {
    // Each stored WAL record is a WalRecord envelope. Orders and cancels are sent to the engine, which
    // unwraps the envelope and reads wall_time_ns as the sequencing time; execution reports are outputs,
    // not inputs, and are not sent.
    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- record seq={} is not a WalRecord (pdu_id={}) -- skipping",
                   record_id, pdu_id);
        return false;
    }
    pubsub_itc_fw_app::WalRecord envelope{};
    const EngineRecordFromWal found = engine_record_from_wal(pdu_id, pdu_payload, pdu_size, decode_arena_buffer(), envelope);
    if (found == EngineRecordFromWal::NotDecodable) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: catch-up -- failed to decode envelope seq={} -- skipping", record_id);
        return false;
    }
    if (found != EngineRecordFromWal::ForEngine) {
        return false;
    }
    send_pdu(conn_id, pubsub_itc_fw_app::WalRecord::message_pdu_id, record_id, envelope);
    return true;
}

} // namespaces
