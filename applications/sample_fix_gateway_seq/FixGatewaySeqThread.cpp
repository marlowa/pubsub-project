// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewaySeqThread.hpp"
#include "FixErEncoder.hpp"

#include <openssl/rand.h>

#include <charconv>
#include <cstring>
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

namespace sample_fix_gateway_seq {

namespace {

static constexpr int16_t pdu_id_authentication_request   = 500;
static constexpr int16_t pdu_id_authentication_challenge = 501;
static constexpr int16_t pdu_id_authentication_proof     = 502;
static constexpr int16_t pdu_id_authentication_result    = 503;

static constexpr size_t scram_client_nonce_size = 16;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const FixGatewaySeqConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "FixGatewaySeqPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

FixGatewaySeqThread::FixGatewaySeqThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                         pubsub_itc_fw::Reactor& reactor, const FixGatewaySeqConfiguration& config)
    : ApplicationThread(token, logger, reactor, "FixGatewaySeqThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , er_inbound_svc_("inbound:" + std::to_string(config.er_listen_port))
    , serialiser_(config.sender_comp_id, config.default_target_comp_id)
    , auth_service_conn_id_{}
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{} {}

void FixGatewaySeqThread::on_app_ready_event() {
    connect_to_service("authentication_service");
    connect_to_service("sequencer_primary");
    if (config_.ha_enabled) {
        connect_to_service("sequencer_secondary");
    }
}

void FixGatewaySeqThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "authentication_service") {
        auth_service_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: authentication service connection {} established", id.get_value());
    } else if (id.service_name() == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: primary sequencer connection {} ({}) established", id.get_value(),
                   id.service_name());
    } else if (id.service_name() == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: secondary sequencer connection {} established", id.get_value());
    } else if (id.service_name() == er_inbound_svc_) {
        // Inbound FrameworkPdu connection from a sequencer on the ER listener.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: sequencer ER inbound connection {} established", id.get_value());
    } else {
        // Inbound RawBytes connection -- FIX client on port 9879.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: FIX client connection {} ({}) established -- active sessions: {}",
                   id.get_value(), id.service_name(), sessions_.size() + 1);
    }
}

void FixGatewaySeqThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        cancel_timer(it->second.logon_timeout_timer_name());
        sessions_.erase(it);
    }

    if (id == auth_service_conn_id_) {
        auth_service_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: authentication service connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: primary sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: secondary sequencer connection {} lost: {}", id.get_value(), reason);
    } else if (id.service_name() == er_inbound_svc_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: sequencer ER inbound connection {} lost: {}", id.get_value(),
                   reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: FIX client connection {} lost: {} -- active sessions: {}",
                   id.get_value(), reason, sessions_.size());
    }
}

void FixGatewaySeqThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID& conn_id = message.connection_id();
    const uint8_t* data = message.payload();
    const int available = message.payload_size();
    const int64_t event_tail_position = message.tail_position();

    if (data == nullptr || available <= 0) {
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixGatewaySeqThread: {} raw bytes received on connection {} ({}) at tail {}", available,
               conn_id.get_value(), conn_id.service_name(), event_tail_position);

    // Hex-dump only the first few hundred bytes. Under burst load, available
    // can reach the MirroredBuffer's capacity (~64 KB), and a full hex_dump
    // produces a multi-hundred-KB string that adds nothing useful to the log
    // and has been observed to crash the gateway under fix8 command "T"
    // (1000 NOS burst). The truncation is purely a logging-helper concern;
    // the rest of this function still processes all `available` bytes.
    constexpr int hex_dump_limit = 256;
    const int hex_dump_len = (available < hex_dump_limit) ? available : hex_dump_limit;
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, pubsub_itc_fw::StringUtils::hex_dump(data, hex_dump_len));

    // Create a session on first data from this connection if not already present.
    // TODO need to document why we use piecewise_construct here.

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
                                             "FixGatewaySeqThread: connection {} ({}) MsgType='{}' before Logon -- disconnecting", conn_id.get_value(),
                                             conn_id.service_name(), type);
                                  disconnect_session(session, "first message was not Logon");
                              } else if (type == MsgType::Heartbeat) {
                                  handle_heartbeat(session, msg);
                              } else if (type == MsgType::TestRequest) {
                                  handle_test_request(session, msg);
                              } else if (type == MsgType::Logout) {
                                  handle_logout(session, msg);
                              } else if (type == MsgType::NewOrderSingle) {
                                  handle_new_order_single(session, msg);
                              } else if (type == MsgType::OrderCancelRequest) {
                                  handle_order_cancel_request(session, msg);
                              } else {
                                  PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: connection {} ({}) ignoring MsgType='{}'",
                                             conn_id.get_value(), conn_id.service_name(), type);
                              }
                          }));

        start_one_off_timer(sessions_.at(conn_id).logon_timeout_timer_name(), config_.logon_timeout);

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: connection {} new FIX session, waiting for Logon "
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
                   "FixGatewaySeqThread: connection {} cumulative-bytes invariant violation: "
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
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: connection {} invalid FIX preamble -- disconnecting",
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: connection {} FIX preamble verified", conn_id.get_value());
    }

    // feed() returns the number of bytes fully consumed by complete FIX messages.
    // Partial-message bytes at the end of the window are not consumed; the
    // MirroredBuffer retains them by advancing its read pointer only by the
    // consumed count. On the next event the window will begin at those partial
    // bytes, followed by whatever new TCP data arrived, and the parser picks up
    // exactly where it left off.
    const size_t consumed = session.parser.feed(new_bytes_ptr, static_cast<size_t>(new_bytes_len));
    commit_raw_bytes(conn_id, static_cast<int64_t>(consumed));
    session.absolute_bytes_committed_ += static_cast<int64_t>(consumed);
}

void FixGatewaySeqThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());

    if (message.connection_id() == auth_service_conn_id_) {
        if (pdu_id == pdu_id_authentication_challenge) {
            handle_authentication_challenge(message);
        } else if (pdu_id == pdu_id_authentication_result) {
            handle_authentication_result(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "FixGatewaySeqThread: unexpected PDU id {} from authentication service -- dropping", pdu_id);
            release_pdu_payload(message);
        }
        return;
    }

    if (pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: unsupported PDU id {} on connection {} -- dropping", pdu_id,
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
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: failed to decode ExecutionReport -- dropping");
        release_pdu_payload(message);
        return;
    }

    // Route to the exact FIX session identified by gateway_session_conn_id, which
    // the gateway stamped on the original NOS and the sequencer echoes back here.
    if (!view.has_gateway_session_conn_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: ExecutionReport OrderID={} ExecID={} has no gateway_session_conn_id -- dropping", view.order_id, view.exec_id);
        release_pdu_payload(message);
        return;
    }

    FixSession* session_ptr = find_session_by_conn_id(view.gateway_session_conn_id);
    if (!session_ptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: ExecutionReport gateway_session_conn_id={} -- "
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
    const size_t wire_length =
        encode_execution_report(view, config_.sender_comp_id, session.client_comp_id, session.outbound_seq_num++, wire_buffer, sizeof(wire_buffer));
    send_raw(session.conn_id, wire_buffer, static_cast<uint32_t>(wire_length));
    release_pdu_payload(message);
}

void FixGatewaySeqThread::on_timer_event(const std::string& name) {
    for (auto& [id, session] : sessions_) {
        if (session.logon_timeout_timer_name() == name) {
            if (!session.session_established) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqThread: connection {} logon timeout -- disconnecting",
                           id.get_value());
                disconnect_session(session, "logon timeout");
            }
            return;
        }
    }
}

void FixGatewaySeqThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// -----------------------------------------------------------------------
// Authentication PDU handlers
// -----------------------------------------------------------------------

void FixGatewaySeqThread::handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationChallengeView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                    bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "FixGatewaySeqThread: failed to decode AuthenticationChallenge -- dropping");
        release_pdu_payload(message);
        return;
    }

    release_pdu_payload(message);

    // Correlate back to the FIX session using request_id == conn_id.
    FixSession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (!session_ptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: AuthenticationChallenge request_id={} -- no matching pending session -- dropping",
                   view.request_id);
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

    const std::vector<uint8_t> auth_message = scram_crypto::compute_auth_message(
        session.client_comp_id, session.scram_client_nonce, server_nonce,
        salt.data(), salt.size(), iterations);

    const std::vector<uint8_t> salted_password = scram_crypto::pbkdf2_sha256(
        config_.scram_password, salt.data(), salt.size(), iterations);

    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(client_key_label.data()), client_key_label.size());

    const std::vector<uint8_t> stored_key = scram_crypto::sha256(client_key.data(), client_key.size());

    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(
        stored_key.data(), stored_key.size(),
        auth_message.data(), auth_message.size());

    static constexpr size_t sha256_size = 32;
    std::vector<uint8_t> client_proof(sha256_size);
    for (size_t i = 0; i < sha256_size; ++i) {
        client_proof[i] = client_key[i] ^ client_signature[i];
    }

    const std::vector<uint8_t> server_key = scram_crypto::hmac_sha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(server_key_label.data()), server_key_label.size());

    session.scram_expected_server_signature = scram_crypto::hmac_sha256(
        server_key.data(), server_key.size(),
        auth_message.data(), auth_message.size());

    pubsub_itc_fw_app::AuthenticationProof proof{};
    proof.request_id   = view.request_id;
    proof.client_proof = pubsub_itc_fw_app::BytesView{client_proof.data(), client_proof.size()};
    send_pdu(auth_service_conn_id_, pdu_id_authentication_proof, 0, proof);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} AuthenticationProof sent request_id={}",
               session.conn_id.get_value(), view.request_id);
}

void FixGatewaySeqThread::handle_authentication_result(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationResultView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                    bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "FixGatewaySeqThread: failed to decode AuthenticationResult -- dropping");
        release_pdu_payload(message);
        return;
    }

    release_pdu_payload(message);

    FixSession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (!session_ptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: AuthenticationResult request_id={} -- no matching pending session -- dropping",
                   view.request_id);
        return;
    }
    FixSession& session = *session_ptr;
    session.auth_pending = false;

    if (view.outcome != pubsub_itc_fw_app::AuthenticationOutcome::Granted) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} authentication failed outcome={} -- sending Logout",
                   session.conn_id.get_value(), static_cast<int32_t>(view.outcome));
        FixMessage logout;
        logout.set(Tag::MsgType, MsgType::Logout);
        logout.set(Tag::Text, std::string("Authentication failed"));
        send_fix_to_session(session, logout);
        disconnect_session(session, "authentication failed");
        return;
    }

    // Verify ServerSignature to confirm we are speaking to the genuine service.
    const std::vector<uint8_t> received_signature(view.server_signature.data,
                                                   view.server_signature.data + view.server_signature.size);
    if (received_signature != session.scram_expected_server_signature) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} ServerSignature mismatch -- possible impostor -- sending Logout",
                   session.conn_id.get_value());
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
    send_fix_to_session(session, reply);
    session.session_established = true;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} authentication succeeded -- FIX session established comp_id='{}'",
               session.conn_id.get_value(), session.client_comp_id);
}

// -----------------------------------------------------------------------
// FIX session handlers
// -----------------------------------------------------------------------

void FixGatewaySeqThread::handle_logon(FixSession& session, const ParsedFixMessage& msg) {
    cancel_timer(session.logon_timeout_timer_name());
    // client_comp_id is stored as std::string for use beyond this callback;
    // the implicit conversion from string_view copies the bytes here.
    session.client_comp_id = msg.get(Tag::SenderCompID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} Logon from SenderCompID='{}' -- initiating SCRAM authentication",
               session.conn_id.get_value(), session.client_comp_id);

    const std::string_view heartbeat_interval_text = msg.get(Tag::HeartBtInt);
    session.heartbeat_interval = 30;
    if (!heartbeat_interval_text.empty()) {
        std::from_chars(heartbeat_interval_text.data(), heartbeat_interval_text.data() + heartbeat_interval_text.size(), session.heartbeat_interval);
    }

    if (!auth_service_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} Logon rejected -- authentication service not connected",
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "FixGatewaySeqThread: connection {} RAND_bytes failed -- disconnecting", session.conn_id.get_value());
        disconnect_session(session, "failed to generate client nonce");
        return;
    }
    session.scram_client_nonce.assign(nonce_bytes, nonce_bytes + sizeof(nonce_bytes));
    session.auth_pending = true;

    pubsub_itc_fw_app::AuthenticationRequest auth_request{};
    auth_request.request_id  = static_cast<int64_t>(session.conn_id.get_value());
    auth_request.comp_id     = session.client_comp_id;
    auth_request.client_nonce = pubsub_itc_fw_app::BytesView{session.scram_client_nonce.data(), session.scram_client_nonce.size()};
    send_pdu(auth_service_conn_id_, pdu_id_authentication_request, 0, auth_request);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} AuthenticationRequest sent request_id={} comp_id='{}'",
               session.conn_id.get_value(), auth_request.request_id, session.client_comp_id);
}

void FixGatewaySeqThread::handle_heartbeat(FixSession& session, [[maybe_unused]] const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixGatewaySeqThread: connection {} Heartbeat", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix_to_session(session, reply);
}

void FixGatewaySeqThread::handle_test_request(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: connection {} TestRequest", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID -- set(int, string_view) copies into the reply
    send_fix_to_session(session, reply);
}

void FixGatewaySeqThread::handle_logout(FixSession& session, const ParsedFixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: connection {} Logout: {}", session.conn_id.get_value(), msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix_to_session(session, reply);
    session.session_established = false;
    disconnect_session(session, "client sent Logout");
}

void FixGatewaySeqThread::handle_new_order_single(FixSession& session, const ParsedFixMessage& msg) {
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
                   "FixGatewaySeqThread: connection {} NewOrderSingle missing required fields"
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
                   "FixGatewaySeqThread: connection {} NewOrderSingle ClOrdID={} rejected "
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
    nos.transact_time = 0; // TODO: parse SendingTime (tag 52) to epoch nanos
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
}

void FixGatewaySeqThread::handle_order_cancel_request(FixSession& session, const ParsedFixMessage& msg) {
    const std::string_view cl_ord_id = msg.get(Tag::ClOrdID);
    const std::string_view orig_cl_ord_id = msg.get(Tag::OrigClOrdID);
    const std::string_view symbol = msg.get(Tag::Symbol);
    const std::string_view side_str = msg.get(Tag::Side);
    const std::string_view order_qty = msg.get(Tag::OrderQty);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} OrderCancelRequest ClOrdID={} "
               "OrigClOrdID={} Symbol={}",
               session.conn_id.get_value(), cl_ord_id, orig_cl_ord_id, symbol);

    if (cl_ord_id.empty() || orig_cl_ord_id.empty() || symbol.empty() || side_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} OrderCancelRequest missing required "
                   "fields -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // If no sequencer is connected, reject the cancel locally. See
    // handle_new_order_single for the rationale.
    if (!sequencer_primary_conn_id_.is_valid() && !sequencer_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} OrderCancelRequest ClOrdID={} "
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
    ocr.transact_time = 0; // TODO: parse SendingTime
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

void FixGatewaySeqThread::disconnect_session(const FixSession& session, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewaySeqThread: disconnecting connection {}: {}", session.conn_id.get_value(), reason);

    pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(cmd);
}

void FixGatewaySeqThread::send_fix_to_session(FixSession& session, const FixMessage& msg) {
    const std::string wire = serialiser_.serialise(msg, session.outbound_seq_num++);
    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
}

void FixGatewaySeqThread::send_reject_execution_report(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason, bool is_cancel) {
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
               "FixGatewaySeqThread: connection {} sending reject ExecutionReport "
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

FixSession* FixGatewaySeqThread::find_session_by_conn_id(int32_t gateway_session_conn_id) {
    auto it = sessions_.find(pubsub_itc_fw::ConnectionID{gateway_session_conn_id});
    if (it == sessions_.end())
        return nullptr;
    return &it->second;
}

FixSession* FixGatewaySeqThread::find_session_by_comp_id(const std::string& comp_id) {
    for (auto& [conn_id, session] : sessions_) {
        if (session.client_comp_id == comp_id) {
            return &session;
        }
    }
    return nullptr;
}

} // namespace sample_fix_gateway_seq
