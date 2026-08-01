// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "BinaryOrderGatewayThread.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace binary_order_gateway {

namespace {

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const BinaryOrderGatewayConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "BinaryOrderGatewayPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

// Sent per drain tick. A client with thousands of resting orders would otherwise stall
// the reactor while every cancel was encoded and forwarded in one go.
// SCRAM client nonce length, matching the FIX gateway's.
constexpr size_t scram_client_nonce_size = 16;

// Orders between GW-PROGRESS lines; matches the FIX order gateway so the two are comparable.
constexpr int64_t order_progress_interval = 1000;

constexpr int cancel_drain_batch_size = 500;
constexpr auto cancel_drain_interval = std::chrono::milliseconds{1};

/** @brief True for an ExecutionReport status that means the order has left the book. */
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

BinaryOrderGatewayThread::BinaryOrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                                   pubsub_itc_fw::Reactor& reactor, const BinaryOrderGatewayConfiguration& config)
    : ApplicationThread(token, logger, reactor, "BinaryOrderGatewayThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(),
                        make_allocator_config(config, logger), pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{}
    , client_inbound_service_("inbound:" + std::to_string(config.listen_port))
    , er_inbound_service_("inbound:" + std::to_string(config.er_listen_port)) {}

void BinaryOrderGatewayThread::on_app_ready_event() {
    open_order_pool_ = std::make_unique<pubsub_itc_fw::ExpandablePoolAllocator<open_orders::OpenOrderEntry>>(
        "BinaryOpenOrderPool", config_.open_order_pool_objects_per_pool, config_.open_order_pool_initial_pools,
        /*expansion_threshold_hint=*/0,
        /*handler_for_pool_exhausted=*/
        [this](void* /*context*/, int objects_per_pool) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOpenOrderPool exhausted: chaining new pool slab ({} objects)",
                       objects_per_pool);
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

void BinaryOrderGatewayThread::on_timer_event(pubsub_itc_fw::TimerID timer_id) {
    if (timer_id == cancel_drain_timer_id_) {
        drain_pending_cancels();
        return;
    }

    if (timer_id == grace_timer_id_) {
        expire_grace_sessions();
        return;
    }

    // A logon left pending because the authentication service never answered. Refusing it
    // matters: a session stuck in auth_pending would otherwise hold a connection open for
    // ever without ever being able to trade.
    for (auto& [conn_id_value, session] : sessions_) {
        if (session.auth_pending && timer_id == session.scram_auth_timeout_timer_id) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: connection {} authentication timed out -- refusing logon",
                       session.conn_id.get_value());
            refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AuthenticationTimeout, "authentication service did not respond");
            return;
        }
    }
}

void BinaryOrderGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& service = id.service_name();

    if (service == "authentication_service_primary") {
        auth_service_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: primary authentication service connection {} established",
                   id.get_value());
        return;
    }
    if (service == "authentication_service_secondary") {
        auth_service_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: secondary authentication service connection {} established",
                   id.get_value());
        return;
    }
    if (service == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: primary sequencer connection {} established", id.get_value());
        return;
    }
    if (service == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: secondary sequencer connection {} established", id.get_value());
        return;
    }
    if (service == er_inbound_service_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: sequencer ER connection {} established", id.get_value());
        return;
    }
    if (service == client_inbound_service_) {
        BinarySession session{};
        session.conn_id = id;
        sessions_.emplace(id.get_value(), std::move(session));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: client connection {} accepted, awaiting Logon", id.get_value());
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: connection {} established on unexpected service '{}'",
               id.get_value(), service);
}

void BinaryOrderGatewayThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == auth_service_primary_conn_id_) {
        auth_service_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: primary authentication service connection {} lost: {}",
                   id.get_value(), reason);
        return;
    }
    if (id == auth_service_secondary_conn_id_) {
        auth_service_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: secondary authentication service connection {} lost: {}",
                   id.get_value(), reason);
        return;
    }
    if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: primary sequencer connection {} lost: {}", id.get_value(),
                   reason);
        return;
    }
    if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: secondary sequencer connection {} lost: {}", id.get_value(),
                   reason);
        return;
    }

    auto it = sessions_.find(id.get_value());
    if (it != sessions_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: client connection {} (comp id '{}') lost: {}", id.get_value(),
                   it->second.comp_id, reason);
        // The client is gone but its orders are still on the book with nobody managing
        // them, so cancel them on its behalf before the session is destroyed.
        queue_session_for_cleanup(it->second);
        sessions_.erase(it);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "BinaryOrderGatewayThread: connection {} lost: {}", id.get_value(), reason);
}

void BinaryOrderGatewayThread::on_itc_message(const pubsub_itc_fw::EventMessage& /*message*/) {}

void BinaryOrderGatewayThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    // ERs arrive on the connection the sequencer dialled; everything else on this thread
    // is a client. Telling them apart by service name rather than by PDU id means a
    // client cannot reach the ER path by sending a PDU it has no business sending.
    if (message.connection_id().service_name() == er_inbound_service_) {
        handle_execution_report(message);
        return;
    }

    const bool from_auth_service =
        message.connection_id() == auth_service_primary_conn_id_ || (config_.ha_enabled && message.connection_id() == auth_service_secondary_conn_id_);
    if (from_auth_service) {
        if (message.pdu_id() == pubsub_itc_fw_app::AuthenticationChallenge::message_pdu_id) {
            handle_authentication_challenge(message);
        } else if (message.pdu_id() == pubsub_itc_fw_app::AuthenticationResult::message_pdu_id) {
            handle_authentication_result(message);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "BinaryOrderGatewayThread: unexpected PDU id {} from the authentication service -- dropping", message.pdu_id());
            release_pdu_payload(message);
        }
        return;
    }

    BinarySession* session = find_session(message.connection_id());
    if (session == nullptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: PDU id {} on unknown connection {} -- dropping",
                   message.pdu_id(), message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    const int16_t pdu_id = message.pdu_id();

    if (pdu_id == pubsub_itc_fw_app::Logon::message_pdu_id) {
        handle_logon(*session, message);
        return;
    }

    if (!session->logged_on) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: PDU id {} on connection {} before Logon -- disconnecting",
                   pdu_id, message.connection_id().get_value());
        release_pdu_payload(message);
        pubsub_itc_fw::ReactorControlCommand command(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        command.connection_id_ = message.connection_id();
        get_reactor().enqueue_control_command(command);
        return;
    }

    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        handle_new_order_single(*session, message);
        return;
    }
    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        handle_order_cancel_request(*session, message);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "BinaryOrderGatewayThread: unsupported PDU id {} from comp id '{}' on connection {} -- dropping", pdu_id, session->comp_id,
               message.connection_id().get_value());
    release_pdu_payload(message);
}

void BinaryOrderGatewayThread::handle_logon(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::LogonView view{};
    const bool decoded =
        pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed);
    release_pdu_payload(message);

    if (!decoded) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: failed to decode Logon on connection {} -- disconnecting",
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::MissingCompId, "Logon could not be decoded");
        return;
    }

    if (session.logged_on || session.auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: second Logon on connection {} -- disconnecting",
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AlreadyLoggedOn, "a logon is already in progress or complete");
        return;
    }

    if (view.comp_id.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: Logon with empty comp id on connection {} -- disconnecting",
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::MissingCompId, "comp_id must not be empty");
        return;
    }

    if (comp_id_in_use(view.comp_id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: Logon for comp id '{}' already logged on -- disconnecting connection {}", view.comp_id,
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::DuplicateCompId, "comp_id is already logged on");
        return;
    }

    // An empty TargetCompID means the client did not care which venue it reached; a
    // populated one that does not match means it has connected somewhere it did not intend,
    // which is a misconfiguration worth failing loudly rather than trading through.
    if (!view.target_comp_id.empty() && view.target_comp_id != config_.sender_comp_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: connection {} Logon names TargetCompID '{}' but this gateway is '{}' -- refusing", session.conn_id.get_value(),
                   view.target_comp_id, config_.sender_comp_id);
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::WrongTargetCompId, "this gateway is " + config_.sender_comp_id);
        return;
    }

    session.comp_id.assign(view.comp_id.data(), view.comp_id.size());
    session.target_comp_id.assign(view.target_comp_id.data(), view.target_comp_id.size());
    session.client_password.assign(view.password.data(), view.password.size());

    // The session is not logged on yet. It becomes so only when the authentication service
    // grants it, so nothing can reach the book on the strength of a comp id alone.
    begin_scram_authentication(session);
}

void BinaryOrderGatewayThread::begin_scram_authentication(BinarySession& session) {
    const pubsub_itc_fw::ConnectionID auth_conn_id =
        auth_service_primary_conn_id_.get_value() != 0 ? auth_service_primary_conn_id_ : auth_service_secondary_conn_id_;
    if (auth_conn_id.get_value() == 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: connection {} Logon refused -- no authentication service connected", session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AuthenticationUnavailable, "authentication service unavailable");
        return;
    }

    uint8_t nonce_bytes[scram_client_nonce_size];
    if (RAND_bytes(nonce_bytes, static_cast<int>(sizeof(nonce_bytes))) != 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "BinaryOrderGatewayThread: connection {} RAND_bytes failed -- disconnecting",
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AuthenticationFailed, "failed to generate a client nonce");
        return;
    }
    session.scram_client_nonce.assign(nonce_bytes, nonce_bytes + sizeof(nonce_bytes));
    session.auth_pending = true;

    pubsub_itc_fw_app::AuthenticationRequest auth_request{};
    // The connection id correlates the reply back to this session, as it does on the FIX
    // gateway: it is unique per connection and the service echoes it untouched.
    auth_request.request_id = static_cast<int64_t>(session.conn_id.get_value());
    auth_request.comp_id = session.comp_id;
    auth_request.client_nonce = pubsub_itc_fw_app::BytesView{session.scram_client_nonce.data(), session.scram_client_nonce.size()};
    send_pdu(auth_conn_id, pubsub_itc_fw_app::AuthenticationRequest::message_pdu_id, 0, auth_request);

    session.scram_auth_timeout_timer_id = start_one_off_timer(config_.scram_auth_timeout);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "BinaryOrderGatewayThread: connection {} AuthenticationRequest sent request_id={} comp_id='{}' timeout={}s", session.conn_id.get_value(),
               auth_request.request_id, session.comp_id, config_.scram_auth_timeout.count());
}

void BinaryOrderGatewayThread::send_logon_ack(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::LogonOutcome outcome, std::string_view text) {
    pubsub_itc_fw_app::LogonAck ack{};
    ack.outcome = outcome;
    if (!text.empty()) {
        ack.has_text = true;
        ack.text = text;
    }
    send_pdu(conn_id, pubsub_itc_fw_app::LogonAck::message_pdu_id, 0, ack);
}

void BinaryOrderGatewayThread::refuse_logon(BinarySession& session, pubsub_itc_fw_app::LogonOutcome outcome, std::string_view text) {
    session.auth_pending = false;
    session.logged_on = false;
    // A refused session may still be holding the password it was given; do not leave it
    // sitting in memory on a connection that is about to be closed.
    std::fill(session.client_password.begin(), session.client_password.end(), '\0');
    session.client_password.clear();

    // The ack is queued before the disconnect so the client learns why, rather than seeing
    // an unexplained close.
    send_logon_ack(session.conn_id, outcome, text);

    pubsub_itc_fw::ReactorControlCommand command(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    command.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(command);
}

void BinaryOrderGatewayThread::handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::AuthenticationChallengeView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "BinaryOrderGatewayThread: failed to decode AuthenticationChallenge -- dropping");
        release_pdu_payload(message);
        return;
    }
    const pubsub_itc_fw::ConnectionID auth_conn_id = message.connection_id();

    BinarySession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (session_ptr == nullptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: AuthenticationChallenge request_id={} -- no matching pending session -- dropping", view.request_id);
        release_pdu_payload(message);
        return;
    }
    BinarySession& session = *session_ptr;

    const std::vector<uint8_t> server_nonce(view.server_nonce.data, view.server_nonce.data + view.server_nonce.size);
    const std::vector<uint8_t> salt(view.salt.data, view.salt.data + view.salt.size);
    const int32_t iterations = view.iterations;
    const int64_t request_id = view.request_id;
    release_pdu_payload(message);

    // The proof is derived here and the password never leaves this process, which is the
    // whole point of SCRAM: the service verifies without ever seeing it.
    static const std::string client_key_label = "Client Key";
    static const std::string server_key_label = "Server Key";

    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message(session.comp_id, session.scram_client_nonce, server_nonce, salt.data(), salt.size(), iterations);
    const std::vector<uint8_t> salted_password = scram_crypto::pbkdf2_sha256(session.client_password, salt.data(), salt.size(), iterations);

    std::fill(session.client_password.begin(), session.client_password.end(), '\0');
    session.client_password.clear();
    session.client_password.shrink_to_fit();

    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(client_key_label.data()), client_key_label.size());
    const std::vector<uint8_t> stored_key = scram_crypto::sha256(client_key.data(), client_key.size());
    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(stored_key.data(), stored_key.size(), auth_message.data(), auth_message.size());

    static constexpr size_t sha256_size = 32;
    std::vector<uint8_t> client_proof(sha256_size);
    for (size_t index = 0; index < sha256_size; ++index) {
        client_proof[index] = client_key[index] ^ client_signature[index];
    }

    const std::vector<uint8_t> server_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(server_key_label.data()), server_key_label.size());
    session.scram_expected_server_signature = scram_crypto::hmac_sha256(server_key.data(), server_key.size(), auth_message.data(), auth_message.size());

    pubsub_itc_fw_app::AuthenticationProof proof{};
    proof.request_id = request_id;
    proof.client_proof = pubsub_itc_fw_app::BytesView{client_proof.data(), client_proof.size()};
    // Reply on the connection the challenge arrived on, so a primary and a secondary
    // service each see their own exchange through.
    send_pdu(auth_conn_id, pubsub_itc_fw_app::AuthenticationProof::message_pdu_id, 0, proof);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: connection {} AuthenticationProof sent request_id={}",
               session.conn_id.get_value(), request_id);
}

void BinaryOrderGatewayThread::handle_authentication_result(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::AuthenticationResultView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "BinaryOrderGatewayThread: failed to decode AuthenticationResult -- dropping");
        release_pdu_payload(message);
        return;
    }

    BinarySession* session_ptr = find_session_by_conn_id(static_cast<int32_t>(view.request_id));
    if (session_ptr == nullptr || !session_ptr->auth_pending) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: AuthenticationResult request_id={} -- no matching pending session -- dropping", view.request_id);
        release_pdu_payload(message);
        return;
    }
    BinarySession& session = *session_ptr;
    const pubsub_itc_fw_app::AuthenticationOutcome outcome = view.outcome;
    const std::vector<uint8_t> server_signature(view.server_signature.data, view.server_signature.data + view.server_signature.size);
    release_pdu_payload(message);

    session.auth_pending = false;
    cancel_timer(session.scram_auth_timeout_timer_id);

    if (outcome != pubsub_itc_fw_app::AuthenticationOutcome::Granted) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: connection {} authentication failed outcome={} -- refusing logon", session.conn_id.get_value(),
                   static_cast<int32_t>(outcome));
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AuthenticationFailed, "authentication failed");
        return;
    }

    // SCRAM is mutual: the service proves itself to the gateway as well. A wrong signature
    // means the far end does not hold the credential it claims to, so the session is
    // refused even though the outcome says granted.
    if (server_signature != session.scram_expected_server_signature) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "BinaryOrderGatewayThread: connection {} ServerSignature mismatch -- refusing logon",
                   session.conn_id.get_value());
        refuse_logon(session, pubsub_itc_fw_app::LogonOutcome::AuthenticationFailed, "server signature mismatch");
        return;
    }

    // Cancel-on-disconnect for this comp id, provisioned in the database and delivered with
    // the authentication result. Absent leaves the optionals empty and the gateway's own
    // configured defaults apply.
    if (view.has_cancel_on_disconnect_enabled) {
        session.cancel_on_disconnect_enabled = view.cancel_on_disconnect_enabled;
    }
    if (view.has_cancel_on_disconnect_grace_period_seconds) {
        session.cancel_on_disconnect_grace_period_seconds = view.cancel_on_disconnect_grace_period_seconds;
    }

    session.logged_on = true;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: connection {} authenticated and logged on as '{}'",
               session.conn_id.get_value(), session.comp_id);

    // Back inside its grace period: the orders this comp id left resting stay resting.
    const size_t reclaimed = reclaim_grace_session(session.comp_id);
    if (reclaimed > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "BinaryOrderGatewayThread: comp_id='{}' reconnected within the grace period -- {} order(s) left resting, none cancelled", session.comp_id,
                   reclaimed);
    }
    send_logon_ack(session.conn_id, pubsub_itc_fw_app::LogonOutcome::Accepted, {});
}

BinarySession* BinaryOrderGatewayThread::find_session_by_conn_id(int32_t conn_id_value) {
    auto it = sessions_.find(conn_id_value);
    return it == sessions_.end() ? nullptr : &it->second;
}

void BinaryOrderGatewayThread::handle_new_order_single(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    ++orders_received_;
    // No ClOrdID here, unlike the FIX gateway's equivalent: this gateway forwards the order
    // without decoding it, and decoding one purely to log it would undo the thing that makes
    // this gateway cheap.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "GW-NOS-RECV connection={} bytes={} comp_id={}", session.conn_id.get_value(),
               message.payload_size(), session.comp_id);
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle), message.payload(),
                              static_cast<size_t>(message.payload_size()), session);
    release_pdu_payload(message);
}

void BinaryOrderGatewayThread::handle_order_cancel_request(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest), message.payload(),
                              static_cast<size_t>(message.payload_size()), session);
    release_pdu_payload(message);
}

void BinaryOrderGatewayThread::forward_order_in_envelope(int16_t inner_pdu_id, const uint8_t* payload, size_t size, const BinarySession& session) {
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.pdu_id = inner_pdu_id;
    envelope.payload.data = payload;
    envelope.payload.size = size;
    // The sequencer stamps seq_no and wall_time_ns; both are left zero here.
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = session.conn_id.get_value();
    envelope.has_origin_gateway_id = true;
    envelope.origin_gateway_id = gateway_ids::binary_order_gateway;
    envelope.has_gateway_instance_id = true;
    envelope.gateway_instance_id = config_.instance_id;
    if (!session.comp_id.empty()) {
        envelope.has_sender_comp_id = true;
        envelope.sender_comp_id = session.comp_id;
    }

    forward_envelope_to_sequencers(envelope);
}

void BinaryOrderGatewayThread::forward_envelope_to_sequencers(const pubsub_itc_fw_app::WalRecord& envelope) {
    if (sequencer_primary_conn_id_.get_value() != 0) {
        send_pdu(sequencer_primary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, 0, envelope);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "BinaryOrderGatewayThread: primary sequencer not connected -- order not forwarded to primary");
    }
    if (config_.ha_enabled) {
        if (sequencer_secondary_conn_id_.get_value() != 0) {
            send_pdu(sequencer_secondary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, 0, envelope);
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "BinaryOrderGatewayThread: secondary sequencer not connected -- order not forwarded to secondary");
        }
    }
}

void BinaryOrderGatewayThread::handle_execution_report(const pubsub_itc_fw::EventMessage& message) {
    if (message.pdu_id() != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: unexpected PDU id {} from the sequencer -- dropping",
                   message.pdu_id());
        release_pdu_payload(message);
        return;
    }

    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    // Only the envelope is decoded. The ER inside it is relayed to the client as the
    // bytes that arrived -- this gateway has no reason to understand a message it
    // merely delivers, and not decoding it means it needs no change when the ER gains
    // a field.
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: failed to decode the ER envelope -- dropping");
        release_pdu_payload(message);
        return;
    }

    if (!envelope.has_gateway_session_conn_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOrderGatewayThread: ER seq={} carries no session connection id -- dropping",
                   envelope.seq_no);
        release_pdu_payload(message);
        return;
    }

    auto it = sessions_.find(envelope.gateway_session_conn_id);
    if (it == sessions_.end()) {
        // The ordinary case is a client that disconnected while its order was in flight,
        // including the acknowledgements of the cancels sent on its behalf.
        ++execution_reports_dropped_;
        report_order_progress();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "BinaryOrderGatewayThread: ER seq={} for connection {} -- client already disconnected -- dropping", envelope.seq_no,
                   envelope.gateway_session_conn_id);
        release_pdu_payload(message);
        return;
    }

    // Decode the report to maintain the open-order set. The bytes relayed to the client
    // are still the ones that arrived -- this reads the report, it does not rebuild it.
    pubsub_itc_fw_app::ExecutionReportView report{};
    size_t report_bytes_consumed = 0;
    if (pubsub_itc_fw_app::decode(report, envelope.payload.data, envelope.payload.size, report_bytes_consumed, arena, arena_bytes_needed)) {
        track_open_order(it->second, report);
    } else {
        // Relay it regardless: the client is the report's audience, and a gateway that
        // cannot read a message still has no business withholding it.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryOrderGatewayThread: ER seq={} could not be decoded for order tracking -- relaying it but not tracking the order", envelope.seq_no);
    }

    send_pdu_payload(it->second.conn_id, envelope.pdu_id, envelope.seq_no, envelope.payload.data, envelope.payload.size);
    ++execution_reports_sent_;
    report_order_progress();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "GW-ER-SENT connection={} seq={} comp_id={}", envelope.gateway_session_conn_id, envelope.seq_no,
               it->second.comp_id);
    release_pdu_payload(message);
}

void BinaryOrderGatewayThread::track_open_order(BinarySession& session, const pubsub_itc_fw_app::ExecutionReportView& report) {
    if (!report.has_cl_ord_id) {
        return;
    }

    if (is_terminal_ord_status(report.ord_status)) {
        // Looking up by string_view compares contents, so this finds the entry keyed by
        // the pool storage without building a std::string.
        auto existing = session.open_orders.find(std::string_view(report.cl_ord_id));
        if (existing != session.open_orders.end()) {
            open_order_pool_->deallocate(existing->second);
            session.open_orders.erase(existing);
        }
        return;
    }

    // Over-long values would overrun the entry's fixed arrays. The gateway validates
    // ClOrdID at ingress, and symbol and quantity come from the matching engine rather
    // than the client, so this is a guard against a pipeline defect, not client input.
    const size_t cl_ord_id_length = report.cl_ord_id.size();
    const size_t symbol_length = report.symbol.size();
    const size_t order_qty_length = report.has_order_qty ? report.order_qty.size() : 0;
    if (cl_ord_id_length > fix_order_limits::max_cl_ord_id_length || symbol_length > open_orders::max_supported_symbol_length ||
        order_qty_length > open_orders::max_supported_order_qty_length) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "BinaryOrderGatewayThread: ExecutionReport field too long to track (ClOrdID {} bytes, symbol {}, qty {}) -- order not tracked for cancel",
                   cl_ord_id_length, symbol_length, order_qty_length);
        return;
    }

    // A repeated non-terminal report for an order already tracked -- a partial fill, say --
    // updates the entry in place. Allocating a second one would strand the first in the
    // pool, since the map can only hold one entry per ClOrdID.
    auto tracked = session.open_orders.find(std::string_view(report.cl_ord_id));
    const bool already_tracked = tracked != session.open_orders.end();
    open_orders::OpenOrderEntry* entry = already_tracked ? tracked->second : open_order_pool_->allocate();

    std::memcpy(entry->cl_ord_id, report.cl_ord_id.data(), cl_ord_id_length);
    entry->cl_ord_id[cl_ord_id_length] = '\0';
    entry->cl_ord_id_len = static_cast<uint8_t>(cl_ord_id_length);
    std::memcpy(entry->symbol, report.symbol.data(), symbol_length);
    entry->symbol[symbol_length] = '\0';
    entry->symbol_len = static_cast<uint8_t>(symbol_length);
    if (order_qty_length > 0) {
        std::memcpy(entry->order_qty, report.order_qty.data(), order_qty_length);
    }
    entry->order_qty[order_qty_length] = '\0';
    entry->order_qty_len = static_cast<uint8_t>(order_qty_length);
    entry->side = static_cast<char>(report.side);
    // Kept so the cancel-on-disconnect drain can leave persistent orders resting; absent
    // means the client sent none, which implies Day and claims no exemption.
    entry->time_in_force = report.has_time_in_force ? static_cast<char>(report.time_in_force) : char{0};

    if (!already_tracked) {
        // The key views the pool storage, which is stable for the entry's lifetime.
        session.open_orders.emplace(std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), entry);
    }
}

void BinaryOrderGatewayThread::report_order_progress() {
    const int64_t accounted = execution_reports_sent_ + execution_reports_dropped_;
    if (accounted % order_progress_interval != 0) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "GW-PROGRESS accounted={} sent={} dropped={} nos_received={}", accounted, execution_reports_sent_,
               execution_reports_dropped_, orders_received_);
}

void BinaryOrderGatewayThread::queue_session_for_cleanup(BinarySession& session) {
    if (session.open_orders.empty()) {
        return;
    }

    const size_t total_orders = session.open_orders.size();

    // Per comp id where provisioned, the gateway's own setting otherwise.
    const bool cancel_enabled = session.cancel_on_disconnect_enabled.value_or(config_.cancel_on_disconnect_enabled);
    const std::chrono::seconds grace_period = session.cancel_on_disconnect_grace_period_seconds.has_value()
                                                  ? std::chrono::seconds{*session.cancel_on_disconnect_grace_period_seconds}
                                                  : config_.cancel_on_disconnect_grace_period;

    // Switched off: the member owns its book across a disconnect. The pool entries still
    // go back -- the session map is about to be destroyed -- but nothing is cancelled.
    if (!cancel_enabled) {
        for (const auto& [cl_ord_id, entry] : session.open_orders) {
            open_order_pool_->deallocate(entry);
        }
        session.open_orders.clear();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "BinaryOrderGatewayThread: connection {} disconnected with {} open order(s) -- cancel-on-disconnect is disabled, leaving them resting",
                   session.conn_id.get_value(), total_orders);
        return;
    }

    DeadSession dead;
    // Collected into a flat vector rather than moving the map: the orders are only ever
    // consumed in sequence from here, and draining a map by repeatedly taking begin()
    // rescans the buckets already emptied, which is quadratic in the order count.
    dead.open_orders.reserve(total_orders);
    size_t persistent_orders = 0;
    for (const auto& [cl_ord_id, entry] : session.open_orders) {
        // GoodTillCancel and GoodTillDate were placed to outlive the session; cancelling
        // them because a socket dropped would override an explicit instruction.
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
                   "BinaryOrderGatewayThread: connection {} disconnected -- {} persistent order(s) (GTC/GTD) left resting", session.conn_id.get_value(),
                   persistent_orders);
    }
    if (dead.open_orders.empty()) {
        return;
    }

    dead.session_conn_id = session.conn_id.get_value();
    dead.comp_id = session.comp_id;
    dead.cancel_id_counter = session.cancel_id_counter;

    // No clean-logout equivalent in this protocol, so every disconnect takes the window.
    if (grace_period.count() == 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "BinaryOrderGatewayThread: connection {} disconnected with {} open order(s) -- queuing cancels now", session.conn_id.get_value(),
                   dead.open_orders.size());
        pending_cancel_sessions_.push_back(std::move(dead));
        if (!cancel_drain_timer_active_) {
            cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
            cancel_drain_timer_active_ = true;
        }
        return;
    }

    dead.cancel_due = std::chrono::steady_clock::now() + grace_period;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "BinaryOrderGatewayThread: connection {} (comp_id='{}') disconnected with {} open order(s) -- holding {}s for reconnect before cancelling",
               session.conn_id.get_value(), dead.comp_id, dead.open_orders.size(), grace_period.count());
    grace_sessions_.push_back(std::move(dead));
    arm_grace_timer();
}

void BinaryOrderGatewayThread::arm_grace_timer() {
    if (grace_timer_active_ || grace_sessions_.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto due = grace_sessions_.front().cancel_due;
    const auto delay = due > now ? std::chrono::duration_cast<std::chrono::milliseconds>(due - now) : std::chrono::milliseconds{1};
    grace_timer_id_ = start_one_off_timer(delay);
    grace_timer_active_ = true;
}

void BinaryOrderGatewayThread::expire_grace_sessions() {
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
                   "BinaryOrderGatewayThread: grace period expired for {} session(s) -- cancelling {} order(s); no reconnect arrived", expired_sessions,
                   expired_orders);
        if (!cancel_drain_timer_active_) {
            cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
            cancel_drain_timer_active_ = true;
        }
    }

    arm_grace_timer();
}

size_t BinaryOrderGatewayThread::reclaim_grace_session(std::string_view comp_id) {
    size_t reclaimed = 0;
    for (auto it = grace_sessions_.begin(); it != grace_sessions_.end();) {
        if (it->comp_id != comp_id) {
            ++it;
            continue;
        }
        // Cancel nothing. The gateway's bookkeeping is released rather than handed to the
        // new session: the matching engine keys an order by the connection it arrived on,
        // so an order placed on the old one cannot be cancelled from the new one. Re-keying
        // onto a recovered session is step 5 of docs/design/gateway_ha.md.
        for (open_orders::OpenOrderEntry* entry : it->open_orders) {
            open_order_pool_->deallocate(entry);
        }
        reclaimed += it->open_orders.size();
        it = grace_sessions_.erase(it);
    }
    return reclaimed;
}

void BinaryOrderGatewayThread::drain_pending_cancels() {
    cancel_drain_timer_active_ = false;

    int sent = 0;
    while (!pending_cancel_sessions_.empty() && sent < cancel_drain_batch_size) {
        DeadSession& dead = pending_cancel_sessions_.front();

        if (dead.next_order_index >= dead.open_orders.size()) {
            pending_cancel_sessions_.pop_front();
            continue;
        }

        open_orders::OpenOrderEntry* entry = dead.open_orders[dead.next_order_index];

        // Formatted into a stack buffer rather than built with std::string: the obvious
        // spelling allocates three times per cancel, on a path that can generate thousands
        // in a burst.
        std::array<char, cancel_cl_ord_id::max_length> cancel_id_buffer{};
        const std::string_view cancel_cl_ord_id_text = cancel_cl_ord_id::format(cancel_id_buffer, "BGW-CXL-", dead.session_conn_id, dead.cancel_id_counter++);
        if (cancel_cl_ord_id_text.empty()) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "BinaryOrderGatewayThread: could not format a cancel ClOrdID for session {} -- order left on the book", dead.session_conn_id);
            open_order_pool_->deallocate(entry);
            ++dead.next_order_index;
            ++sent;
            continue;
        }

        pubsub_itc_fw_app::OrderCancelRequest cancel{};
        cancel.orig_cl_ord_id = std::string_view(entry->cl_ord_id, entry->cl_ord_id_len);
        cancel.cl_ord_id = cancel_cl_ord_id_text;
        cancel.symbol = std::string_view(entry->symbol, entry->symbol_len);
        cancel.side = static_cast<pubsub_itc_fw_app::Side>(entry->side);
        cancel.transact_time = 0; // the sequencer stamps the authoritative time
        if (entry->order_qty_len > 0) {
            cancel.order_qty = std::string_view(entry->order_qty, entry->order_qty_len);
        }

        // Unlike a client order, this one is built here rather than relayed, so it has to
        // be encoded before it can be wrapped. Measure then fit, into a reused buffer.
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(encode(cancel, nullptr, 0, bytes_written, bytes_needed));
        if (cancel_encode_buffer_.size() < bytes_needed) {
            cancel_encode_buffer_.resize(bytes_needed);
        }
        if (!encode(cancel, cancel_encode_buffer_.data(), cancel_encode_buffer_.size(), bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "BinaryOrderGatewayThread: failed to encode the cancel for ClOrdID '{}' ({} bytes needed) -- order left on the book",
                       std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), bytes_needed);
        } else {
            pubsub_itc_fw_app::WalRecord envelope{};
            envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
            envelope.payload.data = cancel_encode_buffer_.data();
            envelope.payload.size = bytes_written;
            // The departed session's identity still rides on the envelope so the cancel
            // is attributed to the client whose order it retires.
            envelope.has_gateway_session_conn_id = true;
            envelope.gateway_session_conn_id = dead.session_conn_id;
            envelope.has_origin_gateway_id = true;
            envelope.origin_gateway_id = gateway_ids::binary_order_gateway;
            envelope.has_gateway_instance_id = true;
            envelope.gateway_instance_id = config_.instance_id;
            if (!dead.comp_id.empty()) {
                envelope.has_sender_comp_id = true;
                envelope.sender_comp_id = dead.comp_id;
            }
            forward_envelope_to_sequencers(envelope);
        }

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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryOrderGatewayThread: cancel drain complete -- {} cancel(s) sent",
                   cancels_sent_this_drain_);
        cancels_sent_this_drain_ = 0;
    }
}

BinarySession* BinaryOrderGatewayThread::find_session(const pubsub_itc_fw::ConnectionID& id) {
    auto it = sessions_.find(id.get_value());
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool BinaryOrderGatewayThread::comp_id_in_use(std::string_view comp_id) const {
    for (const auto& [conn_id, session] : sessions_) {
        if (session.logged_on && session.comp_id == comp_id) {
            return true;
        }
    }
    return false;
}

} // namespaces
