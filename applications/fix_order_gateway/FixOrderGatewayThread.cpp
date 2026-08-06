// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixOrderGatewayThread.hpp"
#include "FixErEncoder.hpp"
#include "FixGroupExtractor.hpp"
#include "GatewayMetrics.hpp"

#include <openssl/rand.h>

#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace fix_order_gateway {

namespace {

constexpr size_t scram_client_nonce_size = 16;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

// A named helper rather than a designated initialiser at the call site: this project builds
// as C++17, where designated initialisers are a C++20 feature and -Werror rejects them.
pubsub_itc_fw::ApplicationThreadConfiguration make_thread_config() {
    pubsub_itc_fw::ApplicationThreadConfiguration configuration;
    // The same scope token the binary gateway uses: it names this thread's role, and the two
    // are told apart by the component label, which is the process instance. That is what
    // makes the FIX-versus-binary comparison a group-by rather than two separate metrics.
    configuration.metrics_scope = "gateway_thread";
    return configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const FixOrderGatewayConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "FixOrderGatewayPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

// Parses a FIX SendingTime (tag 52) string to nanoseconds since the Unix epoch.
// Accepted formats: "YYYYMMDD-HH:MM:SS" and "YYYYMMDD-HH:MM:SS.sss".
// Returns 0 if the string is empty or malformed.
int64_t parse_fix_utc_timestamp(std::string_view sv) {
    if (sv.size() < 17) {
        return 0;
    }

    auto parse_digits = [&](size_t offset, size_t count) -> int {
        int value = 0;
        auto result = std::from_chars(sv.data() + offset, sv.data() + offset + count, value);
        return (result.ec == std::errc{}) ? value : -1;
    };

    const int year = parse_digits(0, 4);
    const int month = parse_digits(4, 2);
    const int day = parse_digits(6, 2);
    const int hour = parse_digits(9, 2);
    const int minute = parse_digits(12, 2);
    const int second = parse_digits(15, 2);

    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return 0;
    }

    struct tm utc_tm {};
    utc_tm.tm_year = year - 1900;
    utc_tm.tm_mon = month - 1;
    utc_tm.tm_mday = day;
    utc_tm.tm_hour = hour;
    utc_tm.tm_min = minute;
    utc_tm.tm_sec = second;

    const time_t epoch_seconds = timegm(&utc_tm);
    if (epoch_seconds == static_cast<time_t>(-1)) {
        return 0;
    }

    int64_t millis = 0;
    if (sv.size() >= 21 && sv[17] == '.') {
        millis = parse_digits(18, 3);
        if (millis < 0) {
            millis = 0;
        }
    }

    return static_cast<int64_t>(epoch_seconds) * 1'000'000'000LL + millis * 1'000'000LL;
}

// Orders between progress lines. The per-order GW-NOS-RECV / GW-ER-SENT lines are at Debug
// because at 200,000 orders they cost around a third of the gateway's CPU in Quill alone --
// more than the entire FIX parse. A running total every thousand keeps the counts an operator
// and the perf harness need, at a thousandth of the cost.
constexpr int64_t order_progress_interval = 1000;

constexpr int cancel_drain_batch_size = 500;
constexpr auto cancel_drain_interval = std::chrono::milliseconds{1};

bool is_terminal_ord_status(pubsub_itc_fw_app::OrdStatus status) {
    switch (status) {
        case pubsub_itc_fw_app::OrdStatus::Filled:
        case pubsub_itc_fw_app::OrdStatus::Canceled:
        case pubsub_itc_fw_app::OrdStatus::DoneForDay:
        case pubsub_itc_fw_app::OrdStatus::Rejected:
        case pubsub_itc_fw_app::OrdStatus::Expired:
            return true;
        default:
            return false;
    }
}

} // namespaces

FixOrderGatewayThread::FixOrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                             pubsub_itc_fw::Reactor& reactor, const FixOrderGatewayConfiguration& config)
    : ApplicationThread(token, logger, reactor, "FixOrderGatewayThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        make_thread_config())
    , config_(config)
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , serialiser_(config.sender_comp_id, config.default_target_comp_id, *config.wall_clock)
    , auth_service_primary_conn_id_{}
    , auth_service_secondary_conn_id_{}
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{}
    , capture_(config.fix_capture_enabled ? std::make_unique<FixCapture>(config.fix_capture_file, logger, static_cast<size_t>(config.fix_capture_ring_bytes))
                                          : nullptr) {
    if (capture_) {
        register_extra_thread(capture_->writer_pthread_id(), "FixCaptureWriter");
    }
    // Registered here rather than in the initialiser list because the handle is a value and
    // default-constructs unbound, so unconfigured bounds simply leave it recording
    // nowhere. The application and component tokens come from configuration; only the scope
    // and the metric name are named here. See docs/design/metrics.md.
    if (!config_.order_round_trip_buckets.empty()) {
        order_round_trip_histogram_ = get_reactor().metrics().register_histogram("gateway_thread", gateway_metrics::order_round_trip_metric_name,
                                                                                 gateway_metrics::order_round_trip_help, config_.order_round_trip_buckets);
    }

    // Start the reusable ER wire buffer at the common-case size; the ER send path grows
    // it if a large ExecutionReport (many echoed group instances) needs more.
    er_wire_buffer_.resize(execution_report_initial_buffer_size);
    group_arena_buffer_.resize(initial_group_arena_size);
}

void FixOrderGatewayThread::on_app_ready_event() {
    // Initialise the open-order pool.
    open_order_pool_ = std::make_unique<pubsub_itc_fw::ExpandablePoolAllocator<OpenOrderEntry>>(
        "OpenOrderPool", config_.open_order_pool_objects_per_pool, config_.open_order_pool_initial_pools,
        /*expansion_threshold_hint=*/0,
        /*handler_for_pool_exhausted=*/
        [this](void* /*context*/, int objects_per_pool) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OpenOrderPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
        },
        /*handler_for_invalid_free=*/nullptr,
        /*handler_for_huge_pages_error=*/nullptr, pubsub_itc_fw::UseHugePagesFlag{pubsub_itc_fw::UseHugePagesFlag::DoNotUseHugePages});

    connect_to_service("authentication_service_primary");
    if (config_.ha_enabled) {
        connect_to_service("authentication_service_secondary");
    }
    connect_to_service("sequencer_primary");
    if (config_.ha_enabled) {
        connect_to_service("sequencer_secondary");
    }
}

void FixOrderGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "authentication_service_primary") {
        auth_service_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: primary authentication service connection {} established",
                   id.get_value());
    } else if (id.service_name() == "authentication_service_secondary") {
        auth_service_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: secondary authentication service connection {} established",
                   id.get_value());
    } else if (id.service_name() == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: primary sequencer connection {} ({}) established", id.get_value(),
                   id.service_name());
    } else if (id.service_name() == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: secondary sequencer connection {} established", id.get_value());
    } else if (id.service_name() == er_inbound_svc_) {
        // Inbound FrameworkPdu connection from a sequencer on the ER listener.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: sequencer ER inbound connection {} established", id.get_value());
    } else {
        // Inbound RawBytes connection -- FIX client on port 9879.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: FIX client connection {} ({}) established -- active sessions: {}",
                   id.get_value(), id.service_name(), sessions_.size() + 1);
    }
}

void FixOrderGatewayThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        FixSession& session = it->second;
        cancel_timer(session.logon_timeout_timer_id);
        cancel_timer(session.scram_auth_timeout_timer_id);
        // Told before the session object goes: the sequencer addresses reports at this
        // connection, and once it is gone there is nothing here to receive them. Announced
        // rather than inferred because the sequencer cannot see a client socket close.
        announce_session_unbound(session);
        queue_session_for_cleanup(session);
        sessions_.erase(it);
    }

    if (id == auth_service_primary_conn_id_) {
        auth_service_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: primary authentication service connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id == auth_service_secondary_conn_id_) {
        auth_service_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: secondary authentication service connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: primary sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: secondary sequencer connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id.service_name() == er_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: sequencer ER inbound connection {} lost: {}", id.get_value(),
                   reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: FIX client connection {} lost: {} -- active sessions: {}",
                   id.get_value(), reason, sessions_.size());
    }
}

void FixOrderGatewayThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const uint8_t* data = message.payload();
    const int available = message.payload_size();
    const int64_t event_tail_position = message.tail_position();

    if (data == nullptr || available <= 0) {
        return;
    }

    // The gateway's ingress instant, taken before anything is parsed, which is as close to
    // "the order entered the venue" as this process can observe. It is stamped on every
    // order forwarded out of this event and returned on the acknowledging ExecutionReport,
    // where it becomes the start of the round-trip measurement.
    //
    // Per read event, not per message: several orders can arrive in one TCP read, and they
    // did all arrive at this instant. Timing each separately would need a clock read per
    // message on the hot path to measure a difference that is parsing cost, not venue
    // latency -- and would flatter the second order in every batch.
    current_read_ingress_ns_ = config_.wall_clock->now_ns();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixOrderGatewayThread: {} raw bytes received on connection {} ({}) at tail {}", available,
               conn_id.get_value(), conn_id.service_name(), event_tail_position);

    // Hex-dump only when Trace logging is active, and only the first few hundred
    // bytes. The hex string is expensive to build so we guard it with a level check.
    if (get_logger().log_level() <= pubsub_itc_fw::FwLogLevel{pubsub_itc_fw::FwLogLevel::Trace}) {
        constexpr int hex_dump_limit = 256;
        const int hex_dump_len = (available < hex_dump_limit) ? available : hex_dump_limit;
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Trace, pubsub_itc_fw::StringUtils::hex_dump(data, hex_dump_len));
    }

    // Create a session on first data from this connection if not already present.
    // piecewise_construct is required because FixSession has no copy or move constructor
    // (it captures a lambda referencing its own members). std::map::emplace with a direct
    // value argument would try to copy/move-construct; piecewise_construct with
    // forward_as_tuple constructs the key and value in-place directly inside the map node,
    // bypassing any copy or move.

    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) {
        sessions_.emplace(
            std::piecewise_construct, std::forward_as_tuple(conn_id),
            std::forward_as_tuple(
                conn_id, get_logger(),
                [this, conn_id](const ParsedFixMessage& msg, const fix_codec::FixMessageReader& reader) {
                    auto sit = sessions_.find(conn_id);
                    if (sit == sessions_.end()) {
                        return;
                    }
                    FixSession& session = sit->second;
                    const std::string_view type = msg.msg_type();

                    if (type == MsgType::Logon) {
                        handle_logon(session, msg);
                    } else if (!session.session_established) {
                        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                                   "FixOrderGatewayThread: connection {} ({}) MsgType='{}' before Logon -- disconnecting", conn_id.get_value(),
                                   conn_id.service_name(), type);
                        disconnect_session(session, "first message was not Logon");
                    } else if (type == MsgType::Heartbeat) {
                        handle_heartbeat(session, msg);
                    } else if (type == MsgType::TestRequest) {
                        handle_test_request(session, msg);
                    } else if (type == MsgType::Logout) {
                        handle_logout(session, msg);
                    } else if (type == MsgType::ResendRequest) {
                        handle_resend_request(session, msg);
                    } else if (type == MsgType::NewOrderSingle) {
                        handle_new_order_single(session, msg, reader);
                    } else if (type == MsgType::OrderCancelRequest) {
                        handle_order_cancel_request(session, msg);
                    } else {
                        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} ({}) ignoring MsgType='{}'",
                                   conn_id.get_value(), conn_id.service_name(), type);
                    }
                },
                [this, conn_id](const ParsedFixMessage& msg, const fix_codec::FixReject& reject) {
                    auto sit = sessions_.find(conn_id);
                    if (sit == sessions_.end()) {
                        return;
                    }
                    FixSession& session = sit->second;
                    char text[128];
                    const std::string_view description = reject.describe(text, sizeof(text));
                    // A message that fails validation before the session is established, or an
                    // invalid Logon, cannot be handled -- disconnect. An established session
                    // gets a FIX Reject (35=3).
                    if (msg.msg_type() == MsgType::Logon || !session.session_established) {
                        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                                   "FixOrderGatewayThread: connection {} MsgType='{}' failed FIX validation ({}) before session established -- disconnecting",
                                   conn_id.get_value(), msg.msg_type(), description);
                        disconnect_session(session, "message failed FIX validation");
                    } else {
                        send_fix_reject(session, msg, reject);
                    }
                }));

        sessions_.at(conn_id).logon_timeout_timer_id = start_one_off_timer(config_.logon_timeout);

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} new FIX session, waiting for Logon "
                   "(timeout {}s) -- active sessions: {}",
                   conn_id.get_value(), config_.logon_timeout.count(), sessions_.size());

        it = sessions_.find(conn_id);
    }

    FixSession& session = it->second;

    // Cumulative-bytes contract reconciliation.
    //
    // payload_size() is the total unconsumed bytes in the MirroredBuffer
    // at enqueue time -- it INCLUDES bytes from previous events that we
    // have already fed to the parser and asked the reactor to commit, if
    // those commits have not yet landed on the tail.
    //
    // The tail can advance partially relative to our in-flight commits:
    // the reactor processes them one at a time. So a naive "tail changed
    // since last event" check would treat the new event as entirely fresh,
    // re-feeding bytes we already fed. The fix is to track absolute byte
    // offsets independent of the reactor's tail value.
    //
    // - absolute_head_seen_     = max event_tail + event_payload_size
    // - absolute_bytes_committed_ = total bytes ever asked to commit
    // - new bytes this event    = absolute_head_seen_ - absolute_bytes_committed_
    // - offset within event window = absolute_bytes_committed_ - event_tail
    //
    // See FixSession.hpp for the full rationale.
    const int64_t event_absolute_head = event_tail_position + static_cast<int64_t>(available);
    if (event_absolute_head > session.absolute_head_seen_) {
        session.absolute_head_seen_ = event_absolute_head;
    }

    const int64_t new_bytes_len64 = session.absolute_head_seen_ - session.absolute_bytes_committed_;
    if (new_bytes_len64 <= 0) {
        // Nothing new in this event; our pending commits will catch up.
        return;
    }

    const int64_t window_offset64 = session.absolute_bytes_committed_ - event_tail_position;
    // window_offset must be within [0, available - new_bytes_len]. Defensive
    // check guards against arithmetic surprises (e.g. an unexpected wrap or
    // a bug in absolute_bytes_committed_).
    if (window_offset64 < 0 || window_offset64 + new_bytes_len64 > static_cast<int64_t>(available)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "FixOrderGatewayThread: connection {} cumulative-bytes invariant violation: "
                   "event_tail={} payload_size={} absolute_head_seen={} absolute_bytes_committed={} "
                   "-- disconnecting",
                   conn_id.get_value(), event_tail_position, available, session.absolute_head_seen_, session.absolute_bytes_committed_);
        disconnect_session(session, "cumulative-bytes invariant violation");
        return;
    }

    const int new_bytes_len = static_cast<int>(new_bytes_len64);
    const uint8_t* new_bytes_ptr = data + static_cast<std::ptrdiff_t>(window_offset64);

    if (!session.preamble_verified) {
        // Preamble check operates on the full unconsumed window starting at
        // `data`. payload_size() is cumulative so `data` still points at
        // the first unverified byte of the session even after multiple
        // events; we have not committed anything yet.
        const size_t bytes_to_check = std::min(static_cast<size_t>(available), expected_preamble.size());

        if (std::memcmp(data, expected_preamble.data(), bytes_to_check) != 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} invalid FIX preamble -- disconnecting",
                       conn_id.get_value());
            disconnect_session(session, "invalid FIX preamble");
            // Commit only the new bytes so the buffer drains correctly.
            commit_raw_bytes(conn_id, static_cast<int64_t>(new_bytes_len));
            session.absolute_bytes_committed_ += new_bytes_len;
            return;
        }

        if (static_cast<size_t>(available) < expected_preamble.size()) {
            // Not enough bytes yet for a full preamble check; don't commit
            // anything (the bytes need to remain in the buffer so the next
            // event sees them too).
            commit_raw_bytes(conn_id, 0);
            return;
        }

        session.preamble_verified = true;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} FIX preamble verified", conn_id.get_value());
    }

    // feed() returns the number of bytes fully consumed by complete FIX messages.
    // Partial-message bytes at the end of the window are not consumed; the
    // MirroredBuffer retains them by advancing its read pointer only by the
    // consumed count. On the next event the window will begin at those partial
    // bytes, followed by whatever new TCP data arrived, and the parser picks up
    // exactly where it left off.
    const size_t consumed = session.parser.feed(new_bytes_ptr, static_cast<size_t>(new_bytes_len));
    if (capture_ != nullptr && consumed > 0) {
        capture_->capture(FixCapture::Direction::Inbound, new_bytes_ptr, consumed, config_.wall_clock->now_ns());
    }
    commit_raw_bytes(conn_id, static_cast<int64_t>(consumed));
    session.absolute_bytes_committed_ += static_cast<int64_t>(consumed);
}

void FixOrderGatewayThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();

    const bool from_auth_service =
        (message.connection_id() == auth_service_primary_conn_id_) || (config_.ha_enabled && message.connection_id() == auth_service_secondary_conn_id_);
    if (from_auth_service) {
        if (pdu_id == pubsub_itc_fw_app::AuthenticationChallenge::message_pdu_id) {
            handle_authentication_challenge(message);
        } else if (pdu_id == pubsub_itc_fw_app::AuthenticationResult::message_pdu_id) {
            handle_authentication_result(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: unexpected PDU id {} from authentication service -- dropping",
                       pdu_id);
            release_pdu_payload(message);
        }
        return;
    }

    // Session PDUs from the sequencer. They arrive on the connection this gateway already
    // holds outbound to the sequencer -- the same one the bindings and the replay request
    // went out on -- rather than on the inbound listener live reports come in on.
    if (pdu_id == pubsub_itc_fw_app::SessionBoundAck::message_pdu_id) {
        handle_session_bound_ack(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::SessionReplayRecord::message_pdu_id) {
        handle_session_replay_record(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::SessionReplayComplete::message_pdu_id) {
        handle_session_replay_complete(message);
        release_pdu_payload(message);
        return;
    }

    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: unsupported PDU id {} on connection {} -- dropping", pdu_id,
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;

    // The sequencer forwards the ExecutionReport wrapped in a WalRecord envelope;
    // the routing metadata (gateway_session_conn_id) rides on the envelope, not
    // inside the DD-derived PDU. Unwrap the envelope, then decode the inner ER.
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: failed to decode WalRecord envelope -- dropping");
        release_pdu_payload(message);
        return;
    }
    if (envelope.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: envelope carries unexpected pdu_id {} -- dropping",
                   envelope.pdu_id);
        release_pdu_payload(message);
        return;
    }

    pubsub_itc_fw_app::ExecutionReportView view{};
    if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: failed to decode ExecutionReport -- dropping");
        release_pdu_payload(message);
        return;
    }

    // Route to the exact FIX session identified by gateway_session_conn_id, which
    // the gateway stamped on the original NOS envelope and the sequencer echoes here.
    if (!envelope.has_gateway_session_conn_id) {
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: ExecutionReport OrderID={} ExecID={} has no gateway_session_conn_id -- dropping", view.order_id, view.exec_id);
        release_pdu_payload(message);
        return;
    }

    FixSession* session_ptr = find_session_by_conn_id(envelope.gateway_session_conn_id);
    if (!session_ptr) {
        // Expected after a client disconnect: in-flight ERs and cancel ACKs
        // arrive for sessions that are already gone.
        ++execution_reports_dropped_;
        report_order_progress();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "FixOrderGatewayThread: ExecutionReport gateway_session_conn_id={} -- "
                   "client already disconnected -- dropping",
                   envelope.gateway_session_conn_id);
        release_pdu_payload(message);
        return;
    }
    FixSession& session = *session_ptr;

    // A resend is running for this session: hold this report until it finishes.
    //
    // The member is being handed a numbered sequence of messages it missed. A live report
    // sent into the middle of that would take a sequence number the resend still needs, and
    // the member would see the numbering jump. Held here and delivered in order once the
    // replay completes -- the window is one round trip to the sequencer.
    if (session.replay_in_progress) {
        session.deferred_execution_reports.emplace_back(envelope.payload.data, envelope.payload.data + envelope.payload.size);
        release_pdu_payload(message);
        return;
    }

    // Maintain the open-orders set from ME acknowledgements (not from NOS
    // forward time) so only orders genuinely on the book are tracked.
    // The view's string_views are backed by the PDU payload, which is valid
    // until release_pdu_payload() below, so copies are safe here.
    if (view.has_cl_ord_id) {
        if (is_terminal_ord_status(view.ord_status)) {
            // Erase without constructing a std::string -- string_view lookup
            // compares contents so this finds the entry keyed by pool storage.
            auto it = session.open_orders.find(std::string_view(view.cl_ord_id));
            if (it != session.open_orders.end()) {
                open_order_pool_->deallocate(it->second);
                session.open_orders.erase(it);
            }
        } else {
            // Allocate a pool entry and copy all string fields inline.
            OpenOrderEntry* entry = open_order_pool_->allocate();
            const size_t clen = view.cl_ord_id.size();
            const size_t slen = view.symbol.size();
            const size_t qlen = view.has_order_qty ? view.order_qty.size() : 0;
            std::memcpy(entry->cl_ord_id, view.cl_ord_id.data(), clen);
            entry->cl_ord_id[clen] = '\0';
            entry->cl_ord_id_len = static_cast<uint8_t>(clen);
            std::memcpy(entry->symbol, view.symbol.data(), slen);
            entry->symbol[slen] = '\0';
            entry->symbol_len = static_cast<uint8_t>(slen);
            if (qlen > 0) {
                std::memcpy(entry->order_qty, view.order_qty.data(), qlen);
            }
            entry->order_qty[qlen] = '\0';
            entry->order_qty_len = static_cast<uint8_t>(qlen);
            entry->side = static_cast<char>(view.side);
            // Kept so the cancel-on-disconnect drain can leave persistent orders resting.
            // Absent means the client sent no tag 59, which implies Day and claims no
            // exemption -- so zero rather than a defaulted enum value.
            entry->time_in_force = view.has_time_in_force ? static_cast<char>(view.time_in_force) : char{0};
            // Key is string_view into pool storage -- stable for entry lifetime.
            session.open_orders.insert_or_assign(std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), entry);
        }
    }

    ++execution_reports_sent_;
    report_order_progress();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "GW-ER-SENT connection={} OrderID={} ExecID={} gateway_session_conn_id={}",
               session.conn_id.get_value(), view.order_id, view.exec_id, envelope.gateway_session_conn_id);

    // Encode the FIX ExecutionReport into a reusable, growable buffer. encode_execution_report
    // returns an empty view if the buffer is too small (a large ER -- many echoed group
    // instances -- can exceed the starting size), so grow and retry rather than cap the size
    // and silently drop the ER. The buffer grows to the high-water mark and is reused, so no
    // per-ER allocation after warmup. The wire view does not begin at the buffer start --
    // FixMessageWriter frames the header backward.
    if (!send_execution_report_to_session(session, view, /*poss_dup=*/false, 0)) {
        release_pdu_payload(message);
        return;
    }

    // The round trip closes here: the ER is encoded and about to be handed to the reactor
    // for sending. What happens to it afterwards -- kernel, wire, client -- this process
    // cannot see, and is not what the venue is answerable for.
    //
    // Only the ER that acknowledges a new order is measured. Every ER for an order carries
    // the same ingress stamp, so a later Canceled ER would otherwise be recorded as a round
    // trip lasting as long as the order rested on the book. A cancel's own round trip is a
    // different measurement of a different thing.
    if (envelope.has_gateway_ingress_ns && view.ord_status == pubsub_itc_fw_app::OrdStatus::New) {
        const int64_t round_trip_ns = config_.wall_clock->now_ns() - envelope.gateway_ingress_ns;
        // A negative delta is not a fast order. The two ends are stamped by different
        // processes after a gateway failover -- the instance that took the session over
        // sends this ER, and its clock shares no origin with the one that read the order.
        // Recording it would put a nonsense value into _sum and quietly bias every average
        // drawn from the family thereafter, which is not recoverable by any later query.
        if (round_trip_ns >= 0) {
            order_round_trip_histogram_.observe(static_cast<double>(round_trip_ns));
        }
    }

    release_pdu_payload(message);
}

bool FixOrderGatewayThread::send_execution_report_to_session(FixSession& session, const pubsub_itc_fw_app::ExecutionReportView& view, bool poss_dup,
                                                             int64_t orig_sending_time_ns) {
    // Encode the FIX ExecutionReport into a reusable, growable buffer. encode_execution_report
    // returns an empty view if the buffer is too small (a large ER -- many echoed group
    // instances -- can exceed the starting size), so grow and retry rather than cap the size
    // and silently drop the ER. The buffer grows to the high-water mark and is reused, so no
    // per-ER allocation after warmup. The wire view does not begin at the buffer start --
    // FixMessageWriter frames the header backward.
    //
    // Shared by the live path and the resend path so the two cannot drift: a resent report
    // differs from a live one only in PossDupFlag and OrigSendingTime, and everything else
    // about it -- encoding, buffer growth, capture, sequence numbering -- must be identical
    // or the member is being told something subtly different about the same event.
    std::string_view wire = encode_execution_report(view, config_.sender_comp_id, session.client_comp_id, session.outbound_seq_num, *config_.wall_clock,
                                                    er_wire_buffer_.data(), er_wire_buffer_.size(), poss_dup, orig_sending_time_ns);
    while (wire.empty() && er_wire_buffer_.size() < max_execution_report_buffer_size) {
        er_wire_buffer_.resize(er_wire_buffer_.size() * 2);
        wire = encode_execution_report(view, config_.sender_comp_id, session.client_comp_id, session.outbound_seq_num, *config_.wall_clock,
                                       er_wire_buffer_.data(), er_wire_buffer_.size(), poss_dup, orig_sending_time_ns);
    }
    if (wire.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "FixOrderGatewayThread: connection {} ExecutionReport too large to encode (>{} bytes) -- not sent", session.conn_id.get_value(),
                   er_wire_buffer_.size());
        return false;
    }
    ++session.outbound_seq_num;
    if (capture_ != nullptr) {
        capture_->capture(FixCapture::Direction::Outbound, reinterpret_cast<const uint8_t*>(wire.data()), wire.size(), config_.wall_clock->now_ns());
    }
    std::string readable_er(wire);
    for (char& c : readable_er) {
        if (c == '\x01')
            c = '|';
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixOrderGatewayThread: connection {} FIX OUT{} ({} bytes): {}", session.conn_id.get_value(),
               poss_dup ? " (resend)" : "", wire.size(), readable_er);

    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
    return true;
}

FixSession* FixOrderGatewayThread::find_session_by_comp_id(std::string_view comp_id) {
    for (auto& [conn_id, session] : sessions_) {
        if (session.client_comp_id == comp_id) {
            return &session;
        }
    }
    return nullptr;
}

FixSession* FixOrderGatewayThread::find_session_by_replay_request(int64_t request_id) {
    if (request_id == 0) {
        return nullptr;
    }
    for (auto& [conn_id, session] : sessions_) {
        if (session.replay_in_progress && session.replay_request_id == request_id) {
            return &session;
        }
    }
    return nullptr;
}

void FixOrderGatewayThread::on_timer_event(pubsub_itc_fw::TimerID timer_id) {
    if (timer_id == cancel_drain_timer_id_) {
        drain_pending_cancels();
        return;
    }

    if (timer_id == grace_timer_id_) {
        expire_grace_sessions();
        return;
    }

    for (auto& [id, session] : sessions_) {
        if (timer_id == session.logon_timeout_timer_id) {
            if (!session.session_established) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: connection {} logon timeout -- disconnecting",
                           id.get_value());
                disconnect_session(session, "logon timeout");
            }
            return;
        }
        if (timer_id == session.scram_auth_timeout_timer_id) {
            if (session.auth_pending) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "FixOrderGatewayThread: connection {} SCRAM authentication timeout -- disconnecting", id.get_value());
                FixMessage logout;
                logout.set(Tag::MsgType, MsgType::Logout);
                logout.set(Tag::Text, std::string("Authentication service timeout"));
                send_fix_to_session(session, logout);
                disconnect_session(session, "SCRAM authentication timeout");
            }
            return;
        }
    }
}

void FixOrderGatewayThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {
    // Do nothing
}

// Authentication PDU handlers

void FixOrderGatewayThread::handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationChallengeView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "FixOrderGatewayThread: failed to decode AuthenticationChallenge -- dropping");
        release_pdu_payload(message);
        return;
    }

    // Capture the connection ID before releasing the payload buffer.
    const pubsub_itc_fw::ConnectionID& auth_service_conn_id = message.connection_id();
    release_pdu_payload(message);

    // Correlate back to the FIX session using request_id == conn_id.
    FixSession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (!session_ptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: AuthenticationChallenge request_id={} -- no matching pending session -- dropping", view.request_id);
        return;
    }
    FixSession& session = *session_ptr;

    const std::vector<uint8_t> server_nonce(view.server_nonce.data, view.server_nonce.data + view.server_nonce.size);
    const std::vector<uint8_t> salt(view.salt.data, view.salt.data + view.salt.size);
    const int32_t iterations = view.iterations;

    // Derive the SCRAM proof and the expected ServerSignature locally.
    // The password never leaves the gateway process.
    static const std::string client_key_label = "Client Key";
    static const std::string server_key_label = "Server Key";

    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message(session.client_comp_id, session.scram_client_nonce, server_nonce, salt.data(), salt.size(), iterations);

    const std::vector<uint8_t> salted_password = scram_crypto::pbkdf2_sha256(session.client_password, salt.data(), salt.size(), iterations);

    // Zero and release the password as soon as the SCRAM derivation is done.
    std::fill(session.client_password.begin(), session.client_password.end(), '\0');
    session.client_password.clear();
    session.client_password.shrink_to_fit();

    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(client_key_label.data()), client_key_label.size());

    const std::vector<uint8_t> stored_key = scram_crypto::sha256(client_key.data(), client_key.size());

    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(stored_key.data(), stored_key.size(), auth_message.data(), auth_message.size());

    static constexpr size_t sha256_size = 32;
    std::vector<uint8_t> client_proof(sha256_size);
    for (size_t i = 0; i < sha256_size; ++i) {
        client_proof[i] = client_key[i] ^ client_signature[i];
    }

    const std::vector<uint8_t> server_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(server_key_label.data()), server_key_label.size());

    session.scram_expected_server_signature = scram_crypto::hmac_sha256(server_key.data(), server_key.size(), auth_message.data(), auth_message.size());

    pubsub_itc_fw_app::AuthenticationProof proof{};
    proof.request_id = view.request_id;
    proof.client_proof = pubsub_itc_fw_app::BytesView{client_proof.data(), client_proof.size()};
    // Send on the connection the challenge arrived on -- correct for both primary and secondary.
    send_pdu(auth_service_conn_id, pubsub_itc_fw_app::AuthenticationProof::message_pdu_id, 0, proof);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} AuthenticationProof sent request_id={}",
               session.conn_id.get_value(), view.request_id);
}

void FixOrderGatewayThread::handle_authentication_result(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationResultView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "FixOrderGatewayThread: failed to decode AuthenticationResult -- dropping");
        release_pdu_payload(message);
        return;
    }

    release_pdu_payload(message);

    FixSession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (!session_ptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: AuthenticationResult request_id={} -- no matching pending session -- dropping", view.request_id);
        return;
    }
    FixSession& session = *session_ptr;
    session.auth_pending = false;
    cancel_timer(session.scram_auth_timeout_timer_id);

    if (view.outcome != pubsub_itc_fw_app::AuthenticationOutcome::Granted) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: connection {} authentication failed outcome={} -- sending Logout",
                   session.conn_id.get_value(), static_cast<int32_t>(view.outcome));
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication failed"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "authentication failed");
        return;
    }

    // Verify ServerSignature to confirm we are speaking to the genuine service.
    const std::vector<uint8_t> received_signature(view.server_signature.data, view.server_signature.data + view.server_signature.size);
    if (received_signature != session.scram_expected_server_signature) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} ServerSignature mismatch -- possible impostor -- sending Logout", session.conn_id.get_value());
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication service identity could not be verified"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "ServerSignature mismatch");
        return;
    }

    // Authenticated, and the service has proved it is the genuine one -- so the
    // provisioning it just sent can be trusted, and this is the point to act on it.
    //
    // A session is provisioned against a primary gateway instance and optionally a backup,
    // and may log on to either. Landing anywhere else is refused: without that, pinning is
    // a convention the member happens to follow rather than a rule, and none of the
    // recovery guarantees it exists to support would hold -- the venue could not say which
    // two instances hold a session's state if the session could appear on any of them.
    //
    // A member with no primary provisioned is not pinned and is let in wherever it landed.
    // That is the "said nothing" case, not a third instance number: absence has to carry it
    // because instances are numbered from 1.
    if (view.has_primary_gateway_instance) {
        const int16_t primary_instance = view.primary_gateway_instance;
        const bool has_backup = view.has_backup_gateway_instance;
        const int16_t backup_instance = has_backup ? view.backup_gateway_instance : static_cast<int16_t>(0);
        const bool provisioned_here = config_.instance_id == primary_instance || (has_backup && config_.instance_id == backup_instance);

        if (!provisioned_here) {
            // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "FixOrderGatewayThread: connection {} comp_id='{}' logon refused -- this is gateway instance {}, "
                       "and the session is provisioned for primary={} backup={}",
                       session.conn_id.get_value(), session.client_comp_id, config_.instance_id, primary_instance,
                       has_backup ? std::to_string(backup_instance) : std::string("(none)"));
            FixMessage logout;
            logout.set(Tag::MsgType, MsgType::Logout);
            // The member is authenticated, so naming its own provisioning tells it nothing
            // it is not entitled to and saves an operator call to find out where to go.
            logout.set(Tag::Text, fmt::format("Session not provisioned for gateway instance {} -- use instance {}{}", config_.instance_id, primary_instance,
                                              has_backup ? fmt::format(" or {}", backup_instance) : std::string()));
            send_fix_to_session(session, logout);
            disconnect_session(session, "session not provisioned for this gateway instance");
            return;
        }

        // Logged at the accepted instance too, and naming the numbers rather than merely
        // saying "accepted": a hop that drops the provisioning leaves the gateway letting
        // everyone in everywhere, which looks identical to a correctly unpinned venue
        // unless the values themselves are visible.
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: comp_id='{}' provisioned for gateway instances primary={} backup={} -- "
                   "this is instance {} ({})",
                   session.client_comp_id, primary_instance, has_backup ? std::to_string(backup_instance) : std::string("(none)"), config_.instance_id,
                   config_.instance_id == primary_instance ? "primary" : "backup");
    }

    // Authenticated and provisioned. Send the FIX Logon reply and open the session.
    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logon);
    reply.set(Tag::EncryptMethod, 0);
    reply.set(Tag::HeartBtInt, session.heartbeat_interval);
    reply.set(Tag::DefaultApplVerID, std::string("9"));
    send_fix_to_session(session, reply);
    // Cancel-on-disconnect for this comp id, provisioned in the database and delivered with
    // the authentication result. Absent leaves the optionals empty, which means this member
    // expressed no preference and the gateway's configured defaults apply.
    if (view.has_cancel_on_disconnect_enabled) {
        session.cancel_on_disconnect_enabled = view.cancel_on_disconnect_enabled;
    }
    if (view.has_cancel_on_disconnect_grace_period_seconds) {
        session.cancel_on_disconnect_grace_period_seconds = view.cancel_on_disconnect_grace_period_seconds;
    }

    session.session_established = true;

    // TEST CONTRACT -- ha_test.py and perf_run.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} authentication succeeded -- FIX session established comp_id='{}'", session.conn_id.get_value(),
               session.client_comp_id);

    // Tell the sequencer where this session now lives, so reports for orders it placed
    // reach it here -- including orders placed through a connection, or an instance, that
    // no longer exists. Sent on every logon rather than only on a reconnect: the sequencer
    // cannot tell the two apart, and a binding it never received is one it cannot use.
    announce_session_bound(session);
    if (session.cancel_on_disconnect_enabled.has_value() || session.cancel_on_disconnect_grace_period_seconds.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: comp_id='{}' cancel-on-disconnect provisioned: enabled={} grace_period={}", session.client_comp_id,
                   session.cancel_on_disconnect_enabled.has_value() ? (*session.cancel_on_disconnect_enabled ? "true" : "false") : "(gateway default)",
                   session.cancel_on_disconnect_grace_period_seconds.has_value() ? std::to_string(*session.cancel_on_disconnect_grace_period_seconds) + "s"
                                                                                 : std::string("(gateway default)"));
    }

    // If this comp id dropped and got back inside its grace period, its orders are still
    // resting and must not be cancelled. This is the case the whole feature exists for: a
    // gateway dies, the member reconnects to another instance, and its book survives.
    const size_t reclaimed = reclaim_grace_session(session.client_comp_id);
    if (reclaimed > 0) {
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: comp_id='{}' reconnected within the grace period -- {} order(s) left resting, none cancelled",
                   session.client_comp_id, reclaimed);
    }
}

// FIX session handlers

void FixOrderGatewayThread::handle_logon(FixSession& session, const ParsedFixMessage& msg) {
    cancel_timer(session.logon_timeout_timer_id);
    // client_comp_id is stored as std::string for use beyond this callback;
    // the implicit conversion from string_view copies the bytes here.
    session.client_comp_id = msg.get(Tag::SenderCompID);

    // Copy tag 554 (Password) while the string_view into the MirroredBuffer is valid.
    // Never logged: the field is intentionally absent from all log messages.
    const std::string_view password_view = msg.get(Tag::Password);
    session.client_password.assign(password_view.data(), password_view.size());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} Logon from SenderCompID='{}' -- initiating SCRAM authentication", session.conn_id.get_value(),
               session.client_comp_id);

    // ResetSeqNumFlag=Y: the member wants both sides to restart at 1. Read here, acted on
    // when the venue's remembered numbering arrives -- see handle_session_bound_ack.
    const std::string_view reset_flag = msg.get(Tag::ResetSeqNumFlag);
    session.reset_seq_num_requested = (reset_flag == "Y" || reset_flag == "y");
    if (session.reset_seq_num_requested) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} Logon requests ResetSeqNumFlag=Y -- starting this session's numbering at 1",
                   session.conn_id.get_value());
    }

    const std::string_view heartbeat_interval_text = msg.get(Tag::HeartBtInt);
    session.heartbeat_interval = 30;
    if (!heartbeat_interval_text.empty()) {
        std::from_chars(heartbeat_interval_text.data(), heartbeat_interval_text.data() + heartbeat_interval_text.size(), session.heartbeat_interval);
    }

    // Select the auth service connection: primary if available, secondary as fallback.
    pubsub_itc_fw::ConnectionID auth_conn_id;
    if (auth_service_primary_conn_id_.is_valid()) {
        auth_conn_id = auth_service_primary_conn_id_;
    } else if (config_.ha_enabled && auth_service_secondary_conn_id_.is_valid()) {
        auth_conn_id = auth_service_secondary_conn_id_;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} primary auth service not connected -- using secondary", session.conn_id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} Logon rejected -- no authentication service connected", session.conn_id.get_value());
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication service unavailable"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "authentication service not connected");
        return;
    }

    uint8_t nonce_bytes[scram_client_nonce_size];
    if (RAND_bytes(nonce_bytes, static_cast<int>(sizeof(nonce_bytes))) != 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "FixOrderGatewayThread: connection {} RAND_bytes failed -- disconnecting",
                   session.conn_id.get_value());
        disconnect_session(session, "failed to generate client nonce");
        return;
    }
    session.scram_client_nonce.assign(nonce_bytes, nonce_bytes + sizeof(nonce_bytes));
    session.auth_pending = true;

    pubsub_itc_fw_app::AuthenticationRequest auth_request{};
    auth_request.request_id = static_cast<int64_t>(session.conn_id.get_value());
    auth_request.comp_id = session.client_comp_id;
    auth_request.client_nonce = pubsub_itc_fw_app::BytesView{session.scram_client_nonce.data(), session.scram_client_nonce.size()};
    send_pdu(auth_conn_id, pubsub_itc_fw_app::AuthenticationRequest::message_pdu_id, 0, auth_request);

    // Arm a timeout so the session is not left pending indefinitely if the
    // authentication service is slow or loses the request.
    session.scram_auth_timeout_timer_id = start_one_off_timer(config_.scram_auth_timeout);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} AuthenticationRequest sent request_id={} comp_id='{}' timeout={}s", session.conn_id.get_value(),
               auth_request.request_id, session.client_comp_id, config_.scram_auth_timeout.count());
}

void FixOrderGatewayThread::handle_heartbeat(FixSession& session, [[maybe_unused]] const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixOrderGatewayThread: connection {} Heartbeat", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix_to_session(session, reply);
}

void FixOrderGatewayThread::handle_test_request(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} TestRequest", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID -- set(int, string_view) copies into the reply
    send_fix_to_session(session, reply);
}

void FixOrderGatewayThread::handle_logout(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} Logout: {}", session.conn_id.get_value(),
               msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix_to_session(session, reply);
    session.session_established = false;
    // Read by queue_session_for_cleanup on the connection-lost path that follows: a member
    // that logs out has said what it wants, so its orders are cancelled at once rather than
    // waiting out a reconnect window it is not going to use.
    session.clean_logout = true;
    disconnect_session(session, "client sent Logout");
}

void FixOrderGatewayThread::handle_resend_request(FixSession& session, const ParsedFixMessage& msg) {
    int begin_seq = 1;
    const std::string_view begin_str = msg.get(Tag::BeginSeqNo);
    if (!begin_str.empty()) {
        std::from_chars(begin_str.data(), begin_str.data() + begin_str.size(), begin_seq);
    }

    // The member is asking for messages it missed. It gets them.
    //
    // This used to answer every ResendRequest with a blanket SequenceReset-GapFill: the gap
    // was declared administrative and skipped, the session survived, and the member was told
    // nothing about what had happened to its orders. That was the only option available,
    // because a gateway holds no record of what it has sent -- and the reports in question
    // may have been sent by a different gateway instance entirely.
    //
    // The reports are in the sequencer's WAL, each stamped with the session it belongs to,
    // so the answer is to ask for that session's slice and resend it. FIX's own rule is
    // followed: application messages are resent (with PossDupFlag=Y), and only the
    // administrative remainder is gap-filled, which is done when the replay completes.
    //
    // A resend already running is not restarted. fix8 will re-ask if it is still short, and
    // restarting would re-issue sequence numbers the first pass is still consuming -- which
    // is the feedback loop the old comment here described, arrived at from the other side.
    if (session.replay_in_progress) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} ResendRequest BeginSeqNo={} while a resend is already running -- ignoring the duplicate request",
                   session.conn_id.get_value(), begin_seq);
        return;
    }

    // Where the session had reached. The resend rewinds the outbound number to the start of
    // the gap and replays into it; whatever is left over is gap-filled up to here, so the
    // member ends at the number it would have been at anyway.
    session.replay_resume_seq_num = session.outbound_seq_num;
    session.outbound_seq_num = begin_seq;
    session.replay_in_progress = true;
    session.replay_request_id = ++next_replay_request_id_;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} comp_id='{}' ResendRequest BeginSeqNo={} -- requesting this session's reports from the sequencer "
               "(request_id={}, will resume at {})",
               session.conn_id.get_value(), session.client_comp_id, begin_seq, session.replay_request_id, session.replay_resume_seq_num);

    pubsub_itc_fw_app::SessionReplayRequest request{};
    request.request_id = session.replay_request_id;
    request.comp_id = session.client_comp_id;
    request.gateway_protocol_id = gateway_ids::fix_order_gateway;
    // From the beginning of what the WAL still holds. A cursor per session would narrow this,
    // and is the obvious next move if replays ever become frequent enough to matter; the
    // member's own BeginSeqNo cannot serve as one, because it is a FIX session number and
    // the WAL is numbered by the venue's own sequence.
    request.from_seq_no = 0;
    // Ask for exactly the gap the member described, and no more. The sequencer returns the
    // most recent that many reports, which is what a member has missed -- it is the tail of
    // its stream that went undelivered, never the beginning. Asking for everything gets the
    // session's whole retained history: far more messages than the member has room for in
    // its gap, oldest first, so the messages it actually wants never arrive.
    const int gap_width = session.replay_resume_seq_num - begin_seq;
    request.max_records = gap_width > 0 ? gap_width : 1;
    forward_pdu_to_sequencers(pubsub_itc_fw_app::SessionReplayRequest::message_pdu_id, request);
}

void FixOrderGatewayThread::handle_session_bound_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::SessionBoundAckView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: failed to decode SessionBoundAck -- dropping");
        return;
    }
    if (!view.known) {
        return; // first time the venue has seen this session; the defaults already apply
    }

    FixSession* session = find_session_by_comp_id(view.comp_id);
    if (session == nullptr) {
        return; // the session went away between binding and the reply
    }
    if (session->reset_seq_num_requested) {
        // The member asked to start again at 1, so the numbering the venue remembered is
        // not restored. Continuing it against the member's explicit wish would put the two
        // sides permanently at odds: the venue would send numbers the member rejects as
        // too high, and the member would ask for a resend of messages it has just disclaimed.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: comp_id='{}' asked for a sequence reset -- discarding the venue's remembered outbound={}", session->client_comp_id,
                   view.outbound_seq_num);
        return;
    }

    // Continue the member's numbering rather than restarting it. Without this a reconnect
    // looks to the member like the venue resetting to 1, which is a break it cannot
    // reconcile and which no amount of replay would fix.
    session->outbound_seq_num = view.outbound_seq_num;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: comp_id='{}' resuming the venue's sequence state -- outbound={} (was 1)",
               session->client_comp_id, view.outbound_seq_num);
}

void FixOrderGatewayThread::handle_session_replay_record(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::SessionReplayRecordView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: failed to decode SessionReplayRecord -- dropping");
        return;
    }

    FixSession* session = find_session_by_replay_request(view.request_id);
    if (session == nullptr) {
        // The session ended while its own replay was in flight. Nothing to deliver to.
        return;
    }

    pubsub_itc_fw_app::ExecutionReportView report{};
    size_t report_consumed = 0;
    size_t report_needed = 0;
    if (!pubsub_itc_fw_app::decode(report, view.payload.data, view.payload.size, report_consumed, arena, report_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: replayed record seq={} did not decode as an ExecutionReport",
                   view.seq_no);
        return;
    }

    // PossDupFlag only within the gap the member actually asked about. A replay routinely
    // runs past it: the reports the venue could not deliver while the session was away are in
    // the same slice, and they have never been sent. Marking those as possible duplicates
    // would invite the member to discard news it is seeing for the first time.
    const bool inside_requested_gap = session->outbound_seq_num < session->replay_resume_seq_num;
    send_execution_report_to_session(*session, report, inside_requested_gap, view.wall_time_ns);
    ++execution_reports_replayed_;
}

void FixOrderGatewayThread::handle_session_replay_complete(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::SessionReplayCompleteView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixOrderGatewayThread: failed to decode SessionReplayComplete -- dropping");
        return;
    }

    FixSession* session_ptr = find_session_by_replay_request(view.request_id);
    if (session_ptr == nullptr) {
        return;
    }
    FixSession& session = *session_ptr;

    // Close the gap. The replayed reports have consumed sequence numbers from the start of
    // the gap; whatever is still missing between here and where the session had reached was
    // administrative traffic -- heartbeats, and the session-level messages the venue is not
    // required to resend. FIX says to gap-fill exactly that remainder, and only it.
    if (session.outbound_seq_num < session.replay_resume_seq_num) {
        const int gap_from = session.outbound_seq_num;
        FixMessage reset;
        reset.set(Tag::MsgType, MsgType::SequenceReset);
        reset.set(Tag::GapFillFlag, std::string("Y"));
        reset.set(Tag::NewSeqNo, std::to_string(session.replay_resume_seq_num));
        send_fix_to_session(session, reset);
        session.outbound_seq_num = session.replay_resume_seq_num;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} resend complete -- {} report(s) resent, gap-filled {}..{} (administrative traffic)",
                   session.conn_id.get_value(), view.record_count, gap_from, session.replay_resume_seq_num);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} resend complete -- {} report(s) resent, no gap left",
                   session.conn_id.get_value(), view.record_count);
    }

    if (view.truncated) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: comp_id='{}' resend was truncated at the sequencer's record cap (last_seq_no={}) -- "
                   "the member has been sent what fitted and gap-filled the rest",
                   session.client_comp_id, view.last_seq_no);
    }

    session.replay_in_progress = false;
    session.replay_request_id = 0;

    // Live reports that arrived mid-resend, now delivered in order behind it.
    std::vector<std::vector<uint8_t>> deferred;
    deferred.swap(session.deferred_execution_reports);
    for (const std::vector<uint8_t>& payload : deferred) {
        pubsub_itc_fw::BumpAllocator deferred_arena(arena_buffer.data(), arena_buffer.size());
        deferred_arena.reset();
        size_t deferred_consumed = 0;
        size_t deferred_needed = 0;
        pubsub_itc_fw_app::ExecutionReportView report{};
        if (!pubsub_itc_fw_app::decode(report, payload.data(), payload.size(), deferred_consumed, deferred_arena, deferred_needed)) {
            continue;
        }
        send_execution_report_to_session(session, report, /*poss_dup=*/false, 0);
    }
    if (!deferred.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} delivered {} report(s) held during the resend",
                   session.conn_id.get_value(), deferred.size());
    }
}

void FixOrderGatewayThread::handle_new_order_single(FixSession& session, const ParsedFixMessage& msg, const fix_codec::FixMessageReader& reader) {
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "entered handle_new_order_single");

    const std::string_view cl_ord_id = msg.get(Tag::ClOrdID);
    const std::string_view symbol = msg.get(Tag::Symbol);
    const std::string_view side_str = msg.get(Tag::Side);
    const std::string_view ord_type_str = msg.get(Tag::OrdType);
    const std::string_view order_qty = msg.get(Tag::OrderQty);
    const std::string_view price_str = msg.get(Tag::Price);

    ++orders_received_;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "GW-NOS-RECV connection={} ClOrdID={} Symbol={} Side={}", session.conn_id.get_value(), cl_ord_id,
               symbol, side_str);

    // Validate required fields.
    if (cl_ord_id.empty() || symbol.empty() || side_str.empty() || ord_type_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} NewOrderSingle missing required fields"
                   " -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // Validate field lengths against runtime-configured limits.
    // An over-length ClOrdID is rejected with an ExecutionReport (Rejected); it is checked
    // against the single shared bound (fix_order_limits::max_cl_ord_id_length) that also sizes
    // the open-order pool entry and the matching-engine book key. Over-length symbol/qty below
    // keep their FIX BusinessReject. Limits are documented in the gateway connectivity spec.
    if (cl_ord_id.size() > fix_order_limits::max_cl_ord_id_length) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} NOS ClOrdID length {} exceeds limit {} -- rejecting",
                   session.conn_id.get_value(), cl_ord_id.size(), fix_order_limits::max_cl_ord_id_length);
        send_reject_execution_report(session, msg, "ClOrdID exceeds maximum length of " + std::to_string(fix_order_limits::max_cl_ord_id_length),
                                     /*is_cancel=*/false);
        return;
    }
    if (symbol.size() > static_cast<size_t>(config_.max_symbol_length)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} NOS Symbol length {} exceeds limit {} -- sending BusinessReject", session.conn_id.get_value(),
                   symbol.size(), config_.max_symbol_length);
        send_business_reject(session, msg, "Symbol exceeds maximum length of " + std::to_string(config_.max_symbol_length));
        return;
    }
    if (order_qty.size() > static_cast<size_t>(config_.max_order_qty_length)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} NOS OrderQty length {} exceeds limit {} -- sending BusinessReject", session.conn_id.get_value(),
                   order_qty.size(), config_.max_order_qty_length);
        send_business_reject(session, msg, "OrderQty exceeds maximum length of " + std::to_string(config_.max_order_qty_length));
        return;
    }

    // If no sequencer is connected, reject the order locally with an ExecutionReport
    // rather than silently dropping it. The client gets a definitive response per
    // order and the FIX session stays up so subsequent orders can be tried once
    // connectivity is restored. During failover the secondary takes over as leader,
    // so orders are forwarded as long as either sequencer connection is alive.
    if (!sequencer_primary_conn_id_.is_valid() && !sequencer_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} NewOrderSingle ClOrdID={} rejected "
                   "locally -- no sequencer connected",
                   session.conn_id.get_value(), cl_ord_id);
        send_reject_execution_report(session, msg, "Sequencer unavailable", /*is_cancel=*/false);
        return;
    }

    // Build the DSL struct. All string fields use string_view pointing into
    // the FIX message -- safe because the struct is only live for this call.
    pubsub_itc_fw_app::NewOrderSingle nos{};
    nos.cl_ord_id = cl_ord_id;
    nos.symbol = symbol;
    nos.side = static_cast<pubsub_itc_fw_app::Side>(side_str[0]);
    nos.ord_type = static_cast<pubsub_itc_fw_app::OrdType>(ord_type_str[0]);
    nos.transact_time = parse_fix_utc_timestamp(msg.get(Tag::SendingTime));
    nos.order_qty = order_qty;

    if (!price_str.empty()) {
        nos.has_price = true;
        nos.price = price_str;
    }

    const std::string_view tif_str = msg.get(Tag::TimeInForce);
    if (!tif_str.empty()) {
        nos.has_time_in_force = true;
        nos.time_in_force = static_cast<pubsub_itc_fw_app::TimeInForce>(tif_str[0]);
    }

    // Exchange instrument identification (tag 48 + tag 22): futures/derivatives venues key on a
    // numeric exchange instrument id rather than Symbol alone. Carry both through so the ME can
    // echo them on the ExecutionReport and downstream topic subscribers (e.g. topic_probe) see them.
    const std::string_view security_id = msg.get(Tag::SecurityID);
    if (!security_id.empty()) {
        nos.has_security_id = true;
        nos.security_id = security_id;
    }
    const std::string_view security_id_source = msg.get(Tag::SecurityIDSource);
    if (!security_id_source.empty()) {
        nos.has_security_id_source = true;
        nos.security_id_source = security_id_source;
    }

    // Remaining optional order attributes, carried through so the ExecutionReport echoes them back.
    const std::string_view stop_px = msg.get(Tag::StopPx);
    if (!stop_px.empty()) {
        nos.has_stop_px = true;
        nos.stop_px = stop_px;
    }
    const std::string_view account = msg.get(Tag::Account);
    if (!account.empty()) {
        nos.has_account = true;
        nos.account = account;
    }
    const std::string_view ex_destination = msg.get(Tag::ExDestination);
    if (!ex_destination.empty()) {
        nos.has_ex_destination = true;
        nos.ex_destination = ex_destination;
    }
    const std::string_view exec_inst_str = msg.get(Tag::ExecInst);
    if (!exec_inst_str.empty()) {
        nos.has_exec_inst = true;
        nos.exec_inst = exec_inst_str; // ExecInst is MULTIPLECHARVALUE -- carried verbatim as a string
    }
    const std::string_view min_qty = msg.get(Tag::MinQty);
    if (!min_qty.empty()) {
        nos.has_min_qty = true;
        nos.min_qty = min_qty;
    }
    const std::string_view max_floor = msg.get(Tag::MaxFloor);
    if (!max_floor.empty()) {
        nos.has_max_floor = true;
        nos.max_floor = max_floor;
    }
    const std::string_view expire_time = msg.get(Tag::ExpireTime);
    if (!expire_time.empty()) {
        nos.has_expire_time = true;
        nos.expire_time = parse_fix_utc_timestamp(expire_time);
    }
    const std::string_view text = msg.get(Tag::Text);
    if (!text.empty()) {
        nos.has_text = true;
        nos.text = text;
    }

    // Repeating groups (NoUnderlyings, NoPartyIDs, nested NoPartySubIDs) cannot be
    // represented by the flat ParsedFixMessage, so they are extracted straight from the
    // framed FIX bytes into the NOS's ListViews. The element arrays live in the reusable
    // group_arena_buffer_ (string_views point into the reader's buffer); both stay valid
    // through forward_order_in_envelope, which encodes synchronously. The arena is sized
    // to need, not a fixed cap: extract, and if the arena was too small (bytes_used
    // exceeds it) grow to the requirement and retry, so a large group set is never
    // silently dropped. bytes_used reports the true need even when an allocation was
    // refused, so doubling converges.
    for (;;) {
        pubsub_itc_fw::BumpAllocator group_arena(group_arena_buffer_.data(), group_arena_buffer_.size());
        nos.no_underlyings = {};
        nos.no_party_i_ds = {};
        extract_new_order_single_groups(reader, group_arena, nos);
        if (group_arena.bytes_used() <= group_arena_buffer_.size() || group_arena_buffer_.size() >= max_group_arena_size) {
            break;
        }
        group_arena_buffer_.resize(std::max(group_arena.bytes_used(), group_arena_buffer_.size() * 2));
    }

    // The originating session's connection id (so the sequencer can route the ER
    // back to this exact FIX session -- unique per TCP connection, avoiding the
    // ClOrdID collision problem) and its SenderCompID (for audit) travel on the
    // WalRecord envelope, not inside the DD-derived PDU. Forward the pure NOS
    // wrapped in that envelope to both sequencer instances.
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle), nos, session.conn_id.get_value(),
                              session.client_comp_id, current_read_ingress_ns_);

    // Do NOT record the order here. We record it when the ME sends back a
    // non-terminal ExecutionReport (OrdStatus=New), which confirms the order
    // is actually on the book. Tracking from NOS time would include every
    // in-flight order and flood the sequencer with OCRs on disconnect.

    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "exit handle_new_order_single");
}

void FixOrderGatewayThread::handle_order_cancel_request(FixSession& session, const ParsedFixMessage& msg) {
    const std::string_view cl_ord_id = msg.get(Tag::ClOrdID);
    const std::string_view orig_cl_ord_id = msg.get(Tag::OrigClOrdID);
    const std::string_view symbol = msg.get(Tag::Symbol);
    const std::string_view side_str = msg.get(Tag::Side);
    const std::string_view order_qty = msg.get(Tag::OrderQty);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} OrderCancelRequest ClOrdID={} "
               "OrigClOrdID={} Symbol={}",
               session.conn_id.get_value(), cl_ord_id, orig_cl_ord_id, symbol);

    if (cl_ord_id.empty() || orig_cl_ord_id.empty() || symbol.empty() || side_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} OrderCancelRequest missing required "
                   "fields -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // Both ClOrdID and OrigClOrdID come from outside; reject an over-length one with an ER
    // (the same shared bound the matching-engine book key uses -- see fix_order_limits).
    if (cl_ord_id.size() > fix_order_limits::max_cl_ord_id_length || orig_cl_ord_id.size() > fix_order_limits::max_cl_ord_id_length) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} OrderCancelRequest ClOrdID/OrigClOrdID exceeds limit {} -- rejecting", session.conn_id.get_value(),
                   fix_order_limits::max_cl_ord_id_length);
        send_reject_execution_report(session, msg, "ClOrdID exceeds maximum length of " + std::to_string(fix_order_limits::max_cl_ord_id_length),
                                     /*is_cancel=*/true);
        return;
    }

    // If no sequencer is connected, reject the cancel locally. See
    // handle_new_order_single for the rationale.
    if (!sequencer_primary_conn_id_.is_valid() && !sequencer_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixOrderGatewayThread: connection {} OrderCancelRequest ClOrdID={} "
                   "OrigClOrdID={} rejected locally -- no sequencer connected",
                   session.conn_id.get_value(), cl_ord_id, orig_cl_ord_id);
        send_reject_execution_report(session, msg, "Sequencer unavailable", /*is_cancel=*/true);
        return;
    }

    pubsub_itc_fw_app::OrderCancelRequest ocr{};
    ocr.orig_cl_ord_id = orig_cl_ord_id;
    ocr.cl_ord_id = cl_ord_id;
    ocr.symbol = symbol;
    ocr.side = static_cast<pubsub_itc_fw_app::Side>(side_str[0]);
    ocr.transact_time = parse_fix_utc_timestamp(msg.get(Tag::SendingTime));
    ocr.order_qty = order_qty;

    // Connection id (cancel-ER routing) and SenderCompID (audit) ride on the
    // WalRecord envelope, not inside the PDU (same mechanism as NOS).
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest), ocr, session.conn_id.get_value(),
                              session.client_comp_id, current_read_ingress_ns_);
}

void FixOrderGatewayThread::disconnect_session(const FixSession& session, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: disconnecting connection {}: {}", session.conn_id.get_value(), reason);

    pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(cmd);
}

void FixOrderGatewayThread::send_fix_to_session(FixSession& session, const FixMessage& msg) {
    const std::string wire = serialiser_.serialise(msg, session.outbound_seq_num++, session.client_comp_id);
    if (capture_ != nullptr) {
        capture_->capture(FixCapture::Direction::Outbound, reinterpret_cast<const uint8_t*>(wire.data()), wire.size(), config_.wall_clock->now_ns());
    }
    // Diagnostic: log outbound FIX message with SOH replaced by '|' for readability.
    std::string readable = wire;
    for (char& c : readable) {
        if (c == '\x01')
            c = '|';
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} FIX OUT ({} bytes): {}", session.conn_id.get_value(),
               wire.size(), readable);
    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
}

void FixOrderGatewayThread::send_reject_execution_report(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason, bool is_cancel) {
    // The matching engine never sees this order, so we synthesise the
    // gateway-side identifiers from per-session counters. Format mirrors the
    // ME-generated IDs (ME-ORD-N / ME-EXEC-N) but with a GW- prefix so the
    // origin is unambiguous in logs and downstream audit trails.
    const std::string order_id = "GW-ORD-" + std::to_string(session.order_id_counter++);
    const std::string exec_id = "GW-EXEC-" + std::to_string(session.exec_id_counter++);

    // Echo identifying fields from the inbound message. Side and OrderQty are
    // optional from a strict FIX standpoint on a Reject (the ME never priced
    // it), but echoing what the client sent keeps the round-trip diagnostic
    // and matches the format of a real ER for the same order.
    // The string_views are valid for the duration of this call; er.set() copies
    // each value into the outbound FixMessage map.
    const std::string_view cl_ord_id = inbound.get(Tag::ClOrdID);
    const std::string_view symbol = inbound.get(Tag::Symbol);
    const std::string_view side_str = inbound.get(Tag::Side);
    const std::string_view order_qty = inbound.get(Tag::OrderQty);

    FixMessage er;
    er.set(Tag::MsgType, MsgType::ExecutionReport);
    er.set(Tag::OrderID, order_id);
    er.set(Tag::ExecID, exec_id);
    er.set(Tag::ExecType, std::string(1, '8'));  // 150 = 8 Rejected
    er.set(Tag::OrdStatus, std::string(1, '8')); //  39 = 8 Rejected
    er.set(Tag::ClOrdID, cl_ord_id);
    er.set(Tag::Symbol, symbol);
    if (!side_str.empty()) {
        er.set(Tag::Side, side_str);
    }
    if (!order_qty.empty()) {
        er.set(Tag::OrderQty, order_qty);
        er.set(Tag::LeavesQty, std::string("0"));
        er.set(Tag::CumQty, std::string("0"));
    }
    er.set(103, 99); // 103 = OrdRejReason, 99 = Other
    er.set(Tag::Text, reason);

    if (is_cancel) {
        // Cancel-reject convention: echo OrigClOrdID so the client can
        // correlate the reject with the original order it tried to cancel.
        const std::string_view orig_cl_ord_id = inbound.get(Tag::OrigClOrdID);
        if (!orig_cl_ord_id.empty()) {
            er.set(Tag::OrigClOrdID, orig_cl_ord_id);
        }
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} sending reject ExecutionReport "
               "OrderID={} ExecID={} ClOrdID={} reason='{}'",
               session.conn_id.get_value(), order_id, exec_id, cl_ord_id, reason);

    send_fix_to_session(session, er);
}

void FixOrderGatewayThread::send_business_reject(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason) {
    FixMessage bmr{};
    bmr.set(Tag::MsgType, std::string("j"));
    bmr.set(Tag::RefSeqNum, inbound.get(Tag::MsgSeqNum).empty() ? std::string("0") : std::string(inbound.get(Tag::MsgSeqNum)));
    bmr.set(Tag::RefMsgType, std::string(inbound.get(Tag::MsgType)));
    bmr.set(Tag::BusinessRejectReason, std::string("0")); // 0 = Other
    bmr.set(Tag::Text, reason);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} BusinessReject: {}", session.conn_id.get_value(), reason);
    send_fix_to_session(session, bmr);
}

void FixOrderGatewayThread::send_fix_reject(FixSession& session, const ParsedFixMessage& inbound, const fix_codec::FixReject& reject) {
    char text[128];
    const std::string_view description = reject.describe(text, sizeof(text));

    FixMessage response{};
    response.set(Tag::MsgType, MsgType::Reject); // 35=3
    response.set(Tag::RefSeqNum, inbound.get(Tag::MsgSeqNum).empty() ? std::string("0") : std::string(inbound.get(Tag::MsgSeqNum)));
    response.set(Tag::RefTagID, reject.ref_tag);
    response.set(Tag::RefMsgType, std::string(reject.ref_msg_type));
    response.set(Tag::SessionRejectReason, static_cast<int>(reject.reason));
    response.set(Tag::Text, std::string(description));
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} Reject (35=3): {}", session.conn_id.get_value(),
               description);
    send_fix_to_session(session, response);
}

// PDU forwarding

// forward_pdu_to_sequencers is a template so it can accept any DSL message
// struct. It calls send_pdu on both the primary and secondary sequencer
// connections. send_pdu handles all slab allocation, PduHeader framing, and
// two-pass encode/write internally.
//
// If a sequencer connection is not currently established (e.g. not yet
// reconnected after a failure), the PDU is dropped for that sequencer and a
// Warning is logged. The other sequencer still receives the PDU so the leader
// can continue operating. When connection retry re-establishes the lost
// connection the follower will resync from the leader's state.

void FixOrderGatewayThread::report_order_progress() {
    // Every execution report is either delivered or dropped because its client has gone, so
    // the two together account for every order. That total is what tells a reader -- or the
    // perf harness -- how far a run has got.
    const int64_t accounted = execution_reports_sent_ + execution_reports_dropped_;
    if (accounted % order_progress_interval != 0) {
        return;
    }
    // TEST CONTRACT -- ha_test.py and perf_run.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "GW-PROGRESS accounted={} sent={} dropped={} nos_received={}", accounted, execution_reports_sent_,
               execution_reports_dropped_, orders_received_);
}

void FixOrderGatewayThread::queue_session_for_cleanup(FixSession& session) {
    if (session.open_orders.empty()) {
        return;
    }

    const size_t total_orders = session.open_orders.size();

    // Per comp id where provisioned, the gateway's own setting otherwise. The member's
    // stored preference wins because it is the more specific statement of intent; the
    // configuration file is the venue's answer for everyone who has not given one.
    const bool cancel_enabled = session.cancel_on_disconnect_enabled.value_or(config_.cancel_on_disconnect_enabled);
    const std::chrono::seconds grace_period = session.cancel_on_disconnect_grace_period_seconds.has_value()
                                                  ? std::chrono::seconds{*session.cancel_on_disconnect_grace_period_seconds}
                                                  : config_.cancel_on_disconnect_grace_period;

    // Cancel-on-disconnect switched off: the member owns its book across a disconnect.
    // The pool entries still have to go back -- the session map is about to be destroyed
    // and they would otherwise leak -- but nothing is cancelled and the orders stay
    // resting exactly where they are.
    if (!cancel_enabled) {
        for (const auto& [cl_ord_id, entry] : session.open_orders) {
            open_order_pool_->deallocate(entry);
        }
        session.open_orders.clear();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} disconnected with {} open order(s) -- cancel-on-disconnect is disabled, leaving them resting",
                   session.conn_id.get_value(), total_orders);
        return;
    }

    DeadSession dead;
    // One pass over the map to collect the entries; the pool still owns them, and each
    // is released as its cancel is sent. Draining a vector afterwards avoids the
    // repeated begin() rescan a map drain performs -- see DeadSession.
    dead.open_orders.reserve(total_orders);
    size_t persistent_orders = 0;
    for (const auto& [cl_ord_id, entry] : session.open_orders) {
        // A GoodTillCancel or GoodTillDate order was placed to outlive the session. The
        // member asked for good-till-cancel, not "until my socket drops", so cancelling it
        // here would silently override an explicit instruction. Release the gateway's
        // bookkeeping and leave the order on the book.
        if (open_orders::is_persistent_time_in_force(entry->time_in_force)) {
            open_order_pool_->deallocate(entry);
            ++persistent_orders;
            continue;
        }
        dead.open_orders.push_back(entry);
    }
    session.open_orders.clear();

    if (persistent_orders > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: connection {} disconnected -- {} persistent order(s) (GTC/GTD) left resting", session.conn_id.get_value(),
                   persistent_orders);
    }
    if (dead.open_orders.empty()) {
        return;
    }

    dead.session_conn_id = session.conn_id.get_value();
    dead.client_comp_id = session.client_comp_id;
    dead.cancel_id_counter = session.cancel_id_counter;

    // A clean Logout means the member has said what it wants, so there is nothing to wait
    // for. An unexpected drop has said nothing, and gets its full window to come back --
    // which is what stops a gateway failure from flattening every book on the instance.
    const bool cancel_now = session.clean_logout || grace_period.count() == 0;

    if (cancel_now) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: connection {} {} with {} open order(s) -- queuing cancels now",
                   session.conn_id.get_value(), session.clean_logout ? "logged out" : "disconnected", dead.open_orders.size());
        pending_cancel_sessions_.push_back(std::move(dead));
        if (!cancel_drain_timer_active_) {
            cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
            cancel_drain_timer_active_ = true;
        }
        return;
    }

    dead.cancel_due = std::chrono::steady_clock::now() + grace_period;
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixOrderGatewayThread: connection {} (comp_id='{}') disconnected with {} open order(s) -- holding {}s for reconnect before cancelling",
               session.conn_id.get_value(), dead.client_comp_id, dead.open_orders.size(), grace_period.count());
    grace_sessions_.push_back(std::move(dead));
    arm_grace_timer();
}

void FixOrderGatewayThread::arm_grace_timer() {
    if (grace_timer_active_ || grace_sessions_.empty()) {
        return;
    }
    // Every entry takes the same grace period and they are appended in the order their
    // connections died, so the front is always the earliest due. A deadline already in the
    // past asks for the shortest timer the reactor will honour rather than a negative one.
    const auto now = std::chrono::steady_clock::now();
    const auto due = grace_sessions_.front().cancel_due;
    const auto delay = due > now ? std::chrono::duration_cast<std::chrono::milliseconds>(due - now) : std::chrono::milliseconds{1};
    grace_timer_id_ = start_one_off_timer(delay);
    grace_timer_active_ = true;
}

void FixOrderGatewayThread::expire_grace_sessions() {
    grace_timer_active_ = false;

    const auto now = std::chrono::steady_clock::now();
    size_t expired_sessions = 0;
    size_t expired_orders = 0;
    while (!grace_sessions_.empty() && grace_sessions_.front().cancel_due <= now) {
        expired_orders += grace_sessions_.front().open_orders.size();
        ++expired_sessions;
        pending_cancel_sessions_.push_back(std::move(grace_sessions_.front()));
        grace_sessions_.pop_front();
    }

    if (expired_sessions > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixOrderGatewayThread: grace period expired for {} session(s) -- cancelling {} order(s); no reconnect arrived", expired_sessions,
                   expired_orders);
        if (!cancel_drain_timer_active_) {
            cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
            cancel_drain_timer_active_ = true;
        }
    }

    arm_grace_timer();
}

size_t FixOrderGatewayThread::reclaim_grace_session(std::string_view comp_id) {
    size_t reclaimed = 0;
    for (auto it = grace_sessions_.begin(); it != grace_sessions_.end();) {
        if (it->client_comp_id != comp_id) {
            ++it;
            continue;
        }
        // Back inside the window: cancel nothing. The orders stay resting on the book,
        // which is the whole reason the grace period exists.
        //
        // The gateway's own bookkeeping for them is released rather than handed to the new
        // session, and that is deliberate rather than an oversight. The matching engine
        // keys an order by the session connection id it arrived on, so an order placed on
        // the old connection cannot be cancelled from the new one -- the ME would not find
        // it. Adopting the entries would make the gateway claim a control it does not have.
        // Re-keying an order onto a recovered session is step 5 of docs/design/gateway_ha.md.
        for (OpenOrderEntry* entry : it->open_orders) {
            open_order_pool_->deallocate(entry);
        }
        reclaimed += it->open_orders.size();
        it = grace_sessions_.erase(it);
    }
    return reclaimed;
}

void FixOrderGatewayThread::drain_pending_cancels() {
    cancel_drain_timer_active_ = false;

    int sent = 0;
    while (!pending_cancel_sessions_.empty() && sent < cancel_drain_batch_size) {
        DeadSession& dead = pending_cancel_sessions_.front();

        if (dead.next_order_index >= dead.open_orders.size()) {
            pending_cancel_sessions_.pop_front();
            continue;
        }

        OpenOrderEntry* entry = dead.open_orders[dead.next_order_index];

        // Formatted into a stack buffer rather than built with std::string: the obvious
        // spelling allocates three times per cancel, on a path that can generate thousands
        // in a burst.
        std::array<char, cancel_cl_ord_id::max_length> cancel_id_buffer{};
        const std::string_view cancel_cl_ord_id = cancel_cl_ord_id::format(cancel_id_buffer, "GW-CXL-", dead.session_conn_id, dead.cancel_id_counter++);
        if (cancel_cl_ord_id.empty()) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "FixOrderGatewayThread: could not format a cancel ClOrdID for session {} -- order left on the book", dead.session_conn_id);
            open_order_pool_->deallocate(entry);
            ++dead.next_order_index;
            ++sent;
            continue;
        }

        pubsub_itc_fw_app::OrderCancelRequest ocr{};
        ocr.orig_cl_ord_id = std::string_view(entry->cl_ord_id, entry->cl_ord_id_len);
        ocr.cl_ord_id = cancel_cl_ord_id;
        ocr.symbol = std::string_view(entry->symbol, entry->symbol_len);
        ocr.side = static_cast<pubsub_itc_fw_app::Side>(entry->side);
        ocr.transact_time = config_.wall_clock->now_ns();
        if (entry->order_qty_len > 0) {
            ocr.order_qty = std::string_view(entry->order_qty, entry->order_qty_len);
        }

        // Connection id and SenderCompID for the dead session travel on the envelope.
        // No ingress stamp: this cancel is the gateway's own doing, not a client's order.
        // The client it belongs to has already gone, so there is nobody waiting on it and
        // no round trip to attribute to the venue.
        forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest), ocr, dead.session_conn_id, dead.client_comp_id,
                                  /*gateway_ingress_ns=*/0);

        open_order_pool_->deallocate(entry);
        ++dead.next_order_index;
        ++sent;
        ++cancels_sent_this_drain_;
    }

    if (!pending_cancel_sessions_.empty()) {
        cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
        cancel_drain_timer_active_ = true;
        return;
    }

    // Report on the tick that empties the queue, whether or not that tick sent anything.
    // Gating on sent > 0 missed the common case: the final tick usually just pops the last
    // exhausted session, so the completion line went unlogged exactly when a drain finished.
    if (cancels_sent_this_drain_ > 0) {
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: cancel drain complete -- {} cancel(s) sent",
                   cancels_sent_this_drain_);
        cancels_sent_this_drain_ = 0;
    }
}

void FixOrderGatewayThread::announce_session_bound(const FixSession& session) {
    if (session.client_comp_id.empty()) {
        return;
    }
    pubsub_itc_fw_app::SessionBound bound{};
    bound.comp_id = session.client_comp_id;
    bound.gateway_protocol_id = gateway_ids::fix_order_gateway;
    bound.gateway_instance_id = config_.instance_id;
    bound.gateway_session_conn_id = session.conn_id.get_value();
    forward_pdu_to_sequencers(pubsub_itc_fw_app::SessionBound::message_pdu_id, bound);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: announced session comp_id='{}' bound to instance {} connection {}",
               session.client_comp_id, config_.instance_id, session.conn_id.get_value());
}

void FixOrderGatewayThread::announce_session_unbound(const FixSession& session) {
    // Only a session that was announced needs unannouncing. An unauthenticated connection
    // that dies during logon never had an identity, so there is nothing to unbind and
    // sending one would name an empty comp id.
    if (!session.session_established || session.client_comp_id.empty()) {
        return;
    }
    pubsub_itc_fw_app::SessionUnbound unbound{};
    unbound.comp_id = session.client_comp_id;
    unbound.gateway_protocol_id = gateway_ids::fix_order_gateway;
    unbound.gateway_instance_id = config_.instance_id;
    unbound.gateway_session_conn_id = session.conn_id.get_value();
    // Where this session's numbering reached, so the next gateway to hold it carries on from
    // here rather than restarting the member at 1.
    unbound.outbound_seq_num = session.outbound_seq_num;
    forward_pdu_to_sequencers(pubsub_itc_fw_app::SessionUnbound::message_pdu_id, unbound);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixOrderGatewayThread: announced session comp_id='{}' unbound from instance {} connection {}",
               session.client_comp_id, config_.instance_id, session.conn_id.get_value());
}

FixSession* FixOrderGatewayThread::find_session_by_conn_id(int32_t gateway_session_conn_id) {
    auto it = sessions_.find(pubsub_itc_fw::ConnectionID{gateway_session_conn_id});
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

FixSession* FixOrderGatewayThread::find_session_by_comp_id(const std::string& comp_id) {
    for (auto& [conn_id, session] : sessions_) {
        if (session.client_comp_id == comp_id) {
            return &session;
        }
    }
    return nullptr;
}

} // namespaces
