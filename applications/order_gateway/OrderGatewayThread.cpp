// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "OrderGatewayThread.hpp"
#include "FixErEncoder.hpp"

#include <openssl/rand.h>

#include <charconv>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace order_gateway {

namespace {

constexpr int16_t pdu_id_authentication_request = 500;
constexpr int16_t pdu_id_authentication_challenge = 501;
constexpr int16_t pdu_id_authentication_proof = 502;
constexpr int16_t pdu_id_authentication_result = 503;

constexpr size_t scram_client_nonce_size = 16;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const OrderGatewayConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "OrderGatewayPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
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

} // namespace

OrderGatewayThread::OrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                       pubsub_itc_fw::Reactor& reactor, const OrderGatewayConfiguration& config)
    : ApplicationThread(token, logger, reactor, "OrderGatewayThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , serialiser_(config.sender_comp_id, config.default_target_comp_id, *config.wall_clock)
    , auth_service_primary_conn_id_{}
    , auth_service_secondary_conn_id_{}
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{}
    , capture_(config.fix_capture_enabled
               ? std::make_unique<FixCapture>(config.fix_capture_file, logger,
                                              static_cast<size_t>(config.fix_capture_queue_depth))
               : nullptr) {}

void OrderGatewayThread::on_app_ready_event() {
    connect_to_service("authentication_service_primary");
    if (config_.ha_enabled) {
        connect_to_service("authentication_service_secondary");
    }
    connect_to_service("sequencer_primary");
    if (config_.ha_enabled) {
        connect_to_service("sequencer_secondary");
    }
}

void OrderGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "authentication_service_primary") {
        auth_service_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: primary authentication service connection {} established",
                   id.get_value());
    } else if (id.service_name() == "authentication_service_secondary") {
        auth_service_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: secondary authentication service connection {} established",
                   id.get_value());
    } else if (id.service_name() == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: primary sequencer connection {} ({}) established", id.get_value(),
                   id.service_name());
    } else if (id.service_name() == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: secondary sequencer connection {} established", id.get_value());
    } else if (id.service_name() == er_inbound_svc_) {
        // Inbound FrameworkPdu connection from a sequencer on the ER listener.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: sequencer ER inbound connection {} established", id.get_value());
    } else {
        // Inbound RawBytes connection -- FIX client on port 9879.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: FIX client connection {} ({}) established -- active sessions: {}",
                   id.get_value(), id.service_name(), sessions_.size() + 1);
    }
}

void OrderGatewayThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        cancel_timer(it->second.logon_timeout_timer_name());
        cancel_timer(it->second.scram_auth_timeout_timer_name());
        sessions_.erase(it);
    }

    if (id == auth_service_primary_conn_id_) {
        auth_service_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: primary authentication service connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id == auth_service_secondary_conn_id_) {
        auth_service_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: secondary authentication service connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: primary sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: secondary sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id.service_name() == er_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: sequencer ER inbound connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: FIX client connection {} lost: {} -- active sessions: {}",
                   id.get_value(), reason, sessions_.size());
    }
}

void OrderGatewayThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const uint8_t* data = message.payload();
    const int available = message.payload_size();
    const int64_t event_tail_position = message.tail_position();

    if (data == nullptr || available <= 0) {
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "OrderGatewayThread: {} raw bytes received on connection {} ({}) at tail {}", available,
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
        sessions_.emplace(std::piecewise_construct, std::forward_as_tuple(conn_id),
                          std::forward_as_tuple(conn_id, get_logger(), [this, conn_id](const ParsedFixMessage& msg) {
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
                                             "OrderGatewayThread: connection {} ({}) MsgType='{}' before Logon -- disconnecting", conn_id.get_value(),
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
                                  handle_new_order_single(session, msg);
                              } else if (type == MsgType::OrderCancelRequest) {
                                  handle_order_cancel_request(session, msg);
                              } else {
                                  PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: connection {} ({}) ignoring MsgType='{}'",
                                             conn_id.get_value(), conn_id.service_name(), type);
                              }
                          }));

        start_one_off_timer(sessions_.at(conn_id).logon_timeout_timer_name(), config_.logon_timeout);

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "OrderGatewayThread: connection {} new FIX session, waiting for Logon "
                   "(timeout {}s) -- active sessions: {}",
                   conn_id.get_value(), config_.logon_timeout.count(), sessions_.size());

        it = sessions_.find(conn_id);
    }

    FixSession& session = it->second;

    // ----------------------------------------------------------------
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
    // ----------------------------------------------------------------
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
                   "OrderGatewayThread: connection {} cumulative-bytes invariant violation: "
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
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} invalid FIX preamble -- disconnecting",
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: connection {} FIX preamble verified", conn_id.get_value());
    }

    // feed() returns the number of bytes fully consumed by complete FIX messages.
    // Partial-message bytes at the end of the window are not consumed; the
    // MirroredBuffer retains them by advancing its read pointer only by the
    // consumed count. On the next event the window will begin at those partial
    // bytes, followed by whatever new TCP data arrived, and the parser picks up
    // exactly where it left off.
    const size_t consumed = session.parser.feed(new_bytes_ptr, static_cast<size_t>(new_bytes_len));
    if (capture_ != nullptr && consumed > 0) {
        capture_->capture(FixCapture::Direction::Inbound, new_bytes_ptr, consumed,
                          config_.wall_clock->now_ns());
    }
    commit_raw_bytes(conn_id, static_cast<int64_t>(consumed));
    session.absolute_bytes_committed_ += static_cast<int64_t>(consumed);
}

void OrderGatewayThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();

    const bool from_auth_service =
        (message.connection_id() == auth_service_primary_conn_id_) || (config_.ha_enabled && message.connection_id() == auth_service_secondary_conn_id_);
    if (from_auth_service) {
        if (pdu_id == pdu_id_authentication_challenge) {
            handle_authentication_challenge(message);
        } else if (pdu_id == pdu_id_authentication_result) {
            handle_authentication_result(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: unexpected PDU id {} from authentication service -- dropping",
                       pdu_id);
            release_pdu_payload(message);
        }
        return;
    }

    if (pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: unsupported PDU id {} on connection {} -- dropping", pdu_id,
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ExecutionReportView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: failed to decode ExecutionReport -- dropping");
        release_pdu_payload(message);
        return;
    }

    // Route to the exact FIX session identified by gateway_session_conn_id, which
    // the gateway stamped on the original NOS and the sequencer echoes back here.
    if (!view.has_gateway_session_conn_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: ExecutionReport OrderID={} ExecID={} has no gateway_session_conn_id -- dropping", view.order_id, view.exec_id);
        release_pdu_payload(message);
        return;
    }

    FixSession* session_ptr = find_session_by_conn_id(view.gateway_session_conn_id);
    if (!session_ptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: ExecutionReport gateway_session_conn_id={} -- "
                   "no matching FIX session (client may have disconnected) -- dropping",
                   view.gateway_session_conn_id);
        release_pdu_payload(message);
        return;
    }
    FixSession& session = *session_ptr;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "GW-ER-SENT connection={} OrderID={} ExecID={} gateway_session_conn_id={}",
               session.conn_id.get_value(), view.order_id, view.exec_id, view.gateway_session_conn_id);

    // Encode the FIX ExecutionReport directly into a stack buffer.
    // No heap allocation: all string_view fields from view are memcpy'd
    // straight into the wire bytes; enum fields are cast to single chars.
    char wire_buffer[execution_report_buffer_size];
    const size_t wire_length = encode_execution_report(view, config_.sender_comp_id, session.client_comp_id, session.outbound_seq_num++, *config_.wall_clock,
                                                       wire_buffer, sizeof(wire_buffer));
    if (capture_ != nullptr) {
        capture_->capture(FixCapture::Direction::Outbound,
                          reinterpret_cast<const uint8_t*>(wire_buffer), wire_length,
                          config_.wall_clock->now_ns());
    }
    send_raw(session.conn_id, wire_buffer, static_cast<uint32_t>(wire_length));
    release_pdu_payload(message);
}

void OrderGatewayThread::on_timer_event(const std::string& name) {
    for (auto& [id, session] : sessions_) {
        if (session.logon_timeout_timer_name() == name) {
            if (!session.session_established) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} logon timeout -- disconnecting",
                           id.get_value());
                disconnect_session(session, "logon timeout");
            }
            return;
        }
        if (session.scram_auth_timeout_timer_name() == name) {
            if (session.auth_pending) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} SCRAM authentication timeout -- disconnecting",
                           id.get_value());
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

void OrderGatewayThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {
    // Do nothing
}

// -----------------------------------------------------------------------
// Authentication PDU handlers
// -----------------------------------------------------------------------

void OrderGatewayThread::handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationChallengeView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "OrderGatewayThread: failed to decode AuthenticationChallenge -- dropping");
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
                   "OrderGatewayThread: AuthenticationChallenge request_id={} -- no matching pending session -- dropping", view.request_id);
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

    const std::vector<uint8_t> salted_password = scram_crypto::pbkdf2_sha256(config_.scram_password, salt.data(), salt.size(), iterations);

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
    // Send on the connection the challenge arrived on — correct for both primary and secondary.
    send_pdu(auth_service_conn_id, pdu_id_authentication_proof, 0, proof);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: connection {} AuthenticationProof sent request_id={}",
               session.conn_id.get_value(), view.request_id);
}

void OrderGatewayThread::handle_authentication_result(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationResultView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "OrderGatewayThread: failed to decode AuthenticationResult -- dropping");
        release_pdu_payload(message);
        return;
    }

    release_pdu_payload(message);

    FixSession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (!session_ptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: AuthenticationResult request_id={} -- no matching pending session -- dropping", view.request_id);
        return;
    }
    FixSession& session = *session_ptr;
    session.auth_pending = false;
    cancel_timer(session.scram_auth_timeout_timer_name());

    if (view.outcome != pubsub_itc_fw_app::AuthenticationOutcome::Granted) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} authentication failed outcome={} -- sending Logout",
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
                   "OrderGatewayThread: connection {} ServerSignature mismatch -- possible impostor -- sending Logout", session.conn_id.get_value());
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication service identity could not be verified"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "ServerSignature mismatch");
        return;
    }

    // Authenticated. Send the FIX Logon reply and open the session.
    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logon);
    reply.set(Tag::EncryptMethod, 0);
    reply.set(Tag::HeartBtInt, session.heartbeat_interval);
    reply.set(Tag::DefaultApplVerID, std::string("9"));
    send_fix_to_session(session, reply);
    session.session_established = true;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "OrderGatewayThread: connection {} authentication succeeded -- FIX session established comp_id='{}'", session.conn_id.get_value(),
               session.client_comp_id);
}

// -----------------------------------------------------------------------
// FIX session handlers
// -----------------------------------------------------------------------

void OrderGatewayThread::handle_logon(FixSession& session, const ParsedFixMessage& msg) {
    cancel_timer(session.logon_timeout_timer_name());
    // client_comp_id is stored as std::string for use beyond this callback;
    // the implicit conversion from string_view copies the bytes here.
    session.client_comp_id = msg.get(Tag::SenderCompID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "OrderGatewayThread: connection {} Logon from SenderCompID='{}' -- initiating SCRAM authentication", session.conn_id.get_value(),
               session.client_comp_id);

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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} primary auth service not connected -- using secondary",
                   session.conn_id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "OrderGatewayThread: connection {} Logon rejected -- no authentication service connected",
                   session.conn_id.get_value());
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication service unavailable"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "authentication service not connected");
        return;
    }

    uint8_t nonce_bytes[scram_client_nonce_size];
    if (RAND_bytes(nonce_bytes, static_cast<int>(sizeof(nonce_bytes))) != 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "OrderGatewayThread: connection {} RAND_bytes failed -- disconnecting",
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
    send_pdu(auth_conn_id, pdu_id_authentication_request, 0, auth_request);

    // Arm a timeout so the session is not left pending indefinitely if the
    // authentication service is slow or loses the request.
    start_one_off_timer(session.scram_auth_timeout_timer_name(), config_.scram_auth_timeout);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "OrderGatewayThread: connection {} AuthenticationRequest sent request_id={} comp_id='{}' timeout={}s", session.conn_id.get_value(),
               auth_request.request_id, session.client_comp_id, config_.scram_auth_timeout.count());
}

void OrderGatewayThread::handle_heartbeat(FixSession& session, [[maybe_unused]] const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "OrderGatewayThread: connection {} Heartbeat", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix_to_session(session, reply);
}

void OrderGatewayThread::handle_test_request(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: connection {} TestRequest", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID -- set(int, string_view) copies into the reply
    send_fix_to_session(session, reply);
}

void OrderGatewayThread::handle_logout(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: connection {} Logout: {}", session.conn_id.get_value(), msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix_to_session(session, reply);
    session.session_established = false;
    disconnect_session(session, "client sent Logout");
}

void OrderGatewayThread::handle_resend_request(FixSession& session, const ParsedFixMessage& msg) {
    int begin_seq = 1;
    const std::string_view begin_str = msg.get(Tag::BeginSeqNo);
    if (!begin_str.empty()) {
        std::from_chars(begin_str.data(), begin_str.data() + begin_str.size(), begin_seq);
    }

    // Fill the entire gap in one shot: NewSeqNo = current outbound position.
    // fix8 will buffer messages it received after the gap; those with seq < NewSeqNo
    // are gap-filled and discarded by fix8's session layer, but the session stays
    // open and subsequent ERs (at NewSeqNo onwards) flow normally.
    // One-at-a-time filling (NewSeqNo=begin_seq+1) creates a feedback loop of
    // thousands of ResendRequest/SequenceReset exchanges that freezes the session.
    const int next_seq = session.outbound_seq_num;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "OrderGatewayThread: connection {} ResendRequest BeginSeqNo={} -- sending SequenceReset-GapFill NewSeqNo={}",
               session.conn_id.get_value(), begin_seq, next_seq);

    session.outbound_seq_num = begin_seq;
    FixMessage reset;
    reset.set(Tag::MsgType, MsgType::SequenceReset);
    reset.set(Tag::GapFillFlag, std::string("Y"));
    reset.set(Tag::NewSeqNo, std::to_string(next_seq));
    send_fix_to_session(session, reset);  // stamps MsgSeqNum=begin_seq, increments to begin_seq+1
    session.outbound_seq_num = next_seq;
}

void OrderGatewayThread::handle_new_order_single(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "entered handle_new_order_single");

    const std::string_view cl_ord_id = msg.get(Tag::ClOrdID);
    const std::string_view symbol = msg.get(Tag::Symbol);
    const std::string_view side_str = msg.get(Tag::Side);
    const std::string_view ord_type_str = msg.get(Tag::OrdType);
    const std::string_view order_qty = msg.get(Tag::OrderQty);
    const std::string_view price_str = msg.get(Tag::Price);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "GW-NOS-RECV connection={} ClOrdID={} Symbol={} Side={}", session.conn_id.get_value(), cl_ord_id,
               symbol, side_str);

    // Validate required fields.
    if (cl_ord_id.empty() || symbol.empty() || side_str.empty() || ord_type_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: connection {} NewOrderSingle missing required fields"
                   " -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // If no sequencer is connected, reject the order locally with an ExecutionReport
    // rather than silently dropping it. The client gets a definitive response per
    // order and the FIX session stays up so subsequent orders can be tried once
    // connectivity is restored. During failover the secondary takes over as leader,
    // so orders are forwarded as long as either sequencer connection is alive.
    if (!sequencer_primary_conn_id_.is_valid() && !sequencer_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: connection {} NewOrderSingle ClOrdID={} rejected "
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

    // Retain SenderCompID for audit/logging in the sequencer WAL.
    if (!session.client_comp_id.empty()) {
        nos.has_sender_comp_id = true;
        nos.sender_comp_id = session.client_comp_id;
    }

    // Stamp the internal connection ID so the sequencer can route the ER back
    // to this exact FIX session.  This is unique per TCP connection and avoids
    // the ClOrdID collision problem that arises when multiple sessions share a
    // SenderCompID or reuse the same ClOrdID sequence.
    nos.has_gateway_session_conn_id = true;
    nos.gateway_session_conn_id = session.conn_id.get_value();

    // Forward the encoded PDU to both sequencer instances.
    // The sequencer will wrap it in a SequencedMessage envelope.
    forward_pdu_to_sequencers(static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle), nos);

    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "exit handle_new_order_single");
}

void OrderGatewayThread::handle_order_cancel_request(FixSession& session, const ParsedFixMessage& msg) {
    const std::string_view cl_ord_id = msg.get(Tag::ClOrdID);
    const std::string_view orig_cl_ord_id = msg.get(Tag::OrigClOrdID);
    const std::string_view symbol = msg.get(Tag::Symbol);
    const std::string_view side_str = msg.get(Tag::Side);
    const std::string_view order_qty = msg.get(Tag::OrderQty);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "OrderGatewayThread: connection {} OrderCancelRequest ClOrdID={} "
               "OrigClOrdID={} Symbol={}",
               session.conn_id.get_value(), cl_ord_id, orig_cl_ord_id, symbol);

    if (cl_ord_id.empty() || orig_cl_ord_id.empty() || symbol.empty() || side_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: connection {} OrderCancelRequest missing required "
                   "fields -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // If no sequencer is connected, reject the cancel locally. See
    // handle_new_order_single for the rationale.
    if (!sequencer_primary_conn_id_.is_valid() && !sequencer_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderGatewayThread: connection {} OrderCancelRequest ClOrdID={} "
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

    // Retain SenderCompID for audit/logging.
    if (!session.client_comp_id.empty()) {
        ocr.has_sender_comp_id = true;
        ocr.sender_comp_id = session.client_comp_id;
    }

    // Stamp the internal connection ID for cancel-ER routing (same mechanism as NOS).
    ocr.has_gateway_session_conn_id = true;
    ocr.gateway_session_conn_id = session.conn_id.get_value();

    forward_pdu_to_sequencers(static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest), ocr);
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

void OrderGatewayThread::disconnect_session(const FixSession& session, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "OrderGatewayThread: disconnecting connection {}: {}", session.conn_id.get_value(), reason);

    pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(cmd);
}

void OrderGatewayThread::send_fix_to_session(FixSession& session, const FixMessage& msg) {
    const std::string wire = serialiser_.serialise(msg, session.outbound_seq_num++);
    if (capture_ != nullptr) {
        capture_->capture(FixCapture::Direction::Outbound,
                          reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
                          config_.wall_clock->now_ns());
    }
    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
}

void OrderGatewayThread::send_reject_execution_report(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason, bool is_cancel) {
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
               "OrderGatewayThread: connection {} sending reject ExecutionReport "
               "OrderID={} ExecID={} ClOrdID={} reason='{}'",
               session.conn_id.get_value(), order_id, exec_id, cl_ord_id, reason);

    send_fix_to_session(session, er);
}

// -----------------------------------------------------------------------
// PDU forwarding
// -----------------------------------------------------------------------

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

FixSession* OrderGatewayThread::find_session_by_conn_id(int32_t gateway_session_conn_id) {
    auto it = sessions_.find(pubsub_itc_fw::ConnectionID{gateway_session_conn_id});
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

FixSession* OrderGatewayThread::find_session_by_comp_id(const std::string& comp_id) {
    for (auto& [conn_id, session] : sessions_) {
        if (session.client_comp_id == comp_id) {
            return &session;
        }
    }
    return nullptr;
}

} // namespace order_gateway
