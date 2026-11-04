// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerThread.hpp"

#include "PublisherRecordFromWal.hpp"

#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <LeasePairNumbers.hpp>
#include <OrderPathMetrics.hpp>
#include <TradingDate.hpp>
#include <lease_messages.hpp>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FileSystemUtils.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

namespace sequencer {

namespace {

// What VenueStateFromTheWal reads from a record's envelope, as decoded (WalRecordView) or about to be written (WalRecord).
template <typename Envelope> VenueStateFromTheWal::Record record_of(const Envelope& envelope) {
    VenueStateFromTheWal::Record record{};
    record.pdu_id = envelope.pdu_id;
    record.payload = envelope.payload.data;
    record.size = envelope.payload.size;
    record.has_answers_seq_no = envelope.has_answers_seq_no;
    record.answers_seq_no = envelope.answers_seq_no;
    record.has_trading_date = envelope.has_trading_date;
    record.trading_date = envelope.trading_date;
    return record;
}

} // un-named namespace

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
        "1 while this sequencer leads and commits orders without waiting for its follower: no follower is connected, the follower is being brought "
        "up to date, or it is too far behind. A loss of resilience. Expected to be 0, apart from a few seconds after a start or a change of leader");

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
    // The WAL keeps a record of the commands it holds, with high availability on or off, so that the leader writes a
    // command into the WAL at most once, and refuses a member that uses an identifier again
    // (docs/availability/one_check_against_sequencing_twice.md).
    wal_configuration.command_identity = &command_identity_;
    wal_configuration.identifiers_reserved = config_.identifiers_reserved;
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
    // A follower engine is sent every record as the WAL holds it, as a publisher is.
    handed_over_engines_.emplace(replicated_wal_->wal(), config_.wal_directory, make_publisher_converter(64 * 1024), publisher_links_,
                                 pubsub_itc_fw::WalKeptRecordsCapacity{config_.records_kept_for_publishers, config_.bytes_kept_for_publishers});
    // Every follower of the WAL is caught up by the reading thread, with high availability on or off.
    wal_follower_service_.emplace(get_logger(), replicated_wal_->wal(), wal_positions_, wal_following_, wal_following_);
    if (config_.ha_enabled) {
        replicated_wal_->use_peer_catch_up(wal_following_);
    }
    const int64_t recovered_seq = replicated_wal_->open(std::chrono::steady_clock::now());
    replicated_wal_->start_building_record_of_commands();
    if (recovered_seq > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: WAL open complete: recovered seq_no={}, record_count={}, next_sequence_number={}", recovered_seq,
                   replicated_wal_->wal().record_count(), replicated_wal_->next_seq_no());
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: WAL is fresh (no prior records), starting from seq_no=1");
    }

    // The last time this process knew of a matching engine it routed orders to, which measures how long the venue was
    // without one when the whole venue was down; no earlier than the first record, before which there was nothing to
    // recover (EngineAbsenceRule).
    presence_.started(config_.wal_directory, replicated_wal_->wal().first_wall_time_ns().value_or(0), config_.wall_clock->now_ns());
    engine_absence_timer_id_ = start_recurring_timer(engine_absence_check_interval);

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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway protocol={} instance={} connection {} established",
                   endpoint->protocol, endpoint->instance, id.get_value());
        reports_.gateway_connected(endpoint->protocol, endpoint->instance, id);
        // Tell it the acceptance state straight away. A transition-only design gets exactly
        // this case wrong: a gateway starting during an outage would otherwise assume the
        // venue was fine, which is the default that caused BUG-0009 in the first place.
        // EnginePresence::refresh_order_acceptance tells every gateway when it changes anything, and this connection
        // is already known by then, so sending again here would duplicate it.
        if (!presence_.refresh_order_acceptance(std::chrono::steady_clock::now())) {
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
        orders_.order_connection_established(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: gateway order connection {} established{}", id.get_value(),
                   orders_.reading() != OrderRelease::Reading::reading ? " -- reading from it is paused while orders wait" : "");
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

    // An engine connection that opens may be to an engine that already announced that it leads.
    if (engine_routing_.is_engine_connection(id)) {
        engine_routing_changed();
    }
}

void SequencerThread::engine_routing_changed() {
    // Said when an engine takes orders again, not when the next order happens to arrive, so that a
    // venue that recovers while nothing is trading says so; and the orders held meanwhile go to it.
    if (role_ != pubsub_itc_fw_app::Role::leader || !replicated_wal_.has_value()) {
        return;
    }
    note_whether_an_engine_takes_orders();
    release_orders_that_waited_for_the_engine();
}

void SequencerThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (test_events_.has_value() && test_events_->on_connection_lost(id, reason)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: the connection to the test's event listener is lost ({}) -- no more test events are sent", reason);
        return;
    }
    if (const std::optional<int64_t> instance = presence_.connection_lost(id); instance.has_value()) {
        // The engine that said it leads has gone, or at least the connection it said so on has: it is no longer taken to
        // lead when a void is asked for, and is sent no orders until it is caught up to lead or says so again, so that a
        // restarted process is not sent orders on its predecessor's word (docs/availability/voided_commands.md, 5.1).
        engine_routing_.leader_announcement_lost(*instance);
        note_whether_an_engine_takes_orders();
    }
    const std::optional<ReportForwarding::GatewayKey> lost_gateway = reports_.connection_lost(id);
    if (lost_gateway.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: gateway id {} connection {} lost: {}", *lost_gateway, id.get_value(),
                   reason);
    } else if (engine_routing_.is_engine_connection(id)) {
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_connection_lost(id);
        }
        engine_servings_.erase(id.get_value());
        if (handed_over_engines_.has_value()) {
            handed_over_engines_->remove(id);
        }
        presence_.catch_up_ended(id);
        const bool carried_orders = engine_routing_.active() == id;
        engine_routing_.lost(id);
        note_whether_an_engine_takes_orders();
        if (carried_orders) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: matching engine order connection {} lost: {} -- the other engine may promote and reconnect", id.get_value(), reason);
            if (orders_.awaiting_engine_position()) {
                // The engine that was asked is gone. Whichever engine acts next catches up from this
                // log before it acts, which sends it everything the asked one lacked, so nothing more
                // is waited for.
                orders_.stop_awaiting_engine_position("the matching engine it asked has disconnected; whichever engine acts next catches up from this log");
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
    } else if (orders_.order_connection_lost(id)) {
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

    if (message.pdu_id() == pubsub_itc_fw_app::HaltRequest::message_pdu_id && engine_routing_.is_engine_connection(conn_id)) {
        handle_halt_request(conn_id, message);
        release_pdu_payload(message);
        return;
    }

    if (message.pdu_id() == pubsub_itc_fw_app::VoidRequest::message_pdu_id && engine_routing_.is_engine_connection(conn_id)) {
        handle_void_request(conn_id, message);
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

        // Every command is checked against the WAL before it is given a number, whatever path it came by, so that
        // the WAL holds each command at most once (docs/availability/one_check_against_sequencing_twice.md, 3.1). One
        // the WAL already holds is answered from the WAL if it was sent again, and refused if it was not
        // (CommandChecks).
        if (command_checks_.check(message.connection_id(), inbound, std::chrono::steady_clock::now()) == CommandChecks::Outcome::not_sequenced) {
            release_pdu_payload(message);
            return;
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
        envelope.has_trading_date = true;
        envelope.trading_date = trading_date_for(wall_time_ns);
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
            MemberSessions::OriginSession origin;
            origin.identity = fix_common::SessionIdentity::make(inbound.sender_comp_id,
                                                                inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent);
            // Remembered, not read: the gateway stamped this when it read the order off the
            // client socket, and gets it back on the ER so it can measure the round trip.
            origin.has_ingress_ns = inbound.has_gateway_ingress_ns;
            origin.gateway_ingress_ns = inbound.gateway_ingress_ns;
            sessions_.order_placed(seq, origin);
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
        // docs/availability/a_follower_behind_does_not_lead.md); or at once with HA off. It goes only to an
        // engine that leads, and while none does it stays held, so that no engine applies it before the
        // engine that leads is sent it (docs/availability/voided_commands.md, section 5.1). It never goes
        // ahead of an order already waiting. Until then it waits, in the storage for held orders, or in
        // the log once that is full; if the storage is full of orders the follower has not acknowledged,
        // the follower is too far behind and the leader runs as if alone.
        if (orders_.held_orders_wait_on_the_follower()) {
            replicated_wal_->follower_too_far_behind("its storage for orders waiting on the follower is full", std::chrono::steady_clock::now());
        }

        orders_.hold(envelope, message.slab_id(), message.payload());
        release_held_orders_through(replicated_wal_->committed_through());
        note_whether_an_engine_takes_orders();
        presence_.order_held();
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
        const MemberSessions::OriginSession* origin = sessions_.origin(er_seq_no);
        if (origin != nullptr) {
            routing_identity = origin->identity;
            has_routing_ingress_ns = origin->has_ingress_ns;
            routing_ingress_ns = origin->gateway_ingress_ns;

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
            const fix_common::SessionDestination* destination = sessions_.destination(routing_identity);
            if (destination != nullptr) {
                has_routing_conn = true;
                routing_conn_id = destination->conn_id;
                routing_gateway_id = routing_identity.protocol;
                routing_gateway_instance = destination->instance;
            } else {
                // The session is not connected anywhere, so the report has nowhere to go. It is
                // counted as dropped where it would have been sent, below, or when ReportForwarding
                // sends it after its record is committed; by then the session may be connected again.
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

    // Which record must be committed before this report goes to the gateway.
    //
    // Ordinarily it is the order's record: a member is not told its order executed until the follower holds the order.
    //
    // A report that answers no order has no order's record to wait for: the reports cancelling the book that a promoted
    // matching engine sends carry seq_no 0. A report that repeats one already sent, as every report from a catch-up
    // does, names an order whose record was committed long ago, so waiting for that record would not wait at all. Both
    // wait for the report's own record instead, which is unique to the report, so that the member learns of it only once
    // the follower holds the report's record.
    const bool gate_on_own_record = (er_seq_no == 0) || possible_repeat;
    const int64_t gate_seq_no = gate_on_own_record ? er_wal_seq : er_seq_no;

    // The report as it would wait, for the follower or for its session to be announced.
    auto as_pending = [&]() {
        ReportForwarding::Pending pending{};
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
            if (reports_.hold_for_unannounced_session(as_pending())) {
                if (erase_routing_entry) {
                    sessions_.order_ended(er_seq_no);
                }
                return;
            }
            sessions_.report_dropped_unbound(routing_identity, er_seq_no);
        }
        reports_.send_to_gateway(routing_gateway_id, routing_gateway_instance, er_seq_no, envelope, is_new_order_ack);
        sessions_.report_forwarded(routing_identity);
        if (erase_routing_entry) {
            sessions_.order_ended(er_seq_no);
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
        reports_.wait_for_commit(gate_seq_no, as_pending());
    }

    // Handled, whether forwarded, waiting, or dropped: reports_forwarded_through() accounts for the
    // ones still waiting.
    reports_.report_seen(position);
}

void SequencerThread::keep_report_from_engine(const pubsub_itc_fw::EventMessage& message) {
    // Where the report stands in the engine's reports, so that it can be discarded once the leader has said it forwarded
    // it.
    std::optional<EngineReportPosition> position;
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed) &&
        view.has_report_engine_epoch && view.has_report_number) {
        position = EngineReportPosition{view.report_engine_epoch, view.report_number};
    }
    kept_reports_.keep(message.seq_no(), message.payload(), static_cast<size_t>(message.payload_size()), position, std::chrono::steady_clock::now());
}

void SequencerThread::KeptReportLinks::forward_kept_report(int64_t er_seq_no, const uint8_t* bytes, size_t size) {
    owner_.forward_report_from_engine(bytes, size, er_seq_no, ReportSource::kept_while_not_leading);
}

void SequencerThread::KeptReportLinks::kept_reports_lost(int64_t total) {
    owner_.kept_reports_lost_gauge_.set(static_cast<double>(total));
}

void SequencerThread::KeptReportLinks::kept_reports_forwarded(int64_t discarded_as_forwarded, int64_t count, int64_t bytes) {
    owner_.report(pubsub_itc_fw_app::KeptReportsForwarded{discarded_as_forwarded, count, bytes});
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

    if (id == engine_absence_timer_id_) {
        check_engine_absence();
        return;
    }

    if (id == acknowledgement_watch_timer_id_) {
        const auto now = std::chrono::steady_clock::now();
        replicated_wal_->on_acknowledgement_watch(now);
        orders_.tick(now);
        if (role_ != pubsub_itc_fw_app::Role::leader) {
            kept_reports_.tick(now);
        }
        replicated_wal_->take_record_of_commands_if_built();
        command_checks_.tick(now);
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
                                        pubsub_itc_fw::CaughtUpMessage::DoNotSend, std::nullopt);
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
    owner_.orders_.end_pause_for_hand_over();
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
        owner_.orders_.end_pause_for_hand_over();
        return;
    }
    const auto engine = owner_.engine_servings_.find(follower.get_value());
    if (engine != owner_.engine_servings_.end()) {
        const EngineServing serving = engine->second;
        owner_.engine_servings_.erase(engine);
        owner_.engine_handed_over(follower, through, records_sent, resume_at, serving);
        owner_.orders_.end_pause_for_hand_over();
    }
}

void SequencerThread::SequencerWalFollowing::hand_over_falling_behind(const pubsub_itc_fw::ConnectionID& /*follower*/) {
    owner_.orders_.pause_for_hand_over();
}

void SequencerThread::SequencerWalFollowing::follower_progress(const pubsub_itc_fw::ConnectionID& follower, int64_t sent_through) {
    // Only a matching engine that asked to be caught up: the recovery limit measures how long its catch-up will take.
    const auto serving = owner_.engine_servings_.find(follower.get_value());
    if (serving == owner_.engine_servings_.end() || !serving->second.answers_catch_up_request) {
        return;
    }
    owner_.presence_.catch_up_progress(follower, RecoveryLimitRule::CatchUpProgress{sent_through, owner_.orders_.released_through()});
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
    owner_.orders_.end_pause_for_hand_over();
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
        orders_.end_pause_for_hand_over();
    }
    // A publisher handed over is sent nothing more from here either; it asks again when told of the new leader.
    if (stops_leading && handed_over_publishers_.has_value()) {
        handed_over_publishers_->remove_all();
    }
    // So is a follower engine, which also asks again when told of the new leader.
    if (stops_leading && handed_over_engines_.has_value()) {
        handed_over_engines_->remove_all();
    }
    // Reports held for sessions not yet announced are the new leader's to send: it forwards what it kept while following.
    if (stops_leading) {
        reports_.stopped_leading();
    }

    if (new_role == pubsub_itc_fw_app::Role::leader) {
        // Every record this instance wrote while it followed was noted as it was written, so the record
        // of identifiers already holds every command in the log. What the old leader said it forwarded
        // is acted on now, so that the kept reports forwarded below are only those it may not have.
        kept_reports_.discard_those_the_leader_forwarded();

        // The previous leader may have died holding reports it had not forwarded. This instance
        // kept every report the engine sent it, so it forwards them all now
        // (docs/availability/change_of_sequencer_leader.md, section 4.4).
        kept_reports_.forward_all();

        // After the kept reports, and on the same connections, so that a gateway has the answers to
        // the commands it holds before it is told to send them again, and sends again only those still
        // unanswered (docs/availability/commands_during_a_change_of_leader.md, section 3.3). The
        // message carries this leadership's epoch, which is what tells a gateway that a new instance
        // leads.
        //
        // Tell the gateways where this venue now stands on accepting orders. They may be holding
        // what the previous leader last said, which was true of a process that is no longer
        // running. This instance has held nothing for want of an engine yet, so it accepts -- but that has to be said
        // rather than assumed, because silence here leaves a refusal in place that nothing will
        // ever lift.
        broadcast_order_acceptance();

        // This instance's log may hold orders the engine never received: the follower wrote and
        // acknowledged them, and the previous leader died before the acknowledgement reached it.
        // Ask the engine how far it has got, and send it the rest before anything new
        // (docs/availability/commands_during_a_change_of_leader.md, section 3.5).
        if (config_.ha_enabled && replicated_wal_.has_value()) {
            orders_.ask_engine_for_position();
        }

        // Every engine this instance declined while it followed is told it leads, and asks again at once
        // rather than when its own timer next fires.
        if (wal_follower_service_.has_value()) {
            wal_follower_service_->on_became_leader(epoch);
        }

        // An absence longer than the venue allows, found at start-up or measured while following, is acted on before
        // anything new is sequenced.
        check_engine_absence();
    } else if (new_role == pubsub_itc_fw_app::Role::follower) {
        command_checks_.stopped_leading();
        orders_.stopped_leading();
        // Another instance leads, so the venue was not down as a whole; and a follower holds nothing for a matching
        // engine, so the time without one is forgotten (EnginePresence::stopped_leading).
        presence_.stopped_leading(config_.wall_clock->now_ns());
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
    // First, before any command is released: a void whose record is now committed, so that every engine knows of it before
    // it is sent a command the void names.
    owner_.send_voids_committed_through(seq_no);
    // The orders and the reports waiting for these records may now go: the orders to the matching
    // engine, in order, and the reports to their gateways.
    owner_.orders_.release_to_leading_engine(seq_no);
    owner_.reports_.deliver_committed_through(seq_no);
    // Last, after the order path's own work: the matching engines that follow the WAL are sent the records released, and
    // the publishers the records just committed.
    owner_.send_released_records_to_follower_engines();
    owner_.send_committed_records_to_publishers(seq_no);
}

void SequencerThread::SequencerReplicatedWalEvents::written_as_follower(int64_t seq_no, const pubsub_itc_fw::WalEntryContents& entry) {
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
    owner_.kept_reports_.leader_record_written(entry.wall_time_ns, view.has_reports_forwarded_through_epoch && view.has_reports_forwarded_through_number
                                                                       ? std::optional<EngineReportPosition>{EngineReportPosition{
                                                                             view.reports_forwarded_through_epoch, view.reports_forwarded_through_number}}
                                                                       : std::nullopt);
    // A follower tells the gateways nothing, so a change in whether trading is halted is only kept, for when this
    // instance leads.
    static_cast<void>(owner_.venue_state_.note_record(seq_no, record_of(view), VenueStateFromTheWal::How::written_as_follower));
}

void SequencerThread::SequencerReplicatedWalEvents::discarded_after(int64_t seq_no) {
    // Identifiers of the discarded records stay in the record of identifiers; a command that matches one
    // of them is checked against the log exactly, and not found, so it is sequenced as new.
    //
    // A void whose record is discarded is no longer in the WAL. Its record was never committed, so no engine was told
    // of it.
    static_cast<void>(owner_.venue_state_.discard_after(seq_no));
}

void SequencerThread::SequencerReplicatedWalEvents::checking_for_commands_already_held(int64_t last_seq_no) {
    PUBSUB_LOG_STR(owner_.get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: commands that may already be in the WAL are arriving -- the WAL proves each one exactly");
    owner_.report(pubsub_itc_fw_app::CheckingLogForRepeats{last_seq_no});
}

void SequencerThread::SequencerReplicatedWalEvents::read_at_open(int64_t seq_no, const pubsub_itc_fw::WalEntryContents& entry) {
    // The voids in the WAL, and the highest command a report answers, read here rather than by a read of the WAL of the
    // sequencer's own, so that both are complete before this instance serves any engine. The envelope is decoded once
    // for both, since this is done for every record the WAL holds.
    if (entry.pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        return;
    }
    auto& arena_buf = owner_.decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, entry.bytes, entry.size, bytes_consumed, arena, arena_bytes_needed)) {
        owner_.venue_state_.record_did_not_decode(seq_no, VenueStateFromTheWal::How::read_at_open);
        return;
    }
    static_cast<void>(owner_.venue_state_.note_record(seq_no, record_of(view), VenueStateFromTheWal::How::read_at_open));
}

void SequencerThread::send_voided_commands(const pubsub_itc_fw::ConnectionID& engine) {
    if (venue_state_.voids().empty() || !engine.is_valid() || !replicated_wal_.has_value()) {
        return;
    }
    const int64_t committed = replicated_wal_->committed_through();
    // A day has few voids, and this is sent once for each catch-up, so the list is built here rather than kept.
    std::vector<pubsub_itc_fw_app::VoidedCommand> committed_voids;
    for (const fix_common::VoidedRange& voided : venue_state_.voids().voids()) {
        if (voided.void_seq_no <= committed) {
            committed_voids.push_back(
                pubsub_itc_fw_app::VoidedCommand{voided.void_seq_no, voided.first_seq_no, voided.last_seq_no, voided.new_orders_only, voided.reason});
        }
    }
    if (committed_voids.empty()) {
        return;
    }
    pubsub_itc_fw_app::VoidedCommands message{};
    message.voids = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::VoidedCommand>{committed_voids.data(), committed_voids.size()};
    send_pdu(engine, pubsub_itc_fw_app::VoidedCommands::message_pdu_id, 0, message);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: sent matching engine connection {} the {} void(s) in the WAL",
               engine.get_value(), committed_voids.size());
}

void SequencerThread::send_voids_committed_through(int64_t seq_no) {
    if (!venue_state_.take_voids_due(seq_no)) {
        return;
    }
    send_voided_commands(engine_routing_.active());
    if (engine_routing_.standby() != engine_routing_.active()) {
        send_voided_commands(engine_routing_.standby());
    }
}

void SequencerThread::SequencerReplicatedWalEvents::granted_lease_to_peer(std::chrono::steady_clock::time_point at) {
    // The peer now leads, and forwards the engine's reports, until at least a lease period from now.
    owner_.kept_reports_.granted_lease_to_leader(at);
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
            MemberSessions::OriginSession origin;
            origin.identity =
                fix_common::SessionIdentity::make(view.sender_comp_id, view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent);
            // has_ingress_ns stays false for the reason above.
            sessions_.order_placed(view.seq_no, origin);
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
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->written(written_as, pubsub_itc_fw_app::WalRecord::message_pdu_id, wal_encode_buffer_.data(), bytes_written);
    }
    if (written_as != envelope.seq_no) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "SequencerThread: the envelope was stamped seq={} but the replicated WAL wrote the record as {} -- the two numberings differ",
                   envelope.seq_no, written_as);
    }
    // Every record this instance writes as leader passes here, so whether trading is halted is kept as it is written. The
    // gateways are told at once of a change.
    // Every record this instance writes as leader is noted as it is written: a report, for the highest command answered;
    // a void of its own, sent to the engines once committed; and a member's command or a TechnicalEvent, for whether
    // trading is halted, which the gateways are told of when it changes.
    if (venue_state_.note_record(written_as, record_of(envelope), VenueStateFromTheWal::How::written_by_this_leader).halt_changed) {
        presence_.refresh_order_acceptance(std::chrono::steady_clock::now());
    }
}

void SequencerThread::send_held_order_to_matching_engine(const HeldOrder& held) {
    // Called only while an engine takes orders (an_engine_takes_orders).
    if (wal_follower_service_.has_value() && wal_follower_service_->is_being_caught_up(engine_routing_.active())) {
        // The reading thread is sending the engine orders that waited in the log, and sends it this order too: it is
        // released, so at or below the limit the reading thread serves up to.
        return;
    }
    if (held.own_payload_size > 0) {
        // A command this sequencer wrote: its payload is held with it, since no inbound message holds it.
        pubsub_itc_fw_app::WalRecord envelope = held.envelope;
        envelope.payload = pubsub_itc_fw_app::BytesView{held.own_payload.data(), held.own_payload_size};
        send_pdu(engine_routing_.active(), pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope.seq_no, envelope,
                 pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
        return;
    }
    send_pdu(engine_routing_.active(), pubsub_itc_fw_app::WalRecord::message_pdu_id, held.envelope.seq_no, held.envelope,
             pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
    if (held.envelope.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        order_path_metrics::observe_checkpoint(order_out_elapsed_histogram_, held.envelope.has_gateway_ingress_ns, held.envelope.gateway_ingress_ns,
                                               config_.wall_clock->now_ns());
    }
}

int64_t SequencerThread::OrderLinks::committed_through() const {
    return owner_.replicated_wal_.has_value() ? owner_.replicated_wal_->committed_through() : 0;
}

int64_t SequencerThread::OrderLinks::last_seq_no() const {
    return owner_.replicated_wal_.has_value() ? owner_.replicated_wal_->last_seq_no() : 0;
}

bool SequencerThread::OrderLinks::waits_for_confirmation() const {
    return owner_.replicated_wal_.has_value() && owner_.replicated_wal_->waits_for_confirmation();
}

bool SequencerThread::OrderLinks::waits_for_follower() const {
    return owner_.replicated_wal_.has_value() && owner_.replicated_wal_->waits_for_follower();
}

bool SequencerThread::OrderLinks::leading() const {
    return owner_.role_ == pubsub_itc_fw_app::Role::leader;
}

bool SequencerThread::OrderLinks::an_engine_takes_orders() const {
    return owner_.an_engine_takes_orders();
}

pubsub_itc_fw::ConnectionID SequencerThread::OrderLinks::leading_engine_connection() const {
    return owner_.engine_routing_.active();
}

void SequencerThread::OrderLinks::send_to_leading_engine(const HeldOrder& held) {
    owner_.send_held_order_to_matching_engine(held);
}

void SequencerThread::OrderLinks::release_through(int64_t committed_through) {
    owner_.release_held_orders_through(committed_through);
}

void SequencerThread::OrderLinks::send_engine_orders_after(int64_t after) {
    owner_.serve_engine(owner_.engine_routing_.active(), after, EngineServing{});
}

void SequencerThread::OrderLinks::release_message(pubsub_itc_fw::SlabHandle slab_id, const uint8_t* buffer) {
    owner_.release_pdu_payload(slab_id, buffer);
}

void SequencerThread::OrderLinks::pause_reading(const pubsub_itc_fw::ConnectionID& connection) {
    owner_.pause_reading(connection);
}

void SequencerThread::OrderLinks::resume_reading(const pubsub_itc_fw::ConnectionID& connection) {
    owner_.resume_reading(connection);
}

void SequencerThread::OrderLinks::released_position_moved(int64_t released_through) {
    if (owner_.replicated_wal_.has_value()) {
        owner_.wal_positions_.publish_released(released_through);
    }
}

void SequencerThread::OrderLinks::ask_engine_position(const pubsub_itc_fw::ConnectionID& connection, int64_t request_id) {
    pubsub_itc_fw_app::EnginePositionQuery query{};
    query.request_id = request_id;
    owner_.send_pdu(connection, pubsub_itc_fw_app::EnginePositionQuery::message_pdu_id, 0, query);
}

void SequencerThread::OrderLinks::held_orders_full(int64_t first_seq_no) {
    owner_.report(pubsub_itc_fw_app::HeldOrdersFull{first_seq_no});
}

bool SequencerThread::PresenceLinks::leading() const {
    return owner_.role_ == pubsub_itc_fw_app::Role::leader;
}

bool SequencerThread::PresenceLinks::an_engine_takes_orders() const {
    return owner_.an_engine_takes_orders();
}

bool SequencerThread::PresenceLinks::an_engine_is_routed_to() const {
    return owner_.engine_routing_.active().is_valid();
}

bool SequencerThread::PresenceLinks::wal_holds_records() const {
    return owner_.replicated_wal_->wal().last_seq_no() > 0;
}

bool SequencerThread::PresenceLinks::trading_halted() const {
    return owner_.venue_state_.trading_halt().halted();
}

bool SequencerThread::PresenceLinks::halted_for_a_recovery_too_long() const {
    return owner_.venue_state_.trading_halt().latest()->reason == pubsub_itc_fw_app::TechnicalEventReason::RecoveryTooLong;
}

void SequencerThread::PresenceLinks::tell_every_gateway() {
    owner_.broadcast_order_acceptance();
}

void SequencerThread::PresenceLinks::engine_takes_orders_again(int64_t without_ms, int64_t orders_held, bool one_had_before) {
    owner_.report(pubsub_itc_fw_app::EngineTakesOrdersAgain{without_ms, orders_held, one_had_before});
}

void SequencerThread::PresenceLinks::write_recovery_halt(int64_t now_ns) {
    owner_.write_recovery_halt(now_ns);
}

std::optional<pubsub_itc_fw::CommandInWal> SequencerThread::CommandLinks::find_command(std::string_view identity, int64_t first_sent_no_earlier_than) {
    return owner_.replicated_wal_->find_command(identity, first_sent_no_earlier_than);
}

bool SequencerThread::CommandLinks::answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command) {
    return owner_.answer_from_wal(inbound, command);
}

void SequencerThread::CommandLinks::refuse(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound, int64_t first_seq_no) {
    const int16_t protocol = inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent;
    owner_.report(pubsub_itc_fw_app::IdentifierRefused{inbound.sender_comp_id, protocol, inbound.cl_ord_id, first_seq_no});
    pubsub_itc_fw_app::CommandRefused refused{};
    refused.gateway_session_conn_id = inbound.has_gateway_session_conn_id ? inbound.gateway_session_conn_id : 0;
    refused.sender_comp_id = inbound.sender_comp_id;
    refused.cl_ord_id = inbound.cl_ord_id;
    refused.command_pdu_id = inbound.pdu_id;
    refused.command = inbound.payload;
    refused.reason = pubsub_itc_fw_app::CommandRefusedReason::IdentifierAlreadyUsed;
    refused.first_seq_no = first_seq_no;
    // On the connection the command came by, which the gateway dialled and certainly has open: the connection this
    // sequencer dials to send the gateway its reports may not be open yet, after the gateway has restarted. A refused
    // command has no reports, so nothing on that connection needs to arrive before this.
    owner_.send_pdu(came_by, pubsub_itc_fw_app::CommandRefused::message_pdu_id, 0, refused);
}

int64_t SequencerThread::CommandLinks::discard_command_search() {
    return owner_.replicated_wal_->discard_command_search();
}

pubsub_itc_fw::ReplicatedWal::CommandCheckCounts SequencerThread::CommandLinks::command_check_counts() const {
    return owner_.replicated_wal_->command_check_counts();
}

bool SequencerThread::HaltLinks::leading() const {
    return owner_.role_ == pubsub_itc_fw_app::Role::leader && owner_.replicated_wal_.has_value();
}

bool SequencerThread::HaltLinks::orders_routed_to(const pubsub_itc_fw::ConnectionID& connection) const {
    return owner_.engine_routing_.active() == connection;
}

bool SequencerThread::HaltLinks::an_engine_takes_orders() const {
    return owner_.an_engine_takes_orders();
}

bool SequencerThread::HaltLinks::another_engine_leads(int64_t instance_id) const {
    return owner_.presence_.another_engine_leads(instance_id);
}

const TradingHaltState& SequencerThread::HaltLinks::trading_halt() const {
    return owner_.venue_state_.trading_halt();
}

int64_t SequencerThread::HaltLinks::highest_command_answered() const {
    return owner_.venue_state_.highest_command_answered();
}

int64_t SequencerThread::HaltLinks::released_through() const {
    return owner_.orders_.released_through();
}

int64_t SequencerThread::HaltLinks::last_seq_no() const {
    return owner_.replicated_wal_->last_seq_no();
}

int64_t SequencerThread::HaltLinks::next_seq_no() const {
    return owner_.replicated_wal_->next_seq_no();
}

int32_t SequencerThread::HaltLinks::trading_day(int64_t now_ns) {
    return owner_.trading_date_for(now_ns);
}

bool SequencerThread::HaltLinks::voided_already(int64_t seq_no) const {
    return owner_.venue_state_.voids().voiding(seq_no, fix_common::VoidedKind::cancel) != nullptr;
}

CommandInRecord SequencerThread::HaltLinks::command_in_wal_at(int64_t seq_no, bool& found) {
    return owner_.command_in_wal_at(seq_no, found);
}

bool SequencerThread::HaltLinks::write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) {
    return owner_.write_own_encoded_command(event, "TechnicalEvent");
}

bool SequencerThread::HaltLinks::write_void_commands(const pubsub_itc_fw_app::VoidCommands& command) {
    return owner_.write_own_encoded_command(command, "VoidCommands");
}

bool SequencerThread::HaltLinks::write_cancel_book(const pubsub_itc_fw_app::CancelBook& command) {
    return owner_.write_own_encoded_command(command, "CancelBook");
}

void SequencerThread::HaltLinks::send_voids(const pubsub_itc_fw::ConnectionID& connection) {
    owner_.send_voided_commands(connection);
}

void SequencerThread::release_held_orders_through(int64_t acknowledged_seq_no) {
    orders_.release_to_leading_engine(acknowledged_seq_no);
    send_released_records_to_follower_engines();
}

void SequencerThread::OrderLinks::send_orders_that_waited_in_the_wal(int64_t first) {
    const pubsub_itc_fw::ConnectionID engine = owner_.engine_routing_.active();
    if (!owner_.wal_follower_service_->is_being_caught_up(engine)) {
        PUBSUB_LOG(owner_.get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: orders from {} waited in the log -- the reading thread sends them to the matching engine on connection {}", first,
                   engine.get_value());
        owner_.report(pubsub_itc_fw_app::EngineServedFromWal{engine.get_value(), first - 1});
        owner_.serve_engine(engine, first - 1, EngineServing{});
    }
}

void SequencerThread::check_engine_absence() {
    if (config_.replay_mode || !replicated_wal_.has_value()) {
        return;
    }
    presence_.check_absence(config_.wall_clock->now_ns(), std::chrono::steady_clock::now());
}

template <typename Command> bool SequencerThread::write_own_encoded_command(const Command& command, const char* name) {
    // Measured, then encoded into a buffer of the size measured.
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(command, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> bytes(bytes_needed);
    if (!pubsub_itc_fw_app::encode(command, bytes.data(), bytes.size(), bytes_written, bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: a {} needs {} bytes and could not be encoded", name, bytes_needed);
        return false;
    }
    write_own_command(static_cast<int16_t>(Command::message_pdu_id), bytes.data(), bytes_written);
    return true;
}

void SequencerThread::write_recovery_halt(int64_t now_ns) {
    halts_.recovery_halt(now_ns);
    presence_.refresh_order_acceptance(std::chrono::steady_clock::now());
}

void SequencerThread::handle_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::HaltRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode HaltRequest -- dropping");
        return;
    }
    halts_.engine_halt_request(conn_id, request.instance_id, request.epoch, request.reason, config_.wall_clock->now_ns());
}

void SequencerThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) {
    // Only the listener for operators delivers raw bytes to this thread. Every whole request is dealt with, and the bytes
    // of a request not yet whole are left for the next delivery.
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const uint8_t* data = message.payload();
    const auto available = static_cast<int64_t>(message.payload_size());
    constexpr int64_t header_size = sizeof(pubsub_itc_fw::PduHeader);
    // A request is a few dozen bytes; anything much larger is not one.
    constexpr uint32_t largest_request = 4096;
    int64_t consumed = 0;
    while (available - consumed >= header_size) {
        // The header is in network byte order (PduHeader.hpp).
        const uint8_t* header = data + consumed;
        const uint32_t byte_count = (uint32_t{header[0]} << 24) | (uint32_t{header[1]} << 16) | (uint32_t{header[2]} << 8) | uint32_t{header[3]};
        const auto pdu_id = static_cast<int16_t>((uint16_t{header[4]} << 8) | uint16_t{header[5]});
        const uint32_t canary = (uint32_t{header[16]} << 24) | (uint32_t{header[17]} << 16) | (uint32_t{header[18]} << 8) | uint32_t{header[19]};
        if (canary != pubsub_itc_fw::pdu_canary_value || byte_count > largest_request) {
            // Not a stream of requests: everything received is discarded.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: operator connection {} sent bytes that are not a request (canary 0x{:08x}, length {}) -- discarding {} byte(s)",
                       conn_id.get_value(), canary, byte_count, available);
            commit_raw_bytes(conn_id, available);
            return;
        }
        if (available - consumed < header_size + static_cast<int64_t>(byte_count)) {
            break;
        }
        const uint8_t* payload = header + header_size;
        if (pdu_id == pubsub_itc_fw_app::OperatorHaltRequest::message_pdu_id) {
            handle_operator_halt_request(conn_id, payload, byte_count);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "SequencerThread: operator connection {} sent message pdu_id={}, which is not a request -- ignored", conn_id.get_value(), pdu_id);
        }
        consumed += header_size + static_cast<int64_t>(byte_count);
    }
    if (consumed > 0) {
        commit_raw_bytes(conn_id, consumed);
    }
}

void SequencerThread::handle_operator_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, size_t size) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::OperatorHaltRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, payload, size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: operator connection {} sent an OperatorHaltRequest that did not decode -- ignored", conn_id.get_value());
        return;
    }
    // Copied, so that nothing logged or answered points into the request's bytes, which are committed once it is dealt with.
    const std::string operator_name(request.operator_name);
    halts_.operator_request(conn_id, operator_name, request.halt, config_.wall_clock->now_ns());
}

void SequencerThread::HaltLinks::answer_operator(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::OperatorHaltOutcome outcome,
                                                 int64_t technical_event_seq_no, const std::string& explanation) {
    pubsub_itc_fw_app::OperatorHaltAnswer answer{};
    answer.outcome = outcome;
    answer.halted = owner_.venue_state_.trading_halt().halted();
    answer.technical_event_seq_no = technical_event_seq_no;
    answer.explanation = explanation;
    // Encoded after a header of the size the framework puts in front of every message.
    constexpr size_t header_size = sizeof(pubsub_itc_fw::PduHeader);
    std::vector<uint8_t> bytes(header_size + pubsub_itc_fw_app::encoded_size(answer));
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    if (!pubsub_itc_fw_app::encode(answer, bytes.data() + header_size, bytes.size() - header_size, bytes_written, bytes_needed)) {
        PUBSUB_LOG(owner_.get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: an OperatorHaltAnswer needs {} bytes and could not be encoded",
                   bytes_needed);
        return;
    }
    // The header in network byte order (PduHeader.hpp): length, message id, version, then zeros but for the canary.
    const auto byte_count = static_cast<uint32_t>(bytes_written);
    const auto pdu_id = static_cast<uint16_t>(pubsub_itc_fw_app::OperatorHaltAnswer::message_pdu_id);
    bytes[0] = static_cast<uint8_t>(byte_count >> 24);
    bytes[1] = static_cast<uint8_t>(byte_count >> 16);
    bytes[2] = static_cast<uint8_t>(byte_count >> 8);
    bytes[3] = static_cast<uint8_t>(byte_count);
    bytes[4] = static_cast<uint8_t>(pdu_id >> 8);
    bytes[5] = static_cast<uint8_t>(pdu_id);
    bytes[6] = 1;
    bytes[16] = static_cast<uint8_t>(pubsub_itc_fw::pdu_canary_value >> 24);
    bytes[17] = static_cast<uint8_t>(pubsub_itc_fw::pdu_canary_value >> 16);
    bytes[18] = static_cast<uint8_t>(pubsub_itc_fw::pdu_canary_value >> 8);
    bytes[19] = static_cast<uint8_t>(pubsub_itc_fw::pdu_canary_value);
    owner_.send_raw(conn_id, bytes.data(), static_cast<uint32_t>(header_size + bytes_written));
}

void SequencerThread::handle_void_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::VoidRequestView request{};
    if (!pubsub_itc_fw_app::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "SequencerThread: failed to decode VoidRequest -- dropping");
        return;
    }
    halts_.void_request(conn_id, request.instance_id, request.seq_no);
}

void SequencerThread::HaltLinks::decline_void(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, pubsub_itc_fw_app::VoidDeclinedReason reason) {
    owner_.send_pdu(connection, pubsub_itc_fw_app::VoidDeclined::message_pdu_id, 0, pubsub_itc_fw_app::VoidDeclined{seq_no, reason});
}

CommandInRecord SequencerThread::command_in_wal_at(int64_t seq_no, bool& found) {
    found = false;
    if (seq_no <= 0 || seq_no > replicated_wal_->last_seq_no()) {
        return CommandInRecord::OtherRecord;
    }
    try {
        pubsub_itc_fw::WalCursor cursor;
        cursor.open(config_.wal_directory, replicated_wal_->wal().scan_start_for(seq_no));
        int64_t record_id = 0;
        const uint8_t* payload = nullptr;
        size_t size = 0;
        while (cursor.read_next(record_id, payload, size)) {
            if (record_id < seq_no) {
                continue;
            }
            if (record_id > seq_no) {
                break;
            }
            found = true;
            const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
            return contents.has_value() ? command_in_record(*contents, decode_arena_buffer()) : CommandInRecord::NotDecodable;
        }
    } catch (const std::exception& error) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "SequencerThread: reading the WAL for command seq={} failed: {}", seq_no, error.what());
    }
    return CommandInRecord::OtherRecord;
}

void SequencerThread::write_own_command(int16_t inner_pdu_id, const uint8_t* bytes, size_t size) {
    if (size > HeldOrder::own_payload_capacity) {
        throw pubsub_itc_fw::PreconditionAssertion("SequencerThread::write_own_command: the command is larger than a held command can carry", __FILE__,
                                                   __LINE__);
    }
    const int64_t seq = replicated_wal_->next_seq_no();
    const int64_t wall_time_ns = config_.wall_clock->now_ns();
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = seq;
    envelope.pdu_id = inner_pdu_id;
    envelope.payload = pubsub_itc_fw_app::BytesView{bytes, size};
    envelope.wall_time_ns = wall_time_ns;
    envelope.has_leader_epoch = true;
    envelope.leader_epoch = current_epoch();
    envelope.has_trading_date = true;
    envelope.trading_date = trading_date_for(wall_time_ns);
    stamp_reports_forwarded_through(envelope);
    append_envelope_to_wal(envelope);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "SequencerThread: wrote command inner_pdu_id={} at seq={}", inner_pdu_id, seq);

    // Sent to the engine as an order is, once committed and while an engine leads, and never ahead of an order already
    // waiting.
    if (orders_.held_orders_wait_on_the_follower()) {
        replicated_wal_->follower_too_far_behind("its storage for orders waiting on the follower is full", std::chrono::steady_clock::now());
    }
    orders_.hold_own_command(envelope, bytes, size);
    release_held_orders_through(replicated_wal_->committed_through());
}

int32_t SequencerThread::trading_date_for(int64_t wall_time_ns) {
    // The first record's time is read from the WAL only while the date is not known, which is once (Wal::first_wall_time_ns,
    // one small read).
    if (venue_state_.trading_date_known()) {
        return venue_state_.trading_date(std::nullopt, 0, wall_time_ns);
    }
    return venue_state_.trading_date(replicated_wal_->wal().first_wall_time_ns(), replicated_wal_->wal().first_seq_no(), wall_time_ns);
}

std::optional<SequencerThread::ReportInWal> SequencerThread::first_report_answering(const pubsub_itc_fw::CommandInWal& command) {
    const int64_t command_seq_no = command.record_id;
    // Only committed records: a report written later is forwarded by the ordinary path once it is committed.
    const int64_t read_through = std::min(replicated_wal_->committed_through(), command_seq_no + reports_read_for_an_answer_at_most);
    if (read_through <= command_seq_no) {
        return std::nullopt;
    }
    try {
        // From the command's own record when its position is known, reading only the pages that hold the records read,
        // since the report is normally a few records later. Otherwise from the start of the segment that holds the
        // command, which may mean reading most of the segment, so the whole segment is read ahead.
        pubsub_itc_fw::WalCursor cursor;
        if (command.position.has_value()) {
            cursor.open(config_.wal_directory, *command.position, pubsub_itc_fw::WalCursor::ReadAhead::pages_read_only);
        } else {
            cursor.open(config_.wal_directory, replicated_wal_->wal().scan_start_for(command_seq_no + 1));
        }
        int64_t record_id = 0;
        const uint8_t* payload = nullptr;
        size_t size = 0;
        // An arena of its own: the command being answered was decoded into the thread's decode arena, and its fields may
        // point into it. Each record's decoding starts it again from the beginning.
        std::array<uint8_t, 64 * 1024> record_arena_buffer{};
        while (cursor.read_next(record_id, payload, size)) {
            if (record_id > read_through) {
                return std::nullopt;
            }
            if (record_id <= command_seq_no) {
                continue;
            }
            const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
            if (!contents.has_value() || contents->size == 0) {
                continue;
            }
            pubsub_itc_fw::BumpAllocator arena(record_arena_buffer.data(), record_arena_buffer.size());
            size_t consumed = 0;
            size_t needed = 0;
            pubsub_itc_fw_app::WalRecordView stored{};
            if (!pubsub_itc_fw_app::decode(stored, contents->bytes, contents->size, consumed, arena, needed)) {
                continue;
            }
            if (stored.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) && stored.has_answers_seq_no &&
                stored.answers_seq_no == command_seq_no) {
                return ReportInWal{record_id, std::vector<uint8_t>(stored.payload.data, stored.payload.data + stored.payload.size)};
            }
        }
    } catch (const std::exception& error) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerThread: reading the WAL forward from record {} for the report answering it failed: {} -- the command is answered by the "
                   "matching engine, as when the WAL holds no report of it",
                   command_seq_no, error.what());
    }
    return std::nullopt;
}

bool SequencerThread::answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command) {
    const int64_t command_seq_no = command.record_id;
    const std::optional<ReportInWal> found = first_report_answering(command);
    if (!found.has_value()) {
        // The engine has not applied the command yet, or applies it after the change of leader, and its report
        // comes by the ordinary path; or the command is sent from the WAL to an engine that never received it.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "SequencerThread: command ClOrdID={} from comp_id='{}' sent again -- the WAL holds no report of record {} within {} records, so "
                   "the matching engine answers it",
                   inbound.cl_ord_id, inbound.sender_comp_id, command_seq_no, reports_read_for_an_answer_at_most);
        return false;
    }
    const int16_t protocol = inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent;
    const fix_common::SessionIdentity identity = fix_common::SessionIdentity::make(inbound.sender_comp_id, protocol);
    // Where the member's session is now, which after a change of gateway may not be where the command came from.
    const fix_common::SessionDestination* destination = sessions_.destination(identity);
    if (destination == nullptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: command ClOrdID={} from comp_id='{}' sent again is answered by record {} of the WAL, but the session is not bound "
                   "to any gateway -- the report is not sent",
                   inbound.cl_ord_id, inbound.sender_comp_id, found->report_seq_no);
        return false;
    }
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = found->report_seq_no;
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    envelope.payload = pubsub_itc_fw_app::BytesView{found->payload.data(), found->payload.size()};
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = destination->conn_id;
    envelope.has_origin_gateway_id = true;
    envelope.origin_gateway_id = protocol;
    envelope.has_gateway_instance_id = true;
    envelope.gateway_instance_id = destination->instance;
    envelope.has_sender_comp_id = true;
    envelope.sender_comp_id = inbound.sender_comp_id;
    // The gateway marks its copy of the command answered by its comp id and ClOrdID.
    envelope.has_cl_ord_id = true;
    envelope.cl_ord_id = inbound.cl_ord_id;
    // The member may have had this report already, from the leader before this one.
    envelope.poss_resend = true;
    // Not timed: the report says nothing about how long the order path takes.
    const bool is_new_order_ack = false;
    reports_.send_to_gateway(protocol, destination->instance, 0, envelope, is_new_order_ack);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: command ClOrdID={} from comp_id='{}' sent again is answered by the report the WAL holds as record {}, forwarded "
               "again marked as a possible repeat",
               inbound.cl_ord_id, inbound.sender_comp_id, found->report_seq_no);
    report(pubsub_itc_fw_app::ReportSentFromWal{inbound.sender_comp_id, protocol, inbound.cl_ord_id, command_seq_no, found->report_seq_no});
    return true;
}

void SequencerThread::stamp_reports_forwarded_through(pubsub_itc_fw_app::WalRecord& envelope) const {
    const std::optional<EngineReportPosition> through = reports_.forwarded_through();
    envelope.has_reports_forwarded_through_epoch = through.has_value();
    envelope.reports_forwarded_through_epoch = through.has_value() ? through->epoch : 0;
    envelope.has_reports_forwarded_through_number = through.has_value();
    envelope.reports_forwarded_through_number = through.has_value() ? through->number : 0;
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
    orders_.engine_position(view.request_id, view.highest_applied);
}

void SequencerThread::release_orders_that_waited_for_the_engine() {
    // The orders that arrived meanwhile now go the way they would have gone: once their records are
    // committed, which may already be the case.
    release_held_orders_through(replicated_wal_->committed_through());
}

void SequencerThread::publish_released_position() {
    if (replicated_wal_.has_value()) {
        wal_positions_.publish_released(orders_.released_through());
    }
}

bool SequencerThread::an_engine_takes_orders() const {
    if (!engine_routing_.active_is_known_to_lead() || routed_engine_follows_the_wal()) {
        return false;
    }
    // An engine being caught up to lead is sent the held orders live once it has been handed over, so that its reports
    // of them are first reports and not possible repeats. One being sent orders that waited in the log already leads.
    const auto serving = engine_servings_.find(engine_routing_.active().get_value());
    return serving == engine_servings_.end() || !serving->second.answers_catch_up_request;
}

void SequencerThread::note_whether_an_engine_takes_orders() {
    if (!replicated_wal_.has_value()) {
        return;
    }
    presence_.note_whether_an_engine_takes_orders(std::chrono::steady_clock::now());
}

void SequencerThread::send_order_acceptance(const pubsub_itc_fw::ConnectionID& conn_id) {
    // Only the leader has an opinion worth sending. A follower forwards nothing to a matching
    // engine, so it holds nothing for one and its state is permanently "accepting" -- and the gateway
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
    state.accepting = presence_.acceptance() == pubsub_itc_fw_app::OrderAcceptanceReason::Accepting;
    state.held_order_count = presence_.orders_held_without_an_engine();
    state.no_engine_for_seconds =
        static_cast<int32_t>(std::chrono::duration_cast<std::chrono::seconds>(presence_.time_without_an_engine(std::chrono::steady_clock::now())).count());
    state.reason = presence_.acceptance();
    state.has_leader_epoch = true;
    state.leader_epoch = current_epoch();
    send_pdu(conn_id, pubsub_itc_fw_app::OrderAcceptance::message_pdu_id, 0, state);
}

void SequencerThread::broadcast_order_acceptance() {
    reports_.for_each_gateway_connection([this](const pubsub_itc_fw::ConnectionID& conn_id) { send_order_acceptance(conn_id); });
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

    const MemberSessions::Bound bound = sessions_.bind(identity, destination, view.still_connected);
    if (bound.still_connected) {
        reports_.send_held_for_session(identity);
        return;
    }
    // Reports held for this session until it was announced. A FIX member that logs on asks, once its numbering is
    // settled, for every report produced since the last it was delivered (UndeliveredReportsRequest), and those are
    // sent from the WAL; the held ones are among them, so sending them here as well would give the member each one
    // twice. The binary gateway asks for no such thing, so a binary member is sent them here.
    if (identity.protocol == gateway_ids::fix_order_gateway) {
        static_cast<void>(reports_.release_held_for_session(identity));
    } else {
        reports_.send_held_for_session(identity);
    }
    const std::optional<MemberSessions::Resume> resumed =
        sessions_.resume(identity, view.reset_seq_nums, bound.previous_session_died, role_ == pubsub_itc_fw_app::Role::leader, replicated_wal_->last_seq_no());
    if (!resumed.has_value()) {
        return;
    }
    std::vector<pubsub_itc_fw_app::SeqNumRange> wire_ranges;
    wire_ranges.reserve(resumed->report_seq_nums.size());
    for (const fix_common::SeqNumRange& range : resumed->report_seq_nums) {
        wire_ranges.push_back(pubsub_itc_fw_app::SeqNumRange{range.from_seq_num, range.to_seq_num});
    }
    pubsub_itc_fw_app::SessionBoundAck ack{};
    ack.comp_id = identity.comp_id_view();
    ack.gateway_protocol_id = identity.protocol;
    ack.known = resumed->known;
    ack.outbound_seq_num = resumed->outbound_seq_num;
    ack.inbound_seq_num = resumed->inbound_seq_num;
    ack.report_seq_nums = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::SeqNumRange>{wire_ranges.data(), wire_ranges.size()};
    send_pdu(conn_id, pubsub_itc_fw_app::SessionBoundAck::message_pdu_id, 0, ack);
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

bool SequencerThread::SessionLinks::expects_no_session_for(std::string_view comp_id) const {
    return owner_.config_.test_events.expects_no_session_for(comp_id);
}

void SequencerThread::SessionLinks::reports_dropped_unbound(int64_t total) {
    owner_.reports_dropped_unbound_gauge_.set(static_cast<double>(total));
}

void SequencerThread::SessionLinks::report_dropped(const fix_common::SessionIdentity& identity, int64_t er_seq_no) {
    owner_.report(pubsub_itc_fw_app::ReportDroppedSessionNotConnected{identity.comp_id_view(), identity.protocol, er_seq_no});
}

void SequencerThread::SessionLinks::session_announced_again(const fix_common::SessionIdentity& identity, const fix_common::SessionDestination& destination) {
    owner_.report(pubsub_itc_fw_app::SessionAnnouncedAgain{identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id,
                                                           owner_.role_ == pubsub_itc_fw_app::Role::leader});
}

void SequencerThread::SessionLinks::session_unbound(const fix_common::SessionIdentity& identity, int16_t instance, int32_t connection) {
    owner_.report(pubsub_itc_fw_app::SessionUnboundRecorded{identity.comp_id_view(), identity.protocol, instance, connection});
}

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
    MemberSessions::Reported reported{};
    reported.outbound_seq_num = view.outbound_seq_num;
    reported.inbound_seq_num = view.inbound_seq_num;
    reported.report_seq_nums = to_seq_num_ranges(view.report_seq_nums);
    reported.has_last_report_delivered = view.has_last_report_delivered;
    reported.last_report_delivered = view.last_report_delivered;
    sessions_.unbind(identity, view.gateway_instance_id, view.gateway_session_conn_id, reported);
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
    // Answered once for each binding.
    const std::optional<MemberSessions::Undelivered> undelivered = sessions_.take_undelivered(identity);
    if (!undelivered.has_value()) {
        return;
    }
    const int64_t after = undelivered->after;
    const int64_t through = undelivered->through;
    const bool possible_repeat = undelivered->possible_repeat;
    const fix_common::SessionDestination* destination = &undelivered->destination;

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
        reports_.send_to_gateway(identity.protocol, destination->instance, 0, envelope, is_new_order_ack);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: session comp_id='{}' protocol={} bound again -- sent it {} report(s) produced while it had no connection (log records {} to "
               "{}){}",
               identity.comp_id_view(), identity.protocol, found.size(), after + 1, through, possible_repeat ? ", marked as possible repeats" : "");
    report(pubsub_itc_fw_app::UndeliveredReportsSent{identity.comp_id_view(), identity.protocol, static_cast<int64_t>(found.size())});
}

void SequencerThread::ReportLinks::send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) {
    owner_.send_pdu(connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, envelope,
                    pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
}

void SequencerThread::ReportLinks::send_encoded_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const uint8_t* bytes, size_t size) {
    owner_.send_pdu_payload(connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, er_seq_no, bytes, size);
}

void SequencerThread::ReportLinks::acknowledgement_sent(const pubsub_itc_fw_app::WalRecord& envelope) {
    order_path_metrics::observe_checkpoint(owner_.er_out_elapsed_histogram_, envelope.has_gateway_ingress_ns, envelope.gateway_ingress_ns,
                                           owner_.config_.wall_clock->now_ns());
}

bool SequencerThread::ReportLinks::leading() const {
    return owner_.role_ == pubsub_itc_fw_app::Role::leader;
}

bool SequencerThread::ReportLinks::expects_no_session_for(std::string_view comp_id) const {
    return owner_.config_.test_events.expects_no_session_for(comp_id);
}

const fix_common::SessionDestination* SequencerThread::ReportLinks::destination(const fix_common::SessionIdentity& identity) const {
    return owner_.sessions_.destination(identity);
}

void SequencerThread::ReportLinks::report_forwarded(const fix_common::SessionIdentity& identity) {
    owner_.sessions_.report_forwarded(identity);
}

void SequencerThread::ReportLinks::report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no) {
    owner_.sessions_.report_dropped_unbound(identity, er_seq_no);
}

void SequencerThread::ReportLinks::order_ended(int64_t seq_no) {
    owner_.sessions_.order_ended(seq_no);
}

void SequencerThread::ReportLinks::reports_held(const fix_common::SessionIdentity& identity, int32_t gateways_not_announced) {
    owner_.report(pubsub_itc_fw_app::ReportsHeld{identity.comp_id_view(), identity.protocol, gateways_not_announced});
}

void SequencerThread::ReportLinks::held_reports_released(std::string_view comp_id, int16_t protocol, int64_t sent, int64_t dropped) {
    owner_.report(pubsub_itc_fw_app::HeldReportsReleased{comp_id, protocol, sent, dropped});
}

void SequencerThread::ReportLinks::report_dropped_gateway_not_connected(int16_t protocol, int16_t instance, int64_t er_seq_no) {
    owner_.report(pubsub_itc_fw_app::ReportDroppedGatewayNotConnected{protocol, instance, er_seq_no});
}

void SequencerThread::ReportLinks::reports_held_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no) {
    owner_.report(pubsub_itc_fw_app::ReportsHeldForGateway{protocol, instance, er_seq_no});
}

void SequencerThread::ReportLinks::reports_sent_to_gateway(int16_t protocol, int16_t instance, int64_t sent, int64_t dropped) {
    owner_.report(pubsub_itc_fw_app::ReportsSentToGateway{protocol, instance, sent, dropped});
}

void SequencerThread::ReportLinks::held_reports_dropped(int64_t total) {
    owner_.held_reports_dropped_gauge_.set(static_cast<double>(total));
}

std::vector<std::pair<int16_t, int16_t>> SequencerThread::configured_gateways(const SequencerConfiguration& config) {
    std::vector<std::pair<int16_t, int16_t>> gateways;
    gateways.reserve(config.gateway_endpoints.size());
    for (const auto& endpoint : config.gateway_endpoints) {
        gateways.emplace_back(endpoint.protocol, endpoint.instance);
    }
    return gateways;
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
    reports_.sessions_announced(view.gateway_protocol_id, view.gateway_instance_id, conn_id, view.sessions_announced);
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
    MemberSessions::Reported reported{};
    reported.outbound_seq_num = view.outbound_seq_num;
    reported.inbound_seq_num = view.inbound_seq_num;
    reported.report_seq_nums = to_seq_num_ranges(view.report_seq_nums);
    reported.has_last_report_delivered = view.has_last_report_delivered;
    reported.last_report_delivered = view.last_report_delivered;
    sessions_.sequence_update(identity, reported);
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
    // The reading thread sends it every record after its position, up to the last committed, and the service hands it
    // over (SequencerWalFollowing::follower_handed_over), after which it is sent each record from here, as it is
    // committed.
    wal_follower_service_->on_follow_request(conn_id, view, leading, pubsub_itc_fw::FollowerLimit::LastCommitted, publisher_converter, 0);
}

void SequencerThread::send_released_records_to_follower_engines() {
    if (!handed_over_engines_.has_value() || handed_over_engines_->empty()) {
        return;
    }
    for (const auto& [engine, damaged_seq_no] : handed_over_engines_->send_to_all(orders_.released_through())) {
        report_follower_cannot_be_served(engine, damaged_seq_no, "a matching engine that follows the WAL");
    }
}

void SequencerThread::send_committed_records_to_publishers(int64_t seq_no) {
    if (!handed_over_publishers_.has_value() || handed_over_publishers_->empty()) {
        return;
    }
    for (const auto& [publisher, damaged_seq_no] : handed_over_publishers_->send_to_all(seq_no)) {
        report_follower_cannot_be_served(publisher, damaged_seq_no, "a matching engine publisher");
    }
}

bool SequencerThread::routed_engine_follows_the_wal() const {
    return handed_over_engines_.has_value() && handed_over_engines_->contains(engine_routing_.active());
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
    const EngineOrderRouting::AnnouncementOutcome outcome = engine_routing_.announced(announcement.instance_id, announcement.current_role, announcement.epoch);
    if (outcome != EngineOrderRouting::AnnouncementOutcome::RefusedAsBehind) {
        presence_.engine_announced(announcement.instance_id, announcement.current_role == pubsub_itc_fw_app::Role::leader, conn_id);
    }
    switch (outcome) {
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
                       "sequencer knows of no leader to hand it to. Orders are held until an engine leads",
                       announcement.instance_id, conn_id.get_value(), announcement.epoch);
            break;
    }
    engine_routing_changed();
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
    const bool asking_to_lead = view.to_lead;
    // Whatever this connection was sent as a follower of the WAL stops: the engine has asked afresh, from the last command
    // it applied, to follow or to lead.
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->remove(conn_id);
    }

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
        wal_follower_service_->on_follow_request(conn_id, view, pubsub_itc_fw::Leading::DoesNotLead, pubsub_itc_fw::FollowerLimit::LastReleased,
                                                 engine_converter, 0);
        return;
    }

    // Every void, before the reading thread is asked to start: this thread and the reading thread send through the
    // reactor's one queue of commands, so the engine is sent the list before WalFollowStarted and before any record the
    // list names (docs/availability/voided_commands.md, section 3.3).
    send_voided_commands(conn_id);

    // Only orders already released to the engine: one still waiting for the follower's
    // acknowledgement, or for a voter's confirmation, is sent live when it is released, and sending it
    // here as well would act on it early.
    const int64_t wal_head = orders_.released_through();

    // The start of the answer, sent before the first record. Its request number is echoed on WalFollowCaughtUp, so that
    // the engine can tell this answer from an earlier one still on its way.
    //
    // It carries the last command the WAL holds every report of, which the reading thread reads from the newest records,
    // so that the engine does not send those reports again: they reach their members through the sequencers
    // (docs/availability/far_back_catch_up.md, section 2). With [catch_up] send_every_report it carries 0, and the
    // engine sends every report again.
    pubsub_itc_fw::WalFollowStart started{};
    started.message.request_number = view.request_number;
    started.message.after_seq_no = last_seq_no;
    started.message.serving_through_seq_no = wal_head;
    started.message.application_value = 0;
    started.application_value_from_wal = !config_.send_every_report;
    EngineServing serving{};
    serving.answers_catch_up_request = true;
    serving.asking_to_lead = asking_to_lead;
    serving.follows_the_wal = !asking_to_lead;
    serving.started = started;

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
                   "SequencerThread: WalFollowRequest from connection {} carries no position -- the engine holds nothing and is placed at head={} without "
                   "catch-up. Any order taken and not yet applied is not recovered by this instance",
                   conn_id.get_value(), wal_head);
        // Through the reading thread, so that the start of the answer reaches the engine after anything it was still
        // sending this connection: it sends the engine only the orders released after the head. An engine that asked to
        // lead, which it does only holding a lease, or always with high availability off, is then sent orders, and the
        // orders held while no engine led go to it (docs/availability/voided_commands.md, section 5.1); one that is
        // starting leaves order routing where it is.
        serve_engine(conn_id, wal_head, serving);
        return;
    }

    // The reading thread reads the WAL and sends the orders, and this thread carries on; the engine is answered when it
    // is handed over (engine_handed_over). Orders released meanwhile reach it too.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: WalFollowRequest from connection {} last_seq_no={} ({}) -- the reading thread sends it the orders after that, up "
               "to the last released, now {}",
               conn_id.get_value(), last_seq_no, asking_to_lead ? "asking to lead" : "starting, not asking to lead", wal_head);
    serve_engine(conn_id, last_seq_no, serving);
}

void SequencerThread::serve_engine(const pubsub_itc_fw::ConnectionID& engine, int64_t after_seq_no, EngineServing serving) {
    publish_released_position();
    engine_servings_[engine.get_value()] = serving;
    // Every engine is served up to the last record released, so that none applies an order before the engine that leads
    // is sent it (docs/availability/voided_commands.md, section 5.1). An engine that is to lead is sent the commands, as
    // order routing sends them. One that follows the WAL is sent every record, reports included, as a publisher is, so
    // that it can tell that none is missing and which reports the WAL holds.
    const int converter = serving.follows_the_wal ? publisher_converter : engine_converter;
    wal_follower_service_->serve(engine, after_seq_no, pubsub_itc_fw::FollowerLimit::LastReleased, converter, pubsub_itc_fw::CaughtUpMessage::DoNotSend,
                                 serving.started);
}

void SequencerThread::engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent, pubsub_itc_fw::WalPosition resume_at,
                                         EngineServing serving) {
    presence_.catch_up_ended(engine);
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
    caught_up.request_number = serving.started.has_value() ? serving.started->message.request_number : 0;
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
        // From here it is sent each record as it is committed, read on from where the reading thread stopped.
        if (serving.follows_the_wal && handed_over_engines_.has_value()) {
            handed_over_engines_->add(engine, through, resume_at);
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "SequencerThread: connection {} caught up but is not to lead -- order routing left where it is, and it is sent each record as it is "
                   "committed, following the WAL",
                   engine.get_value());
        return;
    }
    engine_routing_.caught_up_to_lead(engine);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "SequencerThread: ME order connection promoted to {} -- sequenced orders now route to the caught-up ME", engine.get_value());
    // The orders held while it was caught up go to it now, live (docs/availability/voided_commands.md, section 5.1).
    engine_routing_changed();
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
